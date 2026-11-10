in vec3 vWorld;
in vec3 vNormal;
in vec2 vUV;
out vec4 outColor;

// Interior surface shader. Writes linear HDR radiance; interior_post.frag tonemaps it.
//
//   direct    one-sided rectangular lights, analytic diffuse form factor, soft shadow per light
//   indirect  irradiance probe volume (L1 SH per colour channel), dimmed by screen-space AO
//   specular  box-projected reflection probe; furniture is reflected through box proxies

#define MAX_LIGHTS 4
#define MAX_PROXIES 8
#define SHADOW_TAPS 24

// uPass: what this render is for
#define PASS_FINAL 0   // the image
#define PASS_PROBE 1   // irradiance probe capture: diffuse only, emitters black (they are the direct light)
#define PASS_ENV 2     // reflection probe capture: diffuse + emitters
uniform int uPass;
uniform int uDebugView;

// ---- material -----------------------------------------------------------------------------------
uniform sampler2D uAlbedoTex;
uniform sampler2D uNormalTex;
uniform sampler2D uOrmTex;   // R roughness, G metallic
uniform vec4 uTint;          // rgb tint, a = desaturate
uniform vec4 uMat;           // roughness, metallic, -, normal map strength
uniform vec3 uEmissive;      // radiance; non-zero = the surface is a light diffuser
uniform vec4 uMat2;          // roughness variation from albedo, has orm texture, two sided, dielectric F0

// ---- lights and shadows -------------------------------------------------------------------------
uniform int uLightCount;
uniform vec3 uLightPos[MAX_LIGHTS];
uniform vec3 uLightU[MAX_LIGHTS];        // half extents
uniform vec3 uLightV[MAX_LIGHTS];
uniform vec3 uLightN[MAX_LIGHTS];        // emitting side
uniform vec3 uLightRadiance[MAX_LIGHTS];
uniform mat4 uShadowMat[MAX_LIGHTS];     // world -> shadow clip space
uniform vec4 uShadowRect[MAX_LIGHTS];    // atlas tile: scale xy, offset zw
uniform vec4 uShadowProj;                // near, far, tan(fov/2), penumbra light radius (m)
uniform vec2 uShadowTexel;               // one shadow texel / one blocker texel, in tile uv
uniform sampler2DShadow uShadowTex;
uniform sampler2D uBlockerTex;           // distance to the nearest occluder around each texel, pre-blurred

// ---- indirect -----------------------------------------------------------------------------------
uniform sampler3D uShR, uShG, uShB;      // per channel: (L0, L1x, L1y, L1z), already convolved to irradiance / pi
uniform vec3 uProbeOrigin, uProbeInvExtent;
uniform sampler2D uAOTex;
uniform vec2 uInvScreen;
uniform vec2 uAO;                        // strength, share that also darkens direct light

// ---- reflections --------------------------------------------------------------------------------
uniform samplerCube uEnvTex;
uniform vec3 uEnvPos, uRoomMin, uRoomMax;
uniform vec2 uEnv;                       // max lod, specular scale
uniform int uProxyCount;
uniform vec3 uProxyMin[MAX_PROXIES];
uniform vec3 uProxyMax[MAX_PROXIES];
uniform vec3 uProxyAlbedo;

// ---- hand torch and the master switch for the room's own lights (final pass only) ----
uniform float uSceneLight;   // 1 = lights on, 0 = dark
uniform vec4 uFlashPos;      // xyz, brightness (0 = off)
uniform vec3 uFlashDir;
uniform mat4 uFlashMat;      // world -> the torch's clip space: shadow map and beam pattern
uniform sampler2DShadow uFlashShadow;
uniform sampler2D uFlashCookie;
uniform sampler2D uFlashBounceTex; // one texel: colour of what the beam lands on, how far away that is
uniform float uFlashBounce;        // strength of the light that comes back from there
uniform vec3 uFlashColor;
uniform vec4 uFlashShape;     // beam narrowing (1 = as authored), extra falloff with distance, harshness of the core, highlight strength
uniform vec2 uGloss;          // smallest roughness any surface may have, scale on all roughness
// Irradiance / pi arriving at P for a surface facing N.
vec3 irradiance(vec3 P, vec3 N) {
    vec3 uvw = (P - uProbeOrigin) * uProbeInvExtent;
    vec4 n = vec4(1.0, N);
    return max(vec3(dot(texture(uShR, uvw), n), dot(texture(uShG, uvw), n), dot(texture(uShB, uvw), n)), 0.0);
}

// ---- rectangular area light: diffuse form factor ------------------------------------------------
// Integral over one polygon edge as seen from the shaded point (Heitz/Hill, "LTC" area lights).
vec3 edgeIntegral(vec3 a, vec3 b) {
    float x = dot(a, b);
    float y = abs(x);
    float p = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float q = 3.4175940 + (4.1616724 + y) * y;
    float v = p / q;
    float thetaOverSin = x > 0.0 ? v : 0.5 * inversesqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(a, b) * thetaOverSin;
}

// Fraction of the hemisphere (cosine weighted) the light covers: irradiance = pi * radiance * this.
float rectFormFactor(int i, vec3 P, vec3 N) {
    vec3 c = uLightPos[i] - P;
    if (dot(c, uLightN[i]) > -0.002) return 0.0; // behind or level with the emitter
    vec3 u = uLightU[i], v = uLightV[i];
    vec3 p0 = normalize(c - u - v), p1 = normalize(c + u - v), p2 = normalize(c + u + v), p3 = normalize(c - u + v);
    vec3 F = edgeIntegral(p0, p1) + edgeIntegral(p1, p2) + edgeIntegral(p2, p3) + edgeIntegral(p3, p0);
    float len = length(F);
    if (len < 1e-6) return 0.0;
    vec3 dir = F / len;
    if (dot(dir, c) < 0.0) dir = -dir;
    // the light may dip below the surface's horizon: treat it as a sphere of the same solid angle
    float z = dot(N, dir);
    return len * max((len * len + z) / (len + 1.0), 0.0);
}

// ---- soft shadow --------------------------------------------------------------------------------
// The penumbra widens with the gap between occluder and receiver. The occluder distance comes from a
// pre-blurred map instead of a per-pixel search, so the penumbra size varies smoothly and the fixed
// tap pattern needs no noise.
float shadow(int i, vec3 P, vec3 N) {
    vec4 c = uShadowMat[i] * vec4(P + N * 0.02, 1.0);
    float zr = c.w; // distance from the light along its axis
    if (zr <= uShadowProj.x) return 1.0;
    vec2 uv = c.xy / c.w * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    vec4 rect = uShadowRect[i];
    vec2 buv = clamp(uv, vec2(uShadowTexel.y), vec2(1.0 - uShadowTexel.y));
    float zb = texture(uBlockerTex, buv * rect.xy + rect.zw).r;
    if (zb > zr - 0.04) return 1.0; // nothing between this point and the light

    float penumbra = uShadowProj.w * (zr - zb) / zb;                 // metres, at the receiver
    float radius = max(penumbra / (2.0 * zr * uShadowProj.z), 1.5 * uShadowTexel.x);
    float zref = zr - 0.025;
    float dref = uShadowProj.y * (zref - uShadowProj.x) / (zref * (uShadowProj.y - uShadowProj.x));
    vec2 lo = vec2(uShadowTexel.x), hi = vec2(1.0 - uShadowTexel.x);
    float lit = 0.0;
    for (int k = 0; k < SHADOW_TAPS; k++) {
        vec2 t = clamp(uv + vogel(k, SHADOW_TAPS, 0.0) * radius, lo, hi);
        lit += texture(uShadowTex, vec3(t * rect.xy + rect.zw, dref));
    }
    return lit / float(SHADOW_TAPS);
}

// ---- reflections --------------------------------------------------------------------------------
vec3 envBRDF(vec3 F0, float rough, float NoV) { // Karis' analytic fit of the split-sum BRDF term
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * ab.x + ab.y;
}

vec3 reflection(vec3 P, vec3 R, float rough) {
    vec3 inv = 1.0 / (R + vec3(equal(R, vec3(0.0))) * 1e-6);
    // where the reflected ray leaves the room: the probe then lines up with the real walls
    vec3 tm = max((uRoomMax - P) * inv, (uRoomMin - P) * inv);
    float tRoom = min(tm.x, min(tm.y, tm.z));
    vec3 col = textureLod(uEnvTex, P + R * tRoom - uEnvPos, rough * uEnv.x).rgb;
    // the probe sees the room from one point; rescale it by how much light actually reaches here
    float here = luma(irradiance(P, R)), there = luma(irradiance(uEnvPos, R));
    col *= mix(1.0, clamp(here / max(there, 1e-3), 0.0, 1.0), 0.85);

    // furniture: nearest proxy box hit before the wall
    float best = tRoom;
    vec3 hitN = vec3(0.0);
    for (int i = 0; i < uProxyCount; i++) {
        vec3 ta = (uProxyMin[i] - P) * inv, tb = (uProxyMax[i] - P) * inv;
        vec3 t0 = min(ta, tb), t1 = max(ta, tb);
        float tn = max(t0.x, max(t0.y, t0.z)), tf = min(t1.x, min(t1.y, t1.z));
        if (tn < tf && tn > 0.002 && tn < best) {
            best = tn;
            hitN = -sign(R) * step(t0.yzx, t0.xyz) * step(t0.zxy, t0.xyz);
        }
    }
    if (best < tRoom) {
        vec3 hit = P + R * best;
        vec3 proxy = uProxyAlbedo * irradiance(hit + hitN * 0.15, hitN);
        // rough surfaces lose the reflection with distance instead of blurring it
        col = mix(col, proxy, exp(-best * rough * 3.0));
    }
    return col;
}

// ---- normal mapping without tangents (cotangent frame from screen derivatives) -------------------
vec3 perturbNormal(vec3 N, vec3 P, vec2 uv, vec3 tn) {
    vec3 dp1 = dFdx(P), dp2 = dFdy(P);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    return normalize(mat3(T * invmax, B * invmax, N) * tn);
}

void main() {
    // ---- material ----
    vec3 tex = texture(uAlbedoTex, vUV).rgb;
    float texLuma = luma(tex);
    vec3 albedo = mix(tex, vec3(texLuma), uTint.a) * uTint.rgb;
    float rough = uMat.x, metal = uMat.y;
    if (uMat2.y > 0.5) {
        vec2 orm = texture(uOrmTex, vUV).rg;
        rough *= orm.r;
        metal *= orm.g;
    }
    rough += (0.5 - texLuma) * uMat2.x;

    vec3 Ng = normalize(vNormal);
    if (uMat2.z > 0.5 && !gl_FrontFacing) Ng = -Ng;
    vec3 N = Ng;
    if (uMat.w > 0.0) {
        vec3 tn = texture(uNormalTex, vUV).xyz * 2.0 - 1.0;
        tn.xy *= uMat.w;
        N = perturbNormal(Ng, vWorld, vUV, tn);
    }
    // widen highlights where the normal changes within a pixel: no specular shimmer
    vec3 dnx = dFdx(N), dny = dFdy(N);
    float variance = min(0.5 * (dot(dnx, dnx) + dot(dny, dny)), 0.18);
    rough = sqrt(clamp(rough * rough + variance, 0.0, 1.0));
    rough = clamp(rough * uGloss.y, uGloss.x, 1.0); // stylised: nothing is mirror sharp

    vec3 V = normalize(uCam - vWorld);
    float NoV = max(dot(N, V), 0.0);
    vec3 diffuseColor = albedo * (1.0 - metal);
    vec3 F0 = mix(vec3(uMat2.w), albedo, metal);

    // ---- light ----
    float ao = 1.0;
    if (uPass == PASS_FINAL && uAO.x > 0.0) ao = mix(1.0, texture(uAOTex, gl_FragCoord.xy * uInvScreen).r, uAO.x);

    vec3 direct = vec3(0.0);
    float shadowAll = 0.0, weightAll = 0.0;
    for (int i = 0; i < uLightCount; i++) {
        float ff = rectFormFactor(i, vWorld, N);
        if (ff <= 0.0) continue;
        float s = shadow(i, vWorld, Ng);
        direct += uLightRadiance[i] * (ff * s);
        shadowAll += ff * s;
        weightAll += ff;
    }
    direct *= uSceneLight;
    vec3 indirect = irradiance(vWorld + Ng * 0.15, N) * uSceneLight;

    vec3 flashSpec = vec3(0.0);
    if (uFlashPos.w > 0.0) {
        vec3 Lv = uFlashPos.xyz - vWorld;
        float d2 = dot(Lv, Lv);
        vec3 L = Lv * inversesqrt(d2);
        float c = dot(-L, uFlashDir);
        // a bright core, a dimmer ring around it from the reflector, and a little spill
        float beam = smoothstep(0.5, 0.9, c) * 0.035; // spill; the beam itself comes from the pattern texture
        vec4 fc = uFlashMat * vec4(vWorld + Ng * 0.012, 1.0);
        if (fc.w > 0.0) {
            vec3 fp = fc.xyz / fc.w * 0.5 + 0.5;
            if (fp.x > 0.0 && fp.x < 1.0 && fp.y > 0.0 && fp.y < 1.0 && fp.z < 1.0) {
                vec2 cuv = (fp.xy - 0.5) * uFlashShape.x + 0.5;
                float ck = all(greaterThan(cuv, vec2(0.0))) && all(lessThan(cuv, vec2(1.0))) ? texture(uFlashCookie, cuv).r : 0.0;
                beam += mix(ck, ck * ck * 2.2, uFlashShape.z);
                // a small source: shadows are crisp, only softened enough to hide the texels
                float rot = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
                float lit = 0.0;
                for (int i = 0; i < 6; i++) lit += texture(uFlashShadow, vec3(fp.xy + vogel(i, 6, rot) * (1.6 / 768.0), fp.z));
                beam *= lit / 6.0;
            }
        }
        float NoL = max(dot(N, L), 0.0);
        vec3 E = uFlashColor * (uFlashPos.w * beam * NoL / ((d2 + 0.25) * (1.0 + uFlashShape.y * d2)));
        direct += E;
        if (E != vec3(0.0)) { // GGX highlight
            vec3 H = normalize(L + V);
            float a = rough * rough, a2 = a * a;
            float NoH = max(dot(N, H), 0.0), VoH = max(dot(V, H), 0.0);
            float dn = NoH * NoH * (a2 - 1.0) + 1.0;
            float D = a2 / (dn * dn);
            float k = a * 0.5;
            float vis = 0.25 / ((NoL * (1.0 - k) + k) * (NoV * (1.0 - k) + k) + 1e-4);
            vec3 F = F0 + (1.0 - F0) * pow(1.0 - VoH, 5.0);
            flashSpec = E * D * vis * F * (uEnv.y * uFlashShape.w);
        }
        // one bounce, as a soft point light sitting where the beam lands
        vec4 lit_patch = texelFetch(uFlashBounceTex, ivec2(0), 0);
        vec3 Bv = uFlashPos.xyz + uFlashDir * max(lit_patch.a - 0.25, lit_patch.a * 0.5) - vWorld;
        float b2 = dot(Bv, Bv);
        indirect += uFlashColor * lit_patch.rgb * (uFlashBounce * (dot(N, Bv * inversesqrt(b2 + 1e-4)) * 0.4 + 0.6) / (b2 + 1.0));
    }
    direct *= mix(1.0, ao, uAO.y);
    indirect *= ao;

    vec3 col = diffuseColor * (direct + indirect);

    vec3 spec = vec3(0.0);
    if (uPass == PASS_FINAL) {
        vec3 R = reflect(-V, N);
        R = normalize(R + Ng * max(0.02 - dot(R, Ng), 0.0)); // keep it above the surface
        float specOcclusion = clamp(pow(NoV + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao, 0.0, 1.0);
        spec = reflection(vWorld + Ng * 0.01, R, rough) * envBRDF(F0, rough, NoV) * specOcclusion * uEnv.y * uSceneLight + flashSpec;
        col += spec;
    }

    // light diffusers. Black for the irradiance probes: their light is already the analytic direct term.
    if (uEmissive != vec3(0.0)) col = uPass == PASS_PROBE ? vec3(0.0) : mix(vec3(0.7) * (direct + indirect), uEmissive, uSceneLight); // switched off: a milky sheet of plastic

    if (uDebugView == 1 || uDebugView == 10) col = albedo; // 10: capture for the torch's bounce
    else if (uDebugView == 2) col = N * 0.5 + 0.5;
    else if (uDebugView == 3) col = direct + indirect;
    else if (uDebugView == 4) col = direct;
    else if (uDebugView == 5) col = indirect;
    else if (uDebugView == 6) col = vec3(ao);
    else if (uDebugView == 7) col = vec3(weightAll > 0.0 ? shadowAll / weightAll : 1.0);
    else if (uDebugView == 8) col = vec3(rough, metal, 0.0);
    else if (uDebugView == 9) col = spec;

    outColor = vec4(col, uDebugView == 10 ? distance(uCam, vWorld) : 1.0);
}

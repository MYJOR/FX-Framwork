in vec3 vWorld;
in vec3 vNormal;
out vec4 outColor;

// One glitter layer: a grid of randomly oriented micro-facets. A cell flashes
// only when its facet happens to mirror the sun toward the eye.
float glitterLayer(vec2 wp, float scale, vec3 N, vec3 H, float fade) {
    vec2 g = wp * scale;
    vec2 cell = floor(g);
    vec2 r = hash22(cell);
    float present = step(0.65, hash12(cell + 7.7));
    vec3 facet = normalize(N + vec3(r.x - 0.5, 0.0, r.y - 0.5) * 1.8);
    float s = pow(max(dot(facet, H), 0.0), 700.0);
    // keep each cell a round-ish dot instead of a hard square
    vec2 local = fract(g) - 0.5;
    float dotMask = smoothstep(0.45, 0.1, length(local));
    return s * present * dotMask * fade;
}

uniform vec3 uPlayerPos; // feet position of the player
uniform int uDebugView;  // diagnostic view mode, 0 = final image
uniform sampler2D uTrail; // footprints/trails: R = depression, G = raised rim (see trail.c)
const float TRAIL_SIZE = 64.0;
const float TRAIL_TEXELS = 2048.0;
const float TRAIL_DEPTH_M = 0.07; // metres represented by a full-strength texel value

float trailHeight(vec2 uv) {
    vec2 s = texture(uTrail, uv).rg;
    return s.g - s.r;
}

// Soft capsule shadow: how close does the ray from this point toward the sun pass
// to the player's body (sampled along the capsule axis)? Penumbra widens with distance.
float playerShadow(vec3 F, vec3 L) {
    float best = 1e3, tAt = 0.0;
    for (int i = 0; i < 6; i++) {
        vec3 d = uPlayerPos + vec3(0.0, 0.32 + float(i) * 0.2, 0.0) - F;
        float t = dot(d, L);
        if (t > 0.0) {
            float m = length(d - L * t);
            if (m < best) { best = m; tAt = t; }
        }
    }
    return 1.0 - smoothstep(0.16, 0.34 + tAt * 0.05, best);
}

// ---- specular layer tuning ----
const float SPEC_ROUGH_TIGHT = 0.40;  // glossy lobe: wet-looking highlights on ripples
const float SPEC_ROUGH_WIDE  = 0.65;  // broad lobe: soft sheen across whole slopes
const float SPEC_TIGHT_AMT   = 0.32;
const float SPEC_WIDE_AMT    = 0.38;
const vec3  SPEC_TINT        = vec3(1.0, 0.82, 0.58); // sand reflects warm, not white

float ggxD(float NoH, float a) {
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d);
}
float smithG(float NoV, float NoL, float a) {
    float k = a * 0.5;
    return (NoV / (NoV * (1.0 - k) + k)) * (NoL / (NoL * (1.0 - k) + k));
}

void main() {
    vec3 toCam = uCam - vWorld;
    float dist = length(toCam);
    vec3 V = toCam / dist;
    vec3 L = normalize(uSunDir);
    vec3 N = normalize(vNormal);

    // --- footprints & trails: sample the displacement map around the player
    float tfade = 1.0 - smoothstep(24.0, 30.0, max(abs(vWorld.x - uPlayerPos.x), abs(vWorld.z - uPlayerPos.z)));
    float trailDepth = 0.0, trailDisturb = 0.0;
    vec2 trailGrad = vec2(0.0);
    if (tfade > 0.0) {
        vec2 tuv = vWorld.xz / TRAIL_SIZE;
        float e = 1.0 / TRAIL_TEXELS;
        vec2 c = texture(uTrail, tuv).rg;
        float hx = trailHeight(tuv + vec2(e, 0.0)) - trailHeight(tuv - vec2(e, 0.0));
        float hz = trailHeight(tuv + vec2(0.0, e)) - trailHeight(tuv - vec2(0.0, e));
        trailGrad = vec2(hx, hz) * (TRAIL_DEPTH_M / (2.0 * TRAIL_SIZE / TRAIL_TEXELS)) * tfade;
        trailDepth = c.r * tfade;
        trailDisturb = clamp(max(c.r, c.g * 0.7) * 1.6, 0.0, 1.0) * tfade;
    }

    // --- wind ripples: fine ridges perpendicular to the wind, fading with distance
    float ripFade = (1.0 - smoothstep(20.0, 110.0, dist)) * (1.0 - trailDisturb);
    float ph = dot(vWorld.xz, WIND) * 7.8 + vnoise(vWorld.xz * 1.35) * 5.0;
    float rip = cos(ph) + 0.5 * cos(ph * 2.1 + 1.3);
    vec2 g1 = vec2(vnoise(vWorld.xz * 27.0 + 3.1) - vnoise(vWorld.xz * 27.0 - 3.1));
    N = normalize(N + vec3(WIND.x, 0.0, WIND.y) * rip * 0.07 * ripFade
                    + vec3(g1.x, 0.0, g1.y) * 0.05 * (1.0 - smoothstep(10.0, 60.0, dist)));
    // height gradient tilts the normal: n ~ (-dh/dx, 1, -dh/dz)
    N = normalize(N + vec3(-trailGrad.x, 0.0, -trailGrad.y) * 1.5);

    // --- base colour: warm gold with large-scale variation
    float tint = vnoise(vWorld.xz * 0.012);
    vec3 sand = mix(vec3(0.78, 0.46, 0.24), vec3(0.93, 0.68, 0.38), tint);
    sand *= 0.92 + 0.16 * vnoise(vWorld.xz * 1.0);
    sand *= 1.0 - 0.30 * trailDepth; // disturbed sand is darker and denser

    // --- diffuse with wrap, warm sun, cool-saturated shadow side
    float ndl = dot(N, L);
    float wrapped = clamp((ndl + 0.06) / 1.06, 0.0, 1.0);
    vec3 sun = sunColor();
    float pShadow = playerShadow(vWorld, L);
    sun *= 1.0 - 0.8 * pShadow;
    vec2 pd = vWorld.xz - uPlayerPos.xz;
    float contactAO = 1.0 - 0.35 * exp(-dot(pd, pd) / 0.35);
    vec3 ambientSky = skyColor(vec3(0.0, 1.0, 0.0)) * 0.42;
    vec3 shadowTint = mix(vec3(0.34, 0.20, 0.38), vec3(0.30, 0.34, 0.50), dayK());
    vec3 ambient = mix(shadowTint, ambientSky, 0.5) * 0.42;

    vec3 col = sand * (sun * pow(wrapped, 1.15) + ambient) * contactAO;

    // terminator glow: a thin saturated band where light falls off
    float term = exp(-pow(ndl * 4.5, 2.0));
    col += sand * vec3(0.9, 0.28, 0.16) * term * 0.28 * smoothstep(-0.1, 0.1, sunHeight());

    // --- rim light: grazing-angle sheen on sun-facing slopes
    float fres = pow(1.0 - max(dot(N, V), 0.0), 4.0);
    col += sun * fres * smoothstep(-0.05, 0.45, ndl) * 0.55;

    // --- specular: GGX (tight + wide lobes) with Schlick fresnel
    vec3 Hs = normalize(L + V);
    float NoL = max(ndl, 0.0), NoV = max(dot(N, V), 1e-3), NoH = max(dot(N, Hs), 0.0);
    float VoH = max(dot(V, Hs), 0.0);
    float fr = 0.04 + 0.96 * pow(1.0 - VoH, 5.0);
    float aT = SPEC_ROUGH_TIGHT * SPEC_ROUGH_TIGHT, aW = SPEC_ROUGH_WIDE * SPEC_ROUGH_WIDE;
    float specT = ggxD(NoH, aT) * smithG(NoV, NoL, aT) / (4.0 * NoV * max(NoL, 1e-3));
    float specW = ggxD(NoH, aW) * smithG(NoV, NoL, aW) / (4.0 * NoV * max(NoL, 1e-3));
    float specMask = smoothstep(0.0, 0.2, ndl);
    // dust haze and distance soften highlights (aliasing and realism)
    float specDist = 1.0 - smoothstep(150.0, 450.0, dist);
    vec3 spec = sun * SPEC_TINT * fr * (specT * SPEC_TIGHT_AMT + specW * SPEC_WIDE_AMT) * NoL * specMask * specDist;
    col += min(spec, vec3(3.0));

    // --- sparkle: three scales so grains stay visible at any distance
    vec3 H = normalize(L + V);
    vec2 px = fwidth(vWorld.xz);
    float lit = smoothstep(0.0, 0.15, ndl);
    float glitter = 0.0;
    float scales[3] = float[3](30.0, 120.0, 480.0);
    for (int i = 0; i < 3; i++) {
        float cellPx = (1.0 / scales[i]) / max(max(px.x, px.y), 1e-5); // cell size in pixels
        float fade = smoothstep(1.2, 3.5, cellPx) * (1.0 - smoothstep(60.0, 200.0, cellPx));
        glitter += glitterLayer(vWorld.xz, scales[i], N, H, fade);
    }
    col += sun * glitter * 9.0 * lit;

    // --- gust-driven drifting sand sheets hugging the surface
    float gu = gust(vWorld.xz, uTime);
    vec2 sp = vec2(dot(vWorld.xz, WIND) - uTime * (6.0 + 10.0 * gu), dot(vWorld.xz, vec2(-WIND.y, WIND.x)));
    float streak = vnoise(vec2(sp.x * 0.60, sp.y * 4.0)) * vnoise(vec2(sp.x * 0.18 + 4.0, sp.y * 0.9));
    streak = smoothstep(0.22, 0.65, streak);
    float lowAngle = 1.0 - clamp(V.y * 1.6, 0.0, 1.0);
    vec3 blown = sand * vec3(1.15, 1.02, 0.88) + sun * 0.12;
    col = mix(col, blown, streak * gu * (0.10 + 0.32 * lowAngle) * (1.0 - 0.6 * smoothstep(8.0, 160.0, dist)));

    // --- atmosphere: orange dust haze (see applyHaze in common.glsl)
    col = applyHaze(col, dist, -V, vWorld.y);
    // dissolve the finite terrain edge into the horizon haze
    float edge = smoothstep(250.0, 345.0, max(abs(vWorld.x), abs(vWorld.z)));
    col = mix(col, hazeColor(-V), edge);

    outColor = vec4(grade(col), 1.0);

    // --- debug views (diagnostic overlay): 0 final, 1 normals, 2 trail map, 3 slide zones, 4 wind gust, 5 overdraw-ish distance
    if (uDebugView == 1) {
        outColor = vec4(N * 0.5 + 0.5, 1.0);
    } else if (uDebugView == 2) {
        vec3 g = vec3(0.05) + sand * 0.12;
        vec2 tc = texture(uTrail, vWorld.xz / TRAIL_SIZE).rg;
        g += vec3(1.0, 0.25, 0.1) * tc.r * tfade + vec3(0.2, 0.7, 1.0) * tc.g * tfade;
        outColor = vec4(g, 1.0);
    } else if (uDebugView == 3) {
        float ny = normalize(vNormal).y;
        vec3 z = mix(vec3(0.15, 0.7, 0.25), vec3(0.95, 0.8, 0.1), smoothstep(0.90, 0.84, ny));
        z = mix(z, vec3(0.9, 0.15, 0.1), smoothstep(0.84, 0.55, ny));
        outColor = vec4(z * (0.55 + 0.45 * max(dot(normalize(vNormal), L), 0.0)), 1.0);
    } else if (uDebugView == 4) {
        float gg = gust(vWorld.xz, uTime);
        outColor = vec4(mix(vec3(0.05, 0.1, 0.35), vec3(1.0, 0.85, 0.2), gg), 1.0);
    } else if (uDebugView == 5) {
        float d = clamp(dist / 400.0, 0.0, 1.0);
        outColor = vec4(vec3(1.0 - d), 1.0);
    }
}

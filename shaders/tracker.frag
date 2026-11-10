in vec3 vWorld;
in vec3 vNormal;
in vec3 vObj;
in vec3 vObjNormal;
in vec2 vUV;       // the screen: 0..1 across it. Everything else: x = 1 on a chamfered edge, 0 on a face
flat in float vMat;
out vec4 outColor;

// The motion tracker (src/tracker.c). Writes linear HDR radiance like interior.frag, and alpha 0: the
// mark the depth of field knows a held thing by.
//
//   body     painted metal, plastic and rubber, lit by the room: the irradiance probes at the hand,
//            the ceiling panels as small sources, the room's reflection probe
//   screen   an emissive phosphor display under glass, drawn here from the ping and the contacts

#define MAX_LIGHTS 8
#define MAX_CONTACTS 8

#define MAT_PAINT 0
#define MAT_DARK 1
#define MAT_RUBBER 2
#define MAT_METAL 3
#define MAT_SCREEN 4
#define MAT_LED 5
#define MAT_BUTTON 6
#define MAT_PLATE 7
#define MAT_KEY 8

uniform vec4 uSH[3];              // per colour channel: L0, L1 xyz. Irradiance / pi = L0 + dot(L1, n)
uniform float uSceneLight;
uniform int uLightCount;
uniform vec3 uLightPos[MAX_LIGHTS];
uniform vec3 uLightU[MAX_LIGHTS];
uniform vec3 uLightV[MAX_LIGHTS];
uniform vec3 uLightN[MAX_LIGHTS];
uniform vec3 uLightRadiance[MAX_LIGHTS];
uniform samplerCube uEnvTex;
uniform float uEnvMaxLod;
uniform sampler2D uExposureTex;   // one texel: the gain the eye adaptation adds
uniform vec3 uBack;               // from the device towards the player, world space

uniform vec4 uScreenRect;         // the screen in device space: centre xy, half size
uniform float uScreenZ;
uniform vec4 uPing;               // phase (0..1 crossing, then rest up to .y), span, seconds, power 0..1
uniform vec4 uContact[MAX_CONTACTS]; // x (right), z (ahead) in ranges; strength
uniform int uContactCount;
uniform vec2 uReadout;            // metres to the nearest contact (< 0: none), range in metres

const vec3 PHOSPHOR = vec3(0.16, 1.0, 0.36);

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) { return 0.5 * vnoise(p) + 0.3 * vnoise(p * 2.3 + 11.0) + 0.2 * vnoise(p * 5.1 + 3.0); }

// 1 on a line of half width w around d = 0, antialiased against the screen's pixels. f = how much d
// changes from one pixel to the next (passed in where d repeats: its own derivative jumps at the seams).
float stroke(float d, float w, float f) { return 1.0 - smoothstep(w - f, w + f, abs(d)); }
float stroke(float d, float w) { return stroke(d, w, fwidth(d)); }

float segDist(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));
}
// Seven-segment digit in a cell 0..0.6 wide, 0..1 high. n = 10: just the middle bar. f = one pixel.
float digit(vec2 p, int n, float f) {
    int bits = n == 0 ? 0x3F : n == 1 ? 0x06 : n == 2 ? 0x5B : n == 3 ? 0x4F : n == 4 ? 0x66 :
               n == 5 ? 0x6D : n == 6 ? 0x7D : n == 7 ? 0x07 : n == 8 ? 0x7F : n == 9 ? 0x6F : 0x40;
    const vec2 tl = vec2(0.10, 0.92), tr = vec2(0.50, 0.92), ml = vec2(0.10, 0.50), mr = vec2(0.50, 0.50),
               bl = vec2(0.10, 0.08), br = vec2(0.50, 0.08);
    float d = 1e3;
    if ((bits & 1) != 0) d = min(d, segDist(p, tl, tr));
    if ((bits & 2) != 0) d = min(d, segDist(p, tr, mr));
    if ((bits & 4) != 0) d = min(d, segDist(p, mr, br));
    if ((bits & 8) != 0) d = min(d, segDist(p, bl, br));
    if ((bits & 16) != 0) d = min(d, segDist(p, ml, bl));
    if ((bits & 32) != 0) d = min(d, segDist(p, tl, ml));
    if ((bits & 64) != 0) d = min(d, segDist(p, ml, mr));
    return 1.0 - smoothstep(0.045, 0.045 + f * 1.5, d);
}

// What the tube shows at uv, as radiance.
vec3 display(vec2 uv) {
    float aspect = uScreenRect.z / uScreenRect.w;
    // the player is the mark near the bottom; ahead is up; the outermost ring (r = 1) is the range
    vec2 p = (uv - vec2(0.5, 0.15)) * vec2(2.0 * aspect, 2.0) / 1.62;
    float r = length(p), ang = atan(p.x, p.y);
    float phase = uPing.x, span = uPing.y;

    // the ping: a ring running outwards, the phosphor it has passed still glowing behind it
    float behind = phase - r;
    float trail = behind > 0.0 ? exp(-behind * 5.0) : exp(-(behind + span) * 5.0) * 0.6;
    float front = phase <= 1.05 ? stroke(r - phase, 0.006) * (1.0 - 0.55 * phase) : 0.0;

    // graticule: range rings, bearing lines every 30 degrees, a tick scale on the outer ring
    float inside = step(r, 1.004);
    float fr = fwidth(r), fa = length(vec2(dFdx(p.x), dFdy(p.x))) + length(vec2(dFdx(p.y), dFdy(p.y))); // one pixel, along r and across it
    float rings = stroke((fract(r * 4.0 + 0.5) - 0.5) / 4.0, 0.0035, fr) * step(0.12, r) * inside;
    float sector = 3.14159265 / 6.0;
    float spokes = stroke((fract(ang / sector + 0.5) - 0.5) * sector * r, 0.0028, fa) * step(0.25, r) * inside;
    float tick = 3.14159265 / 36.0;
    float ticks = stroke((fract(ang / tick + 0.5) - 0.5) * tick * r, 0.0028, fa) * step(0.955, r) * inside;
    float grid = rings * 0.60 + spokes * 0.30 + ticks * 0.55;

    // the player's own mark: a small chevron pointing ahead
    vec2 m = vec2(abs(p.x), p.y);
    float mark = stroke(segDist(m, vec2(0.0, 0.045), vec2(0.035, -0.03)), 0.006);

    // contacts: each lights as the ping passes over it and fades until the next one
    float blips = 0.0;
    for (int i = 0; i < uContactCount; i++) {
        vec2 c = uContact[i].xy;
        float cr = length(c);
        if (cr > 1.0) continue;
        c.y = max(c.y, -0.11); // behind the player: held at the bottom edge
        float since = phase - cr;
        if (since < 0.0) since += span;
        float glow = exp(-since * 3.2) * uContact[i].z;
        float d = length(p - c);
        blips += glow * (1.6 * exp(-d * d / (0.060 * 0.060)) + 5.0 * exp(-d * d / (0.022 * 0.022)));
        blips += glow * 0.8 * stroke(d - 0.075 - since * 0.10, 0.004) * step(since, 0.5); // a ripple off the hit
    }

    // readout, bottom right: metres to the nearest contact
    float text = 0.0;
    {
        vec2 q = (uv - vec2(0.635, 0.035)) / 0.085; // one unit = a digit's height
        float fq = fwidth(q.y); // taken out here: derivatives are not defined inside the branch
        if (q.y > -0.1 && q.y < 1.1 && q.x > -0.1 && q.x < 3.6) {
            int cm = uReadout.x < 0.0 ? -1 : int(clamp(uReadout.x, 0.0, 99.99) * 100.0 + 0.5);
            for (int k = 0; k < 4; k++) {
                int div = k == 0 ? 1000 : k == 1 ? 100 : k == 2 ? 10 : 1;
                float x0 = float(k) * 0.78 + (k >= 2 ? 0.28 : 0.0);
                text += digit(q - vec2(x0, 0.0), cm < 0 ? 10 : (cm / div) % 10, fq);
            }
            text += 1.0 - smoothstep(0.05, 0.05 + fq * 1.5, length(q - vec2(1.62, 0.08))); // decimal point
        }
        // range bar, bottom left: one block per ring the nearest contact is inside
        vec2 b = (uv - vec2(0.065, 0.045)) / vec2(0.26, 0.05);
        if (b.x > 0.0 && b.x < 1.0 && b.y > 0.0 && b.y < 1.0) {
            float cell = floor(b.x * 4.0), on = uReadout.x >= 0.0 && uReadout.x <= (cell + 1.0) * 0.25 * uReadout.y ? 1.0 : 0.22;
            vec2 f = abs(vec2(fract(b.x * 4.0), b.y) - 0.5);
            text += on * step(f.x, 0.38) * step(f.y, 0.40);
        }
    }

    float lum = 0.022 * (1.0 - 0.5 * min(r, 1.3))              // the tube's resting glow
              + grid * (0.50 + 1.50 * trail)
              + trail * 0.11 * inside
              + front * 3.4
              + mark * 1.4
              + blips * 1.5
              + text * 1.2;

    // the tube itself: scan lines (as long as the picture is big enough to show them), a little noise,
    // darker towards its rounded corners, mains flicker
    const float LINES = 96.0;
    lum *= 1.0 - 0.22 * (0.5 - 0.5 * sin(uv.y * LINES * 6.2831853)) * (1.0 - smoothstep(0.25, 0.5, fwidth(uv.y) * LINES));
    lum *= 0.92 + 0.16 * hash(floor(uv * vec2(190.0, 130.0)) + floor(uPing.z * 24.0) * 7.0);
    vec2 e = abs(uv - 0.5) * 2.0;
    float corner = length(max(e - 0.86, 0.0)) / 0.14;
    lum *= (1.0 - smoothstep(0.75, 1.0, corner)) * (1.0 - 0.35 * dot(e, e) * 0.5);
    lum *= (0.97 + 0.03 * sin(uPing.z * 41.0)) * uPing.w;
    // the hottest spots burn towards white
    lum *= 1.7; // radiance: the lines sit a little above display white, so they bloom
    return PHOSPHOR * lum + vec3(0.55, 0.9, 0.6) * max(lum - 4.0, 0.0) * 0.35;
}

vec3 envBRDF(vec3 F0, float rough, float NoV) { // Karis' analytic fit of the split-sum BRDF term
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * ab.x + ab.y;
}

void main() {
    int mat = int(vMat + 0.5);
    vec3 N = normalize(vNormal), No = normalize(vObjNormal);
    vec3 V = normalize(uCam - vWorld);
    float NoV = max(dot(N, V), 0.0);

    // surface detail in the plane the face lies in
    vec3 an = abs(No);
    vec2 q = an.z > 0.6 ? vObj.xy : (an.y > 0.6 ? vObj.xz : vObj.zy);
    float grime = fbm(q * 70.0 + float(mat) * 3.7);
    float fine = vnoise(q * 1400.0);
    float scratches = smoothstep(0.80, 0.95, vnoise(vec2(q.x * 60.0 + q.y * 25.0, q.y * 900.0 - q.x * 300.0)));
    float edge = mat == MAT_SCREEN ? 0.0 : vUV.x;
    // paint goes first on the corners and where it has been knocked about
    float wear = smoothstep(0.50, 0.80, edge * 0.55 + grime * 0.55 + scratches * 0.25);

    vec3 albedo = vec3(0.5);
    float rough = 0.5, metal = 0.0, f0 = 0.04;
    vec3 emissive = vec3(0.0);
    float gain = texelFetch(uExposureTex, ivec2(0), 0).r;
    // A lit display looks brighter to an eye adapted to the dark, but nowhere near as much brighter
    // as the room does: take most of the adaptation back out.
    float adapted = pow(max(gain, 1.0), -0.7);

    if (mat == MAT_PAINT) {
        albedo = vec3(0.30, 0.33, 0.27) * (0.80 + 0.35 * grime);
        rough = 0.52 + 0.08 * fine;
        albedo = mix(albedo, vec3(0.56, 0.55, 0.52), wear);
        metal = wear * 0.9;
        rough = mix(rough, 0.34, wear);
    } else if (mat == MAT_DARK) {
        albedo = mix(vec3(0.045), vec3(0.13), wear * 0.7) * (0.8 + 0.4 * grime);
        rough = 0.42 + 0.06 * fine;
    } else if (mat == MAT_RUBBER) {
        float ridge = smoothstep(0.35, 0.65, abs(fract(vObj.y * 110.0) - 0.5) * 2.0);
        albedo = vec3(0.022) * (0.6 + 0.4 * ridge) + vec3(0.02) * grime;
        rough = 0.86;
    } else if (mat == MAT_METAL) {
        albedo = vec3(0.60, 0.60, 0.58) * (0.85 + 0.2 * grime);
        metal = 1.0;
        rough = 0.30 + 0.15 * scratches;
    } else if (mat == MAT_BUTTON) {
        albedo = mix(vec3(0.62, 0.20, 0.03), vec3(0.25, 0.10, 0.03), grime * 0.6);
        rough = 0.48;
    } else if (mat == MAT_KEY) {
        albedo = vec3(0.34, 0.34, 0.32) * (0.75 + 0.3 * grime);
        rough = 0.50;
    } else if (mat == MAT_PLATE) {
        // a stamped data plate: rows of unreadable lettering
        vec2 t = vObj.xy * vec2(260.0, 520.0);
        float row = step(0.30, fract(t.y)) * step(fract(t.y), 0.78);
        float ink = row * step(0.42, hash(vec2(floor(t.x), floor(t.y)))) * step(fract(t.x), 0.80);
        albedo = mix(vec3(0.50, 0.49, 0.44), vec3(0.05), ink * 0.85) * (0.8 + 0.3 * grime);
        metal = 0.7;
        rough = 0.40;
    } else if (mat == MAT_LED) {
        albedo = vec3(0.05, 0.01, 0.01);
        rough = 0.25;
        // blinks with each ping
        float blink = uPing.x < 0.12 ? 1.0 : 0.06;
        emissive = vec3(1.0, 0.16, 0.03) * (3.0 * blink * uPing.w * adapted);
    } else { // MAT_SCREEN: glass over the tube
        albedo = vec3(0.012) + vec3(0.02) * smoothstep(0.55, 0.9, grime); // dust and finger marks
        rough = 0.07 + 0.10 * smoothstep(0.5, 0.9, grime);
        f0 = 0.045;
        emissive = display(vUV) * adapted;
    }

    // ---- light from the room ----
    vec4 n4 = vec4(1.0, N);
    vec3 indirect = max(vec3(dot(uSH[0], n4), dot(uSH[1], n4), dot(uSH[2], n4)), 0.0);
    vec3 direct = vec3(0.0);
    for (int i = 0; i < uLightCount; i++) {
        float through = portalVis(vWorld, uLightPos[i], length(uLightU[i]) * 0.7);
        if (through <= 0.0) continue;
        vec3 c = uLightPos[i] - vWorld;
        float d2 = dot(c, c);
        vec3 L = c * inversesqrt(d2);
        // the panel as one small source, wrapped a little because it is not
        float area = 4.0 * length(uLightU[i]) * length(uLightV[i]);
        float ff = area * max(dot(-L, uLightN[i]), 0.0) * max(dot(N, L) * 0.8 + 0.2, 0.0) / (PI * d2 + area);
        direct += uLightRadiance[i] * (ff * through);
    }
    // whoever holds it stands in the light that would reach it from behind
    float body = 1.0 - 0.45 * smoothstep(0.0, 0.9, dot(N, uBack));
    vec3 light = (direct + indirect) * (uSceneLight * body);

    vec3 diffuseColor = albedo * (1.0 - metal);
    vec3 F0 = mix(vec3(f0), albedo, metal);
    vec3 col = diffuseColor * light;

    vec3 R = reflect(-V, N);
    float bodyR = 1.0 - 0.75 * smoothstep(0.35, 0.95, dot(R, uBack)) * (1.0 - smoothstep(0.2, 0.7, R.y));
    col += textureLod(uEnvTex, R, rough * uEnvMaxLod).rgb * envBRDF(F0, rough, NoV) * (uSceneLight * bodyR);

    // ---- the screen's own light on what stands around it: the inner walls of its bezel, the hood ----
    // Only what stands inside the bezel's outline: the bezel is between the screen and everything else.
    vec2 off = abs(vObj.xy - uScreenRect.xy) - uScreenRect.zw;
    if (mat != MAT_SCREEN && max(off.x, off.y) < 0.0098) {
        vec2 nearest = uScreenRect.xy + clamp(vObj.xy - uScreenRect.xy, -uScreenRect.zw * 0.85, uScreenRect.zw * 0.85);
        vec3 Lo = vec3(nearest, uScreenZ) - vObj;
        float d = length(Lo);
        float facing = max(dot(No, Lo / max(d, 1e-5)) * 0.85 + 0.15, 0.0);
        float spill = facing * exp(-d / 0.020) * step(uScreenZ - 0.0005, vObj.z);
        // brighter for a moment as each ping leaves
        col += (diffuseColor + F0) * PHOSPHOR * (spill * 5.0 * uPing.w * adapted * (0.75 + 0.5 * exp(-uPing.x * 4.0)));
    }

    outColor = vec4(col + emissive, 0.0);
}

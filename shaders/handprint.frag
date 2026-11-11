in vec3 vWorld;
in vec2 vUV;    // -1..1 across the print's quad: x to the hand's right, y towards the fingertips
in vec4 vInfo;  // seconds since the slap, age / life, seed, side (+1 right hand, -1 left)
out vec4 outColor;

// A handprint in tar on the floor (src/handprints.c). The hand is a distance field: palm, four
// fingers, thumb. It presses in from the palm outwards, throws a ring of droplets, stands wet and
// thick for a while, then dries from its edges and breaks up.
//
// Tar is black: nearly all it shows is what it reflects. Output is premultiplied (the renderer blends
// ONE, ONE_MINUS_SRC_ALPHA): alpha blots out the floor, rgb adds the highlights.

uniform float uSceneLight;
uniform samplerCube uEnvTex;
uniform float uEnvMaxLod;
uniform vec4 uFlashPos;   // xyz, brightness (0 = off)
uniform vec4 uFlashDir;   // xyz, beam narrowing
uniform vec3 uFlashColor;

const float SLAP = 0.20;  // seconds for the print to press in

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) { return 0.55 * vnoise(p) + 0.30 * vnoise(p * 2.1 + 17.0) + 0.15 * vnoise(p * 4.3 + 5.0); }

float capsule(vec2 p, vec2 a, vec2 b, float ra, float rb) { // tapering from ra at a to rb at b
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - mix(ra, rb, h);
}
float smin(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

// Distance to a right hand laid palm down, fingers towards +y. Negative inside.
float hand(vec2 p, float seed) {
    float spread = 0.85 + 0.35 * seed; // how far the fingers are splayed
    vec2 q = p - vec2(0.02, -0.30);
    float d = length(max(abs(q) - vec2(0.17, 0.15), 0.0)) - 0.15;                                               // palm
    d = smin(d, capsule(p, vec2(-0.02, -0.52), vec2(0.00, -0.42), 0.20, 0.24), 0.10);                           // heel of the hand
    d = smin(d, capsule(p, vec2(-0.17, -0.06), vec2(-0.17 - 0.17 * spread, 0.58), 0.085, 0.060), 0.05);         // index
    d = smin(d, capsule(p, vec2(-0.02, -0.02), vec2(-0.02 - 0.03 * spread, 0.78), 0.090, 0.062), 0.05);         // middle
    d = smin(d, capsule(p, vec2(0.13, -0.05), vec2(0.13 + 0.10 * spread, 0.68), 0.085, 0.058), 0.05);           // ring
    d = smin(d, capsule(p, vec2(0.26, -0.14), vec2(0.26 + 0.20 * spread, 0.42), 0.072, 0.050), 0.05);           // little finger
    d = smin(d, capsule(p, vec2(-0.22, -0.36), vec2(-0.40 - 0.26 * spread, -0.02 * spread), 0.100, 0.066), 0.07); // thumb
    return d;
}

void main() {
    float age = vInfo.x, life = vInfo.y, seed = vInfo.z;
    vec2 p = vUV * vec2(vInfo.w, 1.0);
    vec2 sp = p + seed * 37.0; // this print's own patch of noise

    // the outline is never clean: the hand slid a little, the tar ran
    vec2 warp = vec2(vnoise(sp * 5.0), vnoise(sp * 5.0 + 9.0)) - 0.5;
    float d = hand(p + warp * 0.07, seed) + (fbm(sp * 9.0) - 0.5) * 0.06;
    // smeared back the way the hand came down
    d = min(d, hand(p + vec2(warp.x * 0.05, 0.10 + 0.08 * seed), seed) + 0.035 + 0.05 * vnoise(sp * vec2(14.0, 3.0)));

    float aa = min(fwidth(d) * 1.2, 0.05) + 1e-4; // taken here: the droplets below jump from cell to cell

    // droplets thrown out by the slap: more of them close to the hand, flying outwards as it lands
    float fly = 1.0 - exp(-age * 14.0);
    for (int k = 0; k < 2; k++) {
        float cells = k == 0 ? 7.0 : 13.0;
        vec2 g = p * cells + seed * 11.0 + float(k) * 3.7, id = floor(g);
        vec2 c = (id + 0.25 + 0.5 * vec2(hash(id), hash(id + 7.3)) - seed * 11.0 - float(k) * 3.7) / cells;
        float hd = hand(c, seed);
        float chance = hash(id + 3.1);
        if (hd > 0.02 && hd < 0.30 && chance > 0.62 + hd * 1.2) {
            vec2 out_ = normalize(c - vec2(0.0, -0.2));
            float r = (k == 0 ? 0.030 : 0.014) * (0.6 + 0.8 * hash(id + 1.7));
            d = min(d, length(p - c - out_ * (fly - 1.0) * hd * 0.8) - r * fly);
        }
    }

    // pressing in: the palm lands first and the tar runs out from under it to the fingertips
    float press = clamp(age / SLAP, 0.0, 1.0);
    press = 1.0 - (1.0 - press) * (1.0 - press);
    float reach = press * 1.45 - length(p - vec2(0.0, -0.32)) + 0.12 * (vnoise(sp * 7.0) - 0.5);
    // drying: the thin edges go first, eaten away unevenly, until nothing is left
    float dry = smoothstep(0.45, 1.0, life);
    float eaten = dry * (0.10 + 0.55 * fbm(sp * 6.0 + 3.0));
    float inside = min(-eaten - d, reach * 0.5); // > 0: tar

    float cover = smoothstep(-aa, aa, inside) * (1.0 - smoothstep(0.90, 1.0, life));
    cover *= 1.0 - smoothstep(0.92, 1.0, max(abs(vUV.x), abs(vUV.y))); // never a hard edge at the quad's border
    // thickness: a rounded bead of tar, with the ripple of the slap running out through it, puckered as it dries
    float t = clamp(inside / 0.16, 0.0, 1.0);
    float h = 1.0 - (1.0 - t) * (1.0 - t);
    h += 0.22 * sin(length(p - vec2(0.0, -0.25)) * 34.0 - age * 26.0) * exp(-age * 5.0) * t;
    h += (fbm(sp * 9.0) - 0.5) * (0.06 + 0.40 * dry); // smooth while wet: one clean highlight, not glitter
    h *= 1.0 - 0.6 * dry;

    // bump: tilt the floor's normal by the slope of the thickness, taken from screen derivatives
    vec3 N = vec3(0.0, 1.0, 0.0);
    vec3 dpx = dFdx(vWorld), dpy = dFdy(vWorld);
    vec3 r1 = cross(dpy, N), r2 = cross(N, dpx);
    float det = dot(dpx, r1);
    vec3 grad = sign(det) * (dFdx(h) * r1 + dFdy(h) * r2);
    N = normalize(abs(det) * N - grad * 0.030);

    vec3 V = normalize(uCam - vWorld);
    float NoV = max(dot(N, V), 0.0);
    float rough = mix(0.10, 0.55, smoothstep(0.15, 0.9, life)); // wet, then tacky, then dull
    const float F0 = 0.05;
    float fres = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0);

    // what it shows: the room in it, and the torch as a hard glint
    vec3 R = reflect(-V, N);
    R.y = abs(R.y);
    vec3 col = textureLod(uEnvTex, R, rough * uEnvMaxLod).rgb * (fres * uSceneLight * (1.0 - 0.6 * rough) * 1.6);
    if (uFlashPos.w > 0.0) {
        vec3 Lv = uFlashPos.xyz - vWorld;
        float d2 = dot(Lv, Lv);
        vec3 L = Lv * inversesqrt(d2);
        float c = dot(-L, uFlashDir.xyz);
        // the beam's disc, roughly: the pattern texture and the shadow map are not worth it for a glint
        float half_angle = radians(25.0) / max(uFlashDir.w, 0.2);
        float beam = smoothstep(cos(half_angle), cos(half_angle * 0.45), c) * 0.6 + smoothstep(cos(half_angle * 0.5), 1.0, c) * 0.4;
        float NoL = max(dot(N, L), 0.0);
        vec3 E = uFlashColor * (uFlashPos.w * beam * NoL / (d2 + 0.25));
        vec3 H = normalize(L + V);
        float a = rough * rough, a2 = a * a;
        float NoH = max(dot(N, H), 0.0);
        float dn = NoH * NoH * (a2 - 1.0) + 1.0;
        float k = a * 0.5;
        float vis = 0.25 / ((NoL * (1.0 - k) + k) * (NoV * (1.0 - k) + k) + 1e-4);
        col += E * (min(a2 / (dn * dn), 60.0) * vis * (F0 + (1.0 - F0) * pow(1.0 - max(dot(V, H), 0.0), 5.0)));
        col += E * 0.003; // tar is not quite black
    }

    // drying thins it: the floor starts to show through before it breaks up
    if (cover < 0.004) discard; // down here: the derivatives above need every pixel of the quad
    float opacity = cover * mix(0.97, 0.80, dry);
    outColor = vec4(col * cover, opacity);
}

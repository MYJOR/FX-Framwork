#version 330 core
// Shared by every shader: prepended automatically by the engine.
// Palette, sky, fog and the final grade live here so sky and sand always agree.

uniform vec3 uSunDir;   // direction TO the sun
uniform vec3 uCam;
uniform float uTime;

const vec2 WIND = vec2(0.906, 0.423); // matches the dune orientation in terrain.c

// ---- wind gusts: MUST match wind_gust() in src/wind.h ----
float gust(vec2 p, float t) {
    float s = dot(p, WIND);
    float q = dot(p, vec2(-WIND.y, WIND.x));
    float a = (s - 9.0 * t) * 0.035 + sin(q * 0.017) * 1.7;
    float b = (s - 6.0 * t) * 0.09 + sin(q * 0.041 + 1.0) * 1.1;
    float g = clamp(0.5 + 0.35 * sin(a) + 0.15 * sin(b), 0.0, 1.0);
    return smoothstep(0.3, 0.9, g);
}

float sunHeight()
 { return uSunDir.y; }
// 0 = low golden sun, 1 = high midday sun
float dayK() { return smoothstep(0.05, 0.95, sunHeight()); }

vec3 sunColor() {
    vec3 low = vec3(1.00, 0.52, 0.22);
    vec3 high = vec3(1.00, 0.93, 0.80);
    float horizonFade = smoothstep(-0.12, 0.08, sunHeight());
    return mix(low, high, dayK()) * (1.6 * horizonFade);
}

vec3 skyColor(vec3 d) {
    float k = dayK();
    vec3 horizon = mix(vec3(1.00, 0.46, 0.18), vec3(0.88, 0.66, 0.48), k);
    vec3 zenith  = mix(vec3(0.05, 0.14, 0.34), vec3(0.12, 0.36, 0.78), k);
    float t = clamp(d.y, 0.0, 1.0);
    vec3 col = mix(horizon, zenith, pow(t, 0.30));

    vec3 S = normalize(uSunDir);
    float sd = max(dot(d, S), 0.0);
    vec3 glow = mix(vec3(1.0, 0.45, 0.15), vec3(1.0, 0.85, 0.6), k);
    col += glow * (pow(sd, 6.0) * 0.25 + pow(sd, 48.0) * 0.55);
    col += vec3(1.0, 0.9, 0.7) * smoothstep(0.99955, 0.99985, sd) * 12.0;

    // below the horizon just carry the horizon colour so fog blends cleanly
    return mix(col, horizon, smoothstep(0.0, -0.15, d.y));
}

// ---- hash / noise helpers --------------------------------------------------
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), f.x),
               mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), f.x), f.y);
}

// ---- final grade: filmic tonemap, slight saturation lift, gamma ------------
vec3 grade(vec3 c) {
    c = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14);
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(l), c, 1.12);
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

// ---- aerial perspective: orange dust haze ------------------------------------
// Tweak these to retune the whole atmosphere.
const vec3  HAZE_EXTINCTION = vec3(0.0038, 0.0050, 0.0075); // blue dies first -> warm distance
const vec3  HAZE_DUST       = vec3(1.00, 0.55, 0.26);       // colour of suspended dust
const float HAZE_DUST_MIX   = 0.75;                          // 0 = pure sky colour, 1 = pure dust
const float HAZE_HEIGHT_FALLOFF = 0.012;                     // thicker in valleys
const float HAZE_SUN_GLOW   = 0.9;                           // in-scattering toward the sun

vec3 hazeColor(vec3 viewDir) {
    vec3 hazeDir = normalize(vec3(viewDir.x, max(viewDir.y, 0.0) * 0.5 + 0.02, viewDir.z));
    vec3 dust = HAZE_DUST * mix(0.35, 1.0, dayK() * 0.5 + 0.5) * (0.4 + 0.6 * smoothstep(-0.1, 0.3, sunHeight()));
    vec3 hazeCol = mix(skyColor(hazeDir), dust, HAZE_DUST_MIX);
    float toSun = max(dot(viewDir, normalize(uSunDir)), 0.0);
    return hazeCol + sunColor() * pow(toSun, 5.0) * HAZE_SUN_GLOW * 0.35;
}

vec3 applyHaze(vec3 col, float dist, vec3 viewDir, float worldY) {
    float heightK = exp(-max(worldY, 0.0) * HAZE_HEIGHT_FALLOFF);
    vec3 trans = exp(-dist * HAZE_EXTINCTION * (0.55 + 0.9 * heightK));
    return col * trans + hazeColor(viewDir) * (1.0 - trans);
}
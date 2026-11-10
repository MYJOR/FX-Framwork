in vec2 vNdc;
out vec4 outColor;

// Resolve + display transform. Each MSAA sample is tonemapped before averaging, so edges against
// the light panels (HDR values far above 1) stay as smooth as every other edge.

uniform sampler2DMS uScene;
uniform int uSamples;
uniform vec2 uOrigin;   // where the view starts in the target, pixels
uniform int uRaw;      // debug views that are not light: skip exposure and tonemapping
uniform vec4 uGrade;   // exposure, saturation, contrast, vignette
uniform sampler2DMS uSceneDepth;
uniform sampler2D uExposureTex; // one texel: the gain the eye adaptation adds, 1 in a lit room
uniform sampler2D uGlowTex;     // bloom and the torch beam in the air, blurred
uniform vec3 uGlowInfo; // 1 / view size, on
uniform vec4 uFilm;     // grain that is always there, its size in pixels, green-grey cast, how much of the bottom end is crushed to black
uniform vec2 uNight;    // largest gain the adaptation can reach; noise seed
uniform vec2 uDofEdge;  // blur amount reached in the corners of the screen; blur over the whole picture
uniform vec4 uDof;     // near, far, 1 / focus start, 1 / (1 / full blur - 1 / focus start); w = 0: off

// Khronos PBR Neutral: keeps base colours as authored up to ~0.8, then rolls highlights off to white.
vec3 tonemap(vec3 c) {
    const float start = 0.8 - 0.04, desat = 0.15;
    float x = min(c.r, min(c.g, c.b));
    c -= x < 0.08 ? x - 6.25 * x * x : 0.04;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < start) return c;
    float d = 1.0 - start;
    float newPeak = 1.0 - d * d / (peak + d - start);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desat * (peak - newPeak) + 1.0);
    return mix(c, vec3(newPeak), g);
}

vec3 toSRGB(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(max(c, 0.0), vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy - uOrigin);
    float gain = uRaw != 0 ? 1.0 : texelFetch(uExposureTex, ivec2(0), 0).r;
    float exposure = uGrade.x * gain;
    vec3 c = vec3(0.0);
    float coc = 0.0; // near-field blur amount, 0..1; the nearest sample decides so outlines stay whole
    for (int s = 0; s < uSamples; s++) {
        if (uDof.w > 0.0) {
            float z = linearDepth(texelFetch(uSceneDepth, ip, s).r, uDof.x, uDof.y);
            coc = max(coc, clamp((1.0 / z - uDof.z) * uDof.w, 0.0, 1.0));
        }
        vec3 hdr = max(texelFetch(uScene, ip, s).rgb, 0.0);
        c += uRaw != 0 ? clamp(hdr, 0.0, 1.0) : tonemap(hdr * exposure);
    }
    c /= float(uSamples);
    // the lens also goes soft away from the middle: nothing inside 0.7 of the way out, most in the corners
    if (uDof.w > 0.0) coc = max(max(coc, uDofEdge.y), uDofEdge.x * smoothstep(0.70, 1.40, length(vNdc)));

    if (uRaw == 0) {
        c = mix(vec3(luma(c)), c, uGrade.y);
        // contrast around mid grey, in display-linear
        c = 0.18 * pow(max(c, 0.0) / 0.18, vec3(uGrade.z));
        c *= 1.0 - uGrade.w * smoothstep(0.55, 1.5, length(vNdc));
    }
    if (uGlowInfo.z > 0.0) {
        vec3 g = texture(uGlowTex, (gl_FragCoord.xy - uOrigin) * uGlowInfo.xy).rgb;
        c += g / (1.0 + luma(g));
    }
    if (uRaw == 0 && uFilm.z + uFilm.w > 0.0) { // a cold, sickly stock
        c *= mix(vec3(1.0), vec3(0.90, 1.0, 0.93), uFilm.z);
        c = max(c - uFilm.w, 0.0) / (1.0 - uFilm.w);
    }
    // Night vision. A dark-adapted eye sees its dimmest tones with the rods: no colour, a blue cast.
    float night = clamp((gain - 1.0) / max(uNight.x - 1.0, 1e-3) * 1.6, 0.0, 1.0);
    float dim = 0.0;
    if (night > 0.0) {
        float l = luma(c);
        dim = 1.0 - smoothstep(0.0, 0.22, l);
        c = mix(c, l * vec3(0.72, 0.92, 1.30), night * dim * 0.75);
    }
    c = toSRGB(clamp(c, 0.0, 1.0));
    if (night > 0.0 || (uFilm.x > 0.0 && uRaw == 0)) { // ... and it is noisy, most where there is least light
        vec2 q = floor(gl_FragCoord.xy / max(uFilm.y, 1.0)) + uNight.y * vec2(37.0, 17.0);
        float g = fract(sin(dot(q, vec2(12.9898, 78.233))) * 43758.5453) + fract(sin(dot(q, vec2(39.3468, 11.135))) * 24634.6345) - 1.0;
        c += g * (night * (0.012 + 0.030 * dim) + uFilm.x * (0.4 + 0.6 * (1.0 - luma(c))));
    }

    // one LSB of ordered noise: flat walls under smooth light would band at 8 bits otherwise
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    outColor = vec4(c + (n - 0.5) / 255.0, uDof.w > 0.0 ? coc : 1.0);
}

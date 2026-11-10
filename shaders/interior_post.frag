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
    vec3 c = vec3(0.0);
    float coc = 0.0; // near-field blur amount, 0..1; the nearest sample decides so outlines stay whole
    for (int s = 0; s < uSamples; s++) {
        if (uDof.w > 0.0) {
            float z = linearDepth(texelFetch(uSceneDepth, ip, s).r, uDof.x, uDof.y);
            coc = max(coc, clamp((1.0 / z - uDof.z) * uDof.w, 0.0, 1.0));
        }
        vec3 hdr = max(texelFetch(uScene, ip, s).rgb, 0.0);
        c += uRaw != 0 ? clamp(hdr, 0.0, 1.0) : tonemap(hdr * uGrade.x);
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
    c = toSRGB(clamp(c, 0.0, 1.0));

    // one LSB of ordered noise: flat walls under smooth light would band at 8 bits otherwise
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    outColor = vec4(c + (n - 0.5) / 255.0, uDof.w > 0.0 ? coc : 1.0);
}

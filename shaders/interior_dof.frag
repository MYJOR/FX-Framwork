in vec2 vNdc;
out vec4 outColor;

// Near-field depth of field, half resolution. Every tap is a foreground pixel that may be blurred
// widely enough to reach this one, so soft foreground spills over whatever is sharp behind it
// instead of being cut off at its own outline.

uniform sampler2D uTex;    // graded frame; alpha = blur amount (0 sharp .. 1 fully blurred)
uniform vec2 uInvScreen;   // one full-resolution texel
uniform float uRadius;     // largest blur radius, full-resolution pixels

const int TAPS = 48;

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    float soft = 1.5 / max(uRadius, 1.0);
    vec3 sum = vec3(0.0);
    float wsum = 0.0, csum = 0.0;
    for (int i = 0; i < TAPS; i++) {
        vec2 o = vogel(i, TAPS, 0.0);
        float d = length(o);
        vec4 s = texture(uTex, uv + o * uRadius * uInvScreen);
        // does this pixel's own blur disc reach us? Wider discs spread their light thinner.
        float reach = smoothstep(d - soft, d + soft, s.a) * step(0.004, s.a);
        float w = reach / max(s.a * s.a * float(TAPS), 1.0);
        sum += s.rgb * w;
        wsum += w;
        csum += s.a * w;
    }
    if (wsum < 1e-4) { outColor = vec4(texture(uTex, uv).rgb, 0.0); return; }
    // x2: at a foreground outline half the disc is foreground, and the blur should still fully cover the
    // hard edge underneath. Barely soft pixels keep the full-resolution image.
    float cover = clamp(2.0 * wsum, 0.0, 1.0) * smoothstep(0.02, 0.30, csum / wsum);
    outColor = vec4(sum / wsum, cover);
}

in vec2 vNdc;
out vec4 outColor;

// Depth of field, half resolution. Every tap is a pixel that may be blurred widely enough to reach
// this one. What is in front of the focus spills over whatever is sharp behind it instead of being
// cut off at its own outline. What is behind the focus (the room, while the lens is on the thing in
// the player's hand) only ever mixes with other things behind the focus, so it never smears over the
// sharp thing in front of it and that thing never bleeds into it.

uniform sampler2D uTex;    // graded frame; alpha = signed blur amount: (128 + 127 * c) / 255, c > 0 in front of the focus, c < 0 behind
uniform vec2 uInvScreen;   // one full-resolution texel
uniform float uRadius;     // largest blur radius, full-resolution pixels

const int TAPS = 48;

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    float soft = 1.5 / max(uRadius, 1.0);
    vec3 nearSum = vec3(0.0), farSum = vec3(0.0);
    float nearW = 0.0, nearC = 0.0, farW = 0.0;
    for (int i = 0; i < TAPS; i++) {
        vec2 o = vogel(i, TAPS, 0.0);
        float d = length(o);
        vec4 s = texture(uTex, uv + o * uRadius * uInvScreen);
        float c = (s.a * 255.0 - 128.0) / 127.0, a = abs(c);
        // does this pixel's own blur disc reach us? Wider discs spread their light thinner.
        float reach = smoothstep(d - soft, d + soft, a) * step(0.006, a);
        float w = reach / max(a * a * float(TAPS), 1.0);
        if (c > 0.0) { nearSum += s.rgb * w; nearW += w; nearC += a * w; }
        else { farSum += s.rgb * w; farW += w; }
    }
    // rgb: the soft background where there is one, with the soft foreground laid over it.
    // alpha: how much of the foreground covers this pixel; interior_dof_mix.frag decides per full
    // resolution pixel whether the background part applies.
    vec3 col = farW > 1e-4 ? farSum / farW : texture(uTex, uv).rgb;
    float cover = 0.0;
    if (nearW > 1e-4) {
        // x2: at a foreground outline half the disc is foreground, and the blur should still fully cover the
        // hard edge underneath. Barely soft pixels keep the full-resolution image.
        cover = clamp(2.0 * nearW, 0.0, 1.0) * smoothstep(0.02, 0.30, nearC / nearW);
        col = farW > 1e-4 ? mix(col, nearSum / nearW, cover) : nearSum / nearW;
    }
    outColor = vec4(col, cover);
}

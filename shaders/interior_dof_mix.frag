in vec2 vNdc;
out vec4 outColor;

// Last pass: lays the blurred foreground over the sharp frame and bends the picture like a lens.

uniform sampler2D uSharp;  // full resolution
uniform sampler2D uNear;   // half resolution blurred foreground, alpha = coverage
uniform int uUseNear;
uniform vec2 uLens;        // barrel distortion strength, aspect ratio

void main() {
    // Barrel distortion: the picture is squeezed more the further it is from the middle, so straight lines
    // near the sides bow outwards. Scaled so the corners stay where they are and nothing outside the frame
    // is ever needed.
    vec2 q = vNdc * vec2(uLens.y, 1.0);
    float k = uLens.x;
    vec2 p = vNdc * (1.0 + k * dot(q, q)) / (1.0 + k * (uLens.y * uLens.y + 1.0));
    vec2 uv = p * 0.5 + 0.5;

    vec3 c = texture(uSharp, uv).rgb;
    float cover = 0.0;
    if (uUseNear != 0) {
        vec4 near = texture(uNear, uv);
        c = mix(c, near.rgb, near.a);
        cover = near.a;
    }
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    outColor = vec4(c + (n - 0.5) / 255.0 * max(cover, k > 0.0 ? 1.0 : 0.0), 1.0);
}

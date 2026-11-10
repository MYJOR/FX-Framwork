in vec2 vNdc;
out vec4 outColor;

// Last pass: lays the blurred foreground over the sharp frame and bends the picture like a lens.

uniform sampler2D uSharp;  // full resolution
uniform sampler2D uNear;   // half resolution blurred foreground, alpha = coverage
uniform int uUseNear;
uniform vec2 uLens;        // barrel distortion strength, aspect ratio
uniform sampler2D uGlowTex; // blurred highlights: what lights up the dirt on the lens
uniform vec2 uFx;          // colour fringing towards the edges, lens dirt

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}
// Smears and specks on the front glass, 0..1.
float lensDirt(vec2 p) {
    float smear = vnoise(p * 2.3 + 7.0) * vnoise(p * vec2(5.0, 1.6) + 3.0);
    float specks = smoothstep(0.55, 0.95, vnoise(p * 6.0) * 0.6 + vnoise(p * 13.0 + 5.0) * 0.4) * 0.6;
    return smoothstep(0.12, 0.55, smear) * 0.7 + specks;
}

void main() {
    // Barrel distortion: the picture is squeezed more the further it is from the middle, so straight lines
    // near the sides bow outwards. Scaled so the corners stay where they are and nothing outside the frame
    // is ever needed.
    vec2 q = vNdc * vec2(uLens.y, 1.0);
    float k = uLens.x;
    vec2 p = vNdc * (1.0 + k * dot(q, q)) / (1.0 + k * (uLens.y * uLens.y + 1.0));
    vec2 uv = p * 0.5 + 0.5;

    vec3 c = texture(uSharp, uv).rgb;
    if (uFx.x > 0.0) { // red and blue focus at slightly different sizes, more so away from the middle
        vec2 off = (uv - 0.5) * uFx.x * dot(vNdc, vNdc);
        c.r = texture(uSharp, uv - off).r;
        c.b = texture(uSharp, uv + off).b;
    }
    float cover = 0.0;
    if (uUseNear != 0) {
        vec4 near = texture(uNear, uv);
        c = mix(c, near.rgb, near.a);
        cover = near.a;
    }
    if (uFx.y > 0.0) { // dirt catches whatever bright thing is anywhere near it
        vec3 g = texture(uGlowTex, uv).rgb;
        for (int i = 0; i < 8; i++) g += texture(uGlowTex, uv + vogel(i, 8, 0.0) * vec2(0.10 / uLens.y, 0.10)).rgb;
        c += g / 9.0 * lensDirt(q) * uFx.y;
    }
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    outColor = vec4(c + (n - 0.5) / 255.0 * max(cover, k > 0.0 ? 1.0 : 0.0), 1.0);
}

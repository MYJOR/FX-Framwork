in vec2 vNdc;
out vec4 outColor;

// What the torch is pointed at, as one texel: the colour of the lit patch (rgb) and how far away it
// is (a). The forward shader turns that into the light the patch throws back into the room.

uniform sampler2D uCapture; // 16 x 16 view down the beam: albedo, distance
uniform sampler2D uPrev;
uniform float uBlend;       // 1 = take the new value at once

void main() {
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++) {
            vec2 d = (vec2(i, j) + 0.5) / 8.0 - 1.0;
            float w = exp(-dot(d, d) * 2.5); // the hot centre of the beam counts most
            sum += texelFetch(uCapture, ivec2(i, j), 0) * w;
            wsum += w;
        }
    outColor = mix(texelFetch(uPrev, ivec2(0), 0), sum / wsum, uBlend);
}

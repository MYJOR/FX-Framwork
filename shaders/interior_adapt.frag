in vec2 vNdc;
out vec4 outColor;

// Eye adaptation, one texel. The eye only ever opens up from the authored exposure (the lit room
// looks exactly as graded); in the dark it slowly gains sensitivity, and loses it again fast when
// the light returns, which is the moment of being dazzled.

uniform sampler2D uMeter;  // 8 x 8 log luminance
uniform sampler2D uPrev;   // last frame's result
uniform vec4 uAdapt;       // luminance the eye wants to see, largest gain, seconds since last frame (0 = settle at once), -

void main() {
    float sum = 0.0, wsum = 0.0;
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            vec2 d = (vec2(i, j) + 0.5) / 4.0 - 1.0;
            float w = exp(-dot(d, d) * 1.6); // what is looked at counts most
            sum += texelFetch(uMeter, ivec2(i, j), 0).r * w;
            wsum += w;
        }
    float avg = exp(sum / wsum);
    float target = clamp(uAdapt.x / avg, 1.0, uAdapt.y);
    float prev = texelFetch(uPrev, ivec2(0), 0).r;
    float rate = target > prev ? 0.55 : 3.5; // getting used to the dark takes far longer than to the light
    float k = uAdapt.z > 0.0 ? 1.0 - exp(-uAdapt.z * rate) : 1.0;
    outColor = vec4(prev + (target - prev) * k, avg, 0.0, 1.0);
}

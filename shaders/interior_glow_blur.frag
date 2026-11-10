in vec2 vNdc;
out vec4 outColor;

// One direction of a gaussian blur, 9 taps.

uniform sampler2D uTex;
uniform vec2 uStep; // one tap's spacing in uv, along the blur direction

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    const float w[5] = float[5](0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162);
    vec3 c = texture(uTex, uv).rgb * w[0];
    for (int i = 1; i < 5; i++)
        c += (texture(uTex, uv + uStep * float(i)).rgb + texture(uTex, uv - uStep * float(i)).rgb) * w[i];
    outColor = vec4(c, 1.0);
}

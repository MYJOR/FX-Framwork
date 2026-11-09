in vec2 vUV;
out vec4 outColor;

uniform float uStrength;
uniform float uLen;     // beam length in metres
uniform int uMode;      // 0 = beam, 1 = round flare

void main() {
    vec3 col;
    if (uMode == 0) {
        float r = abs(vUV.y);
        float along = vUV.x * uLen;
        float n = 0.5 + 0.5 * sin(along * 7.0 - uTime * 40.0 + sin(along * 2.1 + uTime * 9.0) * 2.5);
        float n2 = 0.5 + 0.5 * sin(along * 17.0 - uTime * 63.0 + r * 6.0);
        float wob = mix(0.8, 1.0, n);
        float core = exp(-r * r * 22.0) * (0.75 + 0.25 * n2);
        float glow = exp(-r * r * 3.2) * 0.5 * wob;
        float halo = exp(-r * 2.2) * 0.12;
        col = vec3(0.25, 0.65, 1.0) * (glow + halo) + vec3(0.8, 0.95, 1.0) * core * 1.4;
        col *= smoothstep(0.0, 0.015, vUV.x) * (1.0 - smoothstep(0.985, 1.0, vUV.x) * 0.6);
    } else {
        vec2 q = vec2(vUV.x * 2.0 - 1.0, vUV.y);
        float d2 = dot(q, q);
        float f = 0.65 + 0.35 * sin(uTime * 55.0);
        col = vec3(0.3, 0.7, 1.0) * exp(-d2 * 3.5) * 0.9 + vec3(0.9, 0.97, 1.0) * exp(-d2 * 14.0) * 1.3 * f;
    }
    outColor = vec4(col * uStrength, 0.0);
}

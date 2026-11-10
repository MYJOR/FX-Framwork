in vec2 vNdc;
out vec4 outColor;

// Builds one mip of the reflection probe: the captured cube map convolved with the GGX lobe of a
// given roughness. Plain box-filtered mips leave blocky, seam-ridden highlights around the light
// panels; this is what makes rough reflections smooth.

#define SAMPLES 96

uniform samplerCube uSrc;       // the raw capture, with ordinary mips
uniform vec3 uFaceFwd, uFaceRight, uFaceUp;
uniform float uRoughness;
uniform float uSrcRes;

float radicalInverse(uint b) {
    b = (b << 16u) | (b >> 16u);
    b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
    b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
    b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
    b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
    return float(b) * 2.3283064365386963e-10;
}

void main() {
    vec3 N = normalize(uFaceFwd + vNdc.x * uFaceRight + vNdc.y * uFaceUp);
    if (uRoughness <= 0.0) { outColor = vec4(textureLod(uSrc, N, 0.0).rgb, 1.0); return; }

    float a = uRoughness * uRoughness;
    vec3 up = abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N)), B = cross(N, T);
    float texelSolidAngle = 4.0 * PI / (6.0 * uSrcRes * uSrcRes);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < SAMPLES; i++) {
        vec2 xi = vec2((float(i) + 0.5) / float(SAMPLES), radicalInverse(uint(i)));
        float phi = 2.0 * PI * xi.x;
        float cosT = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
        float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
        vec3 H = T * (sinT * cos(phi)) + B * (sinT * sin(phi)) + N * cosT;
        vec3 L = 2.0 * cosT * H - N; // view = normal
        float NoL = dot(N, L);
        if (NoL <= 0.0) continue;
        // read a mip whose texels match the area this sample stands for: no fireflies from the panels
        float d = (cosT * cosT) * (a * a - 1.0) + 1.0;
        float pdf = a * a / (PI * d * d) * 0.25;
        float lod = max(0.5 * log2(1.0 / (float(SAMPLES) * pdf * texelSolidAngle)) + 1.0, 0.0);
        sum += textureLod(uSrc, L, lod).rgb * NoL;
        wsum += NoL;
    }
    outColor = vec4(sum / max(wsum, 1e-4), 1.0);
}

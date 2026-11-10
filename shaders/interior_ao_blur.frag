in vec2 vNdc;
out vec4 outColor;

// 4x4 box blur over exactly one rotation tile of interior_ao.frag. Taps across a depth edge are
// dropped so obscurance does not bleed between a foreground object and the wall behind it.

uniform sampler2D uAOTex;
uniform sampler2D uDepthTex;
uniform vec4 uProjInfo; // tan(fov/2) * aspect, tan(fov/2), near, far

void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    ivec2 hi = textureSize(uAOTex, 0) - 1;
    float zc = linearDepth(texelFetch(uDepthTex, ip, 0).r, uProjInfo.z, uProjInfo.w);
    float sum = 0.0, wsum = 0.0;
    for (int y = -2; y < 2; y++)
        for (int x = -2; x < 2; x++) {
            ivec2 q = clamp(ip + ivec2(x, y), ivec2(0), hi);
            float z = linearDepth(texelFetch(uDepthTex, q, 0).r, uProjInfo.z, uProjInfo.w);
            float w = max(1.0 - abs(z - zc) / (0.03 * zc + 0.01), 0.0) + 1e-3;
            sum += texelFetch(uAOTex, q, 0).r * w;
            wsum += w;
        }
    outColor = vec4(sum / wsum);
}

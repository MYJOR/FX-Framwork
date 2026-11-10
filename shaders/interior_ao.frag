in vec2 vNdc;
out vec4 outColor;

// Screen-space ambient obscurance (McGuire's "Scalable Ambient Obscurance" estimator).
// The tap pattern is rotated per pixel on a 4x4 tile; interior_ao_blur.frag averages exactly one
// tile, which removes the pattern completely instead of leaving noise.

#define AO_TAPS 12

uniform sampler2D uDepthTex;
uniform sampler2D uNormalTex;
uniform mat4 uView;
uniform vec4 uProjInfo;  // tan(fov/2) * aspect, tan(fov/2), near, far
uniform vec4 uAOParams;  // radius (m), intensity, bias (m), pixels per metre at distance 1
uniform vec2 uInvScreen;

vec3 viewPos(vec2 uv) {
    float z = linearDepth(texture(uDepthTex, uv).r, uProjInfo.z, uProjInfo.w);
    return vec3((uv * 2.0 - 1.0) * uProjInfo.xy * z, -z);
}

void main() {
    vec2 uv = gl_FragCoord.xy * uInvScreen;
    if (texture(uDepthTex, uv).r >= 1.0) { outColor = vec4(1.0); return; }
    vec3 P = viewPos(uv);
    vec3 N = normalize(mat3(uView) * (texture(uNormalTex, uv).xyz * 2.0 - 1.0));

    float radius = uAOParams.x;
    float rPix = min(radius * uAOParams.w / -P.z, 120.0);
    if (rPix < 1.5) { outColor = vec4(1.0); return; }

    ivec2 ip = ivec2(gl_FragCoord.xy);
    float rot = float((ip.x & 3) + ((ip.y & 3) << 2)) * (6.2831853 / 16.0);
    float r2 = radius * radius, sum = 0.0;
    for (int i = 0; i < AO_TAPS; i++) {
        vec3 v = viewPos(uv + vogel(i, AO_TAPS, rot) * rPix * uInvScreen) - P;
        float vv = dot(v, v), vn = dot(v, N);
        float f = max(r2 - vv, 0.0);
        sum += f * f * f * max((vn - uAOParams.z) / (vv + 1e-4), 0.0);
    }
    float ao = max(1.0 - sum * uAOParams.y * 5.0 / (r2 * r2 * r2 * float(AO_TAPS)), 0.0);
    outColor = vec4(ao);
}

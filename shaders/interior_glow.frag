in vec2 vNdc;
out vec4 outColor;

// Quarter resolution source of everything that glows: the parts of the frame too bright for the
// display (they bloom), and the torch beam lighting up the dust in the air. Both are blurred by
// interior_glow_blur.frag and added to the picture after tonemapping.

uniform sampler2DMS uScene;
uniform sampler2DMS uSceneDepth;
uniform sampler2D uExposureTex;
uniform vec2 uSize;        // scene size, pixels
uniform vec4 uGlow;        // exposure, bloom strength, air density (0 = no beam), noise seed
uniform vec4 uProj;        // tan(fov/2) * aspect, tan(fov/2), near, far
uniform vec3 uRight, uUp, uFwd; // camera axes
uniform vec4 uFlashPos;    // xyz, brightness
uniform mat4 uFlashMat;
uniform sampler2DShadow uFlashShadow;
uniform sampler2D uFlashCookie;

uniform vec3 uFlashColor;
uniform vec4 uFlashShape;  // as in interior.frag
const int STEPS = 20;
const float REACH = 9.0; // metres of air that are marched

void main() {
    float exposure = uGlow.x * texelFetch(uExposureTex, ivec2(0), 0).r;
    ivec2 base = ivec2(gl_FragCoord.xy) * 4;
    ivec2 top = ivec2(uSize) - 1;

    // bloom: what is left above display white, from a 4 x 4 block (every other texel)
    vec3 bright = vec3(0.0);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec3 c = max(texelFetch(uScene, min(base + ivec2(i, j) * 2, top), 0).rgb, 0.0) * exposure;
            float l = luma(c);
            bright += c * (clamp(l - 1.0, 0.0, 5.0) / max(l, 1e-4));
        }
    vec3 glow = bright * (0.25 * uGlow.y);

    // the beam in the air
    if (uGlow.z > 0.0 && uFlashPos.w > 0.0) {
        ivec2 ip = min(base + 2, top);
        float z = linearDepth(texelFetch(uSceneDepth, ip, 0).r, uProj.z, uProj.w);
        vec2 ndc = (vec2(ip) + 0.5) / uSize * 2.0 - 1.0;
        vec3 dir = uFwd + uRight * (ndc.x * uProj.x) + uUp * (ndc.y * uProj.y);
        float along = length(dir); // ray length per metre of depth
        dir /= along;
        float tmax = min(z * along, REACH);
        float dt = tmax / float(STEPS);
        float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy + uGlow.w * vec2(5.0, 3.0), vec2(0.06711056, 0.00583715))));
        float sum = 0.0;
        for (int i = 0; i < STEPS; i++) {
            vec3 P = uCam + dir * ((float(i) + jitter) * dt);
            vec4 fc = uFlashMat * vec4(P, 1.0);
            if (fc.w <= 0.0) continue;
            vec3 fp = fc.xyz / fc.w * 0.5 + 0.5;
            if (fp.x <= 0.0 || fp.x >= 1.0 || fp.y <= 0.0 || fp.y >= 1.0 || fp.z >= 1.0) continue;
            vec3 Lv = P - uFlashPos.xyz;
            vec2 cuv = (fp.xy - 0.5) * uFlashShape.x + 0.5;
            if (any(lessThanEqual(cuv, vec2(0.0))) || any(greaterThanEqual(cuv, vec2(1.0)))) continue;
            float ck = texture(uFlashCookie, cuv).r;
            float d2 = dot(Lv, Lv);
            sum += mix(ck, ck * ck * 2.2, uFlashShape.z) * texture(uFlashShadow, fp) / ((d2 + 1.0) * (1.0 + uFlashShape.y * d2));
        }
        glow += uFlashColor * (sum * dt * uFlashPos.w * uGlow.z * exposure);
    }
    outColor = vec4(glow, 1.0);
}

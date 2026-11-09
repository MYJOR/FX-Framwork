layout(location = 0) in vec3 aPos;
layout(location = 1) in float aAlpha;
layout(location = 2) in float aSize; // metres

uniform mat4 uViewProj;
uniform float uPixelScale; // viewport height / (2 * tan(fov/2))

out vec3 vWorld;
out float vAlpha;

void main() {
    vWorld = aPos;
    vAlpha = aAlpha;
    vec4 clip = uViewProj * vec4(aPos, 1.0);
    gl_Position = clip;
    gl_PointSize = clamp(aSize * uPixelScale / max(clip.w, 0.1), 1.0, 180.0);
}

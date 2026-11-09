layout(location = 0) in vec3 aPos;
layout(location = 1) in float aAlpha;

uniform mat4 uViewProj;

out vec3 vWorld;
out float vAlpha;

void main() {
    vWorld = aPos;
    vAlpha = aAlpha;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}

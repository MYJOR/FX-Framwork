layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uViewProj;

out vec3 vWorld;
out vec3 vNormal;

void main() {
    vWorld = aPos;
    vNormal = aNormal;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}

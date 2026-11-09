layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uViewProj;
uniform mat4 uModel;

out vec3 vWorld;
out vec3 vNormal;
out vec3 vLocal;

void main() {
    vec4 w = uModel * vec4(aPos, 1.0);
    vWorld = w.xyz;
    vNormal = mat3(uModel) * aNormal; // rotation only, no scale
    vLocal = aPos;
    gl_Position = uViewProj * w;
}

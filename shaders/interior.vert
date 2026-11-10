layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uViewProj;

out vec3 vWorld;
out vec3 vNormal;
out vec2 vUV;

// The interior is static: geometry is baked into world space when the scene is built.
void main() {
    vWorld = aPos;
    vNormal = aNormal;
    vUV = aUV;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}

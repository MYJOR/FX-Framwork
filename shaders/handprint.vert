layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aInfo; // seconds since the slap, age / life, seed, side (+1 right hand, -1 left)

uniform mat4 uViewProj;

out vec3 vWorld;
out vec2 vUV;
out vec4 vInfo;

void main() {
    vWorld = aPos;
    vUV = aUV;
    vInfo = aInfo;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}

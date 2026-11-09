layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aJoints;
layout(location = 3) in vec4 aWeights;

uniform mat4 uViewProj;
uniform mat4 uModel;
uniform mat4 uBones[80];

out vec3 vWorld;
out vec3 vNormal;
out vec3 vLocal;

void main() {
    mat4 skin = aWeights.x * uBones[int(aJoints.x)] + aWeights.y * uBones[int(aJoints.y)]
              + aWeights.z * uBones[int(aJoints.z)] + aWeights.w * uBones[int(aJoints.w)];
    vec4 p = skin * vec4(aPos, 1.0);
    vec4 w = uModel * p;
    vWorld = w.xyz;
    vNormal = mat3(uModel) * (mat3(skin) * aNormal); // uModel has rotation + uniform scale
    vLocal = aPos;
    gl_Position = uViewProj * w;
}

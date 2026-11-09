layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uViewProj;
uniform mat4 uModel;

out vec3 vWorld;
out vec3 vNormal;   // geometric normal, world space
out vec3 vLocal;    // disc space: axis is +Y, radius 1 at the rim
out vec3 vLocalN;
out vec3 vTX;       // disc-space X and Z axes in world space (bump frame)
out vec3 vTZ;

void main() {
    vec4 w = uModel * vec4(aPos, 1.0);
    vWorld = w.xyz;
    mat3 R = mat3(uModel);
    float s2 = dot(R[0], R[0]);
    vNormal = normalize(R * aNormal);   // uniform scale only
    vLocal = aPos;
    vLocalN = aNormal;
    vTX = R[0] / sqrt(s2);
    vTZ = R[2] / sqrt(s2);
    gl_Position = uViewProj * w;
}

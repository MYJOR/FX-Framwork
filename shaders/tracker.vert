layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aMat;

uniform mat4 uModel;   // device -> world
uniform mat4 uView;
uniform mat4 uProj;
uniform float uPull;   // < 1: drawn this much closer to the eye than it is

out vec3 vWorld;
out vec3 vNormal;
out vec3 vObj;
out vec3 vObjNormal;
out vec2 vUV;
flat out float vMat;

// The tracker is lit where it really is, at arm's length, but drawn pulled towards the eye along the
// line of sight. That leaves its picture on screen exactly as it was and puts it nearer than any wall
// the player can walk up to, so it never pokes through one.
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorld = world.xyz;
    vNormal = mat3(uModel) * aNormal;
    vObj = aPos;
    vObjNormal = aNormal;
    vUV = aUV;
    vMat = aMat;
    vec4 eye = uView * world;
    gl_Position = uProj * vec4(eye.xyz * uPull, 1.0);
}

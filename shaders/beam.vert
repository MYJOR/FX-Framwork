layout(location = 0) in vec2 aUV;   // x along the beam 0..1, y across -1..1

uniform mat4 uViewProj;
uniform vec3 uA;
uniform vec3 uB;
uniform float uWidth;

out vec2 vUV;

void main() {
    vec3 p = mix(uA, uB, aUV.x);
    vec3 side = normalize(cross(uB - uA, uCam - p));
    p += side * aUV.y * uWidth;
    vUV = aUV;
    gl_Position = uViewProj * vec4(p, 1.0);
}

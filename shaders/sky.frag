in vec2 vNdc;
out vec4 outColor;

uniform vec3 uRight, uUp, uFwd;
uniform float uTanHalfFov, uAspect;

void main() {
    vec3 d = normalize(uFwd + uRight * (vNdc.x * uTanHalfFov * uAspect) + uUp * (vNdc.y * uTanHalfFov));
    // the lowest sky merges into the same haze the terrain fades to
    vec3 col = mix(hazeColor(d), skyColor(d), smoothstep(0.0, 0.22, d.y));
    outColor = vec4(grade(col), 1.0);
}

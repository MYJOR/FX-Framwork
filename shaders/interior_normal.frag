in vec3 vWorld;
in vec3 vNormal;
in vec2 vUV;
out vec4 outColor;

// Prepass for ambient obscurance: geometric world normal (depth comes from the depth attachment).
void main() {
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;
    outColor = vec4(n * 0.5 + 0.5, 1.0);
}

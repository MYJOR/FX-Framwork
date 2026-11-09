in vec3 vWorld;
in vec3 vNormal;
in vec3 vLocal;
out vec4 outColor;

// Placeholder character shading. Same light model as the sand so it sits in the scene.
void main() {
    vec3 toCam = uCam - vWorld;
    float dist = length(toCam);
    vec3 V = toCam / dist;
    vec3 L = normalize(uSunDir);
    vec3 N = normalize(vNormal);

    vec3 robe = vec3(0.62, 0.09, 0.07);
    // dark visor patch on the front (local -Z) so facing is readable
    float visor = smoothstep(0.02, -0.02, vLocal.z + 0.12)
                * smoothstep(0.30, 0.22, abs(vLocal.x))
                * step(1.25, vLocal.y) * step(vLocal.y, 1.50);
    vec3 albedo = mix(robe, vec3(0.05, 0.03, 0.03), visor);

    float ndl = dot(N, L);
    float wrapped = clamp((ndl + 0.15) / 1.15, 0.0, 1.0);
    vec3 sun = sunColor();
    vec3 ambientSky = skyColor(vec3(0.0, 1.0, 0.0)) * 0.40;
    vec3 shadowTint = mix(vec3(0.30, 0.18, 0.34), vec3(0.28, 0.32, 0.48), dayK());
    // light bounced up from the sand onto the underside
    vec3 bounce = vec3(0.85, 0.52, 0.28) * sun * 0.30;
    vec3 ambient = mix(bounce, mix(shadowTint, ambientSky, 0.5) * 0.6, clamp(N.y * 0.5 + 0.5, 0.0, 1.0));

    vec3 col = albedo * (sun * wrapped + ambient);

    float fres = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    col += sun * fres * smoothstep(-0.1, 0.5, ndl) * 0.5;

    vec3 H = normalize(L + V);
    col += sun * pow(max(dot(N, H), 0.0), 40.0) * 0.12 * smoothstep(0.0, 0.2, ndl);

    col = applyHaze(col, dist, -V, vWorld.y);
    outColor = vec4(grade(col), 1.0);
}

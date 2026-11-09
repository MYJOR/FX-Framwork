in vec3 vWorld;
in float vAlpha;
out vec4 outColor;

// Soft round puff of sunlit sand dust.
void main() {
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(c, c);
    if (r2 > 1.0) discard;
    float soft = exp(-r2 * 3.2) * (1.0 - r2);

    vec3 toCam = uCam - vWorld;
    float dist = length(toCam);
    vec3 V = toCam / dist;
    float toSun = max(dot(-V, normalize(uSunDir)), 0.0);
    vec3 sun = sunColor();
    vec3 col = vec3(0.92, 0.66, 0.40) * (sun * 0.80 + skyColor(vec3(0, 1, 0)) * 0.30);
    col += sun * pow(toSun, 5.0) * 0.45; // glows when the sun is behind it
    col = applyHaze(col, dist, -V, vWorld.y);

    float a = clamp(vAlpha * soft, 0.0, 1.0);
    outColor = vec4(grade(col) * a, a); // premultiplied
}

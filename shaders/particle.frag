in vec3 vWorld;
in float vAlpha;
out vec4 outColor;

void main() {
    vec3 toCam = uCam - vWorld;
    float dist = length(toCam);
    vec3 V = toCam / dist;

    // lit grain colour: warm sand catching the sun, brightest looking toward it
    float toSun = max(dot(-V, normalize(uSunDir)), 0.0);
    vec3 sun = sunColor();
    vec3 col = vec3(0.95, 0.68, 0.40) * (sun * 0.75 + skyColor(vec3(0, 1, 0)) * 0.25);
    col += sun * pow(toSun, 6.0) * 0.5; // forward-scattered glow when backlit
    col = applyHaze(col, dist, -V, vWorld.y);

    float a = clamp(vAlpha, 0.0, 1.0);
    outColor = vec4(grade(col) * a, a); // premultiplied
}

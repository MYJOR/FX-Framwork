in vec3 vWorld;
in vec3 vNormal;
in vec3 vLocal;
in vec3 vLocalN;
in vec3 vTX;
in vec3 vTZ;
out vec4 outColor;

uniform float uFade;   // 1 normally, falls to 0 while a collected coin shrinks away
uniform float uSeed;   // per-coin variation: wear pattern, tiny colour shift

// ---------------------------------------------------------------------------
// Polished gold coin: GGX sun highlight, sky + sand environment reflection,
// Schlick fresnel with a gold F0, and a procedural embossed face (bead ring,
// groove, five point star) done as a height field that bends the normal.
// ---------------------------------------------------------------------------

const vec3 GOLD_F0 = vec3(1.000, 0.766, 0.336);

float sdStar5(vec2 p, float r, float rf) {
    // classic five point star distance (iq)
    const vec2 k1 = vec2(0.809016994375, -0.587785252292);
    const vec2 k2 = vec2(-k1.x, k1.y);
    p.x = abs(p.x);
    p -= 2.0 * max(dot(k1, p), 0.0) * k1;
    p -= 2.0 * max(dot(k2, p), 0.0) * k2;
    p.x = abs(p.x);
    p.y -= r;
    vec2 ba = rf * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
    float h = clamp(dot(p, ba) / dot(ba, ba), 0.0, r);
    return length(p - ba * h) * sign(p.y * ba.x - p.x * ba.y);
}

// Height of the embossed face, in disc-radius units (outward = positive). p is the disc-plane position.
float faceHeight(vec2 p) {
    float r = length(p);
    float a = atan(p.y, p.x);
    float h = 0.0;

    // raised outer rim with a rounded profile
    h += 0.030 * smoothstep(0.86, 0.90, r) * (1.0 - smoothstep(0.985, 1.0, r));
    // ring of beads just inside the rim
    float beads = pow(0.5 + 0.5 * cos(a * 44.0), 1.5);
    h += 0.016 * beads * smoothstep(0.74, 0.77, r) * (1.0 - smoothstep(0.82, 0.85, r));
    // fine groove between the beads and the field
    h -= 0.012 * exp(-pow((r - 0.855) / 0.012, 2.0));
    // slightly dished field
    h -= 0.010 * (1.0 - smoothstep(0.0, 0.72, r));
    // star emblem with soft shoulders
    float d = sdStar5(p * 1.45, 0.55, 0.45);
    h += 0.030 * (1.0 - smoothstep(-0.012, 0.012, d)) * (1.0 - smoothstep(0.62, 0.72, r));
    // thin inner ring around the star
    h += 0.010 * exp(-pow((r - 0.64) / 0.010, 2.0));
    return h;
}

void main() {
    vec3 toCam = uCam - vWorld;
    float dist = length(toCam);
    vec3 V = toCam / dist;
    vec3 L = normalize(uSunDir);
    vec3 Ng = normalize(vNormal);
    if (!gl_FrontFacing) Ng = -Ng;

    vec2 p = vLocal.xz;
    float r = length(p);
    bool wall = abs(vLocalN.y) < 0.5;
    float roughness = 0.20;
    float cavity = 1.0;
    vec3 N = Ng;

    if (!wall) {
        // bump from the height field (central differences in the disc plane)
        float e = 0.004;
        float h  = faceHeight(p);
        float hx = faceHeight(p + vec2(e, 0.0)) - faceHeight(p - vec2(e, 0.0));
        float hz = faceHeight(p + vec2(0.0, e)) - faceHeight(p - vec2(0.0, e));
        float k = 1.5; // bump strength (slope multiplier)
        N = normalize(Ng - k * (hx / (2.0 * e)) * vTX - k * (hz / (2.0 * e)) * vTZ);
        cavity = 0.80 + 0.20 * smoothstep(-0.012, 0.012, h);
        // the field is a touch rougher than the raised relief, which burnishes brighter
        roughness = mix(0.30, 0.17, smoothstep(-0.004, 0.02, h));
    } else {
        // reeded edge: fine vertical ridges
        float a = atan(p.y, p.x);
        float ridge = sin(a * 130.0);
        vec3 T = normalize(vTX * (-sin(a)) + vTZ * cos(a));
        N = normalize(Ng + T * ridge * 0.35);
        cavity = 0.75 + 0.25 * (0.5 + 0.5 * ridge);
        roughness = 0.28;
    }

    // micro scratches and wear: faint, so it reads as a polished but real object
    vec2 sp = vLocal.xz * 38.0 + uSeed * 17.0;
    float scratch = vnoise(vec2(sp.x * 2.5, sp.y * 0.35)) * vnoise(vec2(sp.y * 2.0 + 7.0, sp.x * 0.3));
    N = normalize(N + (vTX * (vnoise(sp) - 0.5) + vTZ * (vnoise(sp + 31.0) - 0.5)) * 0.06);
    float wear = smoothstep(0.55, 0.9, scratch);
    roughness = clamp(roughness + wear * 0.18, 0.12, 0.6);

    float NoV = max(dot(N, V), 1e-3);
    float NoL = max(dot(N, L), 0.0);
    vec3 H = normalize(L + V);
    float NoH = max(dot(N, H), 0.0);
    float VoH = max(dot(V, H), 0.0);

    vec3 F0 = GOLD_F0 * (0.96 + 0.08 * fract(uSeed * 7.31));
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VoH, 5.0);

    // sun: GGX specular (metals have no diffuse)
    float a = roughness * roughness;
    float a2 = a * a;
    float dd = NoH * NoH * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * dd * dd);
    float kk = a * 0.5;
    float G = (NoV / (NoV * (1.0 - kk) + kk)) * (NoL / (NoL * (1.0 - kk) + kk + 1e-4));
    vec3 sun = sunColor();
    vec3 spec = sun * F * (D * G / (4.0 * NoV * max(NoL, 1e-3))) * NoL;

    // environment: reflect the sky above the horizon and warm sand below it
    vec3 R = reflect(-V, N);
    vec3 skyRefl = skyColor(normalize(vec3(R.x, max(R.y, -0.02), R.z)));
    // rough surfaces see a blurred sky: pull toward the horizon / zenith average
    vec3 blur = mix(skyColor(vec3(0.0, 1.0, 0.0)), hazeColor(R), 0.6);
    skyRefl = mix(skyRefl, blur, smoothstep(0.1, 0.5, roughness));
    vec3 sandCol = vec3(0.85, 0.52, 0.27) * (sun * 0.42 + skyColor(vec3(0.0, 1.0, 0.0)) * 0.22);
    vec3 env = mix(sandCol, skyRefl, smoothstep(-0.08, 0.10, R.y));
    // grazing angles reflect more, as real metal does; keeps the silhouette lit
    vec3 envF = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NoV, 5.0);
    vec3 col = env * envF * cavity * 1.25;
    col += spec;

    // glints: a hot twinkle when a facet mirrors the sun to the eye
    float gl = pow(NoH, 900.0 / max(roughness * roughness * 4.0, 0.05));
    col += sun * gl * 3.0 * (0.5 + 0.5 * wear);

    // a hint of warm bounce from the sand underneath
    col += F0 * vec3(0.9, 0.55, 0.3) * sun * 0.05 * (1.0 - N.y) * cavity;

    col = applyHaze(col, dist, -V, vWorld.y);
    outColor = vec4(grade(col * uFade), 1.0);
}

#version 330 core
// Prelude for the interior scene's shaders (the desert uses common.glsl instead).

uniform vec3 uCam;

const float PI = 3.14159265;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Evenly spread points on a disc: sample i of n, rotated by rot radians.
vec2 vogel(int i, int n, float rot) {
    float r = sqrt((float(i) + 0.5) / float(n));
    float a = float(i) * 2.39996323 + rot;
    return r * vec2(cos(a), sin(a));
}

// Depth buffer value -> distance along the view axis.
float linearDepth(float d, float near, float far) { return near * far / (far - d * (far - near)); }

// ---- two rooms and the doorway between them (IntPortal) ------------------------------------------
uniform vec4 uPortal;        // the wall's faces x0, x1; doorway centre z; half width. x1 <= x0: no wall
uniform float uPortalSpring; // where the arch starts

// Signed distance to the doorway's outline in the wall plane, q = (z - centre, y). Negative inside.
float portalShape(vec2 q) {
    float dz = abs(q.x);
    return q.y < uPortalSpring ? dz - uPortal.w : length(vec2(dz, q.y - uPortalSpring)) - uPortal.w;
}

// How much of a light at L reaches P: all of it when both are in the same room, otherwise what fits
// through the doorway. soft = radius of the light, which blurs the outline.
float portalVis(vec3 P, vec3 L, float soft) {
    if (uPortal.y <= uPortal.x) return 1.0;
    float sp = P.x < uPortal.x ? -1.0 : (P.x > uPortal.y ? 1.0 : 0.0);
    float sl = L.x < uPortal.x ? -1.0 : (L.x > uPortal.y ? 1.0 : 0.0);
    if (sp * sl >= 0.0) return 1.0;
    float vis = 1.0;
    for (int i = 0; i < 2; i++) { // the opening is a short tunnel: the ray has to clear both ends
        float t = ((i == 0 ? uPortal.x : uPortal.y) - P.x) / (L.x - P.x);
        vec3 Q = mix(P, L, t);
        float w = max(soft * t, 0.012);
        vis = min(vis, 1.0 - smoothstep(-w, w, portalShape(vec2(Q.z - uPortal.z, Q.y))));
    }
    return vis;
}
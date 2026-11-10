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

// ---- rooms in a row along x and the doorways between them (IntPortal) -----------------------------
#define MAX_PORTALS 2
uniform vec4 uPortal[MAX_PORTALS];     // per wall: its faces x0, x1; doorway centre z; half width
uniform vec4 uPortalArch[MAX_PORTALS]; // x: where the arch starts
uniform int uPortalCount;              // 0: the level is one room

// Signed distance to doorway k's outline in the wall plane, q = (z - centre, y). Negative inside.
float portalShape(int k, vec2 q) {
    float dz = abs(q.x), spring = uPortalArch[k].x;
    return q.y < spring ? dz - uPortal[k].w : length(vec2(dz, q.y - spring)) - uPortal[k].w;
}

// How much of a light at L reaches P: all of it when both are in the same room, otherwise what fits
// through the doorways between them. soft = radius of the light, which blurs the outline.
float portalVis(vec3 P, vec3 L, float soft) {
    float vis = 1.0;
    for (int k = 0; k < uPortalCount; k++) { // every wall between the two has to be crossed through its doorway
        vec4 d = uPortal[k];
        float sp = P.x < d.x ? -1.0 : (P.x > d.y ? 1.0 : 0.0);
        float sl = L.x < d.x ? -1.0 : (L.x > d.y ? 1.0 : 0.0);
        if (sp * sl >= 0.0) continue;
        for (int i = 0; i < 2; i++) { // the opening is a short tunnel: the ray has to clear both ends
            float t = ((i == 0 ? d.x : d.y) - P.x) / (L.x - P.x);
            vec3 Q = mix(P, L, t);
            float w = max(soft * t, 0.012);
            vis = min(vis, 1.0 - smoothstep(-w, w, portalShape(k, vec2(Q.z - d.z, Q.y))));
        }
    }
    return vis;
}
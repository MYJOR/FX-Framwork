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

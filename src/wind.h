#ifndef WIND_H
#define WIND_H
#include <math.h>

/* Wind model. The same formula exists in shaders/common.glsl (gust()) so the
   CPU particles and the shader effects always agree. Change both together. */
#define WIND_X 0.906f
#define WIND_Z 0.423f

/* 0 = calm lull, 1 = peak gust. Gust fronts travel along the wind direction. */
static inline float wind_gust(float x, float z, float t) {
    float s = x * WIND_X + z * WIND_Z;
    float q = -x * WIND_Z + z * WIND_X;
    float a = (s - 9.0f * t) * 0.035f + sinf(q * 0.017f) * 1.7f;
    float b = (s - 6.0f * t) * 0.09f + sinf(q * 0.041f + 1.0f) * 1.1f;
    float g = 0.5f + 0.35f * sinf(a) + 0.15f * sinf(b);
    g = g < 0 ? 0 : (g > 1 ? 1 : g);
    float k = (g - 0.3f) / 0.6f;
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    return k * k * (3.0f - 2.0f * k);
}

#endif

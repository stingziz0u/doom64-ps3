/* compat/dc/fmath.h -- matematica rapida del SH4.
 *
 * frsqrt/fsqrt son intrinsecos del SH4 (una instruccion). En PPC se resuelven
 * con la libm normal; el compilador de PS3 ya emite fsqrts para sqrtf. */

#ifndef PS3_COMPAT_DC_FMATH_H
#define PS3_COMPAT_DC_FMATH_H

#include <math.h>

static inline float fsqrt(float f)  { return sqrtf(f); }
static inline float frsqrt(float f) { return 1.0f / sqrtf(f); }

#ifndef F_PI
#define F_PI 3.1415926f
#endif

/* Macros de KOS (dc/vec3f.h). Modifican sus argumentos in-place, por eso son
 * macros y no funciones. */
#define vec3f_length(x, y, z, w) \
    do { (w) = sqrtf((x)*(x) + (y)*(y) + (z)*(z)); } while (0)

#define vec3f_normalize(x, y, z) \
    do { float _l = sqrtf((x)*(x) + (y)*(y) + (z)*(z)); \
         if (_l != 0.0f) { (x) /= _l; (y) /= _l; (z) /= _l; } } while (0)

#define vec3f_dot(x1, y1, z1, x2, y2, z2, w) \
    do { (w) = (x1)*(x2) + (y1)*(y2) + (z1)*(z2); } while (0)

#endif

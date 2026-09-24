/* compat/dc/matrix.h -- el registro de matriz XMTRX del SH4, por software.
 *
 * En el Dreamcast hay UNA matriz "actual" dentro de la FPU (XMTRX). El motor
 * la arma una vez por frame con mat_load/mat_apply y despues proyecta cada
 * vertice con mat_trans_single3_nodivw.
 *
 * OJO: en KOS, mat_trans_* son MACROS que modifican sus argumentos EN EL
 * LUGAR (x, y, z, w son lvalues de entrada y salida). La primera version de
 * este shim las declaraba como funciones que reciben copias, y entonces
 * ningun vertice se transformaba nunca: la geometria quedaba en coordenadas
 * de mundo sin que nada fallara. Tienen que ser macros.
 *
 * Convencion (verificada contra R_Frustum/R_Translate de doomdef.h):
 *   matrix_t es column-major: m[columna][fila]. La traslacion esta en m[3][*]
 *   y la columna de perspectiva en m[*][3].
 *   mat_apply(M) hace  XMTRX = XMTRX * M  (post-multiplica), asi que
 *     mat_load(V); mat_apply(P); mat_apply(R); mat_apply(T);
 *   deja XMTRX = V*P*R*T, y un vertice v se transforma como XMTRX * v.
 */
#ifndef PS3_COMPAT_DC_MATRIX_H
#define PS3_COMPAT_DC_MATRIX_H

#include "dc/vector.h"

typedef float matrix_t[4][4];

/* La matriz actual (XMTRX). Vive en ps3_matrix.c. */
extern float ps3_xmtrx[4][4];

void mat_identity(void);
void mat_load(matrix_t *m);
void mat_store(matrix_t *m);
void mat_apply(matrix_t *m);
void mat_transform(vector_t *in, vector_t *out, int veclen, int stride);

/* (x,y,z,1) -> (x,y,z,w) sin dividir. Es la que usa el motor para cada
 * vertice (transform_d64ListVert); el near-z clipping se hace despues con w. */
#define mat_trans_single3_nodivw(x, y, z, w) do {                              \
    const float _mx = (x), _my = (y), _mz = (z);                               \
    (x) = ps3_xmtrx[0][0]*_mx + ps3_xmtrx[1][0]*_my + ps3_xmtrx[2][0]*_mz + ps3_xmtrx[3][0]; \
    (y) = ps3_xmtrx[0][1]*_mx + ps3_xmtrx[1][1]*_my + ps3_xmtrx[2][1]*_mz + ps3_xmtrx[3][1]; \
    (z) = ps3_xmtrx[0][2]*_mx + ps3_xmtrx[1][2]*_my + ps3_xmtrx[2][2]*_mz + ps3_xmtrx[3][2]; \
    (w) = ps3_xmtrx[0][3]*_mx + ps3_xmtrx[1][3]*_my + ps3_xmtrx[2][3]*_mz + ps3_xmtrx[3][3]; \
} while (0)

/* (x,y,z,1) -> (x,y,z), sin dividir y descartando w. */
#define mat_trans_single3_nodiv(x, y, z) do {                                  \
    const float _mx = (x), _my = (y), _mz = (z);                               \
    (x) = ps3_xmtrx[0][0]*_mx + ps3_xmtrx[1][0]*_my + ps3_xmtrx[2][0]*_mz + ps3_xmtrx[3][0]; \
    (y) = ps3_xmtrx[0][1]*_mx + ps3_xmtrx[1][1]*_my + ps3_xmtrx[2][1]*_mz + ps3_xmtrx[3][1]; \
    (z) = ps3_xmtrx[0][2]*_mx + ps3_xmtrx[1][2]*_my + ps3_xmtrx[2][2]*_mz + ps3_xmtrx[3][2]; \
} while (0)

/* (x,y,z,1) -> (x/w, y/w, 1/w): la proyeccion completa, como la espera el TA. */
#define mat_trans_single(x, y, z) do {                                         \
    const float _mx = (x), _my = (y), _mz = (z);                               \
    const float _tw = ps3_xmtrx[0][3]*_mx + ps3_xmtrx[1][3]*_my + ps3_xmtrx[2][3]*_mz + ps3_xmtrx[3][3]; \
    const float _iw = 1.0f / _tw;                                              \
    (x) = (ps3_xmtrx[0][0]*_mx + ps3_xmtrx[1][0]*_my + ps3_xmtrx[2][0]*_mz + ps3_xmtrx[3][0]) * _iw; \
    (y) = (ps3_xmtrx[0][1]*_mx + ps3_xmtrx[1][1]*_my + ps3_xmtrx[2][1]*_mz + ps3_xmtrx[3][1]) * _iw; \
    (z) = _iw;                                                                 \
} while (0)

#endif /* PS3_COMPAT_DC_MATRIX_H */

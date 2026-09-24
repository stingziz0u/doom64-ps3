/* ps3_matrix.c -- XMTRX del SH4 por software. Ver dc/matrix.h. */

#include <string.h>
#include "dc/matrix.h"

float ps3_xmtrx[4][4] __attribute__((aligned(16))) = {
    { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }
};

void mat_identity(void)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            ps3_xmtrx[i][j] = (i == j) ? 1.0f : 0.0f;
}

void mat_load(matrix_t *m)
{
    memcpy(ps3_xmtrx, *m, sizeof(ps3_xmtrx));
}

void mat_store(matrix_t *m)
{
    memcpy(*m, ps3_xmtrx, sizeof(ps3_xmtrx));
}

/* XMTRX = XMTRX * M, column-major:  R[col][row] = sum_k X[k][row] * M[col][k] */
void mat_apply(matrix_t *m)
{
    float r[4][4];
    int col, row;

    for (col = 0; col < 4; col++)
        for (row = 0; row < 4; row++)
            r[col][row] = ps3_xmtrx[0][row] * (*m)[col][0]
                        + ps3_xmtrx[1][row] * (*m)[col][1]
                        + ps3_xmtrx[2][row] * (*m)[col][2]
                        + ps3_xmtrx[3][row] * (*m)[col][3];

    memcpy(ps3_xmtrx, r, sizeof(ps3_xmtrx));
}

/* KOS: transforma 'veclen' vectores separados por 'stride' bytes, con
 * division perspectiva (x/w, y/w, 1/w). El motor no la usa hoy; esta por
 * completitud. */
void mat_transform(vector_t *in, vector_t *out, int veclen, int stride)
{
    int i;
    for (i = 0; i < veclen; i++) {
        float x = in->x, y = in->y, z = in->z;
        mat_trans_single(x, y, z);
        out->x = x; out->y = y; out->z = z;
        in  = (vector_t *)((char *)in  + stride);
        out = (vector_t *)((char *)out + stride);
    }
}

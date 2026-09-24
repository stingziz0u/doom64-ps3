/* backend RSX de SOFTWARE para x86: rasteriza los triangulos como lo haria
 * el RSX (clip space -> division perspectiva -> viewport), con z-buffer y
 * blending, y escribe PPMs. Sirve para VER lo que va a dibujar la consola. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ps3_rsx.h"

#define W 640
#define H 480
static float zbuf[W*H];
static unsigned char cbuf[W*H*3];
int fake_tris = 0, fake_draws = 0;
float fake_minw = 1e30f, fake_maxw = -1e30f;
static int frame = 0;

struct ps3_rtex { int w, h; uint32_t *px; };
int soft_tex_count = 0;
ps3_rtex_t *PS3_RSX_TexCreate(int w, int h, const uint32_t *argb)
{
    ps3_rtex_t *t = malloc(sizeof(*t));
    t->w = w; t->h = h; t->px = malloc((size_t)w*h*4); memcpy(t->px, argb, (size_t)w*h*4);
    soft_tex_count++;
    return t;
}
void PS3_RSX_TexRelease(ps3_rtex_t *t) { if (t) { free(t->px); free(t); soft_tex_count--; } }
static uint32_t sample(const ps3_rtex_t *t, float u, float v, int clamp)
{
    float fu = u * t->w, fv = v * t->h;
    int x = (int)floorf(fu), y = (int)floorf(fv);
    if (clamp & 1) { if (x < 0) x = 0; if (x >= t->w) x = t->w-1; } else { x %= t->w; if (x < 0) x += t->w; }
    if (clamp & 2) { if (y < 0) y = 0; if (y >= t->h) y = t->h-1; } else { y %= t->h; if (y < 0) y += t->h; }
    return t->px[y * t->w + x];
}
int  PS3_RSX_Init(void) { return 0; }
void PS3_RSX_Shutdown(void) {}
int  PS3_RSX_Ready(void) { return 1; }

void soft_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(cbuf, 1, sizeof(cbuf), f);
    fclose(f);
}

void PS3_RSX_BeginScene(uint32_t bg)
{
    for (int i = 0; i < W*H; i++) { zbuf[i] = 1.0f; cbuf[i*3]=bg>>16; cbuf[i*3+1]=bg>>8; cbuf[i*3+2]=bg; }
    frame++;
}

static int depth_pass(int f, float z, float old)
{
    switch (f) {
    case RSX_DEPTH_NEVER: return 0; case RSX_DEPTH_LESS: return z < old;
    case RSX_DEPTH_EQUAL: return z == old; case RSX_DEPTH_LEQUAL: return z <= old;
    case RSX_DEPTH_GREATER: return z > old; case RSX_DEPTH_NOTEQUAL: return z != old;
    case RSX_DEPTH_GEQUAL: return z >= old; default: return 1;
    }
}
static float bf(int f, float sa, float sc, float dc)
{
    switch (f) { case RSX_BLEND_ZERO: return 0; case RSX_BLEND_ONE: return 1;
    case RSX_BLEND_SRCALPHA: return sa; case RSX_BLEND_INVSRCALPHA: return 1-sa;
    case RSX_BLEND_DESTCOLOR: return dc; case RSX_BLEND_INVDESTCOLOR: return 1-dc;
    default: return 1; }
}

typedef struct { float x, y, z, iw, r, g, b, a, u, v; } sv_t;

static void tri(const ps3_rstate_t *st, const ps3_rvert_t *v)
{
    sv_t s[3];
    for (int i = 0; i < 3; i++) {
        if (v[i].w <= 0) return;                    /* el motor ya clipea near */
        float iw = 1.0f / v[i].w;
        float nx = v[i].x * iw, ny = v[i].y * iw, nz = v[i].z * iw;
        s[i].x = (nx * 0.5f + 0.5f) * W;
        s[i].y = (1.0f - (ny * 0.5f + 0.5f)) * H;   /* scale[1] negativo */
        s[i].z = nz * 0.5f + 0.5f;
        s[i].iw = iw;
        uint32_t c = v[i].rgba;
        s[i].r = (c>>24)&255; s[i].g = (c>>16)&255; s[i].b = (c>>8)&255; s[i].a = c&255;
        s[i].u = v[i].u; s[i].v = v[i].v;
    }
    float area = (s[1].x-s[0].x)*(s[2].y-s[0].y) - (s[2].x-s[0].x)*(s[1].y-s[0].y);
    if (fabsf(area) < 1e-6f) return;
    int x0 = floorf(fminf(s[0].x, fminf(s[1].x, s[2].x))), x1 = ceilf(fmaxf(s[0].x, fmaxf(s[1].x, s[2].x)));
    int y0 = floorf(fminf(s[0].y, fminf(s[1].y, s[2].y))), y1 = ceilf(fmaxf(s[0].y, fmaxf(s[1].y, s[2].y)));
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        float px = x + 0.5f, py = y + 0.5f;
        float w0 = ((s[1].x-px)*(s[2].y-py) - (s[2].x-px)*(s[1].y-py)) / area;
        float w1 = ((s[2].x-px)*(s[0].y-py) - (s[0].x-px)*(s[2].y-py)) / area;
        float w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        float z = w0*s[0].z + w1*s[1].z + w2*s[2].z;
        int idx = y*W + x;
        if (!depth_pass(st->depth_func, z, zbuf[idx])) continue;
        /* interpolacion perspectiva-correcta del color */
        float q = w0*s[0].iw + w1*s[1].iw + w2*s[2].iw;
        float r = (w0*s[0].r*s[0].iw + w1*s[1].r*s[1].iw + w2*s[2].r*s[2].iw)/q;
        float g = (w0*s[0].g*s[0].iw + w1*s[1].g*s[1].iw + w2*s[2].g*s[2].iw)/q;
        float b = (w0*s[0].b*s[0].iw + w1*s[1].b*s[1].iw + w2*s[2].b*s[2].iw)/q;
        float a = (w0*s[0].a*s[0].iw + w1*s[1].a*s[1].iw + w2*s[2].a*s[2].iw)/q / 255.0f;
        if (st->tex) {
            float tu = (w0*s[0].u*s[0].iw + w1*s[1].u*s[1].iw + w2*s[2].u*s[2].iw)/q;
            float tv = (w0*s[0].v*s[0].iw + w1*s[1].v*s[1].iw + w2*s[2].v*s[2].iw)/q;
            uint32_t tx = sample(st->tex, tu, tv, st->clamp);
            float ta = (tx >> 24) / 255.0f;
            if (st->alpha_test && ta <= 0.0f) continue;
            r *= ((tx >> 16) & 255) / 255.0f; g *= ((tx >> 8) & 255) / 255.0f; b *= (tx & 255) / 255.0f;
            a *= ta;
        }
        unsigned char *d = &cbuf[idx*3];
        if (st->blend) {
            float c[3] = { r/255, g/255, b/255 }, dd[3] = { d[0]/255.f, d[1]/255.f, d[2]/255.f };
            for (int k = 0; k < 3; k++) {
                float o = c[k]*bf(st->src, a, c[k], dd[k]) + dd[k]*bf(st->dst, a, c[k], dd[k]);
                d[k] = (unsigned char)(fminf(o, 1.0f)*255);
            }
        } else { d[0] = r; d[1] = g; d[2] = b; }
        if (st->depth_write) zbuf[idx] = z;
    }
}

void PS3_RSX_DrawTris(const ps3_rstate_t *st, const ps3_rvert_t *v, int n)
{
    fake_draws++;
    for (int i = 0; i + 2 < n; i += 3) {
        for (int k = 0; k < 3; k++) { if (v[i+k].w < fake_minw) fake_minw = v[i+k].w; if (v[i+k].w > fake_maxw) fake_maxw = v[i+k].w; }
        tri(st, &v[i]); fake_tris++;
    }
}

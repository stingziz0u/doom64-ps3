/* n64synth.c -- el sintetizador de audio del N64, por software.
 *
 * Reemplaza a libultra (alSyn*) + el microcodigo de audio del RSP para el
 * WESS. La logica sigue a:
 *   - libultra (ultralib: env.c, resample.c, load.c, reverb.c, drvrnew.c):
 *     envolventes exponenciales, curva de volumen v*v, paneo de potencia
 *     constante (tabla eqpower), dry/wet del reverb, reverb BIGROOM.
 *   - el microcodigo de audio (segun mupen64plus-rsp-hle): decodificacion
 *     VADPCM, resampler de 4 taps con su tabla, filtro de un polo.
 *
 * Corre a 22050 Hz (la frecuencia de Doom 64) y n64synth_render48k() lo
 * lleva a 48000 Hz estereo para el mezclador del port.
 *
 * Datos del WMD/WDD: big-endian, igual que la PS3 (se leen tal cual; en un
 * host little-endian, como el harness de x86, las muestras RAW16 se dan
 * vuelta con rd16()).
 */
#include "wess_port.h"
#include "wessarc.h"

#define SYN_RATE      22050
#define CHUNK         184            /* muestras por tick del secuenciador (8333 us) */
#define MAX_VOICES    32

/* ------------------------------------------------------------------ */
/* Tablas                                                               */
/* ------------------------------------------------------------------ */
static const s16 eqpower[128] = {
    32767, 32764, 32757, 32744, 32727, 32704, 32677, 32644, 32607, 32564,
    32517, 32464, 32407, 32344, 32277, 32205, 32127, 32045, 31958, 31866,
    31770, 31668, 31561, 31450, 31334, 31213, 31087, 30957, 30822, 30682,
    30537, 30388, 30234, 30075, 29912, 29744, 29572, 29395, 29214, 29028,
    28838, 28643, 28444, 28241, 28033, 27821, 27605, 27385, 27160, 26931,
    26698, 26461, 26220, 25975, 25726, 25473, 25216, 24956, 24691, 24423,
    24151, 23875, 23596, 23313, 23026, 22736, 22442, 22145, 21845, 21541,
    21234, 20924, 20610, 20294, 19974, 19651, 19325, 18997, 18665, 18331,
    17993, 17653, 17310, 16965, 16617, 16266, 15913, 15558, 15200, 14840,
    14477, 14113, 13746, 13377, 13006, 12633, 12258, 11881, 11503, 11122,
    10740, 10357,  9971,  9584,  9196,  8806,  8415,  8023,  7630,  7235,
     6839,  6442,  6044,  5646,  5246,  4845,  4444,  4042,  3640,  3237,
     2833,  2429,  2025,  1620,  1216,   810,   405,     0
};

static const s16 RESAMPLE_LUT[64 * 4] = {
    0x0c39,0x66ad,0x0d46,(s16)0xffdf, 0x0b39,0x6696,0x0e5f,(s16)0xffd8,
    0x0a44,0x6669,0x0f83,(s16)0xffd0, 0x095a,0x6626,0x10b4,(s16)0xffc8,
    0x087d,0x65cd,0x11f0,(s16)0xffbf, 0x07ab,0x655e,0x1338,(s16)0xffb6,
    0x06e4,0x64d9,0x148c,(s16)0xffac, 0x0628,0x643f,0x15eb,(s16)0xffa1,
    0x0577,0x638f,0x1756,(s16)0xff96, 0x04d1,0x62cb,0x18cb,(s16)0xff8a,
    0x0435,0x61f3,0x1a4c,(s16)0xff7e, 0x03a4,0x6106,0x1bd7,(s16)0xff71,
    0x031c,0x6007,0x1d6c,(s16)0xff64, 0x029f,0x5ef5,0x1f0b,(s16)0xff56,
    0x022a,0x5dd0,0x20b3,(s16)0xff48, 0x01be,0x5c9a,0x2264,(s16)0xff3a,
    0x015b,0x5b53,0x241e,(s16)0xff2c, 0x0101,0x59fc,0x25e0,(s16)0xff1e,
    0x00ae,0x5896,0x27a9,(s16)0xff10, 0x0063,0x5720,0x297a,(s16)0xff02,
    0x001f,0x559d,0x2b50,(s16)0xfef4, (s16)0xffe2,0x540d,0x2d2c,(s16)0xfee8,
    (s16)0xffac,0x5270,0x2f0d,(s16)0xfedb, (s16)0xff7c,0x50c7,0x30f3,(s16)0xfed0,
    (s16)0xff53,0x4f14,0x32dc,(s16)0xfec6, (s16)0xff2e,0x4d57,0x34c8,(s16)0xfebd,
    (s16)0xff0f,0x4b91,0x36b6,(s16)0xfeb6, (s16)0xfef5,0x49c2,0x38a5,(s16)0xfeb0,
    (s16)0xfedf,0x47ed,0x3a95,(s16)0xfeac, (s16)0xfece,0x4611,0x3c85,(s16)0xfeab,
    (s16)0xfec0,0x4430,0x3e74,(s16)0xfeac, (s16)0xfeb6,0x424a,0x4060,(s16)0xfeaf,
    (s16)0xfeaf,0x4060,0x424a,(s16)0xfeb6, (s16)0xfeac,0x3e74,0x4430,(s16)0xfec0,
    (s16)0xfeab,0x3c85,0x4611,(s16)0xfece, (s16)0xfeac,0x3a95,0x47ed,(s16)0xfedf,
    (s16)0xfeb0,0x38a5,0x49c2,(s16)0xfef5, (s16)0xfeb6,0x36b6,0x4b91,(s16)0xff0f,
    (s16)0xfebd,0x34c8,0x4d57,(s16)0xff2e, (s16)0xfec6,0x32dc,0x4f14,(s16)0xff53,
    (s16)0xfed0,0x30f3,0x50c7,(s16)0xff7c, (s16)0xfedb,0x2f0d,0x5270,(s16)0xffac,
    (s16)0xfee8,0x2d2c,0x540d,(s16)0xffe2, (s16)0xfef4,0x2b50,0x559d,0x001f,
    (s16)0xff02,0x297a,0x5720,0x0063, (s16)0xff10,0x27a9,0x5896,0x00ae,
    (s16)0xff1e,0x25e0,0x59fc,0x0101, (s16)0xff2c,0x241e,0x5b53,0x015b,
    (s16)0xff3a,0x2264,0x5c9a,0x01be, (s16)0xff48,0x20b3,0x5dd0,0x022a,
    (s16)0xff56,0x1f0b,0x5ef5,0x029f, (s16)0xff64,0x1d6c,0x6007,0x031c,
    (s16)0xff71,0x1bd7,0x6106,0x03a4, (s16)0xff7e,0x1a4c,0x61f3,0x0435,
    (s16)0xff8a,0x18cb,0x62cb,0x04d1, (s16)0xff96,0x1756,0x638f,0x0577,
    (s16)0xffa1,0x15eb,0x643f,0x0628, (s16)0xffac,0x148c,0x64d9,0x06e4,
    (s16)0xffb6,0x1338,0x655e,0x07ab, (s16)0xffbf,0x11f0,0x65cd,0x087d,
    (s16)0xffc8,0x10b4,0x6626,0x095a, (s16)0xffd0,0x0f83,0x6669,0x0a44,
    (s16)0xffd8,0x0e5f,0x6696,0x0b39, (s16)0xffdf,0x0d46,0x66ad,0x0c39
};

static inline s32 clamp16(s32 x) { return x < -32768 ? -32768 : (x > 32767 ? 32767 : x); }

/* ------------------------------------------------------------------ */
/* Tablas del WMD (las arma N64_DriverInit en n64cmd.c)                  */
/* ------------------------------------------------------------------ */
extern uintptr_t     g_wddloc;
extern ALRawLoop2   *samplesrawloopbase;
extern ALADPCMloop2 *samplescmploopbase;
extern ALADPCMBook2 *samplescmphdrbase;
extern loopinfo_header *samplesinfochunk;

static u32 s_wdd_size = 0;
void n64synth_set_wdd_size(u32 size) { s_wdd_size = size; }

static inline s16 rd16(const u8 *p) { return (s16)((p[0] << 8) | p[1]); }

/* ------------------------------------------------------------------ */
/* Voces                                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    int    used;             /* asignada por el WESS */
    int    playing;
    /* onda */
    const u8 *data;
    u32    nsamples;         /* total de muestras de la onda */
    int    adpcm;
    const s16 *book;         /* codebook (order 2) */
    int    npred;
    u32    lstart, lend;     /* loop, en muestras */
    s32    lcount;           /* -1 = infinito, 0 = sin loop */
    const s16 *lstate;       /* estado del decodificador en el loop */
    /* decodificador */
    u32    pos;              /* proxima muestra */
    s32    curframe;
    s16    frame[16];
    s16    last[16];
    int    ended;
    /* resampler */
    s16    hist[4];
    u32    accu;
    u32    pitch;            /* Q16 */
    /* envolvente */
    s32    volume;           /* (v*v)>>15 */
    int    pan;
    s32    dry, wet;
    double cvolL, cvolR, tgtL, tgtR, rateL, rateR;
    s32    segEnd, delta;
} svoice_t;

static svoice_t s_v[MAX_VOICES];

/* DEBUG (temporal): cada voz que arranca deja una linea en el log */
void ps3_logf(const char *fmt, ...);
#define SYN_TRACE(...) do { } while (0)   /* ps3_logf(__VA_ARGS__) para depurar */
static int      s_nvoices = 24;

/* WESS -> voz fisica */
static svoice_t *sv(ALVoice *v)
{
    if (!v || v->index < 0 || v->index >= MAX_VOICES) return NULL;
    return &s_v[v->index];
}

static s32 time_to_samples(ALMicroTime t)
{
    s64 s;
    if (t <= 0) return 0;
    s = ((s64)t * SYN_RATE) / 1000000;
    return (s32)(s & ~0xf);
}

static void env_retarget(svoice_t *v, s32 samples)
{
    v->tgtL = (double)((v->volume * eqpower[v->pan]) >> 15);
    v->tgtR = (double)((v->volume * eqpower[127 - v->pan]) >> 15);
    if (samples <= 0) {
        v->cvolL = v->tgtL; v->cvolR = v->tgtR;
        v->rateL = v->rateR = 1.0;
        v->segEnd = v->delta = 0;
        return;
    }
    if (v->cvolL < 1.0) v->cvolL = 1.0;
    if (v->cvolR < 1.0) v->cvolR = 1.0;
    /* rampa exponencial (como _getRate de libultra) */
    v->rateL = pow((v->tgtL < 1.0 ? 1.0 : v->tgtL) / v->cvolL, 1.0 / samples);
    v->rateR = pow((v->tgtR < 1.0 ? 1.0 : v->tgtR) / v->cvolR, 1.0 / samples);
    v->segEnd = samples;
    v->delta = 0;
}

/* ------------------------------------------------------------------ */
/* Decodificacion                                                       */
/* ------------------------------------------------------------------ */
static void adpcm_frame(svoice_t *v, u32 f)
{
    const u8 *p = v->data + f * 9;
    int scale = p[0] >> 4;
    int pred = p[0] & 0xf;
    int rshift = scale < 12 ? 12 - scale : 0;
    s16 in[16], out[16];
    const s16 *b1, *b2;
    int i, k, half;

    if (pred >= v->npred) pred = 0;
    b1 = v->book + pred * 16;
    b2 = b1 + 8;

    for (i = 0; i < 8; i++) {
        u8 byte = p[1 + i];
        in[2 * i]     = (s16)((s16)((byte & 0xf0) << 8) >> rshift);
        in[2 * i + 1] = (s16)((s16)((byte & 0x0f) << 12) >> rshift);
    }
    for (half = 0; half < 2; half++) {
        const s16 *src = in + half * 8;
        s16 l1 = half ? out[6] : v->last[14];
        s16 l2 = half ? out[7] : v->last[15];
        for (i = 0; i < 8; i++) {
            s32 accu = (s32)src[i] << 11;
            accu += b1[i] * l1 + b2[i] * l2;
            for (k = 0; k < i; k++)
                accu += b2[k] * src[i - 1 - k];
            out[half * 8 + i] = (s16)clamp16(accu >> 11);
        }
    }
    memcpy(v->frame, out, sizeof(out));
    memcpy(v->last, out, sizeof(out));
    v->curframe = (s32)f;
}

static s16 next_sample(svoice_t *v)
{
    s16 s;
    if (v->ended) return 0;
    if (v->lcount != 0 && v->pos == v->lend) {
        if (v->lcount > 0) v->lcount--;
        v->pos = v->lstart;
        if (v->adpcm) {
            /* como el A_LOOP del microcodigo: el estado guardado en el loop
             * hace de "frame anterior" */
            if (v->lstate) memcpy(v->last, v->lstate, sizeof(v->last));
            adpcm_frame(v, v->pos >> 4);
        }
    }
    if (v->pos >= v->nsamples) { v->ended = 1; return 0; }
    if (v->adpcm) {
        if ((s32)(v->pos >> 4) != v->curframe)
            adpcm_frame(v, v->pos >> 4);
        s = v->frame[v->pos & 15];
    } else {
        s = rd16(v->data + v->pos * 2);
    }
    v->pos++;
    return s;
}

/* ------------------------------------------------------------------ */
/* API alSyn* (la que usa n64cmd.c)                                     */
/* ------------------------------------------------------------------ */
s32 alSynAllocVoice(ALSynth *s, ALVoice *voice, ALVoiceConfig *vc)
{
    svoice_t *v = sv(voice);
    (void)s;
    if (!v) return 0;
    v->used = 1;
    voice->priority = vc ? vc->priority : 0;
    return 1;
}

void alSynFreeVoice(ALSynth *s, ALVoice *voice)
{
    svoice_t *v = sv(voice);
    (void)s;
    if (!v) return;
    v->used = 0;
    v->playing = 0;
}

void alSynStartVoiceParams(ALSynth *s, ALVoice *voice, ALWaveTable *w, f32 pitch,
                           s16 vol, ALPan pan, u8 fxmix, ALMicroTime t)
{
    svoice_t *v = sv(voice);
    ALWaveTable2 *wt = (ALWaveTable2 *)w;
    (void)s;
    if (!v || !wt) return;

    memset(v, 0, sizeof(*v));
    v->used = 1;
    SYN_TRACE("[syn] v%d base=%x len=%d tipo=%d loop=%x book=%x pitch=%d vol=%d pan=%d fx=%d t=%d",
              voice->index, (unsigned)wt->base, (int)wt->len, (int)wt->type, (unsigned)wt->loop,
              (unsigned)wt->book, (int)(pitch * 1000.0f), (int)vol, (int)pan, (int)fxmix, (int)t);
    if (s_wdd_size && (wt->base >= s_wdd_size || wt->base + (u32)wt->len > s_wdd_size))
        return;                                  /* fuera del WDD: no suena */

    v->data = (const u8 *)(g_wddloc + wt->base);
    v->adpcm = (wt->type == AL_ADPCM_WAVE);
    v->curframe = -1;
    if (v->adpcm) {
        const ALADPCMBook2 *bk = &samplescmphdrbase[wt->book];
        v->book = bk->book;
        v->npred = bk->npredictors > 8 ? 8 : bk->npredictors;
        v->nsamples = ((u32)wt->len / 9) * 16;
        if (wt->loop != 0xFFFFFFFFu && samplesinfochunk && wt->loop < samplesinfochunk->adpcmcount) {
            const ALADPCMloop2 *lp = &samplescmploopbase[wt->loop];
            v->lstart = lp->start; v->lend = lp->end;
            v->lcount = (s32)lp->count;
            v->lstate = lp->state;
        }
    } else {
        v->nsamples = (u32)wt->len / 2;
        if (wt->loop != 0xFFFFFFFFu && samplesinfochunk && wt->loop < samplesinfochunk->rawcount) {
            const ALRawLoop2 *lp = &samplesrawloopbase[wt->loop];
            v->lstart = lp->start; v->lend = lp->end;
            v->lcount = (s32)lp->count;
        }
    }
    if (v->lend > v->nsamples || v->lstart >= v->lend) v->lcount = 0;

    if (pitch > 1.99996f) pitch = 1.99996f;
    v->pitch = (u32)(pitch * 32768.0f) << 1;     /* Q15 del microcodigo -> Q16 */

    v->volume = ((s32)vol * (s32)vol) >> 15;
    v->pan = pan > 127 ? 127 : pan;
    if (fxmix > 127) fxmix = 127;
    v->dry = eqpower[fxmix];
    v->wet = eqpower[127 - fxmix];
    v->cvolL = v->cvolR = 1.0;
    env_retarget(v, time_to_samples(t));
    v->playing = 1;
}

void alSynStopVoice(ALSynth *s, ALVoice *voice)
{
    svoice_t *v = sv(voice);
    (void)s;
    if (v) v->playing = 0;
}

void alSynSetPitch(ALSynth *s, ALVoice *voice, f32 ratio)
{
    svoice_t *v = sv(voice);
    (void)s;
    if (!v) return;
    if (ratio > 1.99996f) ratio = 1.99996f;
    if (ratio < 0.0f) ratio = 0.0f;
    v->pitch = (u32)(ratio * 32768.0f) << 1;
}

void alSynSetVol(ALSynth *s, ALVoice *voice, s16 vol, ALMicroTime t)
{
    svoice_t *v = sv(voice);
    (void)s;
    if (!v) return;
    v->volume = ((s32)vol * (s32)vol) >> 15;
    env_retarget(v, time_to_samples(t));
}

void alSynSetPan(ALSynth *s, ALVoice *voice, ALPan pan)
{
    svoice_t *v = sv(voice);
    s32 left;
    (void)s;
    if (!v) return;
    v->pan = pan > 127 ? 127 : pan;
    left = v->segEnd - v->delta;
    env_retarget(v, left > 0 ? left : 0);
}

void alSynSetPriority(ALSynth *s, ALVoice *voice, s16 priority)
{
    (void)s;
    if (voice) voice->priority = priority;
}

/* ------------------------------------------------------------------ */
/* Player (el tick de 120 Hz del WESS)                                  */
/* ------------------------------------------------------------------ */
static ALPlayer *s_player = NULL;

void alSynAddPlayer(ALSynth *s, ALPlayer *client)
{
    (void)s;
    s_player = client;
}

/* ------------------------------------------------------------------ */
/* Reverb BIGROOM (reverb.c + drvrnew.c de libultra)                     */
/* ------------------------------------------------------------------ */
#define MS *(((s32)((f32)44.1)) & ~0x7)      /* sic: 40 muestras por "ms" */

typedef struct {
    s32 input, output;
    s16 fbcoef, ffcoef, gain;
    int lp;                  /* filtro pasa bajos */
    s16 fgain, fccoef[16];
    s16 l1, l2;              /* estado del filtro */
    int first;
} fxsec_t;

static s16     s_fxbuf[100 MS];
static s32     s_fxlen = 100 MS;
static s32     s_fxin = 0;               /* posicion de escritura (r->input) */
static fxsec_t s_fx[4];
static int     s_fxcount = 0;

static void init_lp(fxsec_t *d, s16 fcp)
{
    s32 i, temp = fcp * 16384;
    s16 fc = (s16)(temp >> 15);
    double ffc, fcoef;
    d->fgain = 16384 - fc;
    d->first = 1;
    for (i = 0; i < 8; i++) d->fccoef[i] = 0;
    d->fccoef[i++] = fc;
    fcoef = ffc = (double)fc / 16384;
    for (; i < 16; i++) {
        fcoef *= ffc;
        d->fccoef[i] = (s16)(fcoef * 16384);
    }
    d->l1 = d->l2 = 0;
}

static void fx_init(void)
{
    static const s32 bigroom[34] = {
        4, 100 MS,
        0,      66 MS, 9830, -9830,      0, 0, 0, 0,
        22 MS,  54 MS, 3276, -3276, 0x3fff, 0, 0, 0,
        66 MS,  91 MS, 3276, -3276, 0x3fff, 0, 0, 0,
        0,      94 MS, 8000,     0,      0, 0, 0, 0x5000
    };
    int i, j = 2;
    s_fxcount = bigroom[0];
    s_fxlen = bigroom[1];
    memset(s_fxbuf, 0, sizeof(s_fxbuf));
    s_fxin = 0;
    for (i = 0; i < s_fxcount; i++) {
        fxsec_t *d = &s_fx[i];
        memset(d, 0, sizeof(*d));
        d->input  = bigroom[j++];
        d->output = bigroom[j++];
        d->fbcoef = (s16)bigroom[j++];
        d->ffcoef = (s16)bigroom[j++];
        d->gain   = (s16)bigroom[j++];
        j += 2;                                   /* chorus: no en BIGROOM */
        if (bigroom[j]) { d->lp = 1; init_lp(d, (s16)bigroom[j]); }
        j++;
    }
}

static inline s32 fxidx(s32 i)
{
    i %= s_fxlen;
    return i < 0 ? i + s_fxlen : i;
}
static void fx_load(s32 at, s16 *dst, int n) { int k; for (k = 0; k < n; k++) dst[k] = s_fxbuf[fxidx(at + k)]; }
static void fx_save(s32 at, const s16 *src, int n) { int k; for (k = 0; k < n; k++) s_fxbuf[fxidx(at + k)] = src[k]; }

static void mix_into(s16 *dst, const s16 *src, int n, s16 gain)
{
    int k;
    for (k = 0; k < n; k++) dst[k] = (s16)clamp16(dst[k] + ((src[k] * gain) >> 15));
}

static void polef(fxsec_t *d, s16 *buf, int n)
{
    s16 h2[8], h2b[8];
    const s16 *h1 = d->fccoef;
    int i, k, f;
    s16 l1 = d->first ? 0 : d->l1, l2 = d->first ? 0 : d->l2;
    d->first = 0;
    for (i = 0; i < 8; i++) {
        h2b[i] = d->fccoef[8 + i];
        h2[i] = (s16)(((s32)h2b[i] * d->fgain) >> 14);
    }
    for (f = 0; f + 8 <= n; f += 8) {
        s16 frame[8], out[8];
        memcpy(frame, buf + f, sizeof(frame));
        for (i = 0; i < 8; i++) {
            s32 accu = frame[i] * d->fgain;
            accu += h1[i] * l1 + h2b[i] * l2;
            for (k = 0; k < i; k++) accu += h2[k] * frame[i - 1 - k];
            out[i] = (s16)clamp16(accu >> 14);
        }
        memcpy(buf + f, out, sizeof(out));
        l1 = out[6]; l2 = out[7];
    }
    d->l1 = l1; d->l2 = l2;
}

/* auxl/auxr: envio al reverb; out: salida mono del reverb */
static void fx_process(s16 *auxl, s16 *auxr, s16 *out, int n)
{
    s16 b1[CHUNK], b2[CHUNK];
    int i;

    /* .707 L + .707 R, en el buffer de la izquierda (como el original) */
    mix_into(auxl, auxl, n, (s16)0xda83);
    mix_into(auxl, auxr, n, (s16)0x5a82);
    fx_save(s_fxin, auxl, n);
    memset(out, 0, n * sizeof(s16));

    for (i = 0; i < s_fxcount; i++) {
        fxsec_t *d = &s_fx[i];
        s32 in_at = s_fxin - d->input, out_at = s_fxin - d->output;
        fx_load(in_at, b1, n);
        fx_load(out_at, b2, n);
        if (d->ffcoef) {
            mix_into(b2, b1, n, d->ffcoef);
            if (!d->lp) fx_save(out_at, b2, n);
        }
        if (d->fbcoef) {
            mix_into(b1, b2, n, d->fbcoef);
            fx_save(in_at, b1, n);
        }
        if (d->lp) polef(d, b2, n);
        fx_save(out_at, b2, n);
        if (d->gain) mix_into(out, b2, n, d->gain);
    }
    s_fxin = fxidx(s_fxin + n);
}

/* ------------------------------------------------------------------ */
/* Render                                                               */
/* ------------------------------------------------------------------ */
void n64synth_init(int nvoices, ALVoice *voices)
{
    int i;
    memset(s_v, 0, sizeof(s_v));
    s_nvoices = nvoices > MAX_VOICES ? MAX_VOICES : nvoices;
    for (i = 0; i < nvoices; i++) {
        voices[i].index = i < MAX_VOICES ? i : -1;
        voices[i].priority = 0;
    }
    fx_init();
    s_player = NULL;
}

/* Una tanda de CHUNK muestras a 22050 Hz: primero el tick del WESS, despues
 * las voces. out = CHUNK * 2 (L,R). */
static void render_chunk(s16 *out)
{
    s32 ml[CHUNK], mr[CHUNK];
    s32 al[CHUNK], ar[CHUNK];
    s16 auxl[CHUNK], auxr[CHUNK], fx[CHUNK];
    int i, n;

    if (s_player && s_player->handler)
        s_player->handler(s_player);

    memset(ml, 0, sizeof(ml)); memset(mr, 0, sizeof(mr));
    memset(al, 0, sizeof(al)); memset(ar, 0, sizeof(ar));

    for (i = 0; i < s_nvoices; i++) {
        svoice_t *v = &s_v[i];
        if (!v->playing) continue;
        for (n = 0; n < CHUNK; n++) {
            const s16 *lut = RESAMPLE_LUT + ((v->accu & 0xfc00) >> 8);
            s32 smp = clamp16((v->hist[0] * lut[0] + v->hist[1] * lut[1] +
                               v->hist[2] * lut[2] + v->hist[3] * lut[3]) >> 15);
            u32 adv;
            s32 lv, rv, gl, gr, wl, wr;

            v->accu += v->pitch;
            adv = v->accu >> 16;
            v->accu &= 0xffff;
            while (adv--) {
                v->hist[0] = v->hist[1]; v->hist[1] = v->hist[2]; v->hist[2] = v->hist[3];
                v->hist[3] = next_sample(v);
            }

            /* envolvente */
            if (v->delta < v->segEnd) {
                v->cvolL *= v->rateL;
                v->cvolR *= v->rateR;
                if (++v->delta >= v->segEnd) { v->cvolL = v->tgtL; v->cvolR = v->tgtR; }
            }
            lv = (s32)v->cvolL; rv = (s32)v->cvolR;
            gl = clamp16((lv * v->dry + 0x4000) >> 15);
            gr = clamp16((rv * v->dry + 0x4000) >> 15);
            wl = clamp16((lv * v->wet + 0x4000) >> 15);
            wr = clamp16((rv * v->wet + 0x4000) >> 15);
            ml[n] += (smp * gl) >> 15;
            mr[n] += (smp * gr) >> 15;
            al[n] += (smp * wl) >> 15;
            ar[n] += (smp * wr) >> 15;
        }
        if (v->ended && v->delta >= v->segEnd && v->hist[3] == 0 && v->hist[2] == 0)
            ; /* sigue "sonando" en silencio hasta que el WESS la corte */
    }

    for (n = 0; n < CHUNK; n++) { auxl[n] = (s16)clamp16(al[n]); auxr[n] = (s16)clamp16(ar[n]); }
    fx_process(auxl, auxr, fx, CHUNK);
    for (n = 0; n < CHUNK; n++) {
        out[2 * n]     = (s16)clamp16(clamp16(ml[n]) + fx[n]);
        out[2 * n + 1] = (s16)clamp16(clamp16(mr[n]) + fx[n]);
    }
}

/* ------------------------------------------------------------------ */
/* 22050 -> 48000, interpolacion lineal                                 */
/* ------------------------------------------------------------------ */
static s16  s_chunk[CHUNK * 2];
static int  s_chunk_pos = CHUNK;       /* muestras ya consumidas del chunk */
static s16  s_prevL = 0, s_prevR = 0, s_curL = 0, s_curR = 0;
static u32  s_frac = 0;                /* Q16 */
static const u32 s_step = (u32)(((u64)SYN_RATE << 16) / 48000);

static void pull_src(void)
{
    if (s_chunk_pos >= CHUNK) { render_chunk(s_chunk); s_chunk_pos = 0; }
    s_prevL = s_curL; s_prevR = s_curR;
    s_curL = s_chunk[2 * s_chunk_pos];
    s_curR = s_chunk[2 * s_chunk_pos + 1];
    s_chunk_pos++;
}

/* Suma (no pisa) frames*2 floats en out, escalado por gain. */
void n64synth_render48k(float *out, int frames, float gain)
{
    int f;
    float k = gain / 32768.0f;
    for (f = 0; f < frames; f++) {
        float fr = (float)s_frac * (1.0f / 65536.0f);
        out[2 * f]     += ((float)s_prevL + (float)(s_curL - s_prevL) * fr) * k;
        out[2 * f + 1] += ((float)s_prevR + (float)(s_curR - s_prevR) * fr) * k;
        s_frac += s_step;
        while (s_frac >= 65536) { s_frac -= 65536; pull_src(); }
    }
}

/* Para el harness: chunks crudos a 22050 */
void n64synth_render22k(s16 *out, int chunks)
{
    int i;
    for (i = 0; i < chunks; i++) render_chunk(out + i * CHUNK * 2);
}
int n64synth_chunk_size(void) { return CHUNK; }

int n64synth_active_voices(void)
{
    int i, n = 0;
    for (i = 0; i < s_nvoices; i++) if (s_v[i].playing) n++;
    return n;
}

/* ps3_audio.c -- audio del port: la API de KOS que usa el motor
 * (snd_sfx_* de s_sound.c / p_pspr.c y wav_* de sndwav.h) sobre un mixer
 * propio que sale por el audioPort de la PS3.
 *
 *   Efectos: WAV con ADPCM Yamaha (formato 0x20, el del AICA del Dreamcast),
 *            mono 22050 Hz. Se decodifican enteros a PCM16 al cargarlos.
 *   Musica:  mus/<nombre>.adpcm, ADPCM Yamaha estereo 44100 Hz SIN header
 *            (cada byte: nibble bajo = izquierda, alto = derecha). Se lee
 *            del disco en un hilo aparte y se decodifica al mezclar.
 *   Salida:  48000 Hz, 2 canales float, bloques de 256 muestras.
 *
 * ENDIANNESS: todo lo que viene de archivo (header RIFF, PCM16 si lo hubiera)
 * se lee byte por byte en little-endian. El ADPCM son nibbles: no le afecta.
 *
 * En x86 (tools/hosttest) no hay audioPort: se compila el mixer igual y
 * PS3_Audio_MixForTest() deja probarlo escribiendo un WAV. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include <kos.h>
#include <dc/sound/sound.h>
#include "sndwav.h"
#include "ps3_log.h"

#ifdef __PPU__
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/event_queue.h>
#include <audio/audio.h>
#else
#include <pthread.h>
#endif

#define MIX_RATE     48000
#define MIX_BLOCK    256            /* AUDIO_BLOCK_SAMPLES */
#define MAX_SFX      160
#define NUM_VOICES   32
#define RING_SIZE    (256 * 1024)   /* bytes de ADPCM: ~3 s de musica */
#define READ_CHUNK   (32 * 1024)

/* ------------------------------------------------------------------ */
/* Mutex portable                                                      */
/* ------------------------------------------------------------------ */
#ifdef __PPU__
typedef sys_mutex_t amutex_t;
static void am_init(amutex_t *m)
{
    sys_mutex_attr_t a;
    memset(&a, 0, sizeof(a));
    a.attr_protocol  = SYS_MUTEX_PROTOCOL_PRIO;
    a.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    a.attr_pshared   = SYS_MUTEX_ATTR_NOT_PSHARED;
    a.attr_adaptive  = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
    strcpy(a.name, "d64snd");
    sysMutexCreate(m, &a);
}
#define am_lock(m)   sysMutexLock(*(m), 0)
#define am_unlock(m) sysMutexUnlock(*(m))
#else
typedef pthread_mutex_t amutex_t;
static void am_init(amutex_t *m) { pthread_mutex_init(m, NULL); }
#define am_lock(m)   pthread_mutex_lock(m)
#define am_unlock(m) pthread_mutex_unlock(m)
#endif

static amutex_t s_mix_lock;     /* voces + estado de la musica */
static amutex_t s_mus_lock;     /* archivo de musica (hilo lector) */
static int      s_inited = 0;

/* ------------------------------------------------------------------ */
/* ADPCM Yamaha (el del AICA; ffmpeg lo llama adpcm_yamaha)             */
/* ------------------------------------------------------------------ */
typedef struct { int pred, step; } ystate_t;

static const int y_diff[16]  = { 1, 3, 5, 7, 9, 11, 13, 15,
                                -1,-3,-5,-7,-9,-11,-13,-15 };
static const int y_scale[16] = { 230, 230, 230, 230, 307, 409, 512, 614,
                                 230, 230, 230, 230, 307, 409, 512, 614 };

static inline void y_reset(ystate_t *s) { s->pred = 0; s->step = 127; }

static inline int y_decode(ystate_t *s, int nib)
{
    s->pred += (s->step * y_diff[nib]) / 8;
    if (s->pred > 32767) s->pred = 32767;
    else if (s->pred < -32768) s->pred = -32768;
    s->step = (s->step * y_scale[nib]) >> 8;
    if (s->step < 127) s->step = 127;
    else if (s->step > 24576) s->step = 24576;
    return s->pred;
}

/* ------------------------------------------------------------------ */
/* Efectos                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    int16_t  *pcm;
    uint32_t  len;          /* muestras */
    uint32_t  rate;
} sfx_t;

static sfx_t s_sfx[MAX_SFX + 1];   /* handle 0 = invalido */
static int   s_nsfx = 0;

static uint32_t rd_le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd_le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* Parsea un WAV en memoria. Devuelve 0 si no se entiende. */
static int parse_wav(const uint8_t *b, uint32_t size, uint32_t *fmt, uint32_t *ch,
                     uint32_t *rate, const uint8_t **data, uint32_t *dlen)
{
    uint32_t off = 12;
    *fmt = 0; *data = NULL;
    if (size < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return 0;
    while (off + 8 <= size) {
        uint32_t csz = rd_le32(b + off + 4);
        const uint8_t *c = b + off + 8;
        if (!memcmp(b + off, "fmt ", 4) && csz >= 16) {
            *fmt  = rd_le16(c);
            *ch   = rd_le16(c + 2);
            *rate = rd_le32(c + 4);
        } else if (!memcmp(b + off, "data", 4)) {
            *data = c;
            *dlen = csz;
            if (off + 8 + *dlen > size) *dlen = size - off - 8;
        }
        off += 8 + ((csz + 1) & ~1u);
    }
    return *fmt && *data;
}

/* La firma de KOS no trae el tamano: el header RIFF lo tiene. */
sfxhnd_t snd_sfx_load_buf(char *buf)
{
    const uint8_t *b = (const uint8_t *)buf, *data;
    uint32_t size, fmt, ch = 1, rate = 22050, dlen = 0, i, n;
    sfx_t *s;

    if (!buf || s_nsfx >= MAX_SFX) return SFXHND_INVALID;
    size = rd_le32(b + 4) + 8;
    if (!parse_wav(b, size, &fmt, &ch, &rate, &data, &dlen)) {
        ps3_log("[snd] WAV no reconocido");
        return SFXHND_INVALID;
    }

    s = &s_sfx[s_nsfx + 1];
    /* KOS/wadtool marcan el ADPCM del AICA como 20 (0x14); ffmpeg usa 0x20. */
    if ((fmt == 0x14 || fmt == 0x20) && ch == 1) { /* ADPCM Yamaha mono */
        ystate_t st;
        n = dlen * 2;
        s->pcm = (int16_t *)malloc(n * sizeof(int16_t));
        if (!s->pcm) return SFXHND_INVALID;
        y_reset(&st);
        for (i = 0; i < dlen; i++) {
            s->pcm[2 * i]     = (int16_t)y_decode(&st, data[i] & 15);
            s->pcm[2 * i + 1] = (int16_t)y_decode(&st, data[i] >> 4);
        }
    } else if (fmt == 1 && ch == 1) {              /* PCM16 LE mono */
        n = dlen / 2;
        s->pcm = (int16_t *)malloc(n * sizeof(int16_t));
        if (!s->pcm) return SFXHND_INVALID;
        for (i = 0; i < n; i++)
            s->pcm[i] = (int16_t)rd_le16(data + 2 * i);
    } else {
        ps3_logf("[snd] formato WAV %u con %u canales no soportado", (unsigned)fmt, (unsigned)ch);
        return SFXHND_INVALID;
    }
    s->len  = n;
    s->rate = rate ? rate : 22050;
    s_nsfx++;
    return (sfxhnd_t)s_nsfx;
}

sfxhnd_t snd_sfx_load(const char *fn)
{
    void *buf = NULL;
    sfxhnd_t h;
    if (fs_load(fn, &buf) <= 0 || !buf) return SFXHND_INVALID;
    h = snd_sfx_load_buf((char *)buf);
    free(buf);
    return h;
}

void snd_sfx_unload(sfxhnd_t idx) { (void)idx; /* se cargan una sola vez */ }

/* ------------------------------------------------------------------ */
/* Voces                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    int       active;
    int       reserved;     /* snd_sfx_chn_alloc: fuera del reparto automatico */
    const sfx_t *s;
    uint64_t  pos;          /* 32.32 en muestras de la fuente */
    uint64_t  step;
    float     gl, gr;
    int       loop;
    uint32_t  loopstart;
    uint32_t  age;
    int       paused;       /* congelada por S_PauseSound */
} voice_t;

static voice_t  s_voice[NUM_VOICES];
static uint32_t s_age = 0;

static void voice_gains(voice_t *v, int vol, int pan)
{
    /* KOS: vol 0..255, pan 0 (izq) .. 128 (centro) .. 255 (der). El motor
     * usa como maximo 124 (el 127 de la N64), asi que 128 = volumen pleno. */
    float g = (float)vol / 128.0f;
    if (g < 0) g = 0;
    if (g > 2.0f) g = 2.0f;
    if (pan < 0) pan = 0;
    if (pan > 255) pan = 255;
    v->gl = g * ((pan <= 128) ? 1.0f : (float)(255 - pan) / 127.0f);
    v->gr = g * ((pan >= 128) ? 1.0f : (float)pan / 128.0f);
}

static int start_voice(int chn, sfxhnd_t idx, int vol, int pan, int loop, uint32_t loopstart)
{
    voice_t *v;
    if (idx == SFXHND_INVALID || (int)idx > s_nsfx || !s_inited) return -1;

    am_lock(&s_mix_lock);
    if (chn < 0) {
        int i, oldest = -1;
        for (i = 0; i < NUM_VOICES; i++) {
            if (s_voice[i].reserved) continue;
            if (!s_voice[i].active) { chn = i; break; }
            if (oldest < 0 || s_voice[i].age < s_voice[oldest].age) oldest = i;
        }
        if (chn < 0) chn = oldest;           /* todas ocupadas: se roba la mas vieja */
    }
    if (chn < 0 || chn >= NUM_VOICES) { am_unlock(&s_mix_lock); return -1; }

    v = &s_voice[chn];
    v->s    = &s_sfx[idx];
    v->pos  = 0;
    v->step = ((uint64_t)v->s->rate << 32) / MIX_RATE;
    v->loop = loop;
    v->loopstart = (loopstart < v->s->len) ? loopstart : 0;
    v->age  = ++s_age;
    v->paused = 0;
    voice_gains(v, vol, pan);
    v->active = 1;
    am_unlock(&s_mix_lock);
    return chn;
}

int snd_sfx_play(sfxhnd_t idx, int vol, int pan)            { return start_voice(-1, idx, vol, pan, 0, 0); }
int snd_sfx_play_chn(int chn, sfxhnd_t idx, int vol, int pan) { return start_voice(chn, idx, vol, pan, 0, 0); }

int snd_sfx_play_ex(sfx_play_data_t *d)
{
    if (!d) return -1;
    return start_voice(d->chn, d->idx, d->vol, d->pan, d->loop, d->loopstart);
}

void snd_sfx_stop(int chn)
{
    if (chn < 0 || chn >= NUM_VOICES || !s_inited) return;
    am_lock(&s_mix_lock);
    s_voice[chn].active = 0;
    am_unlock(&s_mix_lock);
}

void snd_sfx_stop_all(void)
{
    int i;
    if (!s_inited) return;
    am_lock(&s_mix_lock);
    for (i = 0; i < NUM_VOICES; i++)
        if (!s_voice[i].reserved) s_voice[i].active = 0;
    am_unlock(&s_mix_lock);
}

int snd_sfx_chn_alloc(void)
{
    int i;
    if (!s_inited) return -1;
    am_lock(&s_mix_lock);
    for (i = NUM_VOICES - 1; i >= 0; i--) {
        if (!s_voice[i].reserved && !s_voice[i].active) {
            s_voice[i].reserved = 1;
            am_unlock(&s_mix_lock);
            return i;
        }
    }
    am_unlock(&s_mix_lock);
    return -1;
}

void snd_sfx_chn_free(int chn)
{
    if (chn < 0 || chn >= NUM_VOICES || !s_inited) return;
    am_lock(&s_mix_lock);
    s_voice[chn].reserved = 0;
    s_voice[chn].active = 0;
    am_unlock(&s_mix_lock);
}

/* ------------------------------------------------------------------ */
/* Musica                                                              */
/* ------------------------------------------------------------------ */
static uint8_t           s_ring[RING_SIZE];
static volatile uint32_t s_ring_wr = 0, s_ring_rd = 0;   /* SPSC */
static int      s_mus_fd = -1;
static int      s_mus_loop = 0;
static volatile int s_mus_eof = 0;
static volatile int s_mus_playing = 0;
static volatile int s_mus_prefill = 0;   /* esperando que el ring se llene */
static volatile int s_mus_paused = 0;
#define PREFILL_BYTES (96 * 1024)
static float    s_mus_gain = 1.0f;
static ystate_t s_ml, s_mr;
static int      s_m0l, s_m0r, s_m1l, s_m1r;              /* frames para interpolar */
static uint64_t s_mfrac = 0;
static const uint64_t s_mstep = ((uint64_t)44100 << 32) / MIX_RATE;

static void mus_reset_decoder(void)
{
    y_reset(&s_ml); y_reset(&s_mr);
    s_m0l = s_m0r = s_m1l = s_m1r = 0;
    s_mfrac = 0;
}

int wav_init(void) { return 1; }
void wav_shutdown(void) { wav_destroy(); }

wav_stream_hnd_t wav_create(const char *filename, int loop)
{
    char path[512];
    int fd;
    extern int ps3_fs_open(const char *p, int mode);   /* traduce /pc, /cd, /rd */

    if (!s_inited) return SND_STREAM_INVALID;
    wav_destroy();

    fd = ps3_fs_open(filename, O_RDONLY);
    if (fd < 0) {
        snprintf(path, sizeof(path), "%s", filename);
        ps3_logf("[snd] no se pudo abrir la musica %s", path);
        return SND_STREAM_INVALID;
    }
    am_lock(&s_mus_lock);
    am_lock(&s_mix_lock);
    s_mus_fd = fd;
    s_mus_loop = loop;
    s_mus_eof = 0;
    s_mus_playing = 0;
    s_ring_wr = s_ring_rd = 0;
    mus_reset_decoder();
    am_unlock(&s_mix_lock);
    am_unlock(&s_mus_lock);
    ps3_logf("[snd] musica: %s%s", filename, loop ? " (loop)" : "");
    return 0;
}

void wav_destroy(void)
{
    if (!s_inited) return;
    am_lock(&s_mus_lock);
    am_lock(&s_mix_lock);
    s_mus_playing = 0;
    if (s_mus_fd >= 0) close(s_mus_fd);
    s_mus_fd = -1;
    s_ring_wr = s_ring_rd = 0;
    am_unlock(&s_mix_lock);
    am_unlock(&s_mus_lock);
}

void wav_play(void)
{
    if (s_mus_fd < 0) return;
    if (!s_mus_playing) s_mus_prefill = 1;    /* no arrancar con el ring vacio */
    s_mus_paused = 0;
    s_mus_playing = 1;
}
void wav_pause(void) { s_mus_paused = 1; }
void wav_stop(void)  { s_mus_playing = 0; }
void wav_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;
    s_mus_gain = (float)vol / 255.0f;
}
int wav_is_playing(void) { return s_mus_playing; }

/* Pausa del juego (S_PauseSound / S_ResumeSound): la musica y los efectos
 * que estaban sonando se congelan; los sonidos nuevos (los del menu de
 * pausa) suenan normal. */
void PS3_Audio_Pause(int on)
{
    int i;
    if (!s_inited) return;
    am_lock(&s_mix_lock);
    for (i = 0; i < NUM_VOICES; i++)
        if (s_voice[i].active) s_voice[i].paused = on ? 1 : 0;
    s_mus_paused = on ? 1 : 0;
    am_unlock(&s_mix_lock);
    ps3_logf("[snd] %s", on ? "pausa" : "sigue");
}

/* Hilo lector: mantiene el ring lleno. Llamado en loop. */
static void mus_fill(void)
{
    static uint8_t tmp[READ_CHUNK];
    uint32_t used, room, n, i, wr;
    ssize_t r;

    am_lock(&s_mus_lock);
    if (s_mus_fd < 0 || s_mus_eof) { am_unlock(&s_mus_lock); return; }
    used = s_ring_wr - s_ring_rd;
    room = RING_SIZE - used;
    if (room < READ_CHUNK) { am_unlock(&s_mus_lock); return; }

    r = read(s_mus_fd, tmp, READ_CHUNK);
    if (r <= 0) {
        if (s_mus_loop) {
            lseek(s_mus_fd, 0, SEEK_SET);
            r = read(s_mus_fd, tmp, READ_CHUNK);
        }
        if (r <= 0) { s_mus_eof = 1; am_unlock(&s_mus_lock); return; }
    }
    n = (uint32_t)r;
    wr = s_ring_wr;
    for (i = 0; i < n; i++)
        s_ring[(wr + i) & (RING_SIZE - 1)] = tmp[i];
    __sync_synchronize();
    s_ring_wr = wr + n;
    am_unlock(&s_mus_lock);
}

/* ------------------------------------------------------------------ */
/* Mixer                                                               */
/* ------------------------------------------------------------------ */
static uint32_t s_underruns = 0;

/* Audio del N64 (source/wess): se sintetiza aparte, con su propio lock,
 * y se suma aca. Ganancia: el N64 sale bastante mas bajo que los WAV
 * normalizados de doom64-dc. */
int  PS3_Wess_Ready(void);
void PS3_Wess_Render48k(float *out, int frames, float gain);
#define WESS_GAIN 2.0f
static float s_wess_buf[2 * MIX_BLOCK];

/* limite suave: lineal hasta 0.8, despues se curva hacia 1.0 */
static inline float soft_clip(float x)
{
    float a = x < 0.0f ? -x : x;
    if (a <= 0.8f) return x;
    a = 0.8f + 0.2f * (1.0f - 1.0f / (1.0f + (a - 0.8f) * 5.0f));
    return x < 0.0f ? -a : a;
}

static void mix_block(float *out, int frames)
{
    int f, i;
    float mixl, mixr;
    int wess = PS3_Wess_Ready() && frames <= MIX_BLOCK;

    if (wess) {
        memset(s_wess_buf, 0, sizeof(float) * 2 * frames);
        PS3_Wess_Render48k(s_wess_buf, frames, WESS_GAIN);
    }

    am_lock(&s_mix_lock);
    for (f = 0; f < frames; f++) {
        if (wess) {
            mixl = s_wess_buf[2 * f];
            mixr = s_wess_buf[2 * f + 1];
        } else {
            mixl = mixr = 0.0f;
        }

        /* efectos (interpolacion lineal) */
        for (i = 0; i < NUM_VOICES; i++) {
            voice_t *v = &s_voice[i];
            uint32_t ip;
            float a, b, fr, smp;
            if (!v->active || v->paused) continue;
            ip = (uint32_t)(v->pos >> 32);
            if (ip >= v->s->len) {
                if (v->loop) {
                    v->pos = (uint64_t)v->loopstart << 32;
                    ip = v->loopstart;
                } else {
                    v->active = 0;
                    continue;
                }
            }
            a  = v->s->pcm[ip];
            b  = (ip + 1 < v->s->len) ? v->s->pcm[ip + 1] : a;
            fr = (float)(uint32_t)(v->pos & 0xffffffffu) * (1.0f / 4294967296.0f);
            smp = (a + (b - a) * fr) * (1.0f / 32768.0f);
            mixl += smp * v->gl;
            mixr += smp * v->gr;
            v->pos += v->step;
        }

        /* musica: al arrancar se espera a tener ~1 s en el ring */
        if (s_mus_playing && s_mus_prefill) {
            if (s_ring_wr - s_ring_rd >= PREFILL_BYTES || s_mus_eof)
                s_mus_prefill = 0;
        }
        if (s_mus_playing && !s_mus_prefill && !s_mus_paused) {
            float fr = (float)(uint32_t)(s_mfrac & 0xffffffffu) * (1.0f / 4294967296.0f);
            mixl += ((float)s_m0l + (float)(s_m1l - s_m0l) * fr) * (s_mus_gain / 32768.0f);
            mixr += ((float)s_m0r + (float)(s_m1r - s_m0r) * fr) * (s_mus_gain / 32768.0f);
            s_mfrac += s_mstep;
            while (s_mfrac >= ((uint64_t)1 << 32)) {
                s_mfrac -= ((uint64_t)1 << 32);
                s_m0l = s_m1l; s_m0r = s_m1r;
                if (s_ring_rd != s_ring_wr) {
                    uint8_t byte = s_ring[s_ring_rd & (RING_SIZE - 1)];
                    s_ring_rd++;
                    s_m1l = y_decode(&s_ml, byte & 15);
                    s_m1r = y_decode(&s_mr, byte >> 4);
                } else if (s_mus_eof) {
                    s_mus_playing = 0;       /* termino (sin loop) */
                    s_m1l = s_m1r = 0;
                } else {
                    s_underruns++;
                }
            }
        }

        /* limite suave para no saturar feo */
        mixl = soft_clip(mixl * 0.8f);
        mixr = soft_clip(mixr * 0.8f);
        out[2 * f]     = mixl;
        out[2 * f + 1] = mixr;
    }
    am_unlock(&s_mix_lock);
}

/* Para tools/hosttest: mezcla a un buffer. */
void PS3_Audio_MixForTest(float *out, int frames)
{
    while (frames > 0) {
        int n = frames > MIX_BLOCK ? MIX_BLOCK : frames;
        mus_fill();
        mix_block(out, n);
        out += 2 * n;
        frames -= n;
    }
}

/* ------------------------------------------------------------------ */
/* Salida (PS3)                                                        */
/* ------------------------------------------------------------------ */
#ifdef __PPU__
static u32               s_port = 0;
static audioPortConfig   s_cfg;
static sys_event_queue_t s_queue;
static sys_ipc_key_t     s_key;
static sys_ppu_thread_t  s_mix_tid, s_rd_tid;
static volatile int      s_quit = 0;
static int               s_port_ok = 0;

static void mixer_thread(void *arg)
{
    sys_event_t ev;
    (void)arg;
    while (!s_quit) {
        u64 cur;
        u32 blk;
        float *dst;
        if (sysEventQueueReceive(s_queue, &ev, 20 * 1000) != 0)
            continue;                                /* timeout: probar de nuevo */
        cur = *(volatile u64 *)(u64)s_cfg.readIndex;
        blk = (u32)((cur + 1) % s_cfg.numBlocks);
        dst = (float *)(u64)s_cfg.audioDataStart + blk * MIX_BLOCK * s_cfg.channelCount;
        mix_block(dst, MIX_BLOCK);
    }
    sysThreadExit(0);
}

static void reader_thread(void *arg)
{
    (void)arg;
    while (!s_quit) {
        uint32_t before = s_ring_wr;
        mus_fill();
        if (s_ring_wr == before)
            usleep(10 * 1000);                       /* ring lleno o sin musica */
    }
    sysThreadExit(0);
}

int PS3_Audio_Init(void)
{
    audioPortParam p;
    s32 r;

    am_init(&s_mix_lock);
    am_init(&s_mus_lock);
    memset(s_voice, 0, sizeof(s_voice));
    mus_reset_decoder();

    r = audioInit();
    if (r != 0) { ps3_logf("[snd] audioInit fallo (%d)", (int)r); return -1; }

    memset(&p, 0, sizeof(p));
    p.numChannels = AUDIO_PORT_2CH;
    p.numBlocks   = AUDIO_BLOCK_16;
    p.attrib      = 0;
    p.level       = 1.0f;
    r = audioPortOpen(&p, &s_port);
    if (r != 0) { ps3_logf("[snd] audioPortOpen fallo (%d)", (int)r); audioQuit(); return -1; }
    audioGetPortConfig(s_port, &s_cfg);
    audioCreateNotifyEventQueue(&s_queue, &s_key);
    audioSetNotifyEventQueue(s_key);
    sysEventQueueDrain(s_queue);
    memset((void *)(u64)s_cfg.audioDataStart, 0, s_cfg.portSize);
    audioPortStart(s_port);
    s_port_ok = 1;
    s_inited = 1;

    s_quit = 0;
    sysThreadCreate(&s_mix_tid, mixer_thread, NULL, 200, 128 * 1024, THREAD_JOINABLE, "d64mix");
    sysThreadCreate(&s_rd_tid, reader_thread, NULL, 900, 64 * 1024, THREAD_JOINABLE, "d64mus");
    ps3_logf("[snd] audio listo: puerto %u, %u bloques, %u canales, 48000 Hz",
             (unsigned)s_port, (unsigned)s_cfg.numBlocks, (unsigned)s_cfg.channelCount);
    return 0;
}

void PS3_Audio_Shutdown(void)
{
    u64 rv;
    if (!s_port_ok) return;
    s_quit = 1;
    sysThreadJoin(s_mix_tid, &rv);
    sysThreadJoin(s_rd_tid, &rv);
    audioPortStop(s_port);
    audioRemoveNotifyEventQueue(s_key);
    audioPortClose(s_port);
    sysEventQueueDestroy(s_queue, 0);
    audioQuit();
    s_port_ok = 0;
    s_inited = 0;
    ps3_logf("[snd] audio cerrado (%u faltantes de musica)", (unsigned)s_underruns);
}
#else
int PS3_Audio_Init(void)
{
    am_init(&s_mix_lock);
    am_init(&s_mus_lock);
    memset(s_voice, 0, sizeof(s_voice));
    mus_reset_decoder();
    s_inited = 1;
    return 0;
}
void PS3_Audio_Shutdown(void) { s_inited = 0; }
#endif

/* ps3_wess.c -- el WESS (audio original de Doom 64) montado en la PS3.
 *
 * Reemplaza lo que en el N64 hacian audio.c (el administrador de audio con
 * el RSP) y la parte de s_sound.c que inicializa el WESS. Los datos salen de
 * la ROM (ps3_romconv.c los extrae a USRDIR):
 *
 *   doom64.wmd  instrumentos y tabla de muestras   ("SN64")
 *   doom64.wsd  secuencias: efectos y musica        ("SSEQ")
 *   doom64.wdd  las muestras (VADPCM)
 *
 * Todo el WESS corre bajo un lock recursivo: el hilo del juego llama a la
 * API (disparar efectos, musica) y el hilo del mezclador corre el tick del
 * secuenciador y el sintetizador (n64synth.c).
 *
 * Lo unico global de este grupo de objetos es PS3_Wess_* (ver Makefile).
 */
/* headers del sistema ANTES de wess_port.h (que hace #define long int) */
#ifdef __PPU__
#include <sys/mutex.h>
#include <sys/thread.h>
#else
#include <pthread.h>
#endif

#include "wess_port.h"
#include "wessapi.h"
#include "seqload.h"
#include "wessarc.h"

/* del resto del grupo */
void n64synth_init(int nvoices, ALVoice *voices);
void n64synth_render48k(float *out, int frames, float gain);
void n64synth_set_wdd_size(u32 size);
int  n64synth_active_voices(void);
void n64synth_render22k(s16 *out, int chunks);
int  n64synth_chunk_size(void);
void N64_wdd_location(char *wdd_location);
void N64_set_output_rate(u32 rate);
void SSP_SeqpNew(void);
int  wesssys_init(void);
extern int wess_driver_voices;

/* log del port (fuera del grupo) */
void ps3_logf(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* libultra minimo                                                      */
/* ------------------------------------------------------------------ */
static ALGlobals s_globals;
ALGlobals *alGlobals = &s_globals;
ALVoice   *voice = NULL;             /* lo usa n64cmd.c */

void alHeapInit(ALHeap *hp, u8 *base, s32 len)
{
    hp->base = hp->cur = base;
    hp->len = len;
    hp->count = 0;
}

void *alHeapAlloc(ALHeap *hp, s32 num, s32 size)
{
    s32 bytes = (num * size + 15) & ~15;
    u8 *p = hp->cur;
    if (p + bytes > hp->base + hp->len) {
        ps3_logf("[wess] heap de audio agotado (%d bytes)", (int)bytes);
        return NULL;
    }
    hp->cur += bytes;
    hp->count++;
    memset(p, 0, bytes);
    return p;
}

int wess_memfill(void *dst, unsigned char fill, int count)
{
    memset(dst, fill, count);
    return 0;
}

/* La "ROM" son los archivos ya cargados en memoria. */
int wess_rom_copy(char *src, char *dest, int len)
{
    if (len > 0) memcpy(dest, src, len);
    return len;
}

/* ------------------------------------------------------------------ */
/* Lock (el "disable interrupts" del N64)                                */
/*                                                                      */
/* El WESS se vuelve a trabar a si mismo (la API llama a funciones que   */
/* tambien "deshabilitan interrupciones"), asi que el lock tiene que ser */
/* recursivo. La recursion la llevamos nosotros (duenio + profundidad)   */
/* sobre un mutex comun, igual al del mixer: no dependemos de como trata */
/* el LV2 un mutex recursivo (si lo rechaza, el WESS quedaba sin lock y  */
/* el hilo del juego y el del mixer se pisaban).                         */
/* ------------------------------------------------------------------ */
#ifdef __PPU__
static sys_mutex_t s_mtx;
static void mtx_init(void)
{
    sys_mutex_attr_t a;
    s32 r;
    memset(&a, 0, sizeof(a));
    a.attr_protocol  = SYS_MUTEX_PROTOCOL_PRIO;
    a.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    a.attr_pshared   = SYS_MUTEX_ATTR_NOT_PSHARED;
    a.attr_adaptive  = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
    strcpy(a.name, "d64wess");
    r = sysMutexCreate(&s_mtx, &a);
    if (r != 0) ps3_logf("[wess] ERROR sysMutexCreate = 0x%x", (unsigned)r);
}
static void mtx_lock(void)
{
    s32 r = sysMutexLock(s_mtx, 0);
    static int logged = 0;
    if (r != 0 && !logged) { logged = 1; ps3_logf("[wess] ERROR sysMutexLock = 0x%x", (unsigned)r); }
}
static void mtx_unlock(void) { sysMutexUnlock(s_mtx); }
static u64 self_id(void) { sys_ppu_thread_t id = 0; sysThreadGetId(&id); return (u64)id + 1; }
#else
static pthread_mutex_t s_mtx;
static void mtx_init(void)   { pthread_mutex_init(&s_mtx, NULL); }
static void mtx_lock(void)   { pthread_mutex_lock(&s_mtx); }
static void mtx_unlock(void) { pthread_mutex_unlock(&s_mtx); }
static u64 self_id(void)     { return (u64)(uintptr_t)pthread_self() + 1; }
#endif

static volatile u64 s_owner = 0;   /* 0 = libre */
static int s_depth = 0;

static void lock_init(void) { mtx_init(); s_owner = 0; s_depth = 0; }

static void LOCK(void)
{
    u64 me = self_id();
    if (s_owner == me) { s_depth++; return; }
    mtx_lock();
    s_owner = me;
    s_depth = 1;
}

static void UNLOCK(void)
{
    if (--s_depth == 0) {
        s_owner = 0;
        mtx_unlock();
    }
}

unsigned long wesssys_disable_ints(void) { LOCK(); return 0; }
void wesssys_restore_ints(unsigned long state) { (void)state; UNLOCK(); }

/* ------------------------------------------------------------------ */
/* wess_init / wess_exit (audio.c del N64, sin RSP ni DMA)               */
/* ------------------------------------------------------------------ */
static int s_ready = 0;

void wess_init(WessConfig *wessconfig)
{
    N64_wdd_location(wessconfig->wdd_location);
    N64_set_output_rate(wessconfig->outputsamplerate);
    voice = (ALVoice *)alHeapAlloc(wessconfig->heap_ptr, 1, wess_driver_voices * sizeof(ALVoice));
    n64synth_init(wess_driver_voices, voice);
    SSP_SeqpNew();
    wesssys_init();
}

void wess_exit(void) { }

OSTask *wess_work(void) { return NULL; }

/* ------------------------------------------------------------------ */
/* API para el port                                                     */
/* ------------------------------------------------------------------ */
#define AUDIO_HEAP_SIZE (0x44800 * 2)   /* el del N64, x2 por los punteros de 64 bits */
static u8  *s_heap = NULL;
static char *s_wmd, *s_wsd, *s_wdd;

static void wess_err(char *s, int a, int b)
{
    ps3_logf("[wess] ERROR: %s (%d, %d)", s ? s : "?", a, b);
}

/* 0 = ok. Los buffers quedan en uso (no liberarlos). */
int PS3_Wess_Init(char *wmd, char *wsd, char *wdd, unsigned int wdd_size)
{
    WessConfig cfg;
    ALHeap heap;
    int modulesize, seqtblsize, seqsize, loaded;
    char *moduleptr, *seqtblptr, *seqptr;

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_BIG_ENDIAN__)
    /* los datos del N64 se leen tal cual: solo en big-endian (PS3, qemu-ppc64) */
    ps3_logf("[wess] host little-endian: sin audio del N64");
    return -1;
#endif
    lock_init();
    s_wmd = wmd; s_wsd = wsd; s_wdd = wdd;
    if (memcmp(wmd, "SN64", 4) || memcmp(wsd, "SSEQ", 4)) {
        ps3_logf("[wess] WMD/WSD con cabecera invalida");
        return -1;
    }

    s_heap = (u8 *)malloc(AUDIO_HEAP_SIZE);
    if (!s_heap) return -1;
    alHeapInit(&heap, s_heap, AUDIO_HEAP_SIZE);

    wess_set_error_callback(wess_err);

    memset(&cfg, 0, sizeof(cfg));
    cfg.heap_ptr = &heap;
    cfg.outputsamplerate = 22050;
    cfg.maxACMDSize = 1024 * 3;
    cfg.wdd_location = wdd;
    cfg.reverb_id = WESS_REVERB_BIGROOM;
    cfg.audioframerate = 30.0f;
    n64synth_set_wdd_size(wdd_size);

    LOCK();
    wess_init(&cfg);

    modulesize = (wess_size_module(wmd) + 15) & ~15;
    moduleptr = alHeapAlloc(&heap, 1, modulesize);
    if (!moduleptr) { UNLOCK(); return -1; }
    loaded = wess_load_module(wmd, moduleptr, modulesize);
    if (!loaded) { UNLOCK(); ps3_logf("[wess] wess_load_module fallo"); return -1; }

    seqtblsize = (wess_seq_loader_sizeof(wess_get_master_status(), wsd) + 15) & ~15;
    seqtblptr = alHeapAlloc(&heap, 1, seqtblsize);
    if (!seqtblptr) { UNLOCK(); return -1; }
    wess_seq_loader_init(wess_get_master_status(), wsd, NoOpenSeqHandle, seqtblptr, seqtblsize);

    seqsize = wess_seq_range_sizeof(0, wess_seq_loader_count());
    seqptr = alHeapAlloc(&heap, 1, (seqsize + 15) & ~15);
    if (!seqptr) { UNLOCK(); return -1; }
    wess_seq_range_load(0, wess_seq_loader_count(), seqptr);
    UNLOCK();

    ps3_logf("[wess] listo: modulo %d bytes, %d secuencias (%d bytes), heap usado %d de %d",
             modulesize, wess_seq_loader_count(), seqsize,
             (int)(heap.cur - heap.base), AUDIO_HEAP_SIZE);
    s_ready = 1;
    return 0;
}

int PS3_Wess_Ready(void) { return s_ready; }

/* DEBUG (temporal): cada llamada desde el juego deja una linea en el log,
 * con cuantos bloques mezclo el hilo del audio (para saber si seguia vivo).
 * El hilo del mixer NO escribe en el log (el log no es thread-safe). */
static volatile u32 s_mixblocks = 0;
static int s_trace = 0;   /* 1 = cada llamada del juego al log */
#define TRACE(...) do { if (s_trace) ps3_logf(__VA_ARGS__); } while (0)

void PS3_Wess_Render48k(float *out, int frames, float gain)
{
    if (!s_ready) return;
    LOCK();
    n64synth_render48k(out, frames, gain);
    UNLOCK();
    s_mixblocks++;
}

/* harness: chunks crudos de 22050 Hz */
void PS3_Wess_Render22k(short *out, int chunks)
{
    if (!s_ready) return;
    LOCK();
    n64synth_render22k(out, chunks);
    UNLOCK();
}
int PS3_Wess_ChunkSize(void) { return n64synth_chunk_size(); }
int PS3_Wess_ActiveVoices(void) { return n64synth_active_voices(); }

/* Efecto con volumen/paneo/reverb (S_StartSound del N64). type identifica
 * al emisor para poder cortarlo despues (0 = ninguno). */
void PS3_Wess_StartSound(int seq, unsigned int type, int vol, int pan, int reverb)
{
    TriggerPlayAttr attr;
    if (!s_ready) return;
    memset(&attr, 0, sizeof(attr));
    attr.mask = TRIGGER_VOLUME | TRIGGER_PAN | TRIGGER_REVERB;
    attr.volume = (unsigned char)vol;
    attr.pan = (unsigned char)pan;
    attr.reverb = (unsigned char)reverb;
    TRACE("[wess] +%d t=%x v=%d p=%d r=%d (mix %u, voces %d)", seq, type, vol, pan, reverb,
          (unsigned)s_mixblocks, n64synth_active_voices());
    LOCK();
    wess_seq_trigger_type_special(seq, type, &attr);
    UNLOCK();
    TRACE("[wess] ok");
}

void PS3_Wess_Trigger(int seq)          { if (!s_ready) return; TRACE("[wess] musica %d", seq); LOCK(); wess_seq_trigger(seq); UNLOCK(); }
void PS3_Wess_Stop(int seq)             { if (!s_ready) return; TRACE("[wess] stop %d", seq); LOCK(); wess_seq_stop(seq); UNLOCK(); }
void PS3_Wess_StopType(unsigned int t)  { if (!s_ready) return; TRACE("[wess] stoptype %x", t); LOCK(); wess_seq_stoptype(t); UNLOCK(); }
void PS3_Wess_StopAll(void)             { if (!s_ready) return; TRACE("[wess] stopall"); LOCK(); wess_seq_stopall(); UNLOCK(); }
int  PS3_Wess_Status(int seq)
{
    int r;
    if (!s_ready) return 0;
    LOCK(); r = (wess_seq_status(seq) == SEQUENCE_PLAYING); UNLOCK();
    return r;
}
void PS3_Wess_PauseAll(void)
{
    if (!s_ready) return;
    TRACE("[wess] pausa"); LOCK(); wess_seq_pauseall(YesMute, (REMEMBER_MUSIC | REMEMBER_SNDFX)); UNLOCK();
}
void PS3_Wess_ResumeAll(void)
{
    if (!s_ready) return;
    TRACE("[wess] sigue"); LOCK(); wess_seq_restartall(YesVoiceRestart); UNLOCK();
}
/* 0..100, como el menu. El tope es el del s_sound.c del N64 (85 y 110),
 * pero con raiz cuadrada: el sintetizador eleva el volumen al cuadrado, y
 * asi la barra del menu queda lineal (45 = 45% de amplitud). */
static int vol_map(int v, int top)
{
    if (v <= 0) return 0;
    if (v >= 100) return top;
    return (int)(top * sqrt(v / 100.0) + 0.5);
}
void PS3_Wess_SetSfxVolume(int v) { if (!s_ready) return; LOCK(); wess_master_sfx_vol_set((char)vol_map(v, 85)); UNLOCK(); }
void PS3_Wess_SetMusVolume(int v) { if (!s_ready) return; LOCK(); wess_master_mus_vol_set((char)vol_map(v, 110)); UNLOCK(); }

/* wess_port.h -- lo que el WESS necesita de libultra, reimplementado.
 *
 * El WESS (Williams Entertainment Sound System, el driver de musica y
 * efectos de Doom 64 de N64, reconstruido en DOOM64-RE) le habla al
 * sintetizador de libultra (alSyn*). Aca esa parte la hace n64synth.c: un
 * sintetizador por software que imita al del N64 (ADPCM "VADPCM", el
 * resampler de 4 taps del microcodigo, envolventes exponenciales, paneo de
 * potencia constante y el reverb BIGROOM), y lo mezcla con el resto del
 * audio del port.
 *
 * DOS TRUCOS DE COMPILACION, IMPORTANTES:
 *
 *  1. `long` es de 32 bits en el N64 y de 64 en la PS3. El WESS lee tablas
 *     del archivo (etiquetas de secuencias) con punteros a `unsigned long`.
 *     Para no tocar 7000 lineas, al final de este header se hace
 *     `#define long int` -- DESPUES de incluir todos los headers del
 *     sistema que se usan. Ningun archivo del WESS incluye headers del
 *     sistema despues de este.
 *  2. Los punteros son de 64 bits. Las unicas estructuras que se leen del
 *     archivo y tenian punteros (la tabla de ondas: base/loop/book) se
 *     redefinieron con u32 en wessarc.h; n64synth.c resuelve los indices.
 *
 * Los datos del WMD/WSD/WDD son big-endian, como la PS3: se leen tal cual.
 */
#ifndef PS3_WESS_PORT_H
#define PS3_WESS_PORT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef uint8_t  u8;   typedef int8_t  s8;
typedef uint16_t u16;  typedef int16_t s16;
typedef uint32_t u32;  typedef int32_t s32;
typedef uint64_t u64;  typedef int64_t s64;
typedef float    f32;  typedef double  f64;

#ifndef NULL
#define NULL 0
#endif

typedef s32 ALMicroTime;
typedef u8  ALPan;
typedef s16 ADPCM_STATE[16];

#define AL_ADPCM_WAVE 0
#define AL_RAW16_WAVE 1

typedef struct { int dummy; } OSTask;

/* heap de libaudio: asignador lineal */
typedef struct {
    u8  *base;
    u8  *cur;
    s32  len;
    s32  count;
} ALHeap;
void  alHeapInit(ALHeap *hp, u8 *base, s32 len);
void *alHeapAlloc(ALHeap *hp, s32 num, s32 size);

/* sintetizador */
typedef struct ALVoice_s {
    int   index;          /* voz fisica en n64synth.c, -1 = libre */
    s16   priority;
} ALVoice;

typedef struct {
    s16   priority;
    s16   fxBus;
    u8    unityPitch;
} ALVoiceConfig;

typedef struct ALPlayer_s {
    struct ALPlayer_s *next;
    void              *clientData;
    ALMicroTime      (*handler)(void *);
    s32                callTime;
    s32                samplesLeft;
} ALPlayer;

typedef struct { int dummy; } ALSynth;
typedef struct { ALSynth drvr; } ALGlobals;
extern ALGlobals *alGlobals;

struct ALWaveTable_s;
typedef struct ALWaveTable_s ALWaveTable;

s32  alSynAllocVoice(ALSynth *s, ALVoice *voice, ALVoiceConfig *vc);
void alSynFreeVoice(ALSynth *s, ALVoice *voice);
void alSynStartVoiceParams(ALSynth *s, ALVoice *v, ALWaveTable *w, f32 pitch,
                           s16 vol, ALPan pan, u8 fxmix, ALMicroTime t);
void alSynStopVoice(ALSynth *s, ALVoice *v);
void alSynSetPitch(ALSynth *s, ALVoice *v, f32 ratio);
void alSynSetVol(ALSynth *s, ALVoice *v, s16 vol, ALMicroTime t);
void alSynSetPan(ALSynth *s, ALVoice *v, ALPan pan);
void alSynSetPriority(ALSynth *s, ALVoice *v, s16 priority);
void alSynAddPlayer(ALSynth *s, ALPlayer *client);

/* del final: ver el punto 1 de arriba */
#define long int

#endif

/* compat/dc/sound/sound.h -- audio de KOS. En PS3 va por audioPort (etapa 2). */
#ifndef PS3_COMPAT_DC_SOUND_H
#define PS3_COMPAT_DC_SOUND_H
#include <stdint.h>
#include "stream.h"

typedef uint32_t sfxhnd_t;
#ifndef SFXHND_INVALID
#define SFXHND_INVALID 0
#endif

/* Layout de KOS. El motor usa chn/idx/vol/pan/loop/loopstart/freq. */
typedef struct {
    int      chn;
    sfxhnd_t idx;
    int      vol;
    int      pan;
    int      loop;
    uint32_t loopstart;
    int      freq;
    int      fade;
} sfx_play_data_t;

int      snd_init(void);
void     snd_shutdown(void);
sfxhnd_t snd_sfx_load(const char *fn);
sfxhnd_t snd_sfx_load_buf(char *buf);
void     snd_sfx_unload(sfxhnd_t idx);
int      snd_sfx_play(sfxhnd_t idx, int vol, int pan);
int      snd_sfx_play_chn(int chn, sfxhnd_t idx, int vol, int pan);
int      snd_sfx_play_ex(sfx_play_data_t *data);
void     snd_sfx_stop(int chn);
void     snd_sfx_stop_all(void);
int      snd_sfx_chn_alloc(void);
void     snd_sfx_chn_free(int chn);
#endif

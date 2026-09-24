#ifndef PS3_COMPAT_DC_STREAM_H
#define PS3_COMPAT_DC_STREAM_H
#include <stdint.h>
typedef int snd_stream_hnd_t;
#define SND_STREAM_INVALID (-1)
int  snd_stream_init(void);
void snd_stream_shutdown(void);
snd_stream_hnd_t snd_stream_alloc(void *cb, int bufsize);
void snd_stream_destroy(snd_stream_hnd_t hnd);
void snd_stream_stop(snd_stream_hnd_t hnd);
int  snd_stream_start(snd_stream_hnd_t hnd, uint32_t freq, int st);
int  snd_stream_poll(snd_stream_hnd_t hnd);
void snd_stream_volume(snd_stream_hnd_t hnd, int vol);
#endif

#ifndef PS3_COMPAT_KOS_WORKER_H
#define PS3_COMPAT_KOS_WORKER_H
#include "kos/thread.h"
typedef struct kthread_worker { int dummy; } kthread_worker_t;
kthread_worker_t *thd_worker_create_ex(const char *name, int prio, void (*routine)(void *), void *data);
void thd_worker_wakeup(kthread_worker_t *w);
void thd_worker_destroy(kthread_worker_t *w);
kthread_t *thd_worker_get_thread(kthread_worker_t *w);
#endif

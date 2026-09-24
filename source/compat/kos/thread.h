/* compat/kos/thread.h -- threads de KOS -> sysThread de PSL1GHT (etapa 2). */
#ifndef PS3_COMPAT_KOS_THREAD_H
#define PS3_COMPAT_KOS_THREAD_H
#include <stdint.h>

typedef struct kthread { int tid; } kthread_t;
typedef struct { int prio, label; } kthread_attr_t;

kthread_t *thd_create_ex(kthread_attr_t *attr, void *(*routine)(void *), void *param);
kthread_t *thd_create(int detach, void *(*routine)(void *), void *param);
int        thd_join(kthread_t *thd, void **value_ptr);
void       thd_pass(void);
int        thd_sleep(int ms);

typedef struct { int locked; } mutex_t;
#define MUTEX_INITIALIZER  {0}
int mutex_init(mutex_t *m, int type);
int mutex_destroy(mutex_t *m);
int mutex_lock(mutex_t *m);
int mutex_unlock(mutex_t *m);
int mutex_trylock(mutex_t *m);

typedef struct { int count; } semaphore_t;
int sem_init(semaphore_t *sm, int count);
int sem_destroy(semaphore_t *sm);
int sem_wait(semaphore_t *sm);
int sem_signal(semaphore_t *sm);
#endif

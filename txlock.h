#ifndef _TXLOCK_H_
#define _TXLOCK_H_

#include <time.h> // for cond vars

#ifdef __cplusplus
extern "C" {
#endif

struct _txlock_t;
typedef struct _txlock_t txlock_t;

int tl_alloc(txlock_t **l);
int tl_free(txlock_t *l);

int tl_lock(txlock_t *l);
int tl_trylock(txlock_t *l);
int tl_unlock(txlock_t *l);


struct _txcond_t;
typedef struct _txcond_t txcond_t;

int tc_alloc(txcond_t **cv);
int tc_free(txcond_t *cv);
int tc_init(txcond_t *cv);
int tc_destroy(txcond_t *cv);
int tc_wait(txcond_t *cv, txlock_t *lk);
int tc_timedwait(txcond_t *cv, txlock_t *lk, const struct timespec *abs_timeout);
int tc_signal(txcond_t* cv);
int tc_broadcast(txcond_t* cv);

void tl_replace_libpthread(int);

int tl_pthread_mutex_init(void *m, void *attr);
int tl_pthread_mutex_lock(void *l);
int tl_pthread_mutex_trylock(void *l);
int tl_pthread_mutex_unlock(void *l);

// internal handler, shoud not be called in user app
void __tl_pthread_exit(void *retval);

#ifdef __cplusplus
}
#endif

#endif

#ifndef _TXLOCK_H_
#define _TXLOCK_H_

#ifdef __cplusplus
extern "C" {
#endif

struct _txlock_t;
typedef struct _txlock_t txlock_t;

int tl_alloc(txlock_t **l);
int tl_free(txlock_t *l);

int tl_lock(txlock_t *l);
int tl_unlock(txlock_t *l);

void tl_replace_libpthread(int);

int tl_pthread_mutex_lock(void *l);
int tl_pthread_mutex_unlock(void *l);

#ifdef __cplusplus
}
#endif

#endif

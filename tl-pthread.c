#include "txlock.h"

int pthread_mutex_lock(void *mutex) {
    return tl_pthread_mutex_lock(mutex);
}

int pthread_mutex_trylock(void *mutex) {
    return tl_pthread_mutex_trylock(mutex);
}

int pthread_mutex_unlock(void *mutex) {
    return tl_pthread_mutex_unlock(mutex);
}

int pthread_mutex_init(void *mutex, void *attr) {
    return tl_pthread_mutex_init(mutex, attr);
}

int pthread_cond_broadcast(void *cond) {
    return tc_broadcast((txcond_t*)cond);
}

int pthread_cond_destroy(void *cond) {
    return tc_destroy((txcond_t*)cond);
}

int pthread_cond_init(void *cond, const void *cond_attr) {
    return tc_init((txcond_t*)cond);
}

int pthread_cond_signal(void *cond) {
    return tc_signal((txcond_t*)cond);
}

int pthread_cond_timedwait(void *cond, void *mutex, const struct timespec *timeout) {
    return tc_timedwait((txcond_t*)cond, (txlock_t*)mutex, timeout);
}

int pthread_cond_wait(void *cond, void *mutex) {
    return tc_wait((txcond_t*)cond, (txlock_t*)mutex);
}

void pthread_exit(void *retval) {
    return __tl_pthread_exit(retval);
}

__attribute__((constructor(200)))
static void init_tl_pthread() {
    tl_replace_libpthread(1);
}

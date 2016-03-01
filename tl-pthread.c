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

__attribute__((constructor(200))) 
static void init_tl_pthread() {
    tl_replace_libpthread(1);
}

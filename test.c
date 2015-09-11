#include <pthread.h>
#include <assert.h>
#include "txlock.h"

int main() {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

    assert(tl_pthread_mutex_trylock(&m)==0);
    tl_pthread_mutex_unlock(&m);
    tl_pthread_mutex_lock(&m);
    assert(tl_pthread_mutex_trylock(&m)!=0);
    tl_pthread_mutex_unlock(&m);
    assert(tl_pthread_mutex_trylock(&m)==0);
    tl_pthread_mutex_unlock(&m);
}

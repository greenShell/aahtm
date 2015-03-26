#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h> // for pthread_mutex_t only
#include "htm_raw.h"
#include "txlock.h"

static const char* LIBPTHREAD_PATH = "/lib64/libpthread.so.0";
static void *libpthread_handle = 0;

typedef int (*txlock_func_t)(txlock_t *);
typedef int (*txlock_func_alloc_t)(txlock_t **);
static txlock_func_t func_tl_lock = 0;
static txlock_func_t func_tl_unlock = 0;
static txlock_func_alloc_t func_tl_alloc = 0;
static txlock_func_t func_tl_free = 0;

int tl_pthread_mutex_lock(void *mutex) {
    return func_tl_lock((txlock_t*)mutex);
/*    if (spec_lock == 0) { // not in HTM
        //if (__sync_lock_test_and_set((volatile int*)m, 1)) {
        if (libpthread_mutex_trylock(m)) {
            do {
                spec_lock == m;
                if (HTM_SIMPLE_BEGIN() == HTM_SUCCESSFUL) {
                    return 0;
                }
                spec_lock = 0;
                for (int i=0; i<BACKOFF_INIT; i++)
                    __asm volatile("pause\n": : :"memory");

            //} while (__sync_lock_test_and_set((volatile int*)m, 1));
            } while (libpthread_mutex_trylock(m));
        }
    } else if (spec_lock == m) {
        spec_lock_recursive++;
    }*/
}

int tl_pthread_mutex_unlock (void *mutex) {
    return func_tl_unlock((txlock_t*)mutex);
}

static int tl_pthread_mutex_alloc(pthread_mutex_t **mutex) {
    *mutex = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    *(*mutex) = m;
    return 0;
}

int tl_alloc(txlock_t **l) { return func_tl_alloc(l); }
int tl_free(txlock_t *l) { return func_tl_free(l); }
int tl_lock(txlock_t *l) { return func_tl_lock(l); }
int tl_unlock(txlock_t *l) { return func_tl_unlock(l); }

//
//
static inline void cpu_relax() { __asm volatile("pause\n": : :"memory"); }
static int SPIN_INIT = 64; 
static int SPIN_CELL = 1024; 
static float SPIN_FACTOR = 2; 
static inline int spin_begin() { return SPIN_INIT; }
static inline int spin_wait(int s) { 
    for (int i=0; i<s; i++)
        cpu_relax();        
    int n = SPIN_FACTOR * s; 
    return n > SPIN_CELL ? SPIN_CELL : n;
}


static int tl_general_free(txlock_t *l) { free(l); return 0;}

static __thread void * volatile spec_lock = 0;
static __thread int spec_lock_recursive = 0;

// test-and-set lock
//
struct _tas_lock_t {
    volatile int16_t val;
    volatile int16_t count;
} __attribute__((__packed__));

typedef struct _tas_lock_t tas_lock_t;

static int tas_alloc(tas_lock_t **l) {
    *l = (tas_lock_t*)malloc(64);
    (*l)->val = 0;
    (*l)->count = 0;
    return 0;
};

static inline int tatas(volatile int16_t* val, int16_t v) {
    return *val || __sync_lock_test_and_set(val, v); 
}

static int tas_lock(tas_lock_t *l) {
    if (tatas(&l->val, 1)) {
        //__sync_fetch_and_add(&l->count, 1);
        int s = spin_begin();
        do {
            s = spin_wait(s); 
        } while (tatas(&l->val, 1));
    }
    return 0;
}

static int tas_unlock(tas_lock_t *l) {
    __sync_lock_release(&l->val);
    return 0;
}

static int tas_lock_tm(tas_lock_t *l) {
    if (spec_lock == 0) { // not in HTM
        if (tatas(&l->val, 1)) {
            int s = spin_begin();
                spec_lock = l;
                if (HTM_SIMPLE_BEGIN() == HTM_SUCCESSFUL) {
                    spec_lock_recursive = l->val;
                    return 0;
                }
                spec_lock = 0;


            while (tatas(&l->val, 1))
                s = spin_wait(s); 
        }
    } else if (spec_lock == l) {
        spec_lock_recursive++;
    }
    return 0;
}

static int tas_unlock_tm(tas_lock_t *l) {
    if (spec_lock) { // in htm
       //if (spec_lock == l) {
       //    if (--spec_lock_recursive==0)
                HTM_ABORT(7);
       //}
    } else { // not in HTM
        __sync_lock_release(&l->val);
    }
    return 0;
}

struct _lock_type_t {
    const char *name;
    int lock_size;
    txlock_func_alloc_t alloc_fun;
    txlock_func_t free_fun;
    txlock_func_t lock_fun;
    txlock_func_t unlock_fun;
};
typedef struct _lock_type_t lock_type_t;

static lock_type_t lock_types[] = {
    {"pthread", sizeof(pthread_mutex_t), (txlock_func_alloc_t)tl_pthread_mutex_alloc, tl_general_free, NULL, NULL}, 
    {"pthread_tm", sizeof(pthread_mutex_t), (txlock_func_alloc_t)tl_pthread_mutex_alloc, tl_general_free, NULL, NULL}, 
    {"tas",     sizeof(tas_lock_t), (txlock_func_alloc_t)tas_alloc, tl_general_free, (txlock_func_t)tas_lock, (txlock_func_t)tas_unlock}, 
    {"tas_tm",  sizeof(tas_lock_t), (txlock_func_alloc_t)tas_alloc, tl_general_free, (txlock_func_t)tas_lock_tm, (txlock_func_t)tas_unlock_tm}, 
};

static lock_type_t *using_lock_type = &lock_types[0];

static int replace_libpthread = 0;
void tl_replace_libpthread(int r) {replace_libpthread=r;}

static void setup_pthread_funcs() {
    char *error;
    void *handle = dlopen(LIBPTHREAD_PATH, RTLD_LAZY);
    if (!handle) {
       fputs (dlerror(), stderr);
       exit(1);
    }
    libpthread_handle = handle;

    lock_types[0].lock_fun = (txlock_func_t)dlsym(handle, "pthread_mutex_lock");
    if ((error = dlerror()) != NULL)  {
        fputs(error, stderr);
        exit(1);
    }

    lock_types[0].unlock_fun = (txlock_func_t)dlsym(handle, "pthread_mutex_unlock");
    if ((error = dlerror()) != NULL)  {
        fputs(error, stderr);
        exit(1);
    }
}

__attribute__((constructor(201)))  // after tl-pthread.so
static void init_lib_txlock() {
    setup_pthread_funcs();
 
    const char *type = getenv("LIB_TXLOCK");
    if (type) {
        for (int i=0; i<sizeof(lock_types)/sizeof(lock_type_t); i++) {
            if (strcmp(type, lock_types[i].name) == 0) {
                using_lock_type = &lock_types[i];
                break;
            } 
        }
    }

    func_tl_alloc = using_lock_type->alloc_fun;
    func_tl_free = using_lock_type->free_fun;
    func_tl_lock = using_lock_type->lock_fun;
    func_tl_unlock = using_lock_type->unlock_fun;

    fprintf(stderr, "LIB_TXLOCK: %s\n", using_lock_type->name);
    fflush(stderr);
}


__attribute__((destructor)) 
static void uninit_lib_txlock() {
    if (libpthread_handle)
        dlclose(libpthread_handle);
}

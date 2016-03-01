#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h> // for pthread_mutex_t only
#include "htm_raw.h"
#include "txlock.h"

#if defined(__powerpc__) || defined(__powerpc64__)
static const char* LIBPTHREAD_PATH = "/lib/powerpc64le-linux-gnu/libpthread.so.0";
#else
static const char* LIBPTHREAD_PATH = "/lib64/libpthread.so.0";
#endif
static void *libpthread_handle = 0;

typedef int (*txlock_func_t)(txlock_t *);
typedef int (*txlock_func_alloc_t)(txlock_t **);
static txlock_func_t func_tl_lock = 0;
static txlock_func_t func_tl_trylock = 0;
static txlock_func_t func_tl_unlock = 0;
static txlock_func_alloc_t func_tl_alloc = 0;
static txlock_func_t func_tl_free = 0;

static int pthread_mutex_offset = 0;

typedef int (*fun_pthread_mutex_init_t)(pthread_mutex_t*, const pthread_mutexattr_t*);
static fun_pthread_mutex_init_t libpthread_mutex_init = 0;
static txlock_func_t libpthread_mutex_lock = 0;
static txlock_func_t libpthread_mutex_trylock = 0;
static txlock_func_t libpthread_mutex_unlock = 0;

static __thread void * volatile spec_lock = 0;
static __thread int spec_lock_recursive = 0;


int tl_pthread_mutex_init(void *m, void *attr) {
    pthread_mutex_t *mutex = (pthread_mutex_t*)m;
    memset(&mutex->__data.__txlock, 0, sizeof(mutex->__data.__txlock));
    return libpthread_mutex_init(mutex, (const pthread_mutexattr_t*)attr);
}

int tl_pthread_mutex_lock(void *m) {
    if (pthread_mutex_offset) {
        pthread_mutex_t* mutex = (pthread_mutex_t*)m;
        int self = (int)pthread_self();
        //fprintf(stderr, "A %p %d %d %d\n", mutex, ((pthread_mutex_t*)mutex)->__data.__count, mutex->__data.__owner, self);
        assert(!spec_lock);
        if(!spec_lock && mutex->__data.__count>0 && pthread_equal(mutex->__data.__owner, self)) {
            mutex->__data.__count++;
            return 0;
        } else {
            int ret = func_tl_lock((txlock_t*)&mutex->__data.__txlock); 
            if  (!spec_lock) {
                mutex->__data.__count = 1;
                mutex->__data.__owner = self;
            }
            return ret; 
            //return libpthread_mutex_lock((txlock_t*)mutex);
        }
    } else {
        return func_tl_lock((txlock_t*)m);
    }
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

int tl_pthread_mutex_trylock(void *m) {
    if (pthread_mutex_offset) {
        assert(!spec_lock);
        pthread_mutex_t* mutex = (pthread_mutex_t*)m;
        //fprintf(stderr, "T %p %d\n", mutex, ((pthread_mutex_t*)mutex)->__data.__count);
        int self = pthread_self();
        if(!spec_lock && mutex->__data.__count>0 && pthread_equal(mutex->__data.__owner, self)) {
            mutex->__data.__count++;
            return 0;
        } else {
            int ret = func_tl_trylock((txlock_t*)&mutex->__data.__txlock);
            if  (!ret && !spec_lock) {
                mutex->__data.__count = 1;
                mutex->__data.__owner = self;
            }
            return ret;
        }
        //return libpthread_mutex_trylock((txlock_t*)mutex);
    } else {
        assert(func_tl_trylock);
        return func_tl_trylock((txlock_t*)m);
    }
}


int tl_pthread_mutex_unlock (void *m) {
    if (pthread_mutex_offset) {
        pthread_mutex_t* mutex = (pthread_mutex_t*)m;
        //fprintf(stderr, "R %p %d\n", mutex, ((pthread_mutex_t*)mutex)->__data.__count);
        assert(!spec_lock);
        if  (!spec_lock) {
            if (--mutex->__data.__count == 0) {
                mutex->__data.__owner = 0;
                return func_tl_unlock((txlock_t*)&((pthread_mutex_t*)mutex)->__data.__txlock);
            }
            return 0;
        } else
            return func_tl_unlock((txlock_t*)&((pthread_mutex_t*)mutex)->__data.__txlock);
        //return libpthread_mutex_unlock((txlock_t*)mutex);
    } else {
        return func_tl_unlock((txlock_t*)m);
        //return libpthread_mutex_unlock((txlock_t*)mutex);
    }
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
int tl_trylock(txlock_t *l) { return func_tl_trylock(l); }
int tl_unlock(txlock_t *l) { return func_tl_unlock(l); }

//
//
#if defined(__powerpc__) || defined(__powerpc64__)
    //#define HMT_very_low()   __asm volatile("or 31,31,31   # very low priority")
    //static inline void cpu_relax() { HMT_very_low(); }
    static inline void cpu_relax() { __asm volatile("nop\n": : :"memory"); }
#else
    static inline void cpu_relax() { __asm volatile("pause\n": : :"memory"); }
#endif
static int SPIN_INIT = 16; 
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

// test-and-set lock
//
struct _tas_lock_t {
    volatile int32_t val;
    volatile int32_t count;
} __attribute__((__packed__));

typedef struct _tas_lock_t tas_lock_t;

static int tas_alloc(tas_lock_t **l) {
    *l = (tas_lock_t*)malloc(256);
    (*l)->val = 0;
    (*l)->count = 0;
    return 0;
};

static inline int tatas(volatile int32_t* val, int32_t v) {
    return *val || __sync_lock_test_and_set(val, v); 
}

static int tas_lock(tas_lock_t *l) {
    if (tatas(&l->val, 1)) {
        int s = spin_begin();
        do {
            s = spin_wait(s); 
        } while (tatas(&l->val, 1));
    }
    return 0;
}

static int tas_trylock(tas_lock_t *l) {
    return tatas(&l->val, 1);
}

static int tas_unlock(tas_lock_t *l) {
    __sync_lock_release(&l->val);
    return 0;
}

static int tas_lock_tm(tas_lock_t *l) {
    if (spec_lock == 0) { // not in HTM
        if (tatas(&l->val, 1)) {
            int tries = 0; 
            //int order = __sync_fetch_and_add(&l->count, 1);
            int s = spin_begin();
                spec_lock = l;
                if (tries < 3 && HTM_SIMPLE_BEGIN() == HTM_SUCCESSFUL) {
              //      if (order==0)
              //          spec_lock_recursive = l->val;
                    return 0;
                }
                tries++;
                spec_lock = 0;


            while (tatas(&l->val, 1))
                s = spin_wait(s); 

            //__sync_fetch_and_sub(&l->count, 1);
        }
    } else if (spec_lock == l) {
        //spec_lock_recursive++;
    }
    return 0;
}

static int tas_trylock_tm(tas_lock_t *l) {
    if (spec_lock == 0) { // not in HTM
        return tatas(&l->val, 1);
    } else if (spec_lock == l) {
        //spec_lock_recursive++;
    }
    return 0;
}

static int tas_unlock_tm(tas_lock_t *l) {
    if (spec_lock) { // in htm
    //   if (spec_lock == l) {
    //       if (--spec_lock_recursive==0)
    //            HTM_ABORT(7);
    //   }
    } else { // not in HTM
        __sync_lock_release(&l->val);
    }
    return 0;
}

struct _ticket_lock_t {
    volatile uint32_t next;
    volatile uint32_t now;
} __attribute__((__packed__));

typedef struct _ticket_lock_t ticket_lock_t;

static int ticket_lock(ticket_lock_t *l) {
    uint32_t my_ticket = __sync_fetch_and_add(&l->next, 1);
    while (my_ticket != l->now) {
        uint32_t dist = my_ticket - l->now;
        spin_wait(16*dist); 
    }
    return 0;
}

static int ticket_trylock(ticket_lock_t *l) {
    ticket_lock_t t, n;
    t.now = t.next = l->now;
    n.now = t.now;
    n.next = t.next+1;

    return !(__sync_bool_compare_and_swap((int64_t*)l, *(int64_t*)&t, *(int64_t*)&n));
}

static int ticket_unlock(ticket_lock_t *l) {
    l->now++;
    return 0;
}

static int ticket_lock_tm(ticket_lock_t *l) {
    if (spec_lock)
        return 0;

    int tries = 0;
    uint32_t my_ticket = __sync_fetch_and_add(&l->next, 1);
    while (my_ticket != l->now) {
        uint32_t dist = my_ticket - l->now;
        if (dist <= 2 && tries < 4) {
            spin_wait(8); 
            spec_lock = l;
            if (HTM_SIMPLE_BEGIN() == HTM_SUCCESSFUL) {
            //    if (dist==1)
            //        dist = my_ticket - l->now;
                return 0;
            }
            spec_lock = 0;
            tries++;
        } else
            spin_wait(16*dist); 
    }
    return 0;
}

static int ticket_trylock_tm(ticket_lock_t *l) {
    if (spec_lock) { // in htm
        // nothing
    } else { // not in HTM
        ticket_trylock(l);
    }
    return 0;
}

static int ticket_unlock_tm(ticket_lock_t *l) {
    if (spec_lock) { // in htm
       //if (spec_lock == l) {
       // HTM_ABORT(7);
       //}
    } else { // not in HTM
        l->now++;
    }
    return 0;
}


static int ticket_alloc(ticket_lock_t **l) {
    *l = (ticket_lock_t*)malloc(128);
    (*l)->next = 0;
    (*l)->now = 0;
    return 0;
};



struct _lock_type_t {
    const char *name;
    int lock_size;
    txlock_func_alloc_t alloc_fun;
    txlock_func_t free_fun;
    txlock_func_t lock_fun;
    txlock_func_t trylock_fun;
    txlock_func_t unlock_fun;
};
typedef struct _lock_type_t lock_type_t;

static lock_type_t lock_types[] = {
    {"pthread",     sizeof(pthread_mutex_t), (txlock_func_alloc_t)tl_pthread_mutex_alloc, tl_general_free, NULL, NULL, NULL}, 
    {"pthread_tm",  sizeof(pthread_mutex_t), (txlock_func_alloc_t)tl_pthread_mutex_alloc, tl_general_free, NULL, NULL, NULL}, 
    {"tas",         sizeof(tas_lock_t), (txlock_func_alloc_t)tas_alloc, tl_general_free, (txlock_func_t)tas_lock, (txlock_func_t)tas_trylock, (txlock_func_t)tas_unlock}, 
    {"tas_tm",      sizeof(tas_lock_t), (txlock_func_alloc_t)tas_alloc, tl_general_free, (txlock_func_t)tas_lock_tm, (txlock_func_t)tas_trylock_tm, (txlock_func_t)tas_unlock_tm}, 
//    {"tas_hle",     sizeof(tas_lock_t), (txlock_func_alloc_t)tas_alloc, tl_general_free, (txlock_func_t)tas_lock_hle, (txlock_func_t)tas_trylock, (txlock_func_t)tas_unlock_hle}, 
    {"ticket",      sizeof(ticket_lock_t), (txlock_func_alloc_t)ticket_alloc, tl_general_free, (txlock_func_t)ticket_lock, (txlock_func_t)ticket_trylock, (txlock_func_t)ticket_unlock}, 
    {"ticket_tm",   sizeof(ticket_lock_t), (txlock_func_alloc_t)ticket_alloc, tl_general_free, (txlock_func_t)ticket_lock_tm, (txlock_func_t)ticket_trylock_tm, (txlock_func_t)ticket_unlock_tm}, 
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

    libpthread_mutex_init = (fun_pthread_mutex_init_t)dlsym(handle, "pthread_mutex_init");
    libpthread_mutex_lock = (txlock_func_t)dlsym(handle, "pthread_mutex_lock");
    libpthread_mutex_trylock = (txlock_func_t)dlsym(handle, "pthread_mutex_trylock");
    libpthread_mutex_unlock = (txlock_func_t)dlsym(handle, "pthread_mutex_unlock");

    lock_types[1].lock_fun = lock_types[0].lock_fun = libpthread_mutex_lock; 
    lock_types[1].trylock_fun = lock_types[0].trylock_fun = libpthread_mutex_trylock;
    lock_types[1].unlock_fun = lock_types[0].unlock_fun = libpthread_mutex_unlock;

    if ((error = dlerror()) != NULL)  {
        fputs(error, stderr);
        exit(1);
    }
}

__attribute__((constructor(201)))  // after tl-pthread.so
static void init_lib_txlock() {
    setup_pthread_funcs();
 
    const char *type = getenv("LIBTXLOCK");
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
    func_tl_trylock = using_lock_type->trylock_fun;
    func_tl_unlock = using_lock_type->unlock_fun;

    if (strncmp(using_lock_type->name, "pthread", 7))
        pthread_mutex_offset = 1;

    fprintf(stderr, "LIBTXLOCK: %s\n", using_lock_type->name);
    fflush(stderr);
}


__attribute__((destructor)) 
static void uninit_lib_txlock() {
    if (libpthread_handle)
        dlclose(libpthread_handle);
}

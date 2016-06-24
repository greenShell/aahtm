#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h> // for pthread_mutex_t only
#include "htm_raw.h"
#include "txlock.h"
#include <x86intrin.h> // __rdtsc

// for cond vars
#include <time.h>
#include <semaphore.h>
#include <stdbool.h>
#include <errno.h>

typedef struct _tm_stats_t {
    uint64_t dur;           // total cycles in TM mode
    uint32_t tries;         // # of TM calls
    uint32_t stops;         // # of self-stop
    uint32_t overflows;     // overflow aborts
    uint32_t conflicts;     // conflict aborts
} __attribute__ ((aligned(64))) tm_stats_t;

static __thread tm_stats_t my_tm_stats;
static tm_stats_t tm_stats __attribute__ ((aligned(64)));

#if defined(__powerpc__) || defined(__powerpc64__)
static const char* LIBPTHREAD_PATH = "/lib/powerpc64le-linux-gnu/libpthread.so.0";
#else
static const char* LIBPTHREAD_PATH = "libpthread.so.0"; // without specifying the path dlopen will search for the library on LIB_PATH
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
static void (*libpthread_exit)(void *) = 0;

static __thread void * volatile spec_lock = 0;
//static __thread int spec_lock_recursive = 0;

int tl_pthread_mutex_init(void *m, void *attr) {
    pthread_mutex_t *mutex = (pthread_mutex_t*)m;
    //memset(&mutex->__data.__txlock, 0, sizeof(mutex->__data.__txlock));
    if (pthread_mutex_offset)
        return libpthread_mutex_init(mutex, (const pthread_mutexattr_t*)attr);
    else {
        memset(mutex, 0, sizeof(pthread_mutex_t));
        return 0;
    }
}

int tl_pthread_mutex_lock(void *m) {
    if (pthread_mutex_offset) {
        return libpthread_mutex_lock((txlock_t*)m);
    } else {
        return func_tl_lock((txlock_t*)m);
    }
}

int tl_pthread_mutex_trylock(void *m) {
    if (pthread_mutex_offset) {
        return libpthread_mutex_trylock((txlock_t*)m);
    } else {
        return func_tl_trylock((txlock_t*)m);
    }
}


int tl_pthread_mutex_unlock (void *m) {
    if (pthread_mutex_offset) {
        return libpthread_mutex_unlock((txlock_t*)m);
    } else {
        return func_tl_unlock((txlock_t*)m);
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

static int tas_init(tas_lock_t *l) {
    l->val = 0;
    l->count = 0;
    return 0;
};

static inline int tatas(volatile int32_t* val, int32_t v) {
    return *val || __sync_lock_test_and_set(val, v);
}

static int tas_lock(tas_lock_t *l) {
		//printf("tas-locking %x\n",l);
    if (tatas(&l->val, 1)) {
        int s = spin_begin();
        do {
            s = spin_wait(s);
        } while (tatas(&l->val, 1));
    }
		//printf("tas-locked %x\n",l);
    return 0;
}

static int tas_trylock(tas_lock_t *l) {
		//printf("tas-trylock %x\n",l);
    return tatas(&l->val, 1);
}

static int tas_unlock(tas_lock_t *l) {
		//printf("tas-unlock %x\n",l);
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
        for (size_t i=0; i<sizeof(lock_types)/sizeof(lock_type_t); i++) {
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


void __tl_pthread_exit(void *retval)
{
    // sync my local stats to the global stats
    __sync_fetch_and_add(&tm_stats.dur, my_tm_stats.dur);
    __sync_fetch_and_add(&tm_stats.tries, my_tm_stats.tries);
    __sync_fetch_and_add(&tm_stats.stops, my_tm_stats.stops);
    __sync_fetch_and_add(&tm_stats.overflows, my_tm_stats.overflows);
    __sync_fetch_and_add(&tm_stats.conflicts, my_tm_stats.conflicts);
    // reset my local stats in case the thread stack is reused by a new thread later on
    memset(&my_tm_stats, 0, sizeof(my_tm_stats));
    libpthread_exit(retval);
}

__attribute__((destructor))
static void uninit_lib_txlock()
{
    fprintf(stderr, "LIBTXLOCK: %s", using_lock_type->name);
    if (tm_stats.tries) {
        fprintf(stderr, ", avg_dur: %d, tries: %d, overflows: %d, conflicts: %d, stops: %d",
                        (int)(tm_stats.dur/(tm_stats.tries - tm_stats.stops)),
                        tm_stats.tries, tm_stats.overflows, tm_stats.conflicts, tm_stats.stops);
    }
    fprintf(stderr, "\n");
    fflush(stderr);

    if (libpthread_handle)
        dlclose(libpthread_handle);
}



/*

// Generalized interface to txlocks
struct _txlock_t;
typedef struct _txlock_t txlock_t;
int tl_alloc(txlock_t **l);
int tl_free(txlock_t *l);
int tl_lock(txlock_t *l);
int tl_trylock(txlock_t *l);
int tl_unlock(txlock_t *l);

*/


/*
// pthreads condvar interface


int   pthread_cond_destroy(pthread_cond_t *);
int   pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);

int   pthread_cond_broadcast(pthread_cond_t *);
int   pthread_cond_signal(pthread_cond_t *);
int   pthread_cond_timedwait(pthread_cond_t *,
          pthread_mutex_t *, const struct timespec *);
int   pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);

*/


enum {WAITING, TIMEOUT, AWOKEN};

struct _txcond_node_t {
	struct _txcond_node_t* next;
	struct _txcond_node_t* prev;
	sem_t sem;
	int status;
} __attribute__((__packed__));

typedef struct _txcond_node_t txcond_node_t;

struct _txcond_t {
	txcond_node_t* head;
	txcond_node_t* tail;
	tas_lock_t lk;
	uint32_t cnt;
} __attribute__((__packed__));

typedef struct _txcond_t txcond_t;

int tc_init(txcond_t* cv){
	cv->head = NULL;
	cv->tail = NULL;
	tas_init(&(cv->lk));
	cv->cnt = 5;
	return 0;
}

int tc_alloc(txcond_t **cv){
	int e;
	*cv = (txcond_t*)malloc(256);
	if(!cv){return -1;}
	return tc_init(*cv);
}

int tc_destroy(txcond_t *cv){
	return 0;
}

int tc_free(txcond_t *cv){
	free(cv);
	return 0;
}

static int _tc_waitcommon(txcond_t *cv, txlock_t *lk, bool timed, const struct timespec *abs_timeout){
	int e;

	//printf("tc-wait cv:%x lk:%x ilk:%x\n",cv,lk,&cv->lk);

	// create node
	txcond_node_t* node = malloc(sizeof(txcond_node_t));
	if(!node){assert(false);return -1;}
	e = sem_init(&node->sem, 0, 0);
	if(e!=0){return -1;}
	node->next = NULL;
	node->prev = NULL;
	node->status = WAITING;


	//printf("%d\n",cv->lk);
	// enqueue into cond var queue
	tas_lock(&cv->lk);
	if(cv->tail!=NULL){
		node->prev = cv->tail;
		cv->tail->next = node;
		cv->tail = node;
	}
	else{
		cv->head = node;
		cv->tail = node;
	}
	tas_unlock(&cv->lk);
	//printf("tc-wait-enqueued cv:%x lk:%x\n",cv,lk);
	// release lock now that we're enqueued
	tl_pthread_mutex_unlock(lk);



	// wait
	while(true){
		if(timed){e = sem_timedwait(&node->sem,abs_timeout);}
		else{e = sem_wait(&node->sem);}
		if(e!=0){
			if(errno==EINTR){continue;}
			else if(errno==EINVAL){assert(false);return -1;}
			else if(timed && errno==ETIMEDOUT){
				if(__sync_bool_compare_and_swap(&node->status,WAITING,TIMEOUT)){
					// we won the race to clean up our node,
					// whoever 'wakes' us up later will clean
					errno = ETIMEDOUT;
					return -1;
				}
				else{
					// our semaphore was posted after
					// we timed out, but we lost the race
					// to clean up our node
					free(node);
					break;
				}
			}
			else{assert(false);return -1;} // unknown error
		}
		else{
			// we were woken up via semaphore
			// race on status to figure out cleaner
			if(__sync_bool_compare_and_swap(&node->status,WAITING,AWOKEN)){break;}
			else{ free(node); break; }
		}
	}

	// we've been woken up, so reacquire the lock
	tl_lock(lk);
	return 0;
}


int tc_timedwait(txcond_t *cv, txlock_t *lk, const struct timespec *abs_timeout){
	return _tc_waitcommon(cv,lk,true,abs_timeout);
}
int tc_wait(txcond_t *cv, txlock_t *lk){
	return _tc_waitcommon(cv,lk,false,NULL);
}




int tc_signal(txcond_t* cv){
	int e, i;
	txcond_node_t* node;

	//printf("tc-signal cv:%x ilk:%x\n",cv,&cv->lk);

	// access node
	// tend towards LIFO, but throw in eventual FIFO
	tas_lock(&cv->lk);

	// decide if accessing head or tail
	if(cv->cnt==0){cv->cnt=5;}
	cv->cnt = cv->cnt*1103515245 + 12345;
	i = cv->cnt;

	if(i%10==0){
		if(cv->head == NULL){	tas_unlock(&cv->lk); return 0;}
		node = cv->head;
		cv->head = cv->head->next;
		if(cv->head==NULL){cv->tail=NULL;}
		else{cv->head->prev = NULL;}
	}
	else{
		if(cv->tail == NULL){	tas_unlock(&cv->lk); return 0;}
		node = cv->tail;
		cv->tail = node->prev;
		if(cv->tail==NULL){cv->head=NULL;}
		else{cv->tail->next = NULL;}
	}
	tas_unlock(&cv->lk);

	// awaken waiter
	e = sem_post(&node->sem);
	if(e!=0){assert(false);return -1;}

	// successful awaken, race on GC
	if(!__sync_bool_compare_and_swap(&node->status,WAITING,AWOKEN)){free(node);}

	return 0;
}

int tc_broadcast(txcond_t* cv){

	//printf("tc-broadcast cv:%x ilk:%x\n",cv,&cv->lk);

	int e1, e2;
	txcond_node_t* node;
	txcond_node_t* prev_node;

	// remove entire list
	tas_lock(&cv->lk);
	if(cv->head == NULL){	tas_unlock(&cv->lk); return 0; }
	else{
		node = cv->head;
		cv->head = NULL;
		cv->tail = NULL;
	}
	tas_unlock(&cv->lk);

	// awaken everyone
	e1 = 0;
	while(node!=NULL){
		e2 = sem_post(&node->sem);
		if(e2!=0 && e1==0){e1 = e2;}
		prev_node = node;
		node = node->next;
		if(!__sync_bool_compare_and_swap(&prev_node->status,WAITING,AWOKEN)){free(prev_node);}
	}

	return e1;
}


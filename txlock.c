#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h> // for pthread_mutex_t only

// for cond vars
#include <time.h>
#include <semaphore.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>

#include "txlock.h"

// https://sourceforge.net/p/predef/wiki/Architectures/
#if defined(__x86_64__) || defined(__x86_64)
    #include <immintrin.h>
    #include <x86intrin.h> // __rdtsc

    #define HTM_SIMPLE_BEGIN()  _xbegin()
    #define HTM_END()           _xend()
    #define HTM_ABORT(c)        _xabort(c)
    #define HTM_SUCCESSFUL      _XBEGIN_STARTED
    #define HTM_IS_CONFLICT(c)  ((c) & _XABORT_CONFLICT)
    #define HTM_IS_OVERFLOW(c)  ((c) & _XABORT_CAPACITY)

    static inline uint64_t rdtsc() { return __rdtsc(); }

#elif defined(__powerpc64__) || defined(__ppc64__)
    #include <htmintrin.h>

    #define HTM_SIMPLE_BEGIN()  __builtin_tbegin(0)
    #define HTM_END()           __builtin_tend(0)
    #define HTM_ABORT(c)        __builtin_tabort(c)
    #define HTM_SUCCESSFUL      1
    #define HTM_SUSPEND()       __builtin_tsuspend()
    #define HTM_RESUME()        __builtin_tresume()
    // TODO:
    //#define HTM_IS_CONFLICT(c)
    //#define HTM_IS_OVERFLOW(c)
    //rdtsc
#else
    #error "unsupported CPU"
#endif

typedef struct _tm_stats_t {
    int64_t cycles;        // total cycles in lock mode
    int64_t tm_cycles;     // total cycles in TM mode
    uint32_t locks;         // # of lock acqs
    uint32_t tries;         // # of tm_begins
    uint32_t stops;         // # of self-stop
    uint32_t overflows;     // overflow aborts
    uint32_t conflicts;     // conflict aborts
} __attribute__ ((aligned(64))) tm_stats_t;

static __thread tm_stats_t my_tm_stats; // thread-local stats
static tm_stats_t tm_stats;             // global stats, updated only when a thread exits

#ifdef TM_NO_PROFILING
#define TM_STATS_ADD(stat, value)
#define TM_STATS_SUB(stat, value)
#else
#define TM_STATS_ADD(stat, value) ((stat)+=(value))
#define TM_STATS_SUB(stat, value) ((stat)-=(value))
#endif

_Static_assert(sizeof(txlock_t) == sizeof(pthread_mutex_t), "must be same size as pthreads for drop in replacement");
_Static_assert(sizeof(txcond_t) == sizeof(pthread_cond_t), "must be same size as pthreads for drop in replacement");

// Library search paths
#if defined(__powerpc__) || defined(__powerpc64__)
static const char* LIBPTHREAD_PATH = "/lib/powerpc64le-linux-gnu/libpthread.so.0";
#else
static const char* LIBPTHREAD_PATH = "libpthread.so.0"; // without specifying the path dlopen will search for the library on LIB_PATH
#endif
static void *libpthread_handle = 0;

// ticket_tm parameters
static uint32_t TK_MIN_DISTANCE = 0;
static uint32_t TK_MAX_DISTANCE = 2;
static uint32_t TK_NUM_TRIES    = 4;

// Function pointers used to dispatch lock methods
typedef int (*txlock_func_t)(txlock_t *);
typedef int (*txlock_func_alloc_t)(txlock_t **);
static txlock_func_t func_tl_lock = 0;
static txlock_func_t func_tl_trylock = 0;
static txlock_func_t func_tl_unlock = 0;

// txlock interface, dispatches to above function
// pointers
int tl_lock(txlock_t *l) { return func_tl_lock(l); }
int tl_trylock(txlock_t *l) { return func_tl_trylock(l); }
int tl_unlock(txlock_t *l) { return func_tl_unlock(l); }


// Function pointers back into libpthreads implementations
// (these are set on library load)
//typedef int (*fun_pthread_mutex_init_t)(pthread_mutex_t*, const pthread_mutexattr_t*);
//static fun_pthread_mutex_init_t libpthread_mutex_init = 0;
static txlock_func_t libpthread_mutex_lock = 0;
static txlock_func_t libpthread_mutex_trylock = 0;
static txlock_func_t libpthread_mutex_unlock = 0;
static void (*libpthread_exit)(void *) = 0;


// State for HTM speculation
static __thread void * volatile spec_lock = 0;

// thread local counters
static __thread int spec_lock_recursive = 0;


// ASM optimized synchronization
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


// test-and-set lock =========================
//
struct _tas_lock_t {
    volatile int32_t val;
    volatile int32_t count;
} __attribute__((__packed__));

typedef struct _tas_lock_t tas_lock_t;

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

// test-and-set TM lock =========================
//

static int tas_lock_tm(tas_lock_t *l) {
	if (spec_lock == 0) { // not in HTM
		if (tatas(&l->val, 1)) {
			// if lock is held, start speculating
			int s = spin_begin();
			spec_lock = l;
			if (HTM_SIMPLE_BEGIN() == HTM_SUCCESSFUL) {
				return 0;
			}
			// else, contend for the lock
			spec_lock = 0;
			while (tatas(&l->val, 1))
				s = spin_wait(s);
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
    } else { // not in HTM
        __sync_lock_release(&l->val);
	}
	return 0;
}

// ticket lock =========================
//

struct _ticket_lock_t {
	volatile uint32_t next;
	volatile uint32_t now;
} __attribute__((__packed__));

typedef struct _ticket_lock_t ticket_lock_t;

static int ticket_lock(ticket_lock_t *l) {
    TM_STATS_ADD(my_tm_stats.locks, 1);
    uint32_t my_ticket = __sync_fetch_and_add(&l->next, 1);
    while (my_ticket != l->now) {
        uint32_t dist = my_ticket - l->now;
        spin_wait(16*dist);
    }
    TM_STATS_SUB(my_tm_stats.cycles, rdtsc());
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
    TM_STATS_ADD(my_tm_stats.cycles, rdtsc());
    return 0;
}

// ticket lock TM =========================
//
//
static int ticket_lock_tm(ticket_lock_t *l) {
    if (spec_lock)
        return 0;

    TM_STATS_ADD(my_tm_stats.locks, 1);
    uint32_t tries = 0;
    uint32_t my_ticket = __sync_fetch_and_add(&l->next, 1);
    while (my_ticket != l->now) {
        uint32_t dist = my_ticket - l->now;
        if (dist <= TK_MAX_DISTANCE && dist >= TK_MIN_DISTANCE && tries < TK_NUM_TRIES) {
            TM_STATS_ADD(my_tm_stats.tries, 1);
            TM_STATS_SUB(my_tm_stats.tm_cycles, rdtsc());
            spec_lock = l;
            int ret;
            if ((ret = HTM_SIMPLE_BEGIN()) == HTM_SUCCESSFUL) {
                // TODO: we should self abort if it's my turn
                // but where to check l->now in the txn?
                return 0;
            }
            // abort
            TM_STATS_ADD(my_tm_stats.tm_cycles, rdtsc());
            if (HTM_IS_CONFLICT(ret))
                TM_STATS_ADD(my_tm_stats.conflicts, 1);
            else if (HTM_IS_OVERFLOW(ret))
                TM_STATS_ADD(my_tm_stats.overflows, 1);
            else // self aborts
                ;
            spec_lock = 0;
            spin_wait(8);
            tries++;
        } else {
            spin_wait(16*dist);
        }
    }
    TM_STATS_SUB(my_tm_stats.cycles, rdtsc());
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
        TM_STATS_ADD(my_tm_stats.cycles, rdtsc());
    }
    return 0;
}



// pthreads TM =========================
//

static int pthread_lock_tm(pthread_mutex_t *l) {
  if (spec_lock){return 0;}

  int tries = 0;
  while (libpthread_mutex_trylock((void*)l) != 0) {
		spin_wait(8);
		spec_lock = l;
		if (tries < HTM_SIMPLE_BEGIN() == HTM_SUCCESSFUL) {
			return 0;
		}
		spec_lock = 0;
		tries++;
  }
  return 0;
}

static int pthread_trylock_tm(pthread_mutex_t *l) {
    if (spec_lock) { // in htm
        // nothing
    } else { // not in HTM
        libpthread_mutex_trylock((void*)l);
    }
    return 0;
}

static int pthread_unlock_tm(pthread_mutex_t *l) {
    if (spec_lock) { // in htm
       //if (spec_lock == l) {
       // HTM_ABORT(7);
       //}
    } else { // not in HTM
        libpthread_mutex_unlock((void*)l);
    }
    return 0;
}

// queue lock ================================
struct _mcs_lock_t;

struct _mcs_node_t{
	struct _mcs_node_t* volatile lock_next;
	volatile bool wait;
	volatile bool speculate;
	volatile uint64_t cnt;
	struct _mcs_lock_t* lock;
	struct _mcs_node_t* list_next;
	struct _mcs_node_t* list_prev;
} __attribute__((__packed__));

typedef struct _mcs_node_t mcs_node_t;

static __thread mcs_node_t* my_free_nodes = NULL;
static __thread mcs_node_t* my_used_nodes = NULL;

struct _mcs_lock_t {
  volatile mcs_node_t* tail;
	volatile long now_serving;
} __attribute__((__packed__));

typedef struct _mcs_lock_t mcs_lock_t;

static void alloc_more_nodes(){
	const int NUM_NODES = 8;
	mcs_node_t* nodes = malloc(sizeof(mcs_node_t)*NUM_NODES);
	assert(nodes!=NULL);
	for(int i = 0; i<NUM_NODES; i++){
		nodes[i].list_next = &nodes[i+1];
		nodes[i].list_prev=NULL;
		nodes[i].wait = true;
		nodes[i].speculate = true;
		nodes[i].lock = NULL;
		nodes[i].lock_next=NULL;
		nodes[i].cnt = 0;
	}
	nodes[NUM_NODES-1].list_next=NULL;
	my_free_nodes = nodes;
}

static int inline mcs_lock_common(mcs_lock_t *lk, bool try_lock, bool tm) {
  if (spec_lock){return 0;}

	// get a free node
	mcs_node_t* mine;
	mine = my_free_nodes;
	if(mine == NULL){
		alloc_more_nodes();
		mine = my_free_nodes;
		assert(mine!=NULL);
	}

  // init my qnode
  mine->lock_next = NULL;
	mine->lock = lk;
	mine->wait = true;

	// then swap it into the root pointer
	mcs_node_t* pred = NULL;
	if(try_lock){
		if(!__sync_bool_compare_and_swap(&lk->tail, NULL, mine)){
			return 1; // return failure
		}
	}
	else{
	  pred = (mcs_node_t*)__sync_lock_test_and_set(&lk->tail, mine);
	}

	// we know we'll use the node, so allocate it
	// by moving node off free list and to used list
	my_free_nodes = mine->list_next;
	mine->list_next = my_used_nodes;
	if(my_used_nodes!=NULL){my_used_nodes->list_prev = mine;}
	my_used_nodes = mine;
	mine->list_prev = NULL;

	// now set my flag, point pred to me, and wait for my flag to be unset
	if (pred != NULL) {
		puts("MCS");
		if(!tm){
			pred->lock_next = mine;
			__sync_synchronize(); // is this barrier needed?
			while (mine->wait) {} // spin
		}
		else{
			puts("TM");
			// finish enqueing
			pred->lock_next = mine;
			while(pred->cnt==0){} // wait for predecessor to get its count
			__sync_synchronize(); // is this barrier needed?
			long cnt = pred->cnt+1;
			mine->cnt = cnt;
			__sync_synchronize(); // is this barrier needed?

			// decide whether to speculate
			long now_serving_copy = lk->now_serving;
			if(now_serving_copy<cnt-TK_MIN_DISTANCE && 
			 now_serving_copy>cnt-TK_MAX_DISTANCE &&
			 spec_lock==NULL){
				spec_lock = lk;
				if (HTM_SIMPLE_BEGIN() == HTM_SUCCESSFUL) {
					if(mine->speculate!=true || mine->wait!=true){
						HTM_ABORT(0);
					}					
					else{return 0;}
				}
				spec_lock=NULL;
			}
			// finished speculating
		
			// actually acquire the lock
			while (mine->wait) {}
			__sync_synchronize(); // is this barrier needed?
			assert(lk->now_serving == cnt-1);
			lk->now_serving = cnt;
		}
	}
	else{
		if(tm){
			mine->cnt=lk->now_serving+1;
			lk->now_serving++;
		}
	}

	return 0; // return success
}

static int mcs_lock(mcs_lock_t *lk) {
	return mcs_lock_common(lk,false,false);
}


static int mcs_trylock(mcs_lock_t *lk) {
	return mcs_lock_common(lk,true,false);
}


static inline int mcs_unlock_common(mcs_lock_t *lk, bool tm) {

	// traverse used list to find node
	// (assumes we never hold a lot of locks at once)
	mcs_node_t* mine = my_used_nodes;
	assert(mine!=NULL);
	while(mine->lock!=lk){
		mine = mine->list_next;
		assert(mine!=NULL);
	}

	// if my node is the only one, then if I can zero the lock, do so and I'm
	// done
	if (mine->lock_next == NULL) {
		if (__sync_bool_compare_and_swap(&lk->tail, mine, NULL))
			return 0;
		// uh-oh, someone arrived while I was zeroing... wait for arriver to
		// initialize, fall out to other case
		while (mine->lock_next == NULL) { } // spin
	}

	// halt speculators
	if(tm){
		mcs_node_t* current = mine->lock_next;
		int dist = 1;
		while(current!=NULL){
			if(dist>=TK_MIN_DISTANCE){
				current->speculate = false;
			}
			if(dist>TK_MAX_DISTANCE){break;}
			current = current->lock_next;
			dist++;
		}
	}

	// if someone is waiting on me; set their flag to let them start
	mine->lock_next->wait = false;
		

	// move node out of used list
	mine->lock_next = NULL;
	if(mine->list_prev!=NULL){mine->list_prev->list_next = mine->list_next;}
	else{my_used_nodes->list_next = mine->list_next;}

	// and onto free list
	mine->list_next = my_free_nodes;
	my_free_nodes = mine;

	return 0;
}

static int mcs_unlock(mcs_lock_t *lk) {
	return mcs_unlock_common(lk,false);
}


static int mcs_lock_tm(mcs_lock_t *lk) {
	return mcs_lock_common(lk,false,true);
}


static int mcs_trylock_tm(mcs_lock_t *lk) {
	return mcs_lock_common(lk,true,true);
}

static int mcs_unlock_tm(mcs_lock_t *lk) {
	if(!spec_lock){return mcs_unlock_common(lk,true);}
	else{return 0;}
}


// function dispatch =========================
//

struct _lock_type_t {
    const char *name;
    int lock_size;
    txlock_func_t lock_fun;
    txlock_func_t trylock_fun;
    txlock_func_t unlock_fun;
};
typedef struct _lock_type_t lock_type_t;

static lock_type_t lock_types[] = {
    {"pthread",     sizeof(pthread_mutex_t), NULL, NULL, NULL},
    {"pthread_tm",  sizeof(pthread_mutex_t), (txlock_func_t)pthread_lock_tm, (txlock_func_t)pthread_trylock_tm, (txlock_func_t)pthread_unlock_tm},
    {"tas",         sizeof(tas_lock_t), (txlock_func_t)tas_lock, (txlock_func_t)tas_trylock, (txlock_func_t)tas_unlock},
    {"tas_tm",      sizeof(tas_lock_t), (txlock_func_t)tas_lock_tm, (txlock_func_t)tas_trylock_tm, (txlock_func_t)tas_unlock_tm},
//    {"tas_hle",     sizeof(tas_lock_t), (txlock_func_t)tas_lock_hle, (txlock_func_t)tas_trylock, (txlock_func_t)tas_unlock_hle},
    {"ticket",      sizeof(ticket_lock_t), (txlock_func_t)ticket_lock, (txlock_func_t)ticket_trylock, (txlock_func_t)ticket_unlock},
    {"ticket_tm",   sizeof(ticket_lock_t), (txlock_func_t)ticket_lock_tm, (txlock_func_t)ticket_trylock_tm, (txlock_func_t)ticket_unlock_tm},
    {"mcs",   sizeof(mcs_lock_t), (txlock_func_t)mcs_lock, (txlock_func_t)mcs_trylock, (txlock_func_t)mcs_unlock},
    {"mcs_tm",   sizeof(mcs_lock_t), (txlock_func_t)mcs_lock_tm, (txlock_func_t)mcs_trylock_tm, (txlock_func_t)mcs_unlock_tm}
};

static lock_type_t *using_lock_type = &lock_types[2];

// Dynamically find the libpthread implementations
// and store them before replacing them
static void setup_pthread_funcs() {
    char *error;
    void *handle = dlopen(LIBPTHREAD_PATH, RTLD_LAZY);
    if (!handle) {
       fputs (dlerror(), stderr);
       exit(1);
    }
    libpthread_handle = handle;

		// Find libpthread methods
    libpthread_mutex_lock = (txlock_func_t)dlsym(handle, "pthread_mutex_lock");
    libpthread_mutex_trylock = (txlock_func_t)dlsym(handle, "pthread_mutex_trylock");
    libpthread_mutex_unlock = (txlock_func_t)dlsym(handle, "pthread_mutex_unlock");

		// and store them in the lock_types array
    lock_types[0].lock_fun = libpthread_mutex_lock;
    lock_types[0].trylock_fun = libpthread_mutex_trylock;
    lock_types[0].unlock_fun = libpthread_mutex_unlock;

    // handler for pthread_exit
    libpthread_exit = (void (*)(void *))dlsym(handle, "pthread_exit");

    if ((error = dlerror()) != NULL)  {
        fputs(error, stderr);
        exit(1);
    }
}

__attribute__((constructor(201)))  // after tl-pthread.so
static void init_lib_txlock() {
    setup_pthread_funcs();

		// determine lock type
    const char *type = getenv("LIBTXLOCK_LOCK");
    if (type) {
        for (size_t i=0; i<sizeof(lock_types)/sizeof(lock_type_t); i++) {
            if (strcmp(type, lock_types[i].name) == 0) {
                using_lock_type = &lock_types[i];
                break;
            }
        }
    }

		// set appropriate dispatching functions
    func_tl_lock = using_lock_type->lock_fun;
    func_tl_trylock = using_lock_type->trylock_fun;
    func_tl_unlock = using_lock_type->unlock_fun;

		// read auxiliary arguments
		char* env;
		env = getenv("LIBTXLOCK_MAX_DISTANCE");
    env?TK_MAX_DISTANCE=atoi(env):2;
		env = getenv("LIBTXLOCK_MIN_DISTANCE");
    env?TK_MIN_DISTANCE=atoi(env):0;
		env = getenv("LIBTXLOCK_NUM_TRIES");
    env?TK_NUM_TRIES=atoi(env):4;

	    // notify user of arguments
    fprintf(stderr, "LIBTXLOCK_LOCK: %s\n", using_lock_type->name);
    fflush(stderr);
}


void __tl_pthread_exit(void *retval)
{
    // sync local stats to the global stats
    __sync_fetch_and_add(&tm_stats.cycles, my_tm_stats.cycles);
    __sync_fetch_and_add(&tm_stats.tm_cycles, my_tm_stats.tm_cycles);
    __sync_fetch_and_add(&tm_stats.locks, my_tm_stats.locks);
    __sync_fetch_and_add(&tm_stats.tries, my_tm_stats.tries);
    __sync_fetch_and_add(&tm_stats.stops, my_tm_stats.stops);
    __sync_fetch_and_add(&tm_stats.overflows, my_tm_stats.overflows);
    __sync_fetch_and_add(&tm_stats.conflicts, my_tm_stats.conflicts);
    // call real pthread function
    libpthread_exit(retval);
}


__attribute__((destructor))
static void uninit_lib_txlock()
{
    fprintf(stderr, "LIBTXLOCK_LOCK: %s", using_lock_type->name);
    if (tm_stats.tries) {
        fprintf(stderr, ", avg_lock_cycles: %d, locks: %d, avg_tm_cycles: %d, tm_tries: %d, overflows: %d, conflicts: %d, stops: %d",
                        (int)(tm_stats.cycles/tm_stats.locks), tm_stats.locks,
                        (int)(tm_stats.tm_cycles/tm_stats.tries), tm_stats.tries,
                        tm_stats.overflows, tm_stats.conflicts, tm_stats.stops);
    }
    fprintf(stderr, "\n");
    fflush(stderr);

    if (libpthread_handle)
        dlclose(libpthread_handle);
}


/*

static int replace_libpthread = 0;
void tl_replace_libpthread(int r) {replace_libpthread=r;}

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

typedef struct _txcond_t _txcond_t;

static int _tc_waitcommon(txcond_t* cond_var, txlock_t* lk, bool timed, const struct timespec *abs_timeout){
	int e;

	_txcond_t* cv = (_txcond_t*)cond_var;

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
	tl_unlock(lk);



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




int tc_signal(txcond_t* cond_var){
	int e, i;
	txcond_node_t* node;
	_txcond_t* cv;

	cv = (_txcond_t*)cond_var;
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

int tc_broadcast(txcond_t* cond_var){

	//printf("tc-broadcast cv:%x ilk:%x\n",cv,&cv->lk);

	_txcond_t* cv;
	int e1, e2;
	txcond_node_t* node;
	txcond_node_t* prev_node;
	cv = (_txcond_t*)cond_var;

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



#ifndef TXUTIL_H
#define TXUTIL_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>

// for cond vars
#include <time.h>

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

    inline uint64_t rdtsc() { return __rdtsc(); }

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
    uint32_t threads;     // number of threads
} __attribute__ ((aligned(64))) tm_stats_t;

// initialized in txutil.c
extern __thread tm_stats_t my_tm_stats; // thread-local stats
extern tm_stats_t tm_stats;             // global stats, updated only when a thread exits

#ifdef TM_NO_PROFILING
#define TM_STATS_ADD(stat, value)
#define TM_STATS_SUB(stat, value)
#else
#define TM_STATS_ADD(stat, value) ((stat)+=(value))
#define TM_STATS_SUB(stat, value) ((stat)-=(value))
#endif


// ASM optimized synchronization
#if defined(__powerpc__) || defined(__powerpc64__)
    //#define HMT_very_low()   __asm volatile("or 31,31,31   # very low priority")
    //static inline void cpu_relax() { HMT_very_low(); }
    inline void cpu_relax() { __asm volatile("nop\n": : :"memory"); }
#else
    inline void cpu_relax() { __asm volatile("pause\n": : :"memory"); }
#endif
// initialized in txutil.c
extern int SPIN_INIT;
extern int SPIN_CELL;
extern float SPIN_FACTOR;
inline int spin_begin() { return SPIN_INIT; }
inline int spin_wait(int s) {
    for (int i=0; i<s; i++)
        cpu_relax();
    int n = SPIN_FACTOR * s;
    return n > SPIN_CELL ? SPIN_CELL : n;
}


// lock for utility purposes

struct _utility_lock_t {
    volatile int32_t val;
    volatile int32_t cnt;
} __attribute__((__packed__));

typedef struct _utility_lock_t utility_lock_t;
int inline ul_lock(utility_lock_t *lk) {
    if (lk->val || __sync_lock_test_and_set(&(lk->val), 1)) {
        int s = spin_begin();
        do {
            s = spin_wait(s);
        } while (lk->val || __sync_lock_test_and_set(&(lk->val), 1));
    }
    return 0;
}
int inline ul_unlock(utility_lock_t *lk) {
    __sync_lock_release(&lk->val);
    return 0;
}


// State for HTM speculation (initialized in txutil.c)
extern __thread void * volatile spec_entry;

#endif
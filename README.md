## Use of the libraries:

### libtxlock.[a|so]

If a program has its own lock/cond_var implementation, you can replace
lock/unlock with  tl_*, tc_* functions in txlock.h.

By setting `LIBTXLOCK` env variable, you can specify the type of lock you want
to use for txlock.
Current options (in txlock.c) are:
- tas: basic tatas lock
- tas_tm: tatas lock with prefetching
- ticket & ticket_tm: ticket lock and its prefetching version
- pthread & pthread_tm: system pthread lock and its prefetching version
For example:
```
export LIBTXLOCK=tas_tm
appA
export LIBTXLOCK=pthread
appB
```
appA will then use tas_tm; appB will use pthread lock.


### tl-pthread.so:

Assuming app.bin is a program compiled with pthread library, running app.bin
with commandline: `LD_PRELOAD=path/tl-pthread.so app.bin args` will dynamicly 
replace the following pthread functions with ours (in tl-pthread.h):
- pthread_mutex_*
- pthread_cond_*
Other pthread functions are not affected.

Again, you can specify the type of txlock via `LIBTXLOCK` env variable.


LIBTXLOCK_DIR=/u/lxiang/projects/txlock
LIBTXLOCK_CFLAGS=-isystem $(LIBTXLOCK_DIR)/include -I$(LIBTXLOCK_DIR) 
LIBTXLOCK_CCFLAGS=$(LIBTXLOCK_CFLAGS)
LIBTXLOCK_CXXFLAGS=$(LIBTXLOCK_CCFLAGS)
LIBTXLOCK_LDFLAGS=-L$(LIBTXLOCK_DIR) -Wl,-rpath -Wl,$(LIBTXLOCK_DIR) -ltxlock
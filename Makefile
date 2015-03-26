LIBSYNC_PATH=/u/lxiang/projects/libsync
LIBHTM_PATH=/u/lxiang/projects/libhtm
CFLAGS= -g -std=c11 -O3 -mrtm -I$(LIBSYNC_PATH) -I$(LIBHTM_PATH)

all: libtxlock.so libtxlock.a tl-pthread.so

libtxlock.so: txlock.so
	gcc -shared txlock.so -ldl -o $@

libtxlock.a: txlock.o
	ar rcs $@ txlock.o

tl-pthread.so: tl-pthread.c txlock.so
	gcc $(CFLAGS) -fPIC -flto -c tl-pthread.c -o tl-pthread.o
	gcc -flto -shared tl-pthread.o txlock.so -ldl -o $@

txlock.so: txlock.c txlock.h	
	gcc $(CFLAGS) -fPIC -flto -c txlock.c -o txlock.so

txlock.o: txlock.c txlock.h	
	gcc $(CFLAGS) -c -flto txlock.c -o txlock.o

clean:
	$(RM) *.o *.so *.a

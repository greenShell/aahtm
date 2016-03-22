include config.mk

LIBSYNC_PATH=../libsync
LIBHTM_PATH=../libhtm
CFLAGS = -g -std=c11 -O2 -mrtm $(LIBTXLOCK_CFLAGS) -I$(LIBSYNC_PATH) -I$(LIBHTM_PATH) -D_POSIX_C_SOURCE=200112L

all: libtxlock.a tl-pthread.so test libtxlock.so

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

test: test.c
	gcc $(CFLAGS) $(LIBTXLOCK_CFLAGS) test.c $(LIBTXLOCK_LDFLAGS) -o $@

clean:
	$(RM) *.o *.so *.a

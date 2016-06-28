include config.mk

CFLAGS = -g -std=c11 -O3 -mrtm $(LIBTXLOCK_CFLAGS) -D_POSIX_C_SOURCE=200112L

all: tl-pthread.so libtxlock.so libtxlock.a

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

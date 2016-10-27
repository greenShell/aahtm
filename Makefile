include config.mk

CFLAGS = -g -std=c11 -O3 -mrtm $(LIBTXLOCK_CFLAGS) -D_POSIX_C_SOURCE=200112L



all: tl-pthread.so libtxlock.so libtxlock.a

libtxlock.so: txlock.s.o txcond.s.o txutil.s.o pthread_cond.s.o
	gcc -shared $^ -ldl -o $@

libtxlock.a: txlock.o txcond.o txutil.o pthread_cond.o
	ar rcs $@ $^

tl-pthread.so: tl-pthread.s.o txlock.s.o txcond.s.o txutil.s.o pthread_cond.s.o
	gcc -flto -shared $^ -ldl -o $@

%.s.o: %.c txlock.h txutil.h txcond.h
	gcc $(CFLAGS) -fPIC -flto -c $< -o $@

%.o: %.c txlock.h txutil.h txcond.h
	gcc $(CFLAGS) -c -flto $< -o $@

clean:
	$(RM) *.o *.so *.a

all: a.out

run: all a.out
	export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:$(shell pwd -P)"
	LD_PRELOAD="$(shell pwd -P)/libruntime.so" ./a.out -p 3 10 20 30

a.out:
	clang -std=c99 -Wall -pedantic  *.c -L. -lruntime  
# –L. -lruntime 

tar:
	cd ..
	tar cfvz pa2.tar.gz pa2

clear:
	rm -f a.out
	rm -f events.log
	rm -f pipes.log

%.c:
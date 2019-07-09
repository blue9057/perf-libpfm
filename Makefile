all: cache_count perf_util.o

cache_count: cache_count.c perf_util.o
	$(CC) -o $@ $< perf_util.o -lpfm

perf_util.o: perf_util.c
	$(CC) -o $@ $< -c -O2

clean:
	rm -rf cache_count perf_util.o

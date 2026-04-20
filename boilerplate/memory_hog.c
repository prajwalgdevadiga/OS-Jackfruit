/*
 * memory_hog.c - Memory pressure workload for soft/hard limit testing.
 * Usage: ./memory_hog [chunk_mb] [sleep_ms]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    size_t chunk_mb = (argc>1 && atol(argv[1])>0) ? (size_t)atol(argv[1]) : 8;
    useconds_t sleep_us = (argc>2) ? (useconds_t)(atol(argv[2])*1000U) : 1000U*1000U;
    size_t chunk_bytes=chunk_mb*1024U*1024U; int count=0;
    while (1) {
        char *mem=malloc(chunk_bytes);
        if (!mem) { printf("malloc failed after %d allocations\n",count); break; }
        memset(mem,'A',chunk_bytes); count++;
        printf("allocation=%d chunk=%zuMB total=%zuMB\n",count,chunk_mb,(size_t)count*chunk_mb);
        fflush(stdout); usleep(sleep_us);
    }
    return 0;
}

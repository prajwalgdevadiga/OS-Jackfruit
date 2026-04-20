/*
 * cpu_hog.c - CPU-bound workload for scheduler experiments.
 * Usage: ./cpu_hog [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char *argv[]) {
    unsigned int duration = (argc>1 && atoi(argv[1])>0) ? (unsigned)atoi(argv[1]) : 10;
    time_t start=time(NULL), last=start;
    volatile unsigned long long acc=0;
    while ((unsigned)(time(NULL)-start)<duration) {
        acc=acc*1664525ULL+1013904223ULL;
        if (time(NULL)!=last) {
            last=time(NULL);
            printf("cpu_hog elapsed=%ld acc=%llu\n",(long)(last-start),acc);
            fflush(stdout);
        }
    }
    printf("cpu_hog done duration=%u acc=%llu\n",duration,acc);
    return 0;
}

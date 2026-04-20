/*
 * io_pulse.c - I/O-oriented workload for scheduler experiments.
 * Usage: ./io_pulse [iterations] [sleep_ms]
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_OUTPUT "/tmp/io_pulse.out"

int main(int argc, char *argv[]) {
    unsigned int iters = (argc>1 && atoi(argv[1])>0) ? (unsigned)atoi(argv[1]) : 20;
    unsigned int sleep_ms = (argc>2 && atoi(argv[2])>0) ? (unsigned)atoi(argv[2]) : 200;
    int fd=open(DEFAULT_OUTPUT,O_CREAT|O_WRONLY|O_TRUNC,0644);
    if (fd<0) { perror("open"); return 1; }
    for (unsigned int i=0;i<iters;i++) {
        char line[128];
        int len=snprintf(line,sizeof(line),"io_pulse iteration=%u\n",i+1);
        if (write(fd,line,(size_t)len)!=len) { perror("write"); close(fd); return 1; }
        fsync(fd);
        printf("io_pulse wrote iteration=%u\n",i+1); fflush(stdout);
        usleep(sleep_ms*1000U);
    }
    close(fd); return 0;
}

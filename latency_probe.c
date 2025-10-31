// latency_probe.c
// Compile: gcc -O2 latency_probe.c -o latency_probe -lrt
#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>

static volatile int running = 1;
void handle(int s){ running = 0; }

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <sleep_us> <duration_sec> <out_csv>\n", argv[0]);
        return 1;
    }
    int sleep_us = atoi(argv[1]);
    int duration = atoi(argv[2]);
    const char *out = argv[3];

    signal(SIGINT, handle);
    struct timespec req;
    req.tv_sec = sleep_us / 1000000;
    req.tv_nsec = (sleep_us % 1000000) * 1000;

    FILE *f = fopen(out, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fprintf(f, "#time_s,expected_us,actual_us,delay_us\n");
    fflush(f);

    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    double end_time = before.tv_sec + before.tv_nsec/1e9 + duration;

    while (running) {
        clock_gettime(CLOCK_MONOTONIC, &before);
        nanosleep(&req, NULL);
        clock_gettime(CLOCK_MONOTONIC, &after);

        double expected = (double)sleep_us;
        double actual = (after.tv_sec - before.tv_sec) * 1e6 + (after.tv_nsec - before.tv_nsec) / 1e3;
        double tnow = after.tv_sec + after.tv_nsec/1e9;
        double delay = actual - expected;
        fprintf(f, "%.6f,%.3f,%.3f,%.3f\n", tnow, expected, actual, delay);
        fflush(f);
        if (tnow >= end_time) break;
    }

    fclose(f);
    return 0;
}

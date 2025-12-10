#define _POSIX_C_SOURCE 199309L





#include "timer.h"

#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <stddef.h>



double wall_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void get_cpu_times(double *user_sec, double *sys_sec) {
    if (!user_sec || !sys_sec) return;

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);

    *user_sec = (double)ru.ru_utime.tv_sec +
                (double)ru.ru_utime.tv_usec * 1e-6;
    *sys_sec  = (double)ru.ru_stime.tv_sec +
                (double)ru.ru_stime.tv_usec * 1e-6;
}

uint64_t read_tsc() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL +
           (uint64_t)ts.tv_nsec;
#endif
}

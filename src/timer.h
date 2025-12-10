#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include <stddef.h>
/**
 * Monotonic wall clock time in seconds (high resolution).
 */
double wall_time();

/**
 * Get CPU user and system times (in seconds) for the current process.
 */
void get_cpu_times(double *user_sec, double *sys_sec);

/**
 * Read CPU cycle counter (TSC on x86). On non-x86, falls back to
 * a nanosecond-based monotonic clock.
 */
uint64_t read_tsc();

#endif // TIMER_H
 

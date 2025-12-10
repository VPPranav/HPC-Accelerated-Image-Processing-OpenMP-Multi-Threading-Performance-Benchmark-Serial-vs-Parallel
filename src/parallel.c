#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>
#include <omp.h>
#include <strings.h>
#include <time.h>

#include "filters.h"
#include "timer.h"

/*
 * ABOUT cpu_cycles AND "PERF-LIKE" TOTAL CYCLES
 * ---------------------------------------------
 *
 * We measure cpu_cycles with read_tsc() from timer.c:
 *
 *     uint64_t c_start = read_tsc();
 *     ... work ...
 *     uint64_t c_end   = read_tsc();
 *     cpu_cycles = c_end - c_start;
 *
 * This is the Time Stamp Counter (TSC) delta for the *whole program run*:
 *
 *   cpu_cycles_TSC ≈ f_core * wall_time_sec
 *
 * where f_core is the (approximate) core frequency, and wall_time_sec
 * is the real elapsed time.
 *
 * For SERIAL:
 *   - Only one core is effectively busy, so
 *       total_cycles_all_threads ≈ cpu_cycles_TSC.
 *
 * For PARALLEL:
 *   - Many cores are busy, but TSC is still tied to *time on one core*.
 *   - So cpu_cycles_TSC remains proportional to wall time only.
 *   - It does NOT equal "sum of cycles over all threads".
 *
 * To get something close to what `perf stat -e cycles` reports
 * ("total cycles used by all threads"), we can combine:
 *
 *   cpu_cycles_TSC      ~ f_core * wall_time
 *   cpu_total_time_sec  = cpu_user_time_sec + cpu_system_time_sec
 *                        (this is sum of CPU time across all threads)
 *
 * If we estimate:
 *
 *   f_core_hat = cpu_cycles_TSC / wall_time_sec
 *
 * then:
 *
 *   estimated_total_cycles_all_threads
 *       ≈ f_core_hat * cpu_total_time_sec
 *       = cpu_cycles_TSC * (cpu_total_time_sec / wall_time_sec)
 *
 * This is very close in spirit to what `perf stat` gives for cycles.
 *
 * IMPORTANT:
 *   - We still keep cpu_cycles (TSC delta) as a "time-like" metric.
 *   - We *add* a new derived metric:
 *         estimated_total_cycles_all_threads
 *     which you can interpret as "perf-like total cycles".
 *   - DO NOT just multiply TSC by number_of_threads; that's incorrect.
 */

typedef struct {
    int      images_processed;
    long long total_pixels;

    double   wall_time_sec;
    double   cpu_user_time_sec;
    double   cpu_system_time_sec;

    double   avg_time_per_image_ms;
    double   avg_time_per_pixel_ns;

    uint64_t cpu_cycles;       // TSC ticks over the whole run (wall-clock based)
    double   cycles_per_image;
    double   cycles_per_pixel;

    // Derived "perf-like" estimate:
    // total cycles across all threads, calibrated using TSC + CPU time.
    uint64_t estimated_total_cycles_all_threads;
    double   estimated_cycles_per_image_all_threads;
    double   estimated_cycles_per_pixel_all_threads;

    int      max_width;
    int      max_height;
    int      threads_used;
} Metrics;

typedef struct {
    double speedup_wall_time;
    double speedup_cpu_user;
    double speedup_cpu_system;
    double speedup_pixels_per_sec;
    double parallel_efficiency;

    // For reporting in compare_metrics.json
    uint64_t serial_est_total_cycles_all_threads;
    uint64_t parallel_est_total_cycles_all_threads;
} Comparison;

static int ends_with(const char *name, const char *ext) {
    size_t ln = strlen(name);
    size_t le = strlen(ext);
    if (ln < le) return 0;
    return strcasecmp(name + ln - le, ext) == 0;
}

static int is_image_file(const char *name) {
    return ends_with(name, ".png")  ||
           ends_with(name, ".jpg")  ||
           ends_with(name, ".jpeg") ||
           ends_with(name, ".bmp");
}

static void ensure_directory(const char *path) {
    if (!path) return;
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return;
        fprintf(stderr, "[parallel] %s exists but is not a directory!\n", path);
        return;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        perror("[parallel] mkdir");
    }
}

/*
 * Write metrics for the parallel variant to JSON.
 * These include:
 *   - TSC-based cpu_cycles
 *   - Derived estimated_total_cycles_all_threads (perf-like)
 */
static void write_parallel_metrics_json(const char *json_path,
                                        const Metrics *m,
                                        const char *input_dir,
                                        const char *output_dir) {
    ensure_directory("results");
    ensure_directory("results/logs");

    FILE *f = fopen(json_path, "w");
    if (!f) {
        perror("[parallel] fopen metrics json");
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"variant\": \"parallel\",\n");
    fprintf(f, "  \"input_dir\": \"%s\",\n", input_dir);
    fprintf(f, "  \"output_dir\": \"%s\",\n", output_dir);
    fprintf(f, "  \"metrics\": {\n");
    fprintf(f, "    \"images_processed\": %d,\n", m->images_processed);
    fprintf(f, "    \"total_pixels\": %lld,\n", m->total_pixels);
    fprintf(f, "    \"wall_time_sec\": %.9f,\n", m->wall_time_sec);
    fprintf(f, "    \"cpu_user_time_sec\": %.9f,\n", m->cpu_user_time_sec);
    fprintf(f, "    \"cpu_system_time_sec\": %.9f,\n", m->cpu_system_time_sec);
    fprintf(f, "    \"avg_time_per_image_ms\": %.6f,\n", m->avg_time_per_image_ms);
    fprintf(f, "    \"avg_time_per_pixel_ns\": %.6f,\n", m->avg_time_per_pixel_ns);
    fprintf(f, "    \"cpu_cycles_tsc\": %llu,\n",
            (unsigned long long)m->cpu_cycles);
    fprintf(f, "    \"cycles_per_image_tsc\": %.3f,\n", m->cycles_per_image);
    fprintf(f, "    \"cycles_per_pixel_tsc\": %.3f,\n", m->cycles_per_pixel);
    fprintf(f, "    \"estimated_total_cycles_all_threads\": %llu,\n",
            (unsigned long long)m->estimated_total_cycles_all_threads);
    fprintf(f, "    \"estimated_cycles_per_image_all_threads\": %.3f,\n",
            m->estimated_cycles_per_image_all_threads);
    fprintf(f, "    \"estimated_cycles_per_pixel_all_threads\": %.3f,\n",
            m->estimated_cycles_per_pixel_all_threads);
    fprintf(f, "    \"max_width\": %d,\n", m->max_width);
    fprintf(f, "    \"max_height\": %d,\n", m->max_height);
    fprintf(f, "    \"threads_used\": %d\n", m->threads_used);
    fprintf(f, "  }\n");
    fprintf(f, "}\n");

    fclose(f);
    printf("[parallel] Metrics written to %s\n", json_path);
}

/* --- naive JSON parsing helpers to read serial_metrics.json --- */

static double extract_double(const char *buf, const char *key) {
    const char *pos = strstr(buf, key);
    if (!pos) return 0.0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0.0;
    const char *num = colon + 1;
    while (*num == ' ' || *num == '\t') num++;
    return atof(num);
}

static long long extract_ll(const char *buf, const char *key) {
    const char *pos = strstr(buf, key);
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    const char *num = colon + 1;
    while (*num == ' ' || *num == '\t') num++;
    return atoll(num);
}

static int extract_int(const char *buf, const char *key) {
    return (int)extract_ll(buf, key);
}

/*
 * Load metrics from serial_metrics.json, written by the serial version.
 * We assume it contains at least:
 *   images_processed
 *   total_pixels
 *   wall_time_sec
 *   cpu_user_time_sec
 *   cpu_system_time_sec
 *   cpu_cycles  (TSC-based)
 * and related averages.
 */
static int load_serial_metrics(const char *json_path, Metrics *out) {
    FILE *f = fopen(json_path, "r");
    if (!f) {
        perror("[parallel] fopen serial_metrics.json");
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        fclose(f);
        fprintf(stderr, "[parallel] Out of memory reading serial_metrics.json\n");
        return 0;
    }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    memset(out, 0, sizeof(*out));

    out->images_processed        = extract_int   (buf, "\"images_processed\"");
    out->total_pixels            = extract_ll    (buf, "\"total_pixels\"");
    out->wall_time_sec           = extract_double(buf, "\"wall_time_sec\"");
    out->cpu_user_time_sec       = extract_double(buf, "\"cpu_user_time_sec\"");
    out->cpu_system_time_sec     = extract_double(buf, "\"cpu_system_time_sec\"");
    out->avg_time_per_image_ms   = extract_double(buf, "\"avg_time_per_image_ms\"");
    out->avg_time_per_pixel_ns   = extract_double(buf, "\"avg_time_per_pixel_ns\"");
    // serial JSON may have key "cpu_cycles" or "cpu_cycles_tsc";
    // try both for robustness.
    long long tmp_cycles = extract_ll(buf, "\"cpu_cycles_tsc\"");
    if (tmp_cycles == 0)
        tmp_cycles = extract_ll(buf, "\"cpu_cycles\"");
    out->cpu_cycles              = (uint64_t)tmp_cycles;
    out->cycles_per_image        = extract_double(buf, "\"cycles_per_image\"");
    out->cycles_per_pixel        = extract_double(buf, "\"cycles_per_pixel\"");
    out->max_width               = extract_int   (buf, "\"max_width\"");
    out->max_height              = extract_int   (buf, "\"max_height\"");

    free(buf);
    return 1;
}

/*
 * Write compare_metrics.json, including:
 *   - classic speedups
 *   - parallel efficiency
 *   - pixels/sec
 *   - CPU utilization
 *   - estimated_total_cycles_all_threads for both serial & parallel
 */
static void write_compare_json(const char *json_path,
                               const Metrics *serial,
                               const Metrics *parallel,
                               const Comparison *cmp) {
    ensure_directory("results");
    ensure_directory("results/logs");

    FILE *f = fopen(json_path, "w");
    if (!f) {
        perror("[parallel] fopen compare json");
        return;
    }

    // Pixels-per-second (throughput)
    double serial_pps   = 0.0;
    double parallel_pps = 0.0;
    if (serial->total_pixels > 0 && serial->wall_time_sec > 0.0)
        serial_pps = (double)serial->total_pixels / serial->wall_time_sec;
    if (parallel->total_pixels > 0 && parallel->wall_time_sec > 0.0)
        parallel_pps = (double)parallel->total_pixels / parallel->wall_time_sec;

    // CPU utilization (user+sys) / wall_time  (can be >1.0 for parallel)
    double serial_cpu_util   = 0.0;
    double parallel_cpu_util = 0.0;
    double serial_cpu_time   = serial->cpu_user_time_sec + serial->cpu_system_time_sec;
    double parallel_cpu_time = parallel->cpu_user_time_sec + parallel->cpu_system_time_sec;

    if (serial->wall_time_sec > 0.0)
        serial_cpu_util = serial_cpu_time / serial->wall_time_sec;
    if (parallel->wall_time_sec > 0.0)
        parallel_cpu_util = parallel_cpu_time / parallel->wall_time_sec;

    fprintf(f, "{\n");
    fprintf(f, "  \"comparison\": {\n");
    fprintf(f, "    \"speedup_wall_time\": %.6f,\n", cmp->speedup_wall_time);
    fprintf(f, "    \"speedup_cpu_user\": %.6f,\n", cmp->speedup_cpu_user);
    fprintf(f, "    \"speedup_cpu_system\": %.6f,\n", cmp->speedup_cpu_system);
    fprintf(f, "    \"speedup_pixels_per_sec\": %.6f,\n", cmp->speedup_pixels_per_sec);
    fprintf(f, "    \"parallel_efficiency\": %.6f,\n", cmp->parallel_efficiency);
    fprintf(f, "    \"serial_pixels_per_sec\": %.6f,\n", serial_pps);
    fprintf(f, "    \"parallel_pixels_per_sec\": %.6f,\n", parallel_pps);
    fprintf(f, "    \"serial_cpu_utilization\": %.6f,\n", serial_cpu_util);
    fprintf(f, "    \"parallel_cpu_utilization\": %.6f,\n", parallel_cpu_util);
    fprintf(f, "    \"serial_est_total_cycles_all_threads\": %llu,\n",
            (unsigned long long)cmp->serial_est_total_cycles_all_threads);
    fprintf(f, "    \"parallel_est_total_cycles_all_threads\": %llu\n",
            (unsigned long long)cmp->parallel_est_total_cycles_all_threads);
    fprintf(f, "  },\n");

    fprintf(f, "  \"serial\": {\n");
    fprintf(f, "    \"images_processed\": %d,\n", serial->images_processed);
    fprintf(f, "    \"total_pixels\": %lld,\n", serial->total_pixels);
    fprintf(f, "    \"wall_time_sec\": %.9f,\n", serial->wall_time_sec);
    fprintf(f, "    \"cpu_user_time_sec\": %.9f,\n", serial->cpu_user_time_sec);
    fprintf(f, "    \"cpu_system_time_sec\": %.9f,\n", serial->cpu_system_time_sec);
    fprintf(f, "    \"avg_time_per_image_ms\": %.6f,\n", serial->avg_time_per_image_ms);
    fprintf(f, "    \"avg_time_per_pixel_ns\": %.6f,\n", serial->avg_time_per_pixel_ns);
    fprintf(f, "    \"cpu_cycles_tsc\": %llu,\n",
            (unsigned long long)serial->cpu_cycles);
    fprintf(f, "    \"cycles_per_image_tsc\": %.3f,\n", serial->cycles_per_image);
    fprintf(f, "    \"cycles_per_pixel_tsc\": %.3f,\n", serial->cycles_per_pixel);
    fprintf(f, "    \"max_width\": %d,\n", serial->max_width);
    fprintf(f, "    \"max_height\": %d\n", serial->max_height);
    fprintf(f, "  },\n");

    fprintf(f, "  \"parallel\": {\n");
    fprintf(f, "    \"images_processed\": %d,\n", parallel->images_processed);
    fprintf(f, "    \"total_pixels\": %lld,\n", parallel->total_pixels);
    fprintf(f, "    \"wall_time_sec\": %.9f,\n", parallel->wall_time_sec);
    fprintf(f, "    \"cpu_user_time_sec\": %.9f,\n", parallel->cpu_user_time_sec);
    fprintf(f, "    \"cpu_system_time_sec\": %.9f,\n", parallel->cpu_system_time_sec);
    fprintf(f, "    \"avg_time_per_image_ms\": %.6f,\n", parallel->avg_time_per_image_ms);
    fprintf(f, "    \"avg_time_per_pixel_ns\": %.6f,\n", parallel->avg_time_per_pixel_ns);
    fprintf(f, "    \"cpu_cycles_tsc\": %llu,\n",
            (unsigned long long)parallel->cpu_cycles);
    fprintf(f, "    \"cycles_per_image_tsc\": %.3f,\n", parallel->cycles_per_image);
    fprintf(f, "    \"cycles_per_pixel_tsc\": %.3f,\n", parallel->cycles_per_pixel);
    fprintf(f, "    \"estimated_total_cycles_all_threads\": %llu,\n",
            (unsigned long long)parallel->estimated_total_cycles_all_threads);
    fprintf(f, "    \"estimated_cycles_per_image_all_threads\": %.3f,\n",
            parallel->estimated_cycles_per_image_all_threads);
    fprintf(f, "    \"estimated_cycles_per_pixel_all_threads\": %.3f,\n",
            parallel->estimated_cycles_per_pixel_all_threads);
    fprintf(f, "    \"max_width\": %d,\n", parallel->max_width);
    fprintf(f, "    \"max_height\": %d,\n", parallel->max_height);
    fprintf(f, "    \"threads_used\": %d\n", parallel->threads_used);
    fprintf(f, "  }\n");

    fprintf(f, "}\n");
    fclose(f);
    printf("[parallel] Comparison written to %s\n", json_path);
}

/*
 * Main parallel processing function.
 * - Collects file names
 * - Runs an OpenMP parallel for over images
 * - Measures time, CPU time, TSC
 * - Derives both TSC-based and perf-like cycle metrics
 */
static void process_directory_parallel(const char *input_dir,
                                       const char *output_dir,
                                       Metrics *metrics) {
    memset(metrics, 0, sizeof(*metrics));
    metrics->max_width  = 0;
    metrics->max_height = 0;

    ensure_directory(output_dir);

    DIR *dir = opendir(input_dir);
    if (!dir) {
        perror("[parallel] opendir input_dir");
        return;
    }

    // 1) Collect file names first (so OpenMP loop is clean)
    char **files = NULL;
    int file_count = 0;
    int capacity   = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (!is_image_file(ent->d_name))
            continue;

        if (file_count == capacity) {
            capacity = (capacity == 0) ? 16 : capacity * 2;
            files = (char **)realloc(files, capacity * sizeof(char *));
        }
        files[file_count] = strdup(ent->d_name);
        file_count++;
    }
    closedir(dir);

    if (file_count == 0) {
        printf("[parallel] No images found in %s\n", input_dir);
        free(files);
        return;
    }

    metrics->threads_used = omp_get_max_threads();

    // 2) Start timers and TSC
    double   user_before, sys_before, user_after, sys_after;
    get_cpu_times(&user_before, &sys_before);
    double   t_start = wall_time();
    uint64_t c_start = read_tsc();

    long long total_pixels = 0;
    int max_w = 0, max_h = 0;
    int images_processed = 0;

#pragma omp parallel for reduction(+:total_pixels,images_processed) reduction(max:max_w,max_h)
    for (int i = 0; i < file_count; ++i) {
        char in_path[512];
        char out_path[512];
        snprintf(in_path, sizeof(in_path), "%s/%s", input_dir, files[i]);
        snprintf(out_path, sizeof(out_path), "%s/%s", output_dir, files[i]);

        Image *img = load_image(in_path);
        if (!img) {
            fprintf(stderr, "[parallel] Skip failed load: %s\n", in_path);
            continue;
        }

        long long pixels = (long long)img->width * img->height;
        total_pixels     += pixels;
        images_processed += 1;

        if (img->width  > max_w) max_w = img->width;
        if (img->height > max_h) max_h = img->height;

        // Apply same pipeline as serial version
        apply_grayscale(img);
        apply_box_blur(img, 2);
        apply_sobel_edge(img);

        if (save_image_png(out_path, img) != 0) {
            fprintf(stderr, "[parallel] Failed to save %s\n", out_path);
        }

        free_image(img);
    }

    // 3) Stop timers and TSC
    uint64_t c_end = read_tsc();
    double   t_end = wall_time();
    get_cpu_times(&user_after, &sys_after);

    metrics->images_processed    = images_processed;
    metrics->total_pixels        = total_pixels;
    metrics->max_width           = max_w;
    metrics->max_height          = max_h;
    metrics->wall_time_sec       = t_end - t_start;
    metrics->cpu_user_time_sec   = user_after - user_before;
    metrics->cpu_system_time_sec = sys_after - sys_before;
    metrics->cpu_cycles          = c_end - c_start;  // TSC delta (wall-clock based)

    if (metrics->images_processed > 0) {
        metrics->avg_time_per_image_ms =
            (metrics->wall_time_sec * 1000.0) / metrics->images_processed;
        metrics->cycles_per_image =
            (double)metrics->cpu_cycles / metrics->images_processed;
    } else {
        metrics->avg_time_per_image_ms = 0.0;
        metrics->cycles_per_image      = 0.0;
    }

    if (metrics->total_pixels > 0) {
        metrics->avg_time_per_pixel_ns =
            (metrics->wall_time_sec * 1e9) / (double)metrics->total_pixels;
        metrics->cycles_per_pixel =
            (double)metrics->cpu_cycles / (double)metrics->total_pixels;
    } else {
        metrics->avg_time_per_pixel_ns = 0.0;
        metrics->cycles_per_pixel      = 0.0;
    }

    // 4) DERIVE "PERF-LIKE" TOTAL CYCLES ACROSS ALL THREADS
    //
    // estimated_total_cycles_all_threads
    //   ≈ cpu_cycles_TSC * (cpu_total_time_sec / wall_time_sec)
    //
    // where cpu_total_time_sec = cpu_user_time_sec + cpu_system_time_sec.
    double cpu_total_time_sec =
        metrics->cpu_user_time_sec + metrics->cpu_system_time_sec;

    if (metrics->wall_time_sec > 0.0 && cpu_total_time_sec > 0.0) {
        double factor = cpu_total_time_sec / metrics->wall_time_sec;
        double est_total =
            (double)metrics->cpu_cycles * factor;
        if (est_total < 0.0) est_total = 0.0;  // just in case
        metrics->estimated_total_cycles_all_threads =
            (uint64_t)(est_total + 0.5); // round
    } else {
        metrics->estimated_total_cycles_all_threads = 0;
    }

    if (metrics->images_processed > 0) {
        metrics->estimated_cycles_per_image_all_threads =
            (double)metrics->estimated_total_cycles_all_threads /
            (double)metrics->images_processed;
    } else {
        metrics->estimated_cycles_per_image_all_threads = 0.0;
    }

    if (metrics->total_pixels > 0) {
        metrics->estimated_cycles_per_pixel_all_threads =
            (double)metrics->estimated_total_cycles_all_threads /
            (double)metrics->total_pixels;
    } else {
        metrics->estimated_cycles_per_pixel_all_threads = 0.0;
    }

    for (int i = 0; i < file_count; ++i) {
        free(files[i]);
    }
    free(files);
}

int main(int argc, char **argv) {
    const char *input_dir  = "data/input";
    const char *output_dir = "data/output_parallel";

    if (argc >= 2) input_dir  = argv[1];
    if (argc >= 3) output_dir = argv[2];

    Metrics pm;
    process_directory_parallel(input_dir, output_dir, &pm);

    printf("[parallel] Images processed : %d\n", pm.images_processed);
    printf("[parallel] Total pixels     : %lld\n", pm.total_pixels);
    printf("[parallel] Wall time (s)    : %.6f\n", pm.wall_time_sec);
    printf("[parallel] CPU user time(s) : %.6f\n", pm.cpu_user_time_sec);
    printf("[parallel] CPU sys  time(s) : %.6f\n", pm.cpu_system_time_sec);
    printf("[parallel] CPU cycles (TSC) : %llu\n",
           (unsigned long long)pm.cpu_cycles);
    printf("[parallel] Est. total cycles (all threads, perf-like) : %llu\n",
           (unsigned long long)pm.estimated_total_cycles_all_threads);
    printf("[parallel] Threads used     : %d\n", pm.threads_used);

    write_parallel_metrics_json("results/logs/parallel_metrics.json",
                                &pm, input_dir, output_dir);

    // Try to load serial metrics and build a comparison JSON
    Metrics sm;
    if (load_serial_metrics("results/logs/serial_metrics.json", &sm)) {
        Comparison cmp = (Comparison){0};

        if (pm.wall_time_sec > 0.0 && sm.wall_time_sec > 0.0)
            cmp.speedup_wall_time = sm.wall_time_sec / pm.wall_time_sec;

        if (pm.cpu_user_time_sec > 0.0 && sm.cpu_user_time_sec > 0.0)
            cmp.speedup_cpu_user =
                sm.cpu_user_time_sec / pm.cpu_user_time_sec;

        if (pm.cpu_system_time_sec > 0.0 && sm.cpu_system_time_sec > 0.0)
            cmp.speedup_cpu_system =
                sm.cpu_system_time_sec / pm.cpu_system_time_sec;

        double serial_pps   = 0.0;
        double parallel_pps = 0.0;
        if (sm.total_pixels > 0 && sm.wall_time_sec > 0.0)
            serial_pps = (double)sm.total_pixels / sm.wall_time_sec;
        if (pm.total_pixels > 0 && pm.wall_time_sec > 0.0)
            parallel_pps = (double)pm.total_pixels / pm.wall_time_sec;

        // CORRECT: speedup in pixels/sec = parallel throughput / serial throughput
        if (serial_pps > 0.0 && parallel_pps > 0.0)
            cmp.speedup_pixels_per_sec = parallel_pps / serial_pps;

        if (cmp.speedup_wall_time > 0.0 && pm.threads_used > 0)
            cmp.parallel_efficiency =
                cmp.speedup_wall_time / (double)pm.threads_used;

        // Also compute perf-like total cycles for serial & parallel
        double serial_cpu_time =
            sm.cpu_user_time_sec + sm.cpu_system_time_sec;
        double parallel_cpu_time =
            pm.cpu_user_time_sec + pm.cpu_system_time_sec;

        if (sm.wall_time_sec > 0.0 && serial_cpu_time > 0.0) {
            double factor_s = serial_cpu_time / sm.wall_time_sec;
            double est_s = (double)sm.cpu_cycles * factor_s;
            if (est_s < 0.0) est_s = 0.0;
            cmp.serial_est_total_cycles_all_threads =
                (uint64_t)(est_s + 0.5);
        } else {
            cmp.serial_est_total_cycles_all_threads = 0;
        }

        if (pm.wall_time_sec > 0.0 && parallel_cpu_time > 0.0) {
            double factor_p = parallel_cpu_time / pm.wall_time_sec;
            double est_p = (double)pm.cpu_cycles * factor_p;
            if (est_p < 0.0) est_p = 0.0;
            cmp.parallel_est_total_cycles_all_threads =
                (uint64_t)(est_p + 0.5);
        } else {
            cmp.parallel_est_total_cycles_all_threads = 0;
        }

        write_compare_json("results/logs/compare_metrics.json",
                           &sm, &pm, &cmp);
    } else {
        fprintf(stderr,
                "[parallel] Could not load serial_metrics.json. "
                "Run ./bin/serial first for comparison.\n");
    }

    return 0;
}

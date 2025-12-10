#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>
#include "filters.h"
#include "timer.h"
#include <strings.h>  
typedef struct {
    int images_processed;
    long long total_pixels;
    double wall_time_sec;
    double cpu_user_time_sec;
    double cpu_system_time_sec;
    double avg_time_per_image_ms;
    double avg_time_per_pixel_ns;
    uint64_t cpu_cycles;
    double cycles_per_image;
    double cycles_per_pixel;
    int max_width;
    int max_height;
} Metrics;

static int ends_with(const char *name, const char *ext) {
    size_t ln = strlen(name);
    size_t le = strlen(ext);
    if (ln < le) return 0;
    return strcasecmp(name + ln - le, ext) == 0;
}

static int is_image_file(const char *name) {
    return ends_with(name, ".png") ||
           ends_with(name, ".jpg") ||
           ends_with(name, ".jpeg") ||
           ends_with(name, ".bmp");
}

static void ensure_directory(const char *path) {
    if (!path) return;
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return;
        fprintf(stderr, "[serial] %s exists but is not a directory!\n", path);
        return;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        perror("[serial] mkdir");
    }
}

static void write_serial_metrics_json(const char *json_path,
                                      const Metrics *m,
                                      const char *input_dir,
                                      const char *output_dir) {
    ensure_directory("results");
    ensure_directory("results/logs");

    FILE *f = fopen(json_path, "w");
    if (!f) {
        perror("[serial] fopen metrics json");
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"variant\": \"serial\",\n");
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
    fprintf(f, "    \"cpu_cycles\": %llu,\n",
            (unsigned long long)m->cpu_cycles);
    fprintf(f, "    \"cycles_per_image\": %.3f,\n", m->cycles_per_image);
    fprintf(f, "    \"cycles_per_pixel\": %.3f,\n", m->cycles_per_pixel);
    fprintf(f, "    \"max_width\": %d,\n", m->max_width);
    fprintf(f, "    \"max_height\": %d\n", m->max_height);
    fprintf(f, "  }\n");
    fprintf(f, "}\n");

    fclose(f);
    printf("[serial] Metrics written to %s\n", json_path);
}

static void process_directory_serial(const char *input_dir,
                                     const char *output_dir,
                                     Metrics *metrics) {
    memset(metrics, 0, sizeof(*metrics));
    metrics->max_width = 0;
    metrics->max_height = 0;

    ensure_directory(output_dir);

    DIR *dir = opendir(input_dir);
    if (!dir) {
        perror("[serial] opendir input_dir");
        return;
    }

    double user_before, sys_before, user_after, sys_after;
    get_cpu_times(&user_before, &sys_before);
    double t_start = wall_time();
    uint64_t c_start = read_tsc();

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (!is_image_file(ent->d_name))
            continue;

        char in_path[512];
        char out_path[512];
        snprintf(in_path, sizeof(in_path), "%s/%s", input_dir, ent->d_name);
        snprintf(out_path, sizeof(out_path), "%s/%s", output_dir, ent->d_name);

        Image *img = load_image(in_path);
        if (!img) {
            fprintf(stderr, "[serial] Skip failed load: %s\n", in_path);
            continue;
        }

        long long pixels = (long long)img->width * img->height;
        metrics->images_processed += 1;
        metrics->total_pixels += pixels;

        if (img->width > metrics->max_width)
            metrics->max_width = img->width;
        if (img->height > metrics->max_height)
            metrics->max_height = img->height;

        apply_grayscale(img);
        apply_box_blur(img, 2);
        apply_sobel_edge(img);

        if (save_image_png(out_path, img) != 0) {
            fprintf(stderr, "[serial] Failed to save %s\n", out_path);
        }

        free_image(img);
    }

    closedir(dir);

    uint64_t c_end = read_tsc();
    double t_end = wall_time();
    get_cpu_times(&user_after, &sys_after);

    metrics->wall_time_sec      = t_end - t_start;
    metrics->cpu_user_time_sec  = user_after - user_before;
    metrics->cpu_system_time_sec = sys_after - sys_before;
    metrics->cpu_cycles         = c_end - c_start;

    if (metrics->images_processed > 0) {
        metrics->avg_time_per_image_ms =
            (metrics->wall_time_sec * 1000.0) / metrics->images_processed;
        metrics->cycles_per_image =
            (double)metrics->cpu_cycles / metrics->images_processed;
    } else {
        metrics->avg_time_per_image_ms = 0.0;
        metrics->cycles_per_image = 0.0;
    }

    if (metrics->total_pixels > 0) {
        metrics->avg_time_per_pixel_ns =
            (metrics->wall_time_sec * 1e9) / (double)metrics->total_pixels;
        metrics->cycles_per_pixel =
            (double)metrics->cpu_cycles / (double)metrics->total_pixels;
    } else {
        metrics->avg_time_per_pixel_ns = 0.0;
        metrics->cycles_per_pixel = 0.0;
    }
}

int main(int argc, char **argv) {
    const char *input_dir = "data/input";
    const char *output_dir = "data/output_serial";

    if (argc >= 2) input_dir = argv[1];
    if (argc >= 3) output_dir = argv[2];

    Metrics m;
    process_directory_serial(input_dir, output_dir, &m);

    printf("[serial] Images processed : %d\n", m.images_processed);
    printf("[serial] Total pixels     : %lld\n", m.total_pixels);
    printf("[serial] Wall time (s)    : %.6f\n", m.wall_time_sec);
    printf("[serial] CPU user time(s) : %.6f\n", m.cpu_user_time_sec);
    printf("[serial] CPU sys  time(s) : %.6f\n", m.cpu_system_time_sec);
    printf("[serial] CPU cycles       : %llu\n",
           (unsigned long long)m.cpu_cycles);

    write_serial_metrics_json("results/logs/serial_metrics.json",
                              &m, input_dir, output_dir);

    return 0;
}

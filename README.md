# Efficient Image Processing with Multi-Threading: Performance Benchmarking using OpenMP (Serial vs Parallel)

This project implements and compares **serial** and **parallel (OpenMP)** image-processing pipelines in C.  
It processes a directory of images, applies a fixed sequence of filters, and records detailed **performance metrics** (wall-clock time, CPU time, and CPU cycles) for both versions. The results are exported as JSON files for further analysis and visualization.   

---

![Class Diagram](diagrams/class_diagram.png)

## 1. Project Goals

- Implement a simple but non-trivial **image processing workload**.
- Provide two implementations of the same pipeline:
  - `serial` – single-threaded baseline.
  - `parallel` – multi-threaded using OpenMP.
- Measure and compare:
  - Wall-clock time.
  - CPU user + system time.
  - Raw TSC cycles.
  - **Estimated total cycles across all threads** (perf-like metric).
  - Throughput (pixels / second, images / second).
  - Speedup and parallel efficiency.   

---

## 2. Directory Layout

At the project root you have:

```text
bin/        # Compiled binaries (serial, parallel)
data/
  input/               # Input images (PNG/JPG/BMP/…)
  output_serial/       # Outputs produced by serial run
  output_parallel/     # Outputs produced by parallel run
results/
  logs/
    serial_metrics.json    # Metrics from serial run
    parallel_metrics.json  # Metrics from parallel run
    compare_metrics.json   # Serial vs parallel comparison
src/
  filters.c, filters.h
  serial.c
  parallel.c
  timer.c, timer.h
  stb_image.h
  stb_image_write.h
````

> The `serial` and `parallel` programs default to `data/input` as the input directory and write into `data/output_serial` and `data/output_parallel` respectively. Directories are created automatically if missing.

---

![Dataflow](diagrams/dataflow1.png)

## 3. Image Processing Pipeline

All images are handled as interleaved 8-bit RGB buffers using a simple `Image` struct: 

```c
typedef struct {
    int width;
    int height;
    int channels;      // always 3 (RGB) for our pipeline
    unsigned char *data;
} Image;
```

### 3.1 Loading and Saving Images

* **Loading**: `load_image(const char *path)` (in `filters.c`) uses `stb_image.h` to load images from disk and **forces 3 RGB channels**, independent of the original format.
* **Saving**: `save_image_png(const char *path, const Image *img)` uses `stb_image_write.h` to always save the processed result as PNG.
* **Cleanup**: `free_image(Image *img)` frees both the pixel buffer and the struct.

Supported input formats include PNG, JPEG, BMP and others that stb_image can decode. Input files are detected by their extension: `.png`, `.jpg`, `.jpeg`, `.bmp`.

### 3.2 Filters

The pipeline applied to **every image** is:

1. **Grayscale conversion** – `apply_grayscale(Image *img)`

   * Uses a luminance formula per pixel: `0.299 * R + 0.587 * G + 0.114 * B`.
   * The same gray value is written back into R, G, and B channels. 

2. **Box blur** – `apply_box_blur(Image *img, int radius)`

   * Performs a **separable blur**:

     * Horizontal pass into a temporary buffer.
     * Vertical pass back into the original image.
   * For each pixel, averages all pixels in a sliding window of size `(2 * radius + 1)` along the current axis.
   * Radius is currently fixed to `2` in both serial and parallel pipelines.

3. **Sobel edge detection** – `apply_sobel_edge(Image *img)`

   * First converts RGB data into a temporary grayscale buffer.
   * Applies classic 3×3 Sobel kernels `Gx` and `Gy` to compute gradient magnitude at each pixel:
     `mag = sqrt(sumx² + sumy²)` clamped to `[0,255]`.
   * Writes the edge magnitudes back into all three RGB channels. 

These filters run **in-place** on the `Image` object.

---

## 4. Serial Implementation (`serial.c`)

The serial program:

1. Opens the input directory (`data/input` by default).
2. Iterates through all entries; skips `"."`, `".."`, non-image files, etc.
3. For each image:

   * Constructs full input and output paths.
   * Uses `load_image()` to read it.
   * Updates counters:

     * `images_processed`
     * `total_pixels`
     * `max_width`, `max_height`
   * Applies the filters:

     ```c
     apply_grayscale(img);
     apply_box_blur(img, 2);
     apply_sobel_edge(img);
     ```
   * Saves the processed result into `output_dir` using `save_image_png()`.
4. Measures timings and CPU cycles for the *whole* run:

   * `wall_time()` from `timer.c` for elapsed wall time.
   * `get_cpu_times()` for user and system CPU time.
   * `read_tsc()` to read the Time Stamp Counter (TSC) on x86 (or a nanosecond clock on non-x86). 
5. Computes metrics such as:

   * `avg_time_per_image_ms`
   * `avg_time_per_pixel_ns`
   * `cycles_per_image`
   * `cycles_per_pixel` 
6. Writes all metrics to `results/logs/serial_metrics.json`.

The serial run also prints a concise summary to stdout.

---

## 5. Parallel Implementation (`parallel.c`)

The parallel version is designed to do **exactly the same work** but in parallel using OpenMP.

### 5.1 Overview

1. Collects all eligible image filenames into a dynamically-grown `char **files` array.
2. Starts timers and reads TSC (same as serial).
3. Runs an OpenMP `#pragma omp parallel for` loop over the index range `[0, file_count)`:

   * Each iteration:

     * Builds input/output paths.
     * Loads an image.
     * Applies the same filters (grayscale → box blur(radius=2) → Sobel).
     * Saves the image.
   * Uses `reduction` clauses to safely accumulate:

     * `total_pixels`
     * `images_processed`
     * `max_width`, `max_height` (with `reduction(max: ...)`).
4. Stops timers and TSC; computes the same basic metrics as the serial program.
5. Derives **additional perf-like metrics** for total cycles across all threads.
6. Writes metrics to `results/logs/parallel_metrics.json` and, if possible, also writes `results/logs/compare_metrics.json` comparing serial vs parallel.

### 5.2 Perf-like Cycle Estimation

Because the TSC is tied to wall time on a single core, it does **not** directly equal “total cycles used by all threads”. To approximate something similar to `perf stat -e cycles`, the code uses:

```text
estimated_total_cycles_all_threads
  ≈ cpu_cycles_TSC * (cpu_total_time_sec / wall_time_sec)

where cpu_total_time_sec = cpu_user_time_sec + cpu_system_time_sec
```

From this estimate it computes:

* `estimated_cycles_per_image_all_threads`
* `estimated_cycles_per_pixel_all_threads`

These numbers provide a more realistic view of overall CPU consumption in parallel runs.

### 5.3 Comparison JSON

If `results/logs/serial_metrics.json` exists (i.e., you have run `serial` at least once), the parallel program loads it and writes `results/logs/compare_metrics.json` including:

* `speedup_wall_time` – serial wall time / parallel wall time.
* `speedup_cpu_user`, `speedup_cpu_system`.
* `speedup_pixels_per_sec` – throughput speedup.
* `parallel_efficiency` – `speedup_wall_time / threads_used`.
* Pixels-per-second for both serial and parallel.
* CPU utilization (CPU time / wall time) for both.
* Estimated total cycles across all threads for serial and parallel.

---

## 6. Timing and Cycle Measurement (`timer.c`, `timer.h`)

`timer.c` provides three functions:

* `double wall_time()`

  * Uses `clock_gettime(CLOCK_MONOTONIC, ...)` to return high-resolution wall-clock time in seconds.

* `void get_cpu_times(double *user_sec, double *sys_sec)`

  * Uses `getrusage(RUSAGE_SELF, ...)` to obtain accumulated user and system CPU time in seconds.

* `uint64_t read_tsc()`

  * On x86/x86-64: executes the `RDTSC` instruction to read the Time Stamp Counter.
  * On non-x86 platforms: falls back to a nanosecond-resolution monotonic clock and converts to a pseudo-cycle count. 

These are used by both the serial and parallel binaries.

---

## 7. Building the Project

### 7.1 Requirements

* POSIX-like environment (Linux/macOS or WSL).
* C compiler (e.g., `gcc` or `clang`) with support for:

  * C11 (or at least C99 features used here).
  * OpenMP (`-fopenmp`) for the parallel version.
* `make` (if you provide a Makefile).
* `libm` (math library) for `sqrt` used in Sobel.

### 7.2 Example Build Commands

Assuming all sources live under `src/` and binaries go in `bin/`:

```bash
mkdir -p bin

# Serial
gcc -O3 -Wall -std=c11 \
    src/serial.c src/filters.c src/timer.c \
    -o bin/serial -lm

# Parallel (OpenMP)
gcc -O3 -Wall -std=c11 -fopenmp \
    src/parallel.c src/filters.c src/timer.c \
    -o bin/parallel -lm
```

Notes:

* `filters.c` is the **only** translation unit that defines `STB_IMAGE_IMPLEMENTATION` and `STB_IMAGE_WRITE_IMPLEMENTATION`, so stb_image and stb_image_write are compiled only once.
* If you create a `Makefile`, you can define convenient targets like `make serial`, `make parallel`, and `make all`.

---

## 8. Running the Benchmarks

### 8.1 Preparing Input Data

Place any `.png`, `.jpg`, `.jpeg` or `.bmp` images into:

```text
data/input/
```

The programs will ignore non-image files automatically.

### 8.2 Serial Run

```bash
./bin/serial
```

Optional arguments:

```bash
./bin/serial <input_dir> <output_dir>
# Example:
./bin/serial data/input data/output_serial
```

Outputs:

* Processed images written to `data/output_serial/`.
* Metrics file: `results/logs/serial_metrics.json`.

### 8.3 Parallel Run

```bash
./bin/parallel
```

Optional arguments:

```bash
./bin/parallel <input_dir> <output_dir>
# Example:
./bin/parallel data/input data/output_parallel
```

Outputs:

* Processed images written to `data/output_parallel/`.
* Metrics file: `results/logs/parallel_metrics.json`.
* If `serial_metrics.json` exists, a comparison file `results/logs/compare_metrics.json` is also generated.

> **Tip:** To get full comparison metrics, **always run `serial` first**, then `parallel`.

---

## 9. JSON Metrics Format

### 9.1 `serial_metrics.json` / `parallel_metrics.json`

These files share the same structure (with extra fields for parallel):

```jsonc
{
  "variant": "serial" | "parallel",
  "input_dir": "data/input",
  "output_dir": "data/output_serial or data/output_parallel",
  "metrics": {
    "images_processed": 42,
    "total_pixels": 12345678,
    "wall_time_sec": 0.123456789,
    "cpu_user_time_sec": 0.120000000,
    "cpu_system_time_sec": 0.003000000,
    "avg_time_per_image_ms": 2.345678,
    "avg_time_per_pixel_ns": 9.876543,
    "cpu_cycles_tsc": 1234567890,
    "cycles_per_image_tsc": 12345.678,
    "cycles_per_pixel_tsc": 98.765,
    "max_width": 1920,
    "max_height": 1080,

    // Only in parallel:
    "estimated_total_cycles_all_threads": 1234567890,
    "estimated_cycles_per_image_all_threads": 12345.678,
    "estimated_cycles_per_pixel_all_threads": 98.765,
    "threads_used": 8
  }
}
```

The actual values are produced at runtime; this is just the conceptual layout.

### 9.2 `compare_metrics.json`

Contains three top-level objects: `comparison`, `serial`, and `parallel`. The `comparison` section summarizes the speedups and utilization:

```jsonc
{
  "comparison": {
    "speedup_wall_time": ...,
    "speedup_cpu_user": ...,
    "speedup_cpu_system": ...,
    "speedup_pixels_per_sec": ...,
    "parallel_efficiency": ...,
    "serial_pixels_per_sec": ...,
    "parallel_pixels_per_sec": ...,
    "serial_cpu_utilization": ...,
    "parallel_cpu_utilization": ...,
    "serial_est_total_cycles_all_threads": ...,
    "parallel_est_total_cycles_all_threads": ...
  },
  "serial": { /* copy of serial metrics */ },
  "parallel": { /* copy of parallel metrics + extra fields */ }
}
```

These metrics make it easy to plot speedup curves or compare different hardware/compilers.

---

## 10. Extending the Project

Here are some directions you can take this project further:

1. **Add new filters**

   * Implement additional operations (Gaussian blur, sharpening, histogram equalization) in `filters.c`.
   * Wire them into both `serial.c` and `parallel.c` to keep pipelines equivalent.

2. **Configurable pipeline**

   * Parse command-line flags or a config file to enable/disable specific filters or change blur radius.

3. **Different scheduling strategies**

   * Experiment with OpenMP schedule clauses (`static`, `dynamic`, `guided`) and chunk sizes to see how they affect performance.

4. **Larger datasets & profiling**

   * Benchmark on a large image set, collect multiple runs, and use the JSON logs to perform statistical analysis of performance.

5. **Cross-platform support**

   * Currently relies on POSIX APIs (`clock_gettime`, `getrusage`). For Windows you can provide alternate implementations in `timer.c`.

---

## 11. Third-Party Libraries and Licensing

This project uses the following third-party libraries:

* **stb_image.h** – single-header image loading library.
* **stb_image_write.h** – single-header image writing library.

Both libraries are provided under an **MIT or Public Domain dual license**, so you may choose whichever is more convenient. Their full license text is included at the end of each header.

Your own project code can be licensed under any compatible license (MIT, BSD, GPL, etc.). Be sure to keep the stb license texts with the headers when redistributing.

---

## 12. Summary

In short, this project is a **self-contained benchmarking framework** for image processing in C:

* Clear, reproducible pipeline (grayscale → blur → Sobel).
* Direct comparison between serial and OpenMP parallel implementations.
* Rich metrics (time, cycles, throughput, CPU utilization, estimated total cycles).
* JSON logs ready for further analysis or plotting.

It can serve as a teaching example for:

* Basic digital image processing,
* Performance measurement,
* Parallel programming with OpenMP,
* And practical use of stb single-header libraries in C.

```



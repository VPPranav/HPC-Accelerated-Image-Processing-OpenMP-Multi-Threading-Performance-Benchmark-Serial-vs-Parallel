#ifndef FILTERS_H
#define FILTERS_H

#include <stddef.h>

/**
 * Simple image representation: interleaved unsigned char RGB.
 */
typedef struct {
    int width;
    int height;
    int channels;      // always 3 (RGB) for our pipeline
    unsigned char *data;
} Image;

/**
 * Load image from disk as 3-channel RGB.
 * Returns NULL on failure.
 */
Image *load_image(const char *path);

/**
 * Save image as PNG to disk.
 * Returns 0 on success, non-zero on failure.
 */
int save_image_png(const char *path, const Image *img);

/**
 * Free image memory.
 */
void free_image(Image *img);

/**
 * In-place filters.
 */
void apply_grayscale(Image *img);
void apply_box_blur(Image *img, int radius);
void apply_sobel_edge(Image *img);

#endif // FILTERS_H

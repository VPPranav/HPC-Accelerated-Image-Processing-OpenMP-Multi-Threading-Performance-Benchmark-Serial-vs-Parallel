#include "filters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
/* stb single-header libs */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

Image *load_image(const char *path) {
    if (!path) return NULL;

    int w, h, c;
    unsigned char *data = stbi_load(path, &w, &h, &c, 3); // force RGB
    if (!data) {
        fprintf(stderr, "[load_image] Failed to load: %s\n", path);
        return NULL;
    }

    Image *img = (Image *)malloc(sizeof(Image));
    if (!img) {
        fprintf(stderr, "[load_image] Out of memory.\n");
        stbi_image_free(data);
        return NULL;
    }

    img->width = w;
    img->height = h;
    img->channels = 3;
    img->data = data;

    return img;
}

int save_image_png(const char *path, const Image *img) {
    if (!path || !img || !img->data) return -1;

    int stride = img->width * img->channels;
    int ok = stbi_write_png(path, img->width, img->height,
                            img->channels, img->data, stride);
    if (!ok) {
        fprintf(stderr, "[save_image_png] Failed to save: %s\n", path);
        return -1;
    }
    return 0;
}

void free_image(Image *img) {
    if (!img) return;
    if (img->data) {
        stbi_image_free(img->data);
    }
    free(img);
}

void apply_grayscale(Image *img) {
    if (!img || !img->data || img->channels < 3) return;

    int pixels = img->width * img->height;
    for (int i = 0; i < pixels; ++i) {
        unsigned char *p = &img->data[i * img->channels];
        unsigned char r = p[0];
        unsigned char g = p[1];
        unsigned char b = p[2];

        // simple luminance
        unsigned char gray = (unsigned char)(
            0.299 * r + 0.587 * g + 0.114 * b
        );
        p[0] = p[1] = p[2] = gray;
    }
}

void apply_box_blur(Image *img, int radius) {
    if (!img || !img->data || img->channels < 3 || radius <= 0) return;

    int w = img->width;
    int h = img->height;
    int c = img->channels;
    int size = w * h * c;

    unsigned char *src = img->data;
    unsigned char *tmp = (unsigned char *)malloc(size);
    if (!tmp) {
        fprintf(stderr, "[apply_box_blur] Out of memory.\n");
        return;
    }

    // Horizontal pass
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int rsum[3] = {0, 0, 0};
            int count = 0;

            int xmin = (x - radius < 0) ? 0 : x - radius;
            int xmax = (x + radius >= w) ? w - 1 : x + radius;

            for (int xx = xmin; xx <= xmax; ++xx) {
                int idx = (y * w + xx) * c;
                rsum[0] += src[idx + 0];
                rsum[1] += src[idx + 1];
                rsum[2] += src[idx + 2];
                count++;
            }

            int out_idx = (y * w + x) * c;
            tmp[out_idx + 0] = (unsigned char)(rsum[0] / count);
            tmp[out_idx + 1] = (unsigned char)(rsum[1] / count);
            tmp[out_idx + 2] = (unsigned char)(rsum[2] / count);
        }
    }

    // Vertical pass (in-place back into src)
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int rsum[3] = {0, 0, 0};
            int count = 0;

            int ymin = (y - radius < 0) ? 0 : y - radius;
            int ymax = (y + radius >= h) ? h - 1 : y + radius;

            for (int yy = ymin; yy <= ymax; ++yy) {
                int idx = (yy * w + x) * c;
                rsum[0] += tmp[idx + 0];
                rsum[1] += tmp[idx + 1];
                rsum[2] += tmp[idx + 2];
                count++;
            }

            int out_idx = (y * w + x) * c;
            src[out_idx + 0] = (unsigned char)(rsum[0] / count);
            src[out_idx + 1] = (unsigned char)(rsum[1] / count);
            src[out_idx + 2] = (unsigned char)(rsum[2] / count);
        }
    }

    free(tmp);
}

void apply_sobel_edge(Image *img) {
    if (!img || !img->data || img->channels < 3) return;

    int w = img->width;
    int h = img->height;
    int c = img->channels;

    int pixels = w * h;
    unsigned char *gray = (unsigned char *)malloc(pixels);
    if (!gray) {
        fprintf(stderr, "[apply_sobel_edge] Out of memory.\n");
        return;
    }

    // Convert to grayscale buffer for Sobel
    for (int i = 0; i < pixels; ++i) {
        unsigned char *p = &img->data[i * c];
        unsigned char g = (unsigned char)(
            0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]
        );
        gray[i] = g;
    }

    unsigned char *out = (unsigned char *)malloc(pixels);
    if (!out) {
        fprintf(stderr, "[apply_sobel_edge] Out of memory.\n");
        free(gray);
        return;
    }

    int gx[3][3] = {
        {-1, 0, 1},
        {-2, 0, 2},
        {-1, 0, 1}
    };
    int gy[3][3] = {
        {-1, -2, -1},
        { 0,  0,  0},
        { 1,  2,  1}
    };

    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            int sumx = 0;
            int sumy = 0;

            for (int ky = -1; ky <= 1; ++ky) {
                for (int kx = -1; kx <= 1; ++kx) {
                    int px = x + kx;
                    int py = y + ky;
                    int idx = py * w + px;
                    int val = gray[idx];

                    sumx += gx[ky + 1][kx + 1] * val;
                    sumy += gy[ky + 1][kx + 1] * val;
                }
            }

            int mag = (int)sqrt((double)(sumx * sumx + sumy * sumy));
            if (mag > 255) mag = 255;
            if (mag < 0) mag = 0;
            out[y * w + x] = (unsigned char)mag;
        }
    }

    // copy edges back into RGB image (grayscale)
    for (int i = 0; i < pixels; ++i) {
        unsigned char e = out[i];
        img->data[i * c + 0] = e;
        img->data[i * c + 1] = e;
        img->data[i * c + 2] = e;
    }

    free(gray);
    free(out);
}

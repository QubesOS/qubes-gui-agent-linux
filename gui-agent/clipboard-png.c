/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2026  Frederic Pierret <frederic@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include "clipboard-png.h"

struct msg_clipboard_image *png_to_clipboard_image(const unsigned char *png,
        size_t png_len, size_t *image_len)
{
    png_image image;
    struct msg_clipboard_image *img;
    size_t pixels_len;

    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, png, png_len)) {
        fprintf(stderr, "Failed to read clipboard PNG: %s\n", image.message);
        return NULL;
    }
    image.format = PNG_FORMAT_RGBA;
    if (image.width == 0 || image.height == 0 ||
            image.width > MAX_CLIPBOARD_IMAGE_WIDTH ||
            image.height > MAX_CLIPBOARD_IMAGE_HEIGHT ||
            PNG_IMAGE_SIZE(image) > MAX_CLIPBOARD_IMAGE_BUFFER_SIZE) {
        fprintf(stderr, "Unsupported clipboard image size '%ux%u'\n",
                image.width, image.height);
        png_image_free(&image);
        return NULL;
    }
    pixels_len = PNG_IMAGE_SIZE(image);
    img = malloc(sizeof(*img) + pixels_len);
    if (!img) {
        perror("malloc");
        png_image_free(&image);
        return NULL;
    }
    img->width = image.width;
    img->height = image.height;
    if (!png_image_finish_read(&image, NULL, img->pixels, 0, NULL)) {
        fprintf(stderr, "Failed to decode clipboard PNG: %s\n", image.message);
        png_image_free(&image);
        free(img);
        return NULL;
    }
    *image_len = sizeof(*img) + pixels_len;
    return img;
}

unsigned char *clipboard_image_to_png(const struct msg_clipboard_image *img,
        size_t *png_len)
{
    png_image image;
    png_alloc_size_t size;
    unsigned char *png, *smaller;

    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = img->width;
    image.height = img->height;
    image.format = PNG_FORMAT_RGBA;
    /* worst case size, so that one compression pass is enough */
    size = PNG_IMAGE_PNG_SIZE_MAX(image);
    png = malloc(size);
    if (!png) {
        perror("malloc");
        return NULL;
    }
    if (!png_image_write_to_memory(&image, png, &size, 0, img->pixels, 0,
                NULL)) {
        fprintf(stderr, "Failed to encode clipboard PNG: %s\n", image.message);
        png_image_free(&image);
        free(png);
        return NULL;
    }
    smaller = realloc(png, size);
    if (smaller)
        png = smaller;
    *png_len = size;
    return png;
}

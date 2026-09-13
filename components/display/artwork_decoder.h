#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t *pixels;
  size_t data_size;
  uint16_t width;
  uint16_t height;
} artwork_rgb565_t;

/**
 * Decode a baseline JPEG into a bounded RGB565 PSRAM buffer.
 *
 * The JPEG is decoded at native resolution and box-filtered into an RGB565
 * image whose longest side does not exceed max_dimension. The caller owns
 * out->pixels on success and must release it with artwork_decoder_free().
 */
bool artwork_decoder_decode_jpeg(const uint8_t *jpeg, size_t jpeg_len,
                                 uint16_t max_dimension,
                                 artwork_rgb565_t *out);

void artwork_decoder_free(artwork_rgb565_t *image);

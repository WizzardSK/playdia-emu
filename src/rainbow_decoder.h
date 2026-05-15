#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Decode an AK8000 packet bitstream using the PC-FX RAINBOW
 * algorithm and Huffman tables (ported from Mednafen, GPL-2.0+).
 *
 *   bitstream  — raw bitstream bytes after the 40/44-byte header
 *   bytes      — bitstream length
 *   qtable[16] — AK8000 quantization table from the frame header
 *   qscale     — header byte [3]
 *   rgb_out    — RGB888 destination, must hold width*height*3
 *   out_stride — bytes per output row (=screen pitch in our use)
 *   width, height — target image dimensions, multiples of 16
 *
 * Returns true if the decode completed without exhausting the
 * bitstream; false on failure.                                   */
bool rainbow_decode_frame(const uint8_t *bitstream, int bytes,
                          const uint8_t qtable[16], int qscale,
                          uint8_t *rgb_out, int out_stride,
                          int width, int height);

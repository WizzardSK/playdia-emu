#pragma once
// ─────────────────────────────────────────────────────────────
//  AK8000 picture decoder — recovered 4×4 DCT syntax.
//
//  Ported from PlaydiaEmu (https://github.com/AloysHF/PlaydiaEmu),
//  crates/playdiaemu-core/src/video/{ak8000,structure}.rs.
//  Copyright (c) PlaydiaEmu contributors, BSD-3-Clause.
//
//  A picture is one accumulated F1/F2 packet starting with 00 80 04.
//  Layout: 36-byte header (19-bit 0x400 marker, picture type, quantizer
//  shift, factor, 16-byte luma and 16-byte chroma quantizer tables),
//  then 27 rows of 31 macroblocks, each 4 luma + 2 chroma 4×4 blocks,
//  then a 14-bit 0x21 terminator and 0xFF fill.
// ─────────────────────────────────────────────────────────────

#include <stddef.h>
#include <stdint.h>

#define PD_PIC_W       248
#define PD_PIC_H       216
#define PD_PIC_ROWS     27
#define PD_PIC_MBS      31
#define PD_HEADER_BYTES 36

typedef enum {
    PD_OK = 0,
    PD_ERR_HEADER,          // no 19-bit 0x400 picture marker
    PD_ERR_UNSUPPORTED,     // picture type / quantizer shift / factor out of range
    PD_ERR_TRUNCATED,       // bitstream ended mid-symbol
    PD_ERR_INVALID_CODE,    // no VLC matches
    PD_ERR_COEFF_OVERFLOW,  // run pushed past the 16th coefficient
    PD_ERR_ROW_MARKER,      // row marker missing or out of order
    PD_ERR_TERMINATOR,      // 14-bit 0x21 terminator missing
    PD_ERR_PADDING          // trailer is not <=15 zero bits before 0xFF fill
} pd_status;

typedef struct {
    pd_status status;
    int       bit;    // bit offset where decoding stopped
    int       row;    // 1-based row, 0 before the first row marker
    int       block;  // block index within the row
} pd_result;

// Decode one picture into a tight PD_PIC_W * PD_PIC_H * 3 RGB888 buffer.
pd_result pd_picture_decode(const uint8_t *data, size_t len, uint8_t *rgb);

// Check entropy and framing without running the pixel transform.
pd_result pd_picture_validate(const uint8_t *data, size_t len);

// Human-readable name for a status, for logs.
const char *pd_status_name(pd_status s);

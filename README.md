# Playdia Emulator

Emulator for the **Bandai Playdia** (1994), an obscure Japanese FMV console.

## Hardware
- **Console**: Bandai Playdia Quick Interactive System (1994)
- **Main CPU**: Toshiba TLCS-870 @ 8MHz
- **I/O CPU**: NEC µPD78214 (78K/0) @ 12MHz
- **Video/Audio**: Asahi Kasei AK8000 custom ASIC (no datasheet exists)
- **Video DAC**: Philips TDA8772AH Triple 8-bit (CD-i compatible)
- **Media**: CD-ROM XA Mode 2

## Building

```bash
make
```

Dependencies: SDL2, libavcodec, libavutil, libswscale, libzip

## Running

```bash
./playdia game.cue              # Run with SDL2 window
./playdia game.cue --headless   # Run without display (300 frames)
./playdia game.cue --debug      # Run with per-second stats
./playdia --test                # CPU self-test
```

Controls: Arrow keys = D-pad, Z/X = A/B, Enter = Start, Space = Select, F1 = Fullscreen, Esc = Quit

## Emulator Status

| Component | Status |
|-----------|--------|
| TLCS-870 CPU | Basic instruction set |
| NEC 78K/0 CPU | Basic instruction set |
| CD-ROM | CUE/BIN loading (single + multi-file), raw Mode 2/2352, ZIP support |
| BIOS HLE | Auto-scan for GLB/AJS, FMV playback loop |
| Video decode | **DC + AC (direct sequential VLC) + integer IDCT + deblocking** (proprietary AK8000 codec) |
| Audio decode | **XA ADPCM** with resampling to 44100 Hz |
| Interactive | **F2 commands** — jumps, choices (F2 44), loops, quiz, timeout |
| Display | SDL2, 320×240, 256×144 video centered |
| Pipeline | 10 sectors/frame @ 30fps (~4× CD speed) |

### Interactive FMV Commands

The Playdia uses F2 sectors with submode `0x09` for interactive control:

| Command | Function | Implementation |
|---------|----------|---------------|
| F2 40 | Unconditional jump / scene loop | Forward=auto-seek, backward=loop until button |
| F2 44 | Player choice (7 button destinations) | Wait for input, seek to chosen destination |
| F2 50 | Quiz answer verification | Wait for input, extra byte = correct/wrong flag |
| F2 60 | Timed jump / animation | Auto-seek with duration parameter |
| F2 80 | Timeout handler | Sets fallback destination for preceding F2 44 |
| F2 90 | Score/result display | Auto-seek, sequential counter per button |
| F2 A0 | Loop/animation control | Flag F0 = boot marker (skipped) |

Button mapping: B1=Start/default, B2=Up, B3=Down, B4=Left, B5=Right, B6=A, B7=B

MSF destination encoding: `target_LBA = M×4500 + S×75 + F − 150` (binary, not BCD)

---

# AK8000 Video Codec — Reverse-Engineered Specification

The AK8000 uses a proprietary DCT-based video codec. No documentation exists anywhere — this was reverse-engineered from raw disc data across multiple games.

## Disc Structure
- **F1 sectors**: Video data (marker byte `0xF1`, 2047 bytes payload)
- **F2 sectors**: End-of-frame marker (triggers decode of assembled frame)
- **F3 sectors**: Scene marker (reset frame accumulator)
- Each video packet: **6–13 F1 sectors** (typically 8 = 16376 bytes, containing 3 frames)

## Frame Header (40 bytes)
```
Offset  Content
[0-2]   00 80 04          — frame marker
[3]     QS                — quantization scale (observed: 4–40)
[4-19]  16-byte qtable    — quantization table (constant across all games tested)
[20-35] 16-byte qtable    — identical copy
[36-38] 00 80 24          — second marker
[39]    TYPE              — purpose unknown (common values: 0x00, 0x06, 0x07)
```

The qtable is always `0A 14 0E 0D 12 25 16 1C 0F 18 0F 12 12 1F 11 14` — likely hardcoded in the AK8000 chip.

## Image Format
- **Resolution**: 256×144 pixels
- **Color**: YCbCr 4:2:0 — 6 blocks per macroblock (4Y + Cb + Cr)
- **Macroblocks**: 16×9 = 144 macroblocks, 864 blocks total
- **Y sub-block order**: TL, TR, BL, BR within each 16×16 macroblock
- **IDCT**: Standard orthonormal 8×8 DCT, pixel = IDCT(coeff) + 128

## VLC Table (Extended MPEG-1 Luminance DC)

Used for **both DC differences and AC levels**. The standard MPEG-1 table (sizes 0–11) is extended to size 16:

```
Size  Code bits      Code value   Total bits (code + magnitude)
0     100            0x4          3
1     00             0x0          3
2     01             0x1          4
3     101            0x5          6
4     110            0x6          7
5     1110           0xE          9
6     11110          0x1E         11
7     111110         0x3E         13
8     1111110        0x7E         15
9     11111110       0xFE         17
10    111111110      0x1FE        19
11    1111111110     0x3FE        21
12    11111111110    0x7FE        23  (extended)
13    111111111110   0xFFE        25  (extended)
14    1111111111110  0x1FFE       27  (extended)
15    11111111111110 0x3FFE       29  (extended)
16    111111111111110 0x7FFE      31  (extended)
```

### Magnitude Encoding (Under Investigation)

The magnitude bits encoding is not yet fully confirmed. Two candidates:

- **MPEG-1 standard**: if value < 2^(size-1), subtract 2^size - 1. Produces systematic negative bias (mean diff ≈ -1.6/value).
- **Offset+1** (best candidate): `value = raw_bits - 2^(size-1) + 1`. Reduces DC drift from ~-1420 to ~-78 per frame. For size 2: 00→-1, 01→0, 10→+1, 11→+2.

The offset+1 formula gives near-zero DC sum for some packets (Yumi: -72) but not all (Aqua: -309). See [Research Notes](#dc-encoding-research-2026-03-14) below.

## Multi-Frame Packing

Each packet contains **2–4 independent frames** in a continuous bitstream (DC+AC per frame, packed back-to-back with no byte alignment). Frame count depends on content complexity and QS:

| F1 sectors | Packet size | Frames | Frequency |
|------------|-------------|--------|-----------|
| 8 | 16376 bytes | 3 | 90.8% |
| 7 | 14329 bytes | 3 | 6.0% |
| 13 | 26611 bytes | 3 | 2.5% |
| 6 | 12282 bytes | 3 | 0.6% |

Bitstream analysis confirms: each frame's DC section (~3800 bits) + AC section (~29000 bits) consumes ~33% of the packet. The last frame may be slightly truncated when data runs out.

## Bitstream Structure (per frame)

### Phase 1: DC Coefficients
864 DC differences decoded sequentially using the extended VLC table.

**Per-component DPCM**: Three separate predictors for Y, Cb, Cr (all initialized to 0).
- Blocks 0–3 of each macroblock: Y predictor
- Block 4: Cb predictor
- Block 5: Cr predictor

### Phase 2: AC Coefficients — Direct Sequential VLC

864 blocks of VLC-coded AC coefficients placed sequentially at zigzag positions.

Each VLC value is a **direct coefficient**:
- Value = 0 → **end of block** (EOB)
- Value ≠ 0 → coefficient placed at next zigzag position, dequantized by × 16

**Chroma handling**: Cb/Cr blocks have AC data in the bitstream (must be read to maintain sync) but it is discarded — only luma (Y) AC coefficients are applied. Applying chroma AC produces rainbow noise artifacts.

### Pseudocode
```c
for each block (0..863):
    k = 1  // starting zigzag position
    while k < 64:
        v = read_vlc()
        if v == 0: break          // EOB
        if is_luma_block:
            coeff[k] = v * 16     // direct value × fixed dequant
        k++
```

### Validation

The direct sequential model is validated by multi-frame decoding:
- Produces exactly **864 EOBs per frame** (one per block) — the rl3 run-level model produced only 775 EOBs + 76 overflows
- Consumes exactly 100% of packet bits across 3 frames with 0 bits remaining
- Produces balanced frame sizes (~33% each for 3-frame packets)
- Frame boundaries are self-consistent (frame 2 starts exactly where frame 1 ends)

## Dequantization

- **DC**: Raw VLC-decoded DPCM values used directly as DCT coefficient[0]. The IDCT's 1/8 normalization gives correct pixel values with +128 level shift.
- **AC**: `value × 16` (direct VLC value × fixed multiplier, Y blocks only). The dequant is QS-independent — QS controls encoder-side quantization aggressiveness but the decoder uses a constant scaling factor. DC values span ±800–1900 (full frame), which the IDCT's 1/8 normalization maps to ~0–255 pixels. AC at ×16 provides within-block detail (gradients, edges, textures) at proportional scale.

## XA ADPCM Audio

Standard CD-ROM XA ADPCM audio:
- 18 sound groups × 128 bytes per sector
- Each group: 16 bytes header + 112 bytes sample data (28 words × 4 bytes)
- 4-bit ADPCM with 4 IIR filter modes
- Coding byte: bit 0 = stereo, bit 2 = half rate (18900 Hz vs 37800 Hz)
- Resampled to 44100 Hz via linear interpolation for SDL output

## Test Games

All games share identical qtable values and frame header format.

| Game | Notes |
|------|-------|
| Dragon Ball Z - Uchuu-hen | Primary test game, QS=13/11/8 frames extracted |
| Aqua Adventure - Blue Lilty | Different intro, same codec |
| Ultraman Powered | Shares Bandai intro with DBZ |
| Sailor Moon S | Shares Bandai intro with DBZ |
| Elements Voice Series | Various titles tested |

4/5 games share identical Bandai logo intro. QS decreases over intro: 13→13→13→13→11→10→10→8→8→7 (quality ramp-up).

## Reverse Engineering Methodology

### Key Discovery: Direct Sequential AC Coding
The AC coding was resolved by comparing two models:
- **rl3 run-level**: packed (run, level) pairs — produced only 775 EOBs + 76 overflows per frame
- **Direct sequential**: VLC values placed directly as coefficients — produced exactly **864 EOBs per frame** (one per block, perfect alignment)

The direct model also consumes the entire bitstream across 3 frames with 0 bits remaining, confirming correct synchronization.

**Validation**: Multi-frame decode consumes exactly 100% of packet bits. Frame sizes are balanced (~33% each for 3-frame packets). The interleaved DC+AC model was explicitly ruled out (produces wrong colors).

### DC-Only Test
Rendering with DC coefficients only produces blocky output with correct color regions. Adding AC with the direct sequential model adds within-block detail (edges, textures). Chroma AC must be discarded (applying it produces rainbow noise).

### Models Ruled Out (50+ tested)
- Packed run-level (rl3) — 775 EOBs + 76 overflows vs 864 expected; replaced by direct sequential
- Run-level coding with 6-bit EOB + unary run + VLC level — produces artifacts
- MPEG-1 AC VLC (Table B.14) — all block organizations fail
- MPEG-2 AC VLC (Table B.15) — 12–17% consumption, 30%+ errors
- JPEG standard AC Huffman — self-calibrating on random data
- PS1 MDEC 16-bit fixed-width — only 42–53% consumed
- Per-block coding (DC+AC interleaved) — artifacts
- Exp-Golomb, Golomb-Rice, arithmetic coding — self-calibrating or wrong paradigm

See git history for the full set of ~80 test tools in `tools/`.

## Open Questions
1. **DC magnitude encoding**: The standard MPEG-1 sign convention produces systematic negative drift. The offset+1 formula (`val = raw - 2^(size-1) + 1`) is closer but still imperfect. See research notes below.
2. **DC predictor initialization**: Bytes 40-43 of the header vary per packet. The product `byte[40] × QS / 8` is remarkably consistent at 206-234 across all packets (pixel brightness range), suggesting these bytes encode DC predictor initialization or mean frame brightness.
3. **AC dequantization formula**: The current fixed multiplier (×16) produces within-block detail. Tested alternatives: MPEG-1 `(2×level+1)×QS×qt/32` — too aggressive at high QS (36), produces noise.
4. **Horizontal DC drift**: Per-row MB predictor reset eliminates vertical drift, but horizontal drift within each macroblock row persists. After linear detrending, recognizable image structure IS visible (different per game/frame), confirming real data is being decoded.
5. **Qtable purpose**: The 16-entry qtable is constant across all games. Not yet used in decoding.
6. **P-frames**: All tested frames decode as independent I-frames. No evidence of delta/motion compensation found.

---

## DC Encoding Research (2026-03-14)

### Confirmed
- **VLC table**: Extended MPEG-1 luminance DC VLC (sizes 0-16) — same table used for both DC and AC
- **Bit order**: MSB-first
- **Per-component DPCM**: 3 separate predictors (Y, Cb, Cr)
- **Interleaved DC+AC per block**: Each block has DC value followed by AC coefficients (val=0=EOB), NOT sequential all-DC then all-AC
- **Per-MB-row predictor reset**: Eliminates vertical drift; predictors reset at the start of each macroblock row
- **Multiple frames per packet**: 2-3 frames packed back-to-back
- **Static images**: pkt0==pkt1 in ALL games (boot screen); 6 unique boot screens across 34 games; Yumi game has 41 identical boot frames then animation (11232 total packets)

### Sign Convention Analysis

The standard MPEG-1 sign gives systematic size-dependent negative bias:

| Size | Count | MSB=1 (pos) | MSB=0 (neg) | Neg % |
|------|-------|-------------|-------------|-------|
| 1 | 294 | 143 | 151 | 51% |
| 2 | 245 | 88 | 157 | **64%** |
| 3 | 103 | 38 | 65 | **63%** |
| 4 | 75 | 20 | 55 | **73%** |

The bias INCREASES with VLC size — the smoking gun. Raw bit patterns have MSB=0 dominant at sizes 2+, regardless of sign convention.

### Candidate DC Formulas Tested

| Formula | DC sum (DBZ) | DC sum (Yumi) | Notes |
|---------|-------------|---------------|-------|
| MPEG-1 standard | -1420 | -72 | Systematic negative drift |
| Inverted sign | +499 | — | Range=253 but still gradient |
| **Offset+1** `raw - 2^(N-1) + 1` | **-78** | **+252** | Best balance |
| Sign=LSB | +82 | -84 | Decent balance |
| Two's complement | +187 | — | OK but high V error |
| Offset binary `raw - 2^(N-1)` | -843 | — | Too negative |

### Header Bytes 40-43

| Game | QS | byte40 | b40×8 | b40×QS/8 |
|------|-----|--------|-------|----------|
| Aqua Adventure | 13 | 127 | 1016 | **206** |
| Bandai Collection | 13 | 144 | 1152 | **234** |
| Sailor Moon | 13 | 144 | 1152 | **234** |
| Dragon Ball Z | 15 | 124 | 992 | **233** |
| Ie Naki Ko | 13 | 140 | 1120 | **228** |
| Yumi Playdia | 15 | 118 | 944 | **221** |

`byte[40] × QS / 8` gives remarkably consistent values 206-234 — plausible mean pixel brightness.

### Models Tested (80+)
- All VLC permutations (120), all sign conventions (7), all prediction models (DPCM/2D/JPEG-LS/per-row/per-MB)
- Different grid dimensions, block orderings, init values, scale factors
- Modular arithmetic, fixed bias correction, 6-predictor model
- MPEG-1 AC Table B.14/B.15 (reduces errors vs DC VLC for AC, but has ~150 unrecognized codes)
- Exp-Golomb, fixed-width, chroma DC VLC table

### Breakthrough: Correct VLC Size Assignment (2026-03-14)

The VLC code-to-size assignment is **NOT** standard MPEG-1. The correct mapping discovered by exhaustive search over all 120 permutations:

```
Code   Bits  Size  (MPEG-1 had)
00     2     0     (was 1)
01     2     2     (was 2) ✓
100    3     1     (was 0)
101    3     4     (was 3)
110    3     3     (was 4)
1110+  4+    5+    (unchanged)
```

Key: the most frequent 2-bit code `00` maps to **size 0** (zero diff), not size 1. This makes sense — in a mostly-white boot screen, most DC blocks are identical to their predecessor.

Results with offset+1 value formula:
- **V correlation: 0.8848** (vs 0.7821 with MPEG-1 assignment)
- **Row drift: 13.7** (vs 19.7)
- All 6 unique boot screens produce visually distinct, structured images

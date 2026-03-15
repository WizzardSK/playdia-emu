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
| Video decode | **AK8000 proprietary DCT codec** — DC offset + fixed 10 AC per block, modified MPEG-1 VLC |
| Audio decode | **XA ADPCM** with resampling to 44100 Hz |
| Interactive | **F2 commands** — jumps, choices (F2 44), loops, quiz, timeout |
| Display | SDL2, 320×240, 192×144 video centered |
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
- **Resolution**: 192×144 pixels (4:3 aspect ratio)
- **Color**: YCbCr 4:2:0 — 6 blocks per macroblock (4Y + Cb + Cr)
- **Macroblocks**: 12×9 = 108 macroblocks, 648 blocks total
- **Y sub-block order**: TL, TR, BL, BR within each 16×16 macroblock
- **IDCT**: Standard orthonormal 8×8 DCT, pixel = IDCT(coeff), NO level shift

## VLC Table (Modified MPEG-1 Luminance DC)

Used for **both DC and AC values**. Based on MPEG-1 luminance DC VLC but with sizes 7/8 **compressed to both 6 bits**:

```
Size  Code bits      Note
0     100            3 bits — value 0
1     00             2 bits
2     01             2 bits
3     101            3 bits
4     110            3 bits
5     1110           4 bits
6     11110          5 bits
7     111110         6 bits — standard MPEG-1
8     111111         6 bits — COMPRESSED (standard would be 1111110 = 7 bits)
```

**Key difference from standard MPEG-1**: sizes 7 and 8 are both 6-bit codes, differentiated by the last bit (0=size 7, 1=size 8). This saves 1 bit per size-8 occurrence.

After reading the size code, `size` additional bits encode the magnitude (MPEG-1 sign convention).

### Magnitude Encoding
Standard MPEG-1 sign convention: if value < 2^(size-1), subtract 2^size - 1.
**Note**: AC values show negative bias (mean=-1.73), suggesting sign convention may need inversion.

## Multi-Frame Packing

Each packet contains **2–4 independent frames** in a continuous bitstream (DC+AC per frame, packed back-to-back with no byte alignment). Frame count depends on content complexity and QS:

| F1 sectors | Packet size | Frames | Frequency |
|------------|-------------|--------|-----------|
| 8 | 16376 bytes | 3 | 90.8% |
| 7 | 14329 bytes | 3 | 6.0% |
| 13 | 26611 bytes | 3 | 2.5% |
| 6 | 12282 bytes | 3 | 0.6% |

Bitstream analysis confirms: each frame's DC section (~3800 bits) + AC section (~29000 bits) consumes ~33% of the packet. The last frame may be slightly truncated when data runs out.

## Bitstream Structure (per frame) — Current Best Model (2026-03-15)

### Header Init
- Bytes 40-42: DC base values for Y, Cb, Cr
- Byte 43: flags (purpose TBD)
- Bitstream starts at byte 44

### Per-Block Decode (MB-interleaved: 4Y + Cb + Cr per macroblock)
Each block has **11 VLC values**: 1 DC + 10 AC (fixed count, NO EOB marker).

- **DC**: `pixel_dc = init[component] + vlc_diff` (offset from base, NO DPCM accumulation)
- **AC**: 10 VLC values placed at zigzag positions 1-10. Value 0 = zero coefficient (NOT EOB).
- **Dequantization**: Under investigation. Qtable may apply to zigzag positions.
- **IDCT**: Standard 8×8 orthonormal DCT. DC coeff = dc_value × 8 (IDCT divides by 8).

### Pseudocode
```c
for each macroblock (row-major, 12×9):
    for each block (4Y + Cb + Cr):
        diff = read_vlc()
        dc = init[comp] + diff           // offset, no accumulation
        coeff[0] = dc * 8
        for i = 1..10:
            coeff[zigzag[i]] = read_vlc() // × dequant TBD
        pixel_block = idct_8x8(coeff)     // no level shift
```

### Validation
- Consumes exactly **33.0%** of packet bitstream per frame → 3 frames per packet ✓
- DC base colors match expected content (blue=space, green=frogs) ✓
- AC blocks show real DCT basis function patterns ✓
- No VLC decode errors ✓

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

### Critical: DC Reconstruction (UNSOLVED)
The VLC entropy coding is confirmed correct (0 errors across full packets), but the **DC value reconstruction is fundamentally wrong**. Decoded DC values show **zero correlation** (corr ≈ 0.00) with the known QIS boot screen reference image when compared at block level.

**What works:** VLC decodes 21,232 values per packet with 0 errors. Frame structure (864 DC + ~6,200 AC per frame, 3 frames per packet) is consistent.

**What doesn't work:** No tested combination of sign convention, prediction model, scaling, scan order, or init value produces DC values that correlate with the real QIS boot screen. 100+ models tested.

### Other Open Questions
1. **16-entry quantization table**: Constant across all games (`0A 14 0E 0D 12 25 16 1C 0F 18 0F 12 12 1F 11 14`). Only 16 entries is unusual for 8×8 DCT (which needs 64). Could indicate **4×4 DCT** (16 = 4×4) or a repeating pattern (qt[k%16]).
2. **Resolution**: Most likely **192×144 (4:3)** based on correlation tests, but not confirmed. 4:2:2 chroma (8 blocks/MB = 864) gives exact 4:3 ratio.
3. **AC dequantization**: Unknown formula. Same VLC as DC, val=0=EOB. ~7.2 AC coefficients per block average.
4. **I-P-P frame structure**: Frames 2-3 within a packet are NOT identical to frame 1 (confirmed for static boot screens). Likely P-frames (delta).
5. **Header bytes 40-43**: Vary per packet. `byte[40] × QS / 8 ≈ 206-234` consistently. Purpose unknown.

---

## DC Encoding Research (2026-03-14)

### Confirmed
- **VLC table**: Extended MPEG-1 luminance DC VLC (sizes 0-16) — decodes ENTIRE packet (21,232 values) with **0 errors**. Same table used for both DC and AC sections.
- **Bit order**: MSB-first
- **Per-component DPCM**: 3 separate predictors (Y, Cb, Cr)
- **Sequential DC-then-AC**: 864 DC values first, then AC for 864 blocks (val=0=EOB). NOT interleaved per-block.
- **Frame structure**: ~7,077 VLC values per frame (864 DC + ~6,213 AC), 3 frames per packet = ~21,000 total
- **I-P-P frames**: Frames 2-3 within a packet differ from frame 1 (delta/P-frames for static content)
- **Resolution**: Likely 192×144 (4:3) with 4:2:2 chroma (108 MBs × 8 blocks = 864)
- **Static images**: pkt0==pkt1 in ALL games (boot screen); 6 unique boot screens across 34 games; Yumi game has 41 identical boot frames then animation (11,232 total packets)
- **VCD-based**: EmularPlaydia blog confirms "VCD format" (MPEG-1 family)
- **AKM AK8xxx = video chips**: AK8859 is NTSC/PAL video decoder, confirming AK8000 is in AKM's video product line

### Reference Images
Real hardware screenshots in `reference/` directory:
- `pd_real_qis.png` — **QIS boot screen** (brown Q, yellow I, blue S on gray marble rectangle, pink/white background)
- `pd_real_suzu.png` — Ie Naki Ko live-action FMV
- `pd_real_keroppi.png` — Kero Kero Keroppi animation
- `pd_real_forest.png` — Forest Sways title screen with readable text

**QIS expected Y profile at 24×18 blocks**: row1=194(bright), rows4-5=131(dark letters), row16=183(bright), row17=111(bottom border). Current decode shows **0 correlation** with this pattern.

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

### Models Tested (100+)
- All VLC code-to-size permutations (120) with multiple sign conventions
- Sign conventions: MPEG-1 standard, inverted, two's complement, offset binary, offset+1, sign-magnitude, separate sign bit, sign=LSB
- Scan orders: row-major, column-major (both with and without per-row/column reset)
- DC scaling: ×1, ×4, ×8, ×QS, ×qtable[0], ×QS×qtable/16
- Init values: 0, 128, byte[40], byte[40]×8, various combinations
- Fixed-width: 4-bit, 5-bit, 6-bit two's complement
- Alternative VLC: Exp-Golomb, Rice k=1, unary sign-magnitude, MPEG-1 chroma DC
- Alternative codecs: DYUV (CD-i), 4×4 DCT (16-entry qtable match)
- Prediction: 1D DPCM, 2D (left, above, JPEG-LS), 6-predictor model, subtractive DPCM
- AC tables: same DC VLC, MPEG-1 Table B.14/B.15 (124 unrecognized codes)
- MPEG-1 AC Table B.14/B.15 run-level coding
- Reference matching: correlated decoded DC with downscaled QIS screenshot (max corr ≈ 0.00)

### Observations (not yet explained)

**Column-major vs row-major scan**: Column-major gives V=0.998, row-major gives H=0.991. Both produce high correlation in the scan direction due to DPCM smoothing — this is an artifact of accumulation, not proof of correct scan order.

**VLC permutation 00→0**: Reduces DC sum from -1420 to -109 (for DBZ) but produces very flat images (range ≈ 20 DC units = 2.5 pixels). Standard MPEG-1 assignment produces more contrast but with systematic drift.

**DC × QS scaling**: Produces visible colored output from the emulator — different games/frames show different horizontal color bands. But images are NOT recognizable compared to reference screenshots.

**The fundamental problem**: No tested DC reconstruction formula produces values that correlate with the known QIS boot screen reference image. The VLC entropy coding is correct, but the mapping from VLC-decoded values to DCT coefficients (or pixel values) remains unknown.

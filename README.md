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
| CD-ROM | CUE/BIN loading, raw Mode 2/2352, ZIP support |
| BIOS HLE | Auto-scan for video, FMV playback loop |
| Video decode | **Full DC+AC+IDCT** (proprietary AK8000 codec) |
| Audio decode | **XA ADPCM** with resampling to 44100 Hz |
| Display | SDL2, 320×240, 256×144 video centered |
| Pipeline | 10 sectors/frame @ 30fps (~4× CD speed) |

---

# AK8000 Video Codec — Reverse-Engineered Specification

The AK8000 uses a proprietary DCT-based video codec. No documentation exists anywhere — this was reverse-engineered from raw disc data across multiple games.

## Disc Structure
- **F1 sectors**: Video data (marker byte `0xF1`, 2047 bytes payload)
- **F2 sectors**: End-of-frame marker (triggers decode of assembled frame)
- **F3 sectors**: Scene marker (reset frame accumulator)
- Each video frame: **6 × F1 sectors × 2047 bytes = 12282 bytes**

## Frame Header (40 bytes)
```
Offset  Content
[0-2]   00 80 04          — frame marker
[3]     QS                — quantization scale (observed: 4–40)
[4-19]  16-byte qtable    — quantization table (constant across all games tested)
[20-35] 16-byte qtable    — identical copy
[36-38] 00 80 24          — second marker
[39]    TYPE              — frame type (0=I, 1=P, plus 2,3,5,6,31,237)
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

Magnitude bits use sign-magnitude: if value < 2^(size-1), subtract 2^size - 1.

## Multi-Frame Packing

Each 12282-byte packet contains **2–4 independent frames** in the bitstream:

| QS | Frames per packet | Notes |
|----|-------------------|-------|
| 13 | 3 | ~33% bits each |
| 11 | 3 | ~33% bits each |
| 8  | 4 | ~25% bits each |

The "100" = EOB discovery was key: it produces **even frame distribution** (confirmed by bit consumption analysis). Without it, 76% of bits go to the first frame.

Frames are **byte-aligned** between boundaries. The last frame is always partial (data runs out).

## Bitstream Structure (per frame)

### Phase 1: DC Coefficients
864 DC differences decoded sequentially using the extended VLC table.

**Per-component DPCM**: Three separate predictors for Y, Cb, Cr (all initialized to 0).
- Blocks 0–3 of each macroblock: Y predictor
- Block 4: Cb predictor
- Block 5: Cr predictor

### Phase 2: AC Coefficients
864 blocks of run-level coded AC coefficients (zigzag positions 1–63).

For each block, repeat until EOB or position 63:

1. **6-bit EOB check**: Peek 6 bits. If `000000` → end of block (consume 6 bits)
2. **Unary run code**: Count leading `0` bits (max 5), then `1` delimiter. Run = count of zeros.
3. **Alternate EOB check**: Peek 3 bits. If `100` (VLC size 0) → end of block (consume 3 bits)
4. **Level VLC**: Read signed value using the extended VLC table (sizes 1–16)
5. Skip `run` positions, place `level` at current position, advance by 1

### Pseudocode
```c
for each block (0..863):
    k = 1  // start at AC position 1
    while k < 64:
        if peek(6) == 0b000000:   // 6-bit EOB
            consume(6); break

        run = 0
        while run < 5:
            bit = read(1)
            if bit == 1: break    // delimiter
            run++

        if peek(3) == 0b100:     // "100" = alternate EOB
            consume(3); break

        level = read_vlc()       // extended VLC, sizes 1-16
        k += run
        if k < 64: coeff[k] = level
        k++
```

## Dequantization (PARTIALLY SOLVED)

- **DC**: Scale ×1 gives correct pixel values (IDCT's 1/8 normalization handles it)
- **AC**: Raw coefficients produce visible block artifacts. Dividing by 8, QS, or qtable all reduce artifacts. The exact formula involving the 16-entry qtable and QS is still unknown.
- AC coefficient range: avg |AC| ≈ 10–15, outliers up to ±2033

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

### Key Discovery: "100" = Alternate EOB
Testing "100" (VLC size 0) as EOB vs as level=0:
- **Level=0**: First frame consumes 76% of bits, second gets 24% → unbalanced
- **EOB**: Three frames at ~33% each → **even distribution confirms EOB interpretation**
- This also fixed QS=8 decoding (previously failed at 687/864 AC blocks)

### Random Data Control
Any variable-length code applied to random data will consume a predictable percentage and show patterns that look like real decoding. The ONLY reliable test is comparing decoded statistics on real vs random data. The unary-run + VLC level + 6-bit EOB scheme shows 2.3× real-vs-random ratio — the strongest differentiation of all schemes tested.

### Models Ruled Out (50+ tested)
- MPEG-1 AC VLC (Table B.14) — all block organizations fail
- MPEG-2 AC VLC (Table B.15) — 12–17% consumption, 30%+ errors
- JPEG standard AC Huffman — self-calibrating on random data
- PS1 MDEC 16-bit fixed-width — only 42–53% consumed
- Exp-Golomb (orders 0–3) — self-calibrating
- Golomb-Rice (all k values) — self-calibrating at k≥1
- Per-position flag+VLC — always self-calibrating
- Fixed-width run-level pairs — all produce noise
- Arithmetic coding hypothesis — unlikely for 1994 hardware
- CD-i DYUV pixel-domain coding — wrong paradigm
- 25+ prefix code structures — none reach 85% consumption

See git history for the full set of ~80 test tools in `tools/`.

## Open Questions
1. **AC dequantization formula**: The 16-entry qtable and QS byte control AC scaling, but the exact formula is unknown.
2. **Frame types beyond I/P**: Types 2, 3, 5, 6, 31, 237 have unknown semantics.
3. **P-frame coding**: Only I-frames are currently decoded. P-frames likely use motion compensation.
4. **IDCT optimization**: Currently uses reference floating-point IDCT (works but slow for non-optimized builds).

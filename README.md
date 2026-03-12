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
| Video decode | **DC + AC (split Y/C clamp) + integer IDCT + deblocking** (proprietary AK8000 codec) |
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

Magnitude bits use sign-magnitude: if value < 2^(size-1), subtract 2^size - 1.

## Multi-Frame Packing

Each packet contains **3 independent frames** in a continuous bitstream (DC+AC per frame, packed back-to-back with no byte alignment):

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

### Phase 2: AC Coefficients
864 blocks of sequential VLC-coded AC coefficients (zigzag positions 1–63).

For each block:
1. Read VLC value for position k (starting at k=1)
2. If value = 0 (`100` in binary, VLC size 0) → **end of block**
3. Otherwise, store value as coefficient at zigzag position k, advance k
4. Repeat until EOB or k reaches 64

### Pseudocode
```c
for each block (0..863):
    is_luma = (block_index % 6 < 4)
    clamp_val = is_luma ? 32 : 4
    for k = 1 to 63:
        level = read_vlc()       // same extended VLC table as DC
        if level == 0: break     // EOB ("100" = VLC size 0)
        coeff[k] = clamp(level, -clamp_val, clamp_val)
```

### AC Clamping — Split Y/Cb/Cr

The sequential VLC model correctly identifies frame boundaries and consumes the right number of bits (97935/97936 for a typical 12282-byte packet). However, ~33% of decoded AC values are outliers (|value| > 3), uniformly distributed across all zigzag positions and block types, suggesting an additional coding layer not yet understood.

Raw VLC-decoded values are used directly as DCT coefficients (no QS multiplication needed). Testing confirmed that unclamped Y blocks reveal real image detail (scenery, edges), while unclamped Cb/Cr blocks create visible colored-dot artifacts. The split clamping approach:

- **Y (luma) blocks**: clamp to ±32 — allows genuine AC detail through the IDCT. Values up to ±32 contribute ~5–6 pixel variation, which is visible as real texture/edges. Luma noise from outlier values manifests as subtle grain rather than colored dots.
- **Cb/Cr (chroma) blocks**: clamp to ±4 — chroma outliers create highly visible colored dots even at small magnitudes. Tight clamping eliminates these while preserving low-frequency color detail.

This approach produces consistent quality across all QS values (7–36) without needing QS in the dequantization formula.

## Dequantization

- **DC**: Raw VLC-decoded DPCM values used directly as DCT coefficient[0]. The IDCT's 1/8 normalization gives correct pixel values with +128 level shift.
- **AC**: Raw VLC values used directly with split Y/C clamping (Y: ±32, Cb/Cr: ±4). No quantization scale multiplication needed — the true role of the QS header byte remains unknown.

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

### Key Discovery: Sequential VLC for AC
The breakthrough came from bitstream analysis showing that each packet contains exactly 3 frames, each consuming ~33% of the data:
- **DC section**: ~3800 bits (864 DPCM VLC-coded coefficients)
- **AC section**: ~29000 bits (864 blocks of sequential VLC values, VLC=0 as EOB)
- Three frames fit perfectly in the available data (97935/97936 bits consumed)

Previous models (run-level coding with dual EOB, per-block coding) all produced colored-dot artifacts because they consumed the wrong number of bits per block, causing desynchronization across subsequent blocks and frames.

### DC-Only Test
Rendering with DC coefficients only produces clean, recognizable images (blocky but correct colors). Adding AC with the sequential VLC model and ±4 clamping adds subtle detail without introducing artifacts. This confirms the DC DPCM scheme is correct and the AC section follows immediately after.

### Models Ruled Out (50+ tested)
- Run-level coding with 6-bit EOB + unary run + VLC level — produces artifacts
- MPEG-1 AC VLC (Table B.14) — all block organizations fail
- MPEG-2 AC VLC (Table B.15) — 12–17% consumption, 30%+ errors
- JPEG standard AC Huffman — self-calibrating on random data
- PS1 MDEC 16-bit fixed-width — only 42–53% consumed
- Per-block coding (DC+AC interleaved) — artifacts
- Exp-Golomb, Golomb-Rice, arithmetic coding — self-calibrating or wrong paradigm

See git history for the full set of ~80 test tools in `tools/`.

## Open Questions
1. **AC outlier values (~33%)**: About one-third of AC VLC-decoded values have |level| > 3, uniformly distributed across all zigzag positions and block types. Unclamped Y blocks reveal real image detail, confirming the VLC values contain genuine AC information. The true AC coding likely involves additional structure that would eliminate these outliers. Possible explanations: different VLC table for AC, embedded run-length coding, or an additional coding layer.
2. **QS purpose**: The quantization scale byte (header[3], range 4–40) does NOT participate in AC dequantization — raw VLC values work directly as DCT coefficients. QS may control encoder-side quantization without being needed for decoding, or may be used in a more complex formula once the AC outlier issue is resolved.
3. **Qtable purpose**: The 16-entry qtable is constant across all games and all QS values (`0A 14 0E 0D 12 25 16 1C 0F 18 0F 12 12 1F 11 14`). Does not currently participate in decoding. May be used in a dequantization step once the true AC coding is understood.
4. **Byte [39] purpose**: Common values 0x00, 0x01, 0x06, 0x07 (150+ distinct values observed). NOT a simple I/P frame type — all packets decode as independent I-frames. May encode packet metadata or scene flags.
5. **P-frames**: All tested frames decode as independent I-frames. No evidence of delta/motion compensation found. The codec may be I-frame only (appropriate for 1994 hardware at 256×144@15fps).

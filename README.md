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

## Bitstream Structure — UNSOLVED (2026-03-15)

**The video bitstream encoding remains unsolved.** After extensive analysis including 600K+ brute-force VLC permutation tests, 225K structural parameter combinations, and comparison with multiple game-era codecs, no decode model produces recognizable images.

### What IS known
- Bitstream starts somewhere around bytes 40-44 of the packet
- Each frame consumes approximately **28-33%** of the packet bitstream → 3 frames per packet
- The VLC produces values with a **correct brightness distribution** (QQ-correlation 0.96 with reference images)
- AC sections within blocks show **real DCT basis function patterns**
- Bytes 40-42 correlate with global frame color (Y, Cb, Cr)
- The MPEG-1 luminance DC VLC decodes without errors (but this is trivially true for ANY bitstream since it's a complete prefix code)

### What is NOT known
- The correct VLC table (MPEG-1 DC VLC is assumed but NOT confirmed)
- Whether DC and AC use the **same or different VLC tables** (PC-FX uses different tables)
- The DC prediction model (DPCM, offset, 2D predictor, or other)
- The AC coding model (run-level, sequential, fixed count)
- The spatial block ordering (scan order)
- The dequantization formula
- Whether level shift (+128) is applied

## Dequantization — UNKNOWN

The 16-byte quantization table and QS byte are present but their exact use is unknown. Possible models:
- `coeff × qtable[pos]` (simple multiply, used by PC-FX)
- `coeff × qtable[pos] × QS / N` (MPEG-1 style)
- DC and AC may use different dequantization

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

### Exhaustive Search (2026-03-14 to 2026-03-15)

Over 1 million decode configurations tested including:

**VLC tables**: Standard MPEG-1 DC (sizes 0-16), modified size 7/8 (both 6-bit), 362K permutations of size assignments, H.261, MPEG-2, JPEG AC, Exp-Golomb, Rice coding

**DC models**: DPCM (continuous, per-row reset, per-MB reset), DC offset (init+diff), absolute, 2D predictor (avg left+above), vertical DPCM, various init values (0, 128, byte[40])

**AC models**: Sequential VLC 0=EOB, fixed count (7-63), 6-bit EOB + unary run + VLC level, combined run-level (|v|-1)/3, MPEG-1 B.14/B.15, no AC

**Structural**: Resolutions 128-288 width, 4:2:0/4:2:2, MB orders (row/col/zigzag/boustro + flips), Y block orders (std/col/rev), bit orders (MSB/LSB), start bytes (36-48), bit offsets (0-7)

**Other codecs**: PS1 MDEC, CD-i DYUV, Hadamard transform, DST, pixel DPCM, Cinepak-like VQ, 4-bit raw, 4×4 DCT sub-blocks

**Correlation methods**: Raw Pearson, 1D detrended (row means), 2D detrended (row+col means), Spearman rank, QQ-plot, cross-game matching, shuffle tests

### Key Findings

1. **Format is 100% proprietary** — no MPEG-1 start codes, no MPEG-PS pack headers, ffprobe recognizes no standard codec or container
2. **QIS boot logo appears on all 32 tested games** — Group A (12 games, Y_init=127) and Group B (16 games, Y_init=144) with identical bitstreams within each group
3. **VLC value DISTRIBUTION matches reference** (QQ-correlation 0.96) but **spatial ordering does not** — the brightness values are correct but placed at wrong 2D positions
4. **All high correlations proved to be DPCM drift artifacts** — cumulative sums always create gradients that spuriously match reference images (1D, 2D, diagonal variants)
5. **VLC brute-force overfits** — different packets produce different "best" VLC tables, confirming the matches are statistical coincidences, not the real VLC
6. **AC blocks show DCT basis patterns** — horizontal/vertical gradients within 8×8 blocks consistent with DCT transform

### Comparison with PC-FX (similar era proprietary FMV)

The PC-FX RAINBOW decoder (reversed by David Michel, implemented in Mednafen) uses:
- **Different VLC tables for DC vs AC** and for **luma vs chroma**
- DPCM DC prediction, run-level AC coding
- Dequant: `coeff × qtable[pos]`
- Y block order: TL, **BL**, TR, BR (not standard TL, TR, BL, BR)
- Level shift +128

The Playdia AK8000 likely uses a similar DCT+VLC architecture but with unknown custom tables.

### What's Needed to Solve

- **Hardware capture**: Record Playdia composite video output synchronized with disc sector reads to create ground-truth input/output pairs
- **AK8000 die shot**: Decap the chip and analyze the decoder logic
- **Japanese community contact**: Someone may have reversed this already (Twitter #レトロゲーム, #プレイディア)

See git history for ~80 test tools in `tools/` and analysis scripts in `/tmp/pd_*.py`.

## Open Questions

1. **VLC table**: Is it MPEG-1 DC luminance, or a custom table? DC and AC may use **different tables** (as PC-FX does). The MPEG-1 DC VLC is a complete prefix code that decodes ANY bitstream without errors, so clean decoding proves nothing.
2. **Spatial ordering**: The VLC produces correct VALUE distributions but incorrect spatial arrangement. The scan order, MB layout, and block-within-MB ordering are all unknown.
3. **DC prediction**: DPCM, offset, 2D predictor? The prediction model determines spatial reconstruction.
4. **Quantization table**: 16 entries could be 4×4 matrix or first 16 zigzag positions of 8×8. Formula unknown.
5. **Resolution**: Likely 192×144 (4:3) but not confirmed.
6. **Header bytes 40-43**: Byte 40-42 correlate with frame color (Y, Cb, Cr). Byte 43 and byte 39 purpose unknown.
7. **Frame types**: Byte 39 values (0x00, 0x06, 0x07) may indicate I-frame vs P-frame.

## Reference Images

Real hardware screenshots in `reference/` directory:
- `pd_real_qis.png` — QIS boot screen
- `pd_real_suzu.png` — Ie Naki Ko live-action FMV
- `pd_real_keroppi.png` — Kero Kero Keroppi animation
- `pd_real_forest.png` — Forest Sways title screen

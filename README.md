# Playdia Video Codec Reverse Engineering

## Hardware
- **Console**: Bandai Playdia (1994), obscure Japanese FMV console
- **Video chip**: Asahi Kasei AK8000 (no datasheet exists anywhere)
- **Video DAC**: Philips TDA8772AH Triple 8-bit (CD-i compatible)
- **Media**: CD-ROM XA Mode 2 Form 1

## Frame Structure (confirmed)
- CD sectors: F1 = video data, F2 = end of frame, F3 = scene marker
- Each frame: 6× F1 sectors × 2047 bytes = **12282 bytes**
- **Header (40 bytes)**:
  - `[0-3]`: `00 80 04 QS` — QS = quantizer scale (varies: 07, 08, 0A, 0B, 0D)
  - `[4-19]`: 16-byte quantization table (always `[10,20,14,13,18,37,22,28,15,24,15,18,18,31,17,20]`)
  - `[20-35]`: duplicate of qtable
  - `[36-39]`: `00 80 24 XX` — XX = frame type (00=intra?, 01=inter?)
- **Bitstream**: bytes 40+ (12242 bytes = 97936 bits)

## Confirmed VLC (MPEG-1 DC Luminance VLC, slightly modified)
The VLC used for ALL coefficients (DC and AC) is confirmed as the **MPEG-1 DC luminance table**:
```
Code    | Size | Value range    | Total bits
--------|------|----------------|----------
00      |  1   | ±1             | 3
01      |  2   | ±2, ±3         | 4
100     |  0   | 0              | 3
101     |  3   | ±4..±7         | 6
110     |  4   | ±8..±15        | 7
1110    |  5   | ±16..±31       | 9
11110   |  6   | ±32..±63       | 11
111110  |  7   | ±64..±127      | 13
111111  |  8   | ±128..±255     | 14 (differs from MPEG-1 standard which uses 1111110)
```
Value decoding: read `size` bits → if < 2^(size-1), subtract 2^size - 1 (sign extension).

### VLC Statistics (Frame 0)
- Total VLC values decoded: **20967** (from 97936 bits = 4.67 bits/value average)
- Value distribution: 0→12.2%, ±1→32.6%, ±2,±3→21.6%, larger→33.6%
- Size distribution: 3-bit→44.8%, 4-bit→26.2%, 6-bit→8.7%, 7-bit→11.3%, 9-bit→4.3%

### Key Observation: Block Count Mismatch
- For 128×144 4:2:0 with 8×8 blocks: 432 blocks × 64 coefficients = 27648 values expected
- But only **20967 VLC values** fit in 97936 bits (75.9% of expected)
- This means either blocks have variable-length (EOB) or there are skip flags
- The previous "100% match for 432×64" was an artifact: after EOF, `br_get1()` returns 0 without incrementing the counter, so remaining blocks silently decode as garbage

## Approaches Tested for Block Structure

### 1. All 64 coefficients per block (no EOB, no flags)
- Only **327 complete blocks** fit in 97936 bits (of 432 expected)
- Average 4.68 bits/coeff, only 54/72 macroblocks complete
- **Conclusion**: 432×64 flat decode does NOT work

### 2. EOB (VLC returning 0 = end of block)
- All 432 blocks complete, average **6.5 AC coefficients/block**
- Only **17%** of bits consumed → too aggressive truncation
- **Conclusion**: 0 as EOB alone doesn't work

### 3. 1-bit flag per block (0=skip, 1=coded)
- 432 blocks total: ~194 coded, ~238 skipped
- **59% of bits** consumed
- DC-only image shows **recognizable horizontal band structure** — best DC result so far!
- **Conclusion**: Promising DC images but too many bits remaining

### 4. DC always + 1-bit AC flag per block (0=DC-only, 1=DC+all AC)
- 432 blocks, ~190 AC-coded blocks
- **58% of bits** consumed
- **Conclusion**: Similar to approach 3

### 5. Per-AC-coefficient 1-bit flag (0=zero, 1=present+VLC)
- All 432 blocks complete, 44.3% of AC positions non-zero
- **88% of bits** consumed — closest match so far!
- Remaining 12% could be end padding or per-MB overhead
- Full IDCT output still garbled — dequantization likely wrong
- **Conclusion**: Most promising structure candidate

### 6. Run/Level VLC pairs with EOB
- 432 blocks, ~4289 R/L pairs
- Only **43% of bits** consumed
- **Conclusion**: Doesn't match

## Test Games
- `Mari-nee no Heya (Japan).zip` — LBA 502
- `Yumi to Tokoton Playdia (Japan) (Dokidoki Campaign).zip` — LBA 502
- Both games have identical frame structure, qtable, and header format

## Tools Created
| File | Purpose | Key Finding |
|------|---------|-------------|
| `vcodec_analyze.c` | Bitstream analysis (entropy, histogram) | 7.6 bits/byte entropy |
| `vcodec_dc_scan.c` | DC-only scan at multiple widths | DC drift without proper structure |
| `vcodec_mpeg1_full.c` | MPEG-1 AC VLC (Table B.14) | Only 8-12% bits consumed, '10' is NOT EOB |
| `vcodec_probe.c` | Pattern distance analysis, DPCM | '10' distances random → NOT structural |
| `vcodec_indiv.c` | Individual VLC per coefficient | Magnitude/sign used 85047/97936 (87%) for Y-only |
| `vcodec_refine.c` | Refine magnitude/sign with quantization | Exact bit match with 4:2:0 (97936/97936) — but was illusory |
| `vcodec_zeroflag.c` | Zero-flag VLC variants vs MPEG-1 DC | Confirmed VLC identity, but 100% match was artifact |
| `vcodec_final.c` | Full decoder with 8 dequant modes | All modes produce garbled images |
| `vcodec_dconly.c` | DC-only extraction | Only 55/72 MBs complete in MB-interleaved mode |
| `vcodec_dconly2.c` | Plane-sequential vs interleaved DC | 288 Y blocks sequential uses 85047 bits |
| `vcodec_verify.c` | Exact block count verification | 327 complete blocks, NOT 432 |
| `vcodec_cbp.c` | Coded block pattern tests | 1-bit block flag → best DC images |
| `vcodec_eob.c` | EOB hypothesis (0=end of block) | Only 17% bits used |
| `vcodec_acbit.c` | Per-AC bit flag | **88% bits used** — most promising |
| `vcodec_dump.c` | VLC value/size distribution dump | 4.67 bits/value average |
| `vcodec_sectors.c` | Sector structure analysis | 6 F1 sectors/frame, Mode 2 Form 1 |

## Open Questions
1. **What is the exact block structure?** Per-AC bit flag uses 88% — what's the other 12%?
2. **What is the dequantization formula?** None of the tested scalings produce recognizable images after IDCT
3. **Is the zigzag scan correct?** Standard MPEG-1 zigzag assumed but could be different
4. **What's the macroblock overhead?** There might be per-MB flags or headers consuming the remaining bits
5. **Does byte 2 of header (0x04) encode anything important?** Could be block mode, color format, etc.

## What Has NOT Been Tried Yet
- [ ] **MPEG-1 Coded Block Pattern (CBP)** — variable-length CBP per macroblock (not just 1-bit)
- [ ] **Macroblock type headers** (like MPEG-1 macroblock_type VLC)
- [ ] **Different zigzag scan orders** (row-major, column-major, or custom)
- [ ] **Testing later frames/LBAs** for content with more visual structure
- [ ] **Comparing frame type 00 vs 01** more carefully (I vs P frame decoding)
- [ ] **JPEG baseline subset** — the AK8000 might implement simplified JPEG
- [ ] **Different IDCT normalization** — maybe the IDCT uses a different scaling convention
- [ ] **H.261 compatibility** — testing H.261 macroblock/block layer structure
- [ ] **Trying without IDCT** — what if it's DPCM-based, not DCT-based?
- [ ] **Brute-force the quant matrix mapping** — 16 entries could map to 64 positions in many ways

## Key Insights
1. VLC is confirmed as MPEG-1 DC luminance (magnitude/sign) for all coefficients
2. The 432-block "100% match" was illusory — actual block count is ~327 at 64 coeff/block
3. Per-AC bit flag gives 88% match — most promising structural candidate
4. 1-bit block flag produces the best DC-only images (recognizable scene structure)
5. The 16-entry qtable maps to 8×8 matrix via nearest-neighbor (2×2 → each qt entry)
6. Both games have identical qtable — it may be hardcoded in the AK8000
7. Frame type alternates 00/01 — likely intra/inter (keyframe/delta)

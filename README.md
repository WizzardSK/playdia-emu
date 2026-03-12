# Playdia Video Codec Reverse Engineering

## Hardware
- **Console**: Bandai Playdia (1994), obscure Japanese FMV console
- **Video chip**: Asahi Kasei AK8000 (no datasheet exists anywhere)
- **Video DAC**: Philips TDA8772AH Triple 8-bit (CD-i compatible)
- **Media**: CD-ROM XA Mode 2 Form 1

## Frame Structure (confirmed across 3 games)
- CD sectors: F1 = video data, F2 = end of frame, F3 = scene marker
- Each frame: 6× F1 sectors × 2047 bytes = **12282 bytes**
- **Header (40 bytes)**:
  - `[0-3]`: `00 80 04 QS` — QS = quantizer scale (observed: 7–40)
  - `[4-19]`: 16-byte quantization table (always `0A 14 0E 0D 12 25 16 1C 0F 18 0F 12 12 1F 11 14`)
  - `[20-35]`: **identical copy** of qtable (always same as [4-19])
  - `[36-39]`: `00 80 24 TYPE` — TYPE: 00=I-frame, 01=P-frame, plus types 2, 3, 5, 6, 31, 237 found across multiple games
- **Bitstream**: bytes 40+ (12242 bytes = 97936 bits)
- Some frames have 0xFF padding at end; most are 98–100% real data
- All frames naturally align to byte boundary before padding
- Ultra-sparse padded frames exist (e.g., Ie Naki Ko: 5052 bytes 0xFF padding, only 62.35 bits/block for AC)

## DC Coefficient Decoding (PARTIALLY CONFIRMED — see caveats)

### MPEG-1 Luminance DC VLC
DC differences appear to decode using MPEG-1 Table B.12 (DC luminance):
```
Code         Size  Value range       Total bits
100          0     0                 3
00           1     ±1                3
01           2     ±2..±3            4
101          3     ±4..±7            6
110          4     ±8..±15           7
1110         5     ±16..±31          9
11110        6     ±32..±63          11
111110       7     ±64..±127         13
1111110      8     ±128..±255        15
11111110     9     ±256..±511        17
111111110    10    ±512..±1023       19
1111111110   11    ±1024..±2047      21
```

**IMPORTANT CAVEAT**: Random position test shows MPEG-1 DC VLC decodes from
86% of arbitrary starting positions in real data, and 21% from random data.
The 100% success from position 40×8 may be coincidental. The VLC table is
nearly self-calibrating for this data. The true DC coding scheme is uncertain.

- With DPCM: Y predictors diverge to [-1114..+42] after 864 blocks (unreasonable)
- Without DPCM: values are small differentials [-214..+61] (flat gray image)
- Lum VLC for Y + Chr VLC for CbCr produces best-looking output with DPCM + scale=1
- Row-reset DPCM (per macroblock row) prevents extreme divergence
- DC uses ~3800-4100 bits per frame (~3.9-4.2% of bitstream)
- Average ~4.5 bits per DC coefficient

### Image Layout (confirmed)
- **Resolution**: 256×144 pixels (16×9 macroblocks)
- **Color**: YCbCr 4:2:0 → 6 blocks per macroblock (4Y + Cb + Cr)
- **Total blocks**: 864 per frame
- **Block order**: macroblock-interleaved (Y0, Y1, Y2, Y3, Cb, Cr)
- **Y sub-block order**: TL, TR, BL, BR within each 16×16 macroblock
- **IDCT**: standard 8×8 DCT with DC×8 scaling, pixel = IDCT + 128

### DC-Only Images — Current Status
Multiple DC decode modes tested on Dragon Ball Z first frame:
- **Lum/Chr DPCM scale=1**: green gradient (most structure, Y diverges)
- **Row-reset DPCM**: prevents divergence, shows horizontal color bands
- **No DPCM (absolute)**: scattered colored dots on gray
- **Fixed 8-bit per block**: random noise

No mode produces a clearly recognizable image yet. The DC coding scheme
is not definitively confirmed — the MPEG-1 VLC match may be coincidental.

## AC Coefficient Coding: UNSOLVED

### Bitstream structure (CONFIRMED)
- **Bit-packed VLC bitstream** — confirmed by 0xFF padding at end of some frames
- **Run-length coding of zero AC coefficients** — proven by zero-run excess (7× at 8-bit runs vs iid model)
- **All I-frames are independent** — 99%+ byte difference between sequential frames (no inter-prediction even between consecutive I-frames)
- **No fixed-size macroblock allocation** — variable-length coded throughout
- **No periodic structure at any stride** — byte-level and bit-level autocorrelation flat at all lags
- **Uniform entropy** — ~3.88 bits per 16-byte sliding window, no low-entropy regions anywhere
- **Zero bytes inversely correlate with QS** — QS=13: 2.72% zeros, QS=7: 0.44% (higher quantization → more zeros → more RLC)
- **First 237 payload bytes contain NO zero bytes** — suggests first ~16 MBs have dense non-zero coefficients

### What is known about the AC bitstream
After DC decode (4100 bits), ~93800 bits remain for AC data:
- **864 blocks × 63 AC positions = 54432 values** to code
- Average: **1.72 bits per AC position** (1.53 for padded frames)
- High entropy: near-random byte distribution, no detectable patterns
- Bit ratio: 56.2% zeros, 43.8% ones (slight bias)
- No autocorrelation at any period (including 64, the block size)
- No zero-byte landmarks or repeating structures

### Bitstream run-length fingerprint (KEY CLUE)
The AC bitstream has a distinctive run-length signature that differs from random data:
- **Max bit run capped at 12** (vs ~17 expected for random data at this length)
- **1-bit runs**: excess at r6 (2.67× expected), deficit at r7 (0.40× expected)
- **0-bit runs**: excess at r12 (5.36× expected!!!), r13=0 (hard cutoff)
- The r6 excess for 1-runs matches a VLC code with max 6 consecutive 1-bits
  (like DC VLC's `1111110` — but DC is already separated, so AC may use similar table)
- The r12 max for 0-runs could be two adjacent 6-zero patterns or a different constraint
- Bit transition probabilities: P(0|0)=0.575, P(1|0)=0.425, P(0|1)=0.547, P(1|1)=0.453

### Random data control methodology
**CRITICAL DISCOVERY**: Any variable-length code applied to random data will consume
a predictable percentage of bits and show patterns that LOOK like real decoding. The
ONLY reliable test is comparing decoded statistics on real data vs random data with
the same decoder. If they are similar → the scheme is self-calibrating (wrong).

### Padded frame analysis
- **F03** (Mari-nee, LBA 502+18): 1328 bytes 0xFF padding, data ends at byte 10914
  - Real AC data: 83079 bits = 96.2 bits/block = 1.53 bits/coeff
  - Last bytes before padding: `27 87 07 4A 56 34 79 C3 8F 9D 08 60 21 00 84 00`
- **F00**: 217 bytes padding, real AC: 92097 bits = 106.6 bits/block
- Most frames have no or minimal padding
- **QS distribution** (32 frames): I-frames=28, P-frames=3; QS 7–40 observed
- Higher QS (more quantization) → fewer AC bits needed → more padding

### MPEG-1 AC VLC with multiple block organizations: RULED OUT
Tested MPEG-1 Table B.14 AC VLC with 5 different block/macroblock configurations:
| Hypothesis | Block size | MB size | Blocks | Bits consumed | Result |
|------------|-----------|---------|--------|--------------|--------|
| A: Standard 8×8, 16×16 MB | 8×8 | 16×16 | 864 | 2.5% (90 blocks) | VLC error after 15 MBs |
| B: 4×4 DCT, 8×8 MB | 4×4 | 8×8 | 3456 | 12.2% (567 blocks) | Best but still fails early |
| C: 4×4 DCT, 16×16 MB (24 blk/MB) | 4×4 | 16×16 | 3456 | 10.1% (463 blocks) | VLC error |
| D: 4×4 DCT, flat raster | 4×4 | — | 3456 | 10.1% (475 blocks) | VLC error |
| E: 4×4 DCT, luma DC for all | 4×4 | 8×8 | 3456 | 10.1% (475 blocks) | VLC error |

All hypotheses hit VLC decode errors well before consuming the full bitstream.
**The codec does NOT use MPEG-1 AC VLC tables in any block organization.**

#### 2-Pass Per-Position Flag+VLC (MPEG-1 DC VLC for AC values)
- Scheme: 2 passes × 864 blocks × 63 zigzag positions; flag=0→zero, flag=1→read VLC, VLC=0→EOB
- **Real data: 100.0%, Random data: 98.3%** — indistinguishable
- Band NZ roll-off present in BOTH real and random (artifact of sequential scanning)
- Per-band ratio INVERTED: real has fewer NZ at low frequencies than random
- AC adds noise to image, not detail — flat DC + AC produces uniform gray with random pixels
- N-pass sweep: 1=51%, 2=100%, 3+=100% for real; random saturates at N=3
- **SELF-CALIBRATING → RULED OUT**

#### Unary-run + Sign+7bit Level, EOB = 6 zero bits (BEST LEAD)
- Scheme: peek 6 bits, if 000000→EOB; else count leading zeros→run, then 1+sign+7bit magnitude→level
- **Real data: 864/864 blocks, 99.1% consumed**
- **Random data: 375/864 blocks (43%)** — strongest differentiation found (2.3× ratio)
- Band NZ: real shows frequency roll-off (pos 1: 409, pos 60: 33); random uniform (193→114)
- **NOT self-calibrating** — first scheme to show strong real-vs-random signal
- **HOWEVER**: inconsistent across frames — works for f00 (QS=13), f06 (QS=10), aqua (QS=13); fails for f04 (QS=11, 835/864) and f08 (QS=8, 481/864)
- Parameter sweep: eob=3-5 with any lb always gives 864 blocks at 20-60% consumption (self-calibrating); eob=6 with lb=8 is the only data-dependent sweet spot; eob=7+ self-calibrates again at 100%
- All rendered images show noise/bands — no recognizable content in any DC/dequant mode
- **Extra `00 80` byte patterns** in some frames are NOT sub-chunk markers — confirmed absent from f04/f06/f08
- **PARTIALLY PROMISING but level encoding likely wrong or incomplete**

### Models tested and RULED OUT

#### JPEG Standard AC Huffman (Annex K, Table K.5)
- 70.1% consumption, 12853 NZ, 805 EOB, bands: 4097→323 (natural roll-off!)
- **Random control**: 74.5%, 12558 NZ, 790 EOB, bands: 3868→329
- **Nearly identical → JPEG AC is self-calibrating, RULED OUT**
- Cross-frame: 63–84% varying (inconsistent → further evidence against)

#### MPEG-1 AC VLC (Table B.14, 111 entries + escape)
- With interleaved DC+AC and EOB='10': uses only **25% of bits**
- Natural-looking run/level histogram (mostly level 1, short runs)
- **BUT**: band-by-band analysis shows **perfectly uniform stats** across all 63 frequency bands
  - Each band: ~675 NZ, ~190 EOBs, ~4200 bits — identical regardless of frequency
  - Real video would show strong frequency roll-off (low bands = many NZ, high bands = few)
- Random control: 16.9% vs real 20.7% (similar)
- **Conclusion**: Self-calibrating artifact.

#### Per-AC-position 1-bit flag (0=zero, 1=VLC value)
- Consumes 88–93% of bits
- **ALL per-position VLC models are self-calibrating** — consume ~100% of bits
  regardless of VLC table, because any variable-length code will eat all remaining bits
- Uniform statistics across all 63 positions (no frequency decay) proves this is wrong

#### 1-bit flag + fixed-width value (0=zero, 1+Nbits=value)
- ALL variants (3–8 bit values) consume exactly 100% with UNIFORM bands
- Random control: nearly identical (e.g., 5-bit: real NZ=12524, random NZ=13106)
- Flag + DC VLC value: also 100%, uniform bands
- **All self-calibrate → RULED OUT**

#### PS1 MDEC-style 16-bit fixed-width (6-bit run + 10-bit level)
- Only 42–53% of bits consumed — doesn't match

#### Fixed-width run-level pairs (4+4, 4+6, 6+6, 4+8, 6+4, 6+8, 6+10 bits)
- ALL show uniform band distribution on both real and random data
- **Self-calibrating → RULED OUT**

#### DC-VLC for AC coefficients
- Interleaved DC VLC for both DC and AC (size=0=EOB): 34.1%, random: 34.8%
- DC VLC run + DC VLC level: 43.2%, similar on random
- MPEG chrominance DC VLC: only 10.6%
- Real vs random very similar → **self-calibrates, RULED OUT**

#### Two-pass VLC+EOB
- Pass 1 (VLC+EOB): 25% bits, Pass 2 (continue from EOB): +16% → 41% total
- Still 59% of bits unused

#### Exp-Golomb run-level pairs (order 0–3)
- Order 0: 30.2% real vs 20.4% random (REAL higher = unusual)
- Order 1: 46.1% real vs 50.4% random
- Order 2–3: ~50% both, very few EOBs
- No order shows correct behavior → **RULED OUT**

#### Golomb-Rice per-coefficient (k=1 to k=6)
- All consume 100% with uniform band distribution
- **Self-calibrating → RULED OUT**

#### Golomb-Rice run-level (kr,kl combinations)
- Tested 15 combinations (kr=0–3, kl=0–3)
- **Best differentiation**: (kr=0,kl=2): REAL=47.9%, RAND=72.7%
  - Bands: 2961 1827 1044 682 419 240 133 90 (real, nice roll-off)
  - Bands: 2819 2165 1684 1309 955 703 595 381 (random, less roll-off)
- (kr=1,kl=1): REAL=38.8%, RAND=57.7% (also good differentiation)
- **BUT**: none consume close to 100% on real data → **likely not correct**

#### Flag+value with EOB optimization
- Scheme A (0=zero, 1+Nbit=val, val=0→EOB): 3-bit: REAL=21.9%, RAND=35.7%
  - Shows differentiation, but consumption too low for any bit width
- Scheme B (0=zero, 10=EOB, 11+Nbit=val): all <11% consumed → too aggressive EOB
- Scheme D (00=zero, 01=EOB, 1+Nbit=val): ~12–17% consumed
- Scheme C (0=zero, 1+DC_VLC, size=0=EOB): 28% with errors

#### LSB-first bit reading
- Tested systematically — MSB-first confirmed for DC VLC
- **RULED OUT**

#### MPEG-2 Table B.15 intra AC VLC
- 12–17% consumption, 600–750 errors per frame, 30%+ error rate
- Both B.15 and MPEG-1 B.14 definitively **RULED OUT**

#### Count + Position + Value scheme
- Self-calibrating — produces duplicate positions and unreasonably large values
- **RULED OUT**

#### CBP (Coded Block Pattern)
- Multiple variants tested (per-macroblock, per-block), none achieve ~100% consumption
- **RULED OUT**

#### Systematic prefix code testing
- 25+ prefix code structures tested (all permutations of 1–3 bit prefixes for zero/nonzero/EOB)
- None reach 85% consumption on unpadded frames
- **RULED OUT**

#### Run-level coding with fixed bits
- Run=2–6 bits, level=DC VLC. Best results: run=4bit ~62%, run=3bit ~60%
- None reach 100% without self-calibrating
- **RULED OUT**

#### Fixed (run_bits, size_bits) + magnitude
- r2+s4 and r3+s4 appeared to reach 100% with "all DC first, then all AC" structure
- BUT r3+s4 self-calibrates on random data (864 blocks from random)
- r2+s4 fails on non-padded frames with interleaved DC+AC (684/864 blocks)
- Generated images produce pure noise — **RULED OUT**

#### Other schemes tested
- Unary coding: 6.9% — **RULED OUT**
- Exp-Golomb: 1.2%–11.6% — **RULED OUT**
- Rice k=0–3: 7.8%–65.7% — **RULED OUT**
- MB-level coded flag: 17% — **RULED OUT**
- Block-level coded flag: 15.9% — **RULED OUT**

#### Fixed 96-bit block budget
- AC bits mod 96 varies wildly across frames (33, 35, 16, 41, 34, 11, 39, 41, 75, 60)
- **RULED OUT**

### 16-Entry Quantization Table Hypothesis
The qtable has exactly 16 entries. This suggests **only 16 AC coefficients** (lowest
frequencies in zigzag order) may be coded per block. With DC, that's 17 values/block:
- 864 blocks × 17 values × ~6.67 bits/value = 97936 bits ≈ payload size (exact match!)
- Fixed 7-bit per coefficient (17×7=119 bits/block) gives 99.9% bit consumption
- **BUT**: all fixed-width approaches produce noise images — the coding is variable-length

### Fixed-Width AC Coding: RULED OUT
Tested DC (7-9 bits) + 16 AC (5-8 bits) fixed-width signed coefficients with IDCT:
- 7+7 = 119 bits/block: 99.9% consumption but pure noise image
- 8+7 = 120 bits/block: slightly over budget
- All three modes tested (interleaved, all-DC-first, unsigned): all produce noise
- **The bit count match is coincidental — true coding is variable-length**

### Remaining hypotheses
1. **AK8000 is a custom ASIC** — ~50% of Asahi Kasei's IC sales were custom-designed.
   No datasheet, no patents found. No one has previously reverse-engineered this codec.
   Web search confirmed: NO existing Playdia emulation, no AK8000 documentation anywhere online.
2. **Unary-run + variable-length level** — the EOB=6 zeros scheme (2.3× real-vs-random) is the
   strongest lead. The run encoding (unary) may be correct while the level encoding is wrong.
   Level may use VLC (Exp-Golomb, Huffman, or custom) instead of fixed 8-bit.
3. **Arithmetic/range coding** — explains high entropy, but rare in 1994 hardware
4. **Proprietary VLC table** stored in AK8000 ROM — impossible to determine without decap
5. **Golomb-Rice k=6 (truncated)** — would produce codes with unary prefix up to 6 bits
   + 6-bit fixed suffix = 12 max length, matching the run-length fingerprint
6. **Non-standard coefficient ordering** — maybe not zigzag but some other scan order
7. **Separate coding for different frequency bands** — different scheme for low vs high AC
8. **Only 16 AC coefficients coded** — qtable size matches, but coding scheme unknown
9. **CD-i DYUV-style pixel-domain coding** — Video DAC is Philips TDA8772AH (same as CD-i).
   97936 bits / 55296 pixels = 1.77 bits/pixel. Possible with Huffman-coded deltas.
10. **Chip decap** — may be the only way to extract the VLC table from the AK8000 ROM

## Test Games
| Game | Video LBAs | Notes |
|------|-----------|-------|
| `Mari-nee no Heya (Japan).zip` | 277, 502, 757, 1112, 1872, 3072, 5232 | Primary test game |
| `Yumi to Tokoton Playdia (Japan).zip` | 502+ | Second game, same codec |
| `Bandai Item Collection 70' (Japan).zip` | 150, 205, 235+ | Third game, confirmed same codec |
| `Dragon Ball Z - Shin Saiyajin Zetsumetsu Keikaku (Japan).zip` | — | Bandai intro identical to 3 others |
| `Ultraman - Hikari no Kyojin (Japan).zip` | — | Shares first 10 frames with DBZ |
| `Aqua Adventure (Japan).zip` | — | Different intro, same QS=13 first frame |
| `Sailor Moon (Japan).zip` | — | Bandai intro identical to DBZ |
| `Ie Naki Ko (Japan).zip` | — | Ultra-sparse padded frames found |

All games share identical qtable values and frame header format — qtable is likely hardcoded in the AK8000 chip.
4 out of 5 tested games share identical first frame (Bandai logo). DBZ and Ultraman share identical first 10 frames (full Bandai intro animation).

## Tools Created
| File | Purpose | Key Finding |
|------|---------|-------------|
| `vcodec_fullvlc.c` | Full MPEG-1 AC VLC (111 entries) | 25% bits with EOB, band-uniform → wrong |
| `vcodec_vlcimage.c` | Visual comparison DC vs VLC+EOB | VLC adds noise, not detail |
| `vcodec_diag.c` | Comprehensive diagnostics | DC stats, dequant tests, padding analysis |
| `vcodec_twopass.c` | Two-pass VLC hypothesis | Pass 1+2: 41% max, uniform bands |
| `vcodec_rawbits.c` | Raw bitstream pattern analysis | High entropy, no patterns, no autocorrelation |
| `vcodec_scan.c` | Find valid video frames in disc | Third game: video starts at LBA 150 |
| `vcodec_twostage.c` | Two-stage AC coding | Stage 1: 25%, Stage 2: 42% total |
| `vcodec_groundtruth.c` | Padded frames as ground truth | Every VLC self-calibrates to 100% |
| `vcodec_signmag.c` | Sign+magnitude AC variants | All models self-calibrate |
| `vcodec_runlevel.c` | MPEG-1 style run-level models | All failed, extreme values |
| `vcodec_bitdump.c` | Bitstream structure dump | DC=4132 bits, AC=93804 bits, 1.72 bits/coeff |
| `vcodec_mpeg1ac.c` | Various VLC-only models | All per-position models consume 100% |
| `vcodec_chromdc.c` | Chrominance DC + inter-frame | Luma+chroma VLC uses 4.0% bits |
| `vcodec_psxstyle.c` | PS1 MDEC 16-bit coding | Only 42–53% consumed |
| `vcodec_padding2.c` | Padding and header analysis | Both qtables always identical |
| `vcodec_runlevel_eg.c` | Exp-Golomb, Rice, fixed-width, length-prefix | All self-calibrate or error out |
| `vcodec_jpeg_ac.c` | JPEG standard AC Huffman + entropy analysis | 70% consumed but self-calibrates |
| `vcodec_verify.c` | **Random data control tests** (JPEG, MPEG) | JPEG AC on random ≈ real → ruled out |
| `vcodec_fixedwidth.c` | Fixed-width run-level pairs + block structure | All uniform bands → ruled out |
| `vcodec_structure.c` | Deep structural analysis, run-lengths, entropy | Run-length fingerprint discovered |
| `vcodec_reveng.c` | Run-length by bit value, n-gram, transition probs | r6/r12 anomalies found |
| `vcodec_dcvlc_ac.c` | DC luminance VLC for AC coefficients | Self-calibrates on random |
| `vcodec_flagval.c` | 1-bit flag + fixed/VLC value models | All consume 100%, uniform |
| `vcodec_eob.c` | Flag+value with EOB, Golomb-Rice, Exp-Golomb | GR(0,2) shows real≠random |
| `vcodec_lsb_boundary.c` | LSB/MSB test, padded frame boundary analysis | MSB-first confirmed, byte-aligned padding |
| `vcodec_sparse.c` | Ultra-sparse frame analysis | Ie Naki Ko: 5052 bytes padding, 62.35 bits/block |
| `vcodec_dcvlc_image.c` | DC VLC per-coefficient image generation | DC-only images validated |
| `vcodec_countpos.c` | Count + position + value AC coding test | Self-calibrating → ruled out |
| `vcodec_mpeg2intra.c` | MPEG-2 Table B.15 and MPEG-1 B.14 test | 12–17% consumption, 30%+ errors |
| `vcodec_cbp2.c` | CBP and interleaved DC/AC hypotheses | No variant reaches ~100% |
| `vcodec_prefix.c` | Systematic prefix code structure testing | 25+ structures, none reach 85% |
| `vcodec_prefix2.c` | Extended prefix + interleaved testing | Same results as prefix.c |
| `vcodec_runsize.c` | JPEG-style (run, size) AC coding test | run=4bit ~62%, run=3bit ~60% |
| `vcodec_rs_image.c` | Run-size candidate validation + image generation | r2+s4/r3+s4 produce noise → ruled out |
| *(older tools)* | Various earlier approaches | See git history |

## Emulator Status
The emulator (`playdia`) builds and runs with:
- **CPU emulation**: TLCS-870 main CPU + NEC 78K/0 I/O CPU
- **CD-ROM**: CUE/BIN disc loading, raw Mode 2/2352 sector reading
- **BIOS HLE**: Auto-scans for first F1 video sector (typically LBA 150)
- **Video pipeline**: F1/F2/F3 sector assembly, DC-only frame decode
- **Audio**: XA ADPCM decoding to ring buffer, SDL2 audio output
- **Display**: SDL2 window at 320×240, 256×144 video centered
- **Frame decode**: DC-only (blocky) — AC coefficients not yet decoded
- **Rate**: 10 sectors/frame @ 30fps (4× CD speed), ~212 frames in 300 emulator frames

## Key Insights
1. **DC coding may use MPEG-1 luminance VLC** — but random position test shows 86% success rate, so this could be coincidental
2. **Resolution is 256×144** at 4:2:0 (16×9 macroblocks, 864 blocks)
3. **Both qtable copies in header are always identical** — purpose of duplication unknown
4. **Frame types**: 0=I-frame, 1=P-frame, plus types 2, 3, 5, 6, 31, 237 found across games
5. **Qtable is constant across ALL games tested** (Mari-nee, DBZ, Ie Naki Ko) — likely hardcoded in AK8000
6. **AC coding is NOT any standard VLC** — JPEG, MPEG-1, MPEG-2, H.263, exp-Golomb, Golomb-Rice all ruled out
7. **Self-calibration trap**: ANY variable-length code applied to random data produces
   plausible-looking statistics. Always compare real vs random data before concluding.
8. **The AC bitstream is high-entropy** — consistent with arithmetic coding or unknown VLC
9. **QS values observed**: 4, 7, 8, 10, 11, 12, 13, 14, 20, 23, 26, 36, 37, 39, 40
10. **Run-length fingerprint is the best structural clue** — max run=12, r6/r12 anomalies
    suggest a coding scheme with ~6-bit maximum codeword component
11. **AK8000 is a custom ASIC** — no documentation exists anywhere, no patents, no existing emulation
12. **Padded frames provide ground truth** — exact data boundaries known from 0xFF padding
13. **MSB-first bit reading confirmed** — LSB-first tested and ruled out
14. **DC-only video decoder implemented** in `ak8000.c` with MPEG-1 luminance DC VLC, DPCM, 4:2:0 YCbCr→RGB, 256x144
15. **No fixed block budget** — AC bits mod 96 varies wildly across frames
16. **Bitstream is VLC bit-packed** — confirmed by 0xFF padding, byte-aligned flush before padding
17. **Run-length coding of zeros confirmed** — zero-run excess 7× above iid model at 8-bit runs
18. **All I-frames are fully independent** — 99%+ byte difference between consecutive I-frames
19. **Cross-game frame identity**: 4/5 games share identical Bandai logo intro; DBZ+Ultraman share first 10 frames
20. **QS decreases over intro animation**: 13→13→13→13→11→10→10→8→8→7 (frames 0-9, quality ramp-up)
21. **Frame 9 is first P-frame** (TYPE=0x01) in the Bandai intro sequence
22. **MPEG-1 AC VLC fails in ALL block organizations** — 8×8, 4×4, 16×16 MB, 8×8 MB, flat raster all error out early

## Open Questions
1. **What is the AC coding model?** The bitstream after DC is high-entropy with a distinctive
   run-length fingerprint (max run=12, r6/r12 anomalies). Every standard and semi-standard
   scheme has been ruled out. Remaining hypotheses: arithmetic coding, proprietary VLC table
   in AK8000 ROM, or Golomb-Rice variant with k≈6. May require chip decap to solve.
2. **What role does the qtable play?** DC-only works without dequantization. The qtable may
   control AC quantization step sizes but the AC coding must be solved first.
3. **What are frame types 2, 3, 5, 6, 31, 237?** Beyond I-frames (0) and P-frames (1),
   multiple other types exist with unknown semantics.
4. **Different coding for chroma vs luma AC?** The AK8000 might use separate coding tables.
5. **Non-zigzag scan order?** Coefficient positions might use a proprietary scan pattern.

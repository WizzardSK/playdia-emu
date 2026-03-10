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
  - `[36-39]`: `00 80 24 TYPE` — TYPE: 00=I-frame, 01=P-frame
- **Bitstream**: bytes 40+ (12242 bytes = 97936 bits)
- Some frames have 0xFF padding at end; most are 98–100% real data

## DC Coefficient Decoding (confirmed)

### MPEG-1 DC Luminance VLC (extended)
DC differences are coded using MPEG-1 Table B.12 (DC luminance), **extended to size 11**:
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
- DC uses **DPCM**: separate predictors for Y, Cb, Cr components
- DC uses ~3800-4400 bits per frame (~4.2% of bitstream)
- Average ~4.8 bits per DC coefficient
- Sizes 9-11 are used in frames with high contrast (needed for full decode)

### Image Layout (confirmed)
- **Resolution**: 256×144 pixels (16×9 macroblocks)
- **Color**: YCbCr 4:2:0 → 6 blocks per macroblock (4Y + Cb + Cr)
- **Total blocks**: 864 per frame
- **Block order**: macroblock-interleaved (Y0, Y1, Y2, Y3, Cb, Cr)
- **Y sub-block order**: TL, TR, BL, BR within each 16×16 macroblock
- **IDCT**: standard 8×8 DCT with DC×8 scaling, pixel = IDCT + 128

### DC-Only Images
DC-only decode (zeroing all AC) produces **recognizable color video frames** across all 3 games:
- Scene content clearly visible (landscapes, characters, objects)
- Standard BT.601 YCbCr→RGB conversion works
- DC DPCM accumulates slight error over the frame (bottom rows drift)
- Per-MB DC reset gives range [-248, 223] (acceptable for 8-bit)

## AC Coefficient Coding: UNSOLVED

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

### Remaining hypotheses
1. **AK8000 is a custom ASIC** — ~50% of Asahi Kasei's IC sales were custom-designed.
   No datasheet, no patents found. No one has previously reverse-engineered this codec.
2. **Arithmetic/range coding** — explains high entropy, but rare in 1994 hardware
3. **Proprietary VLC table** stored in AK8000 ROM — impossible to determine without decap
4. **Golomb-Rice k=6 (truncated)** — would produce codes with unary prefix up to 6 bits
   + 6-bit fixed suffix = 12 max length, matching the run-length fingerprint
5. **Non-standard coefficient ordering** — maybe not zigzag but some other scan order
6. **Block-level structure** — fixed bit budget per block or macroblock with internal coding
7. **Separate coding for different frequency bands** — different scheme for low vs high AC

## Test Games
| Game | Video LBAs | Notes |
|------|-----------|-------|
| `Mari-nee no Heya (Japan).zip` | 277, 502, 757, 1112, 1872, 3072, 5232 | Primary test game |
| `Yumi to Tokoton Playdia (Japan).zip` | 502+ | Second game, same codec |
| `Bandai Item Collection 70' (Japan).zip` | 150, 205, 235+ | Third game, confirmed same codec |

All three games share identical qtable values and frame header format.

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
| *(older tools)* | Various earlier approaches | See git history |

## Key Insights
1. **DC coding is MPEG-1 luminance VLC with DPCM** — confirmed, produces recognizable images
2. **Resolution is 256×144** at 4:2:0 (16×9 macroblocks, 864 blocks)
3. **Both qtable copies in header are always identical** — purpose of duplication unknown
4. **Frame types**: 00 = intra (I-frame), 01 = inter (P-frame); P-frames have small DC diffs
5. **Qtable is constant across all 3 games** — likely hardcoded in AK8000
6. **AC coding is NOT any standard VLC** — JPEG, MPEG-1, exp-Golomb all ruled out
7. **Self-calibration trap**: ANY variable-length code applied to random data produces
   plausible-looking statistics. Always compare real vs random data before concluding.
8. **The AC bitstream is high-entropy** — consistent with arithmetic coding or unknown VLC
9. **QS values observed**: 4, 7, 8, 10, 11, 12, 13, 14, 20, 23, 26, 36, 37, 39, 40
10. **Run-length fingerprint is the best structural clue** — max run=12, r6/r12 anomalies
    suggest a coding scheme with ~6-bit maximum codeword component
11. **AK8000 is a custom ASIC** — no documentation exists anywhere, confirmed via web search
12. **Padded frames provide ground truth** — exact data boundaries known from 0xFF padding

## Open Questions
1. **What is the AC coding model?** The bitstream after DC is high-entropy with a distinctive
   run-length fingerprint (max run=12, r6/r12 anomalies). Remaining hypotheses: arithmetic
   coding, proprietary VLC table in AK8000 ROM, or Golomb-Rice variant with k≈6.
2. **What role does the qtable play?** DC-only works without dequantization. The qtable may
   control AC quantization step sizes but the AC coding must be solved first.
3. **Is there inter-frame data in I-frames?** VLC+EOB uses 25%, but 75% of real data remains.
   Could the extra data be prediction/motion data for subsequent P-frames?
4. **LSB-first bit reading?** Not yet tested — some hardware reads bits from LSB of each byte.
5. **Different coding for chroma vs luma AC?** The AK8000 might use separate coding tables.
6. **Non-zigzag scan order?** Coefficient positions might use a proprietary scan pattern.

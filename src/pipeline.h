#pragma once
#include "playdia.h"
#include "cdrom.h"
#include "ak8000.h"

// ═══════════════════════════════════════════════════════════════
//  CD-ROM → AK8000 Pipeline
//
//  Manages the data path:
//    CD-ROM sectors → sector type detection → routing:
//      Mode 2 Form 2 (FMV/XA) → ak8000_feed_sector()
//      Mode 1 / Form 1 (data) → mem[0x8000] CD window
//
//  CD-ROM speed:  1× = 75 sectors/sec  → 2.5 sectors/frame @30fps
//  We read PIPE_SECTORS_PER_FRAME sectors each frame.
//  Extra AK8000 audio is drained into SDL each frame.
//
//  Sector type detection heuristics:
//    raw  Mode 2 Form 2 → mem[18] bit5 set → FMV
//    raw  Mode 2 Form 1 → mem[18] bit5 clear + sync → data/FMV
//    cooked → sniff for MPEG start code → decide
//    MPEG sync 0xFF 0xEx in first 16 bytes → audio stream
// ═══════════════════════════════════════════════════════════════

// Number of CD sectors to read per video frame
// 75 sectors/s ÷ 30 fps = 2.5 → we do 3 per frame
// Playdia FMV needs ~10 sectors per video frame at ~15fps
// At emulator's 30fps, read 10 sectors/frame = 300 sectors/sec ≈ 4× CD speed
#define PIPE_SECTORS_PER_FRAME  10

// How many decoded audio samples to drain per frame
// (1470 stereo pairs = one full frame of 44100/30)
#define PIPE_AUDIO_PER_FRAME    SAMPLES_PER_FRAME

typedef enum {
    SECTOR_TYPE_UNKNOWN = 0,
    SECTOR_TYPE_FMV,        // MPEG video/audio → AK8000
    SECTOR_TYPE_DATA,       // program / filesystem data → CD window
    SECTOR_TYPE_AUDIO,      // raw CDDA (not used on Playdia)
} SectorType;

typedef struct Pipeline {
    // Statistics (reset each second)
    uint32_t sectors_fed;       // to AK8000
    uint32_t sectors_skipped;   // data/unknown sectors
    uint32_t audio_samples_out; // samples drained to SDL
    uint32_t video_frames_out;  // AK8000 decoded frames

    // Running totals
    uint64_t total_sectors;
    uint64_t total_audio_samples;

    // Drain buffer: one frame of audio in SDL-ready interleaved format
    int16_t  drain_buf[PIPE_AUDIO_PER_FRAME * CHANNELS];
    int      drain_samples;  // valid samples in drain_buf (pairs)
} Pipeline;

// ── API ───────────────────────────────────────────────────────
void pipeline_init (Pipeline *pl);
void pipeline_reset(Pipeline *pl);

// Run one video frame worth of CD reads + AK8000 feeding.
// p->cdrom must have streaming=true for FMV, or call pipeline_feed_lba().
// Returns number of FMV sectors fed to AK8000.
int  pipeline_run_frame(Pipeline *pl, CDROM *cd, AK8000 *av);

// Feed a specific LBA range directly (used by HLE BIOS read commands)
int  pipeline_feed_lba(Pipeline *pl, CDROM *cd, AK8000 *av,
                       uint32_t lba, int n_sectors);

// Drain decoded audio from AK8000 ring buffer into pl->drain_buf.
// Returns number of stereo sample pairs available in drain_buf.
int  pipeline_drain_audio(Pipeline *pl, AK8000 *av);

// Debug print
void pipeline_stats(Pipeline *pl);

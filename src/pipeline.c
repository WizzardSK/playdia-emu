#include "pipeline.h"
#include <string.h>
#include <stdio.h>

// ─────────────────────────────────────────────────────────────
//  Sector type detection
//
//  Priority order:
//   1. Raw sector: check Mode byte + subheader Form flag
//   2. MPEG-PS Pack start code  (0x000001BA)
//   3. MPEG-1 video sequence    (0x000001B3)
//   4. MPEG audio sync word     (0xFF 0xEX)
//   5. Everything else          → DATA
// ─────────────────────────────────────────────────────────────
static inline uint32_t r32be(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8) | p[3];
}

static SectorType detect_type(CDROM *cd) {
    // Raw ISO: use Mode/subheader byte
    if (cd->raw_mode) {
        uint8_t mode = cd->sector_buf[15];
        if (mode == 2) {
            uint8_t flags = cd->sector_buf[18]; // subheader byte 2
            if (flags & 0x20) return SECTOR_TYPE_FMV; // Form 2 = XA/FMV
            // Form 1 — sniff payload
        }
    }

    // Sniff first 8 bytes of data payload
    const uint8_t *d = cd->data_ptr;
    int            n = cd->data_len;
    if (!d || n < 4) return SECTOR_TYPE_UNKNOWN;

    // MPEG-PS pack header
    if (n >= 4 && r32be(d) == 0x000001BA) return SECTOR_TYPE_FMV;

    // Scan up to 32 bytes for MPEG start codes / audio sync
    int scan = n < 64 ? n : 64;
    for (int i = 0; i + 3 < scan; i++) {
        uint32_t sc = r32be(d + i);
        // Video sequence / GOP / picture
        if (sc == 0x000001B3 || sc == 0x000001B8 ||
            (sc & 0xFFFFFF00) == 0x00000100)
            return SECTOR_TYPE_FMV;
        // PES video/audio streams
        if ((sc & 0xFFFFFFF0) == 0x000001E0) return SECTOR_TYPE_FMV;
        if ((sc & 0xFFFFFFE0) == 0x000001C0) return SECTOR_TYPE_FMV;
    }
    // MPEG audio sync (0xFF 0xEX or 0xFF 0xFX)
    for (int i = 0; i + 1 < scan; i++) {
        if (d[i] == 0xFF && (d[i+1] & 0xE0) == 0xE0)
            return SECTOR_TYPE_FMV;
    }

    return SECTOR_TYPE_DATA;
}

// ─────────────────────────────────────────────────────────────
void pipeline_init (Pipeline *pl) { memset(pl, 0, sizeof *pl); }
void pipeline_reset(Pipeline *pl) {
    pl->sectors_fed = pl->sectors_skipped = 0;
    pl->audio_samples_out = pl->video_frames_out = 0;
    pl->drain_samples = 0;
}

// ─────────────────────────────────────────────────────────────
//  Feed one already-read sector to AK8000 or CD window
// ─────────────────────────────────────────────────────────────
static int route_sector(Pipeline *pl, CDROM *cd, AK8000 *av,
                        uint8_t *cd_window, int window_size) {
    // Playdia XA path: check subheader for audio/video markers
    if (cd->raw_mode) {
        uint8_t submode = cd->sector_buf[18];
        uint8_t marker = cd->sector_buf[24]; // first payload byte

        // XA Audio (submode bit 2 = Audio flag)
        if (submode & 0x04) {
            ak8000_feed_xa_sector(av, cd->sector_buf);
            pl->sectors_fed++;
            pl->total_sectors++;
            return 1;
        }

        // Playdia video (F1/F2/F3 markers)
        if (marker == 0xF1 || marker == 0xF2 || marker == 0xF3) {
            ak8000_feed_xa_sector(av, cd->sector_buf);
            pl->sectors_fed++;
            pl->total_sectors++;
            return 1;
        }
    }

    // Data sector → copy to CD window (0x8000 in main CPU)
    if (cd_window && cd->data_len <= window_size)
        memcpy(cd_window, cd->data_ptr, cd->data_len);
    pl->sectors_skipped++;
    pl->total_sectors++;
    return 0;
}

// ─────────────────────────────────────────────────────────────
//  pipeline_run_frame
// ─────────────────────────────────────────────────────────────
int pipeline_run_frame(Pipeline *pl, CDROM *cd, AK8000 *av) {
    if (!cd->disc_present || !cd->streaming) return 0;

    // If waiting for player input (F2 44/50), pause streaming
    if (av->waiting_for_input) return 0;

    // Handle pending seek (from F2 command resolution)
    if (av->seek_target > 0) {
        uint32_t target = av->seek_target;
        av->seek_target = 0;
        av->interactive_pending = false;

        // Reset video frame accumulator for clean scene start
        av->vid_frame_pos = 0;

        printf("[Pipeline] SEEK → LBA %u\n", target);
        cdrom_seek(cd, target);
    }

    // Throttle: don't feed more sectors if frame queue is nearly full.
    // This prevents decoding faster than we can display.
    if (av->fq_count >= PD_FRAME_QUEUE_SIZE - 2) return 0;

    int fed = 0;
    for (int i = 0; i < PIPE_SECTORS_PER_FRAME; i++) {
        // Stop feeding if an interactive command was just parsed
        if (av->interactive_pending) break;
        // Stop feeding if queue filled during this batch
        if (av->fq_count >= PD_FRAME_QUEUE_SIZE - 1) break;

        if (cdrom_stream_tick(cd) && cd->sector_ready)
            fed += route_sector(pl, cd, av, NULL, 0);
    }
    return fed;
}

// ─────────────────────────────────────────────────────────────
//  pipeline_feed_lba  —  explicit LBA range (used by HLE BIOS)
// ─────────────────────────────────────────────────────────────
int pipeline_feed_lba(Pipeline *pl, CDROM *cd, AK8000 *av,
                      uint32_t lba, int n_sectors) {
    int fed = 0;
    for (int i = 0; i < n_sectors; i++) {
        if (cdrom_seek(cd, lba + (uint32_t)i) != 0) break;
        if (cdrom_read_sector(cd) != 0) break;
        fed += route_sector(pl, cd, av, NULL, 0);
    }
    return fed;
}

// ─────────────────────────────────────────────────────────────
//  pipeline_drain_audio
//
//  Drains samples from AK8000's ring buffer into pl->drain_buf.
//  The ring buffer is:
//    av->audio_buf[0 .. ring_size-1]  int16_t interleaved stereo
//    write_pos = next write position
//    read_pos  = next read position
//
//  We extract up to PIPE_AUDIO_PER_FRAME * CHANNELS int16_t values
//  (= 1 frame's worth of stereo samples).
// ─────────────────────────────────────────────────────────────
int pipeline_drain_audio(Pipeline *pl, AK8000 *av) {
    int ring_size = (int)(sizeof av->audio_buf / sizeof av->audio_buf[0]);
    int want      = PIPE_AUDIO_PER_FRAME * CHANNELS; // int16 values
    int avail;

    // How many int16 values are in the ring?
    int wr = av->audio_write_pos;
    int rd = av->audio_read_pos;
    avail = (wr - rd + ring_size) % ring_size;
    if (avail > want) avail = want;

    // Copy linearly, handling ring wraparound
    for (int i = 0; i < avail; i++) {
        pl->drain_buf[i] = av->audio_buf[rd];
        rd = (rd + 1) % ring_size;
    }
    av->audio_read_pos = rd;

    // If we got fewer samples than needed, zero-pad (silence)
    if (avail < want)
        memset(pl->drain_buf + avail, 0, (want - avail) * sizeof(int16_t));

    int pairs = avail / CHANNELS;
    pl->drain_samples         = pairs;
    pl->audio_samples_out    += (uint32_t)pairs;
    pl->total_audio_samples  += (uint64_t)pairs;
    return pairs;
}

// ─────────────────────────────────────────────────────────────
void pipeline_stats(Pipeline *pl) {
    printf("[Pipeline] sectors fed=%u skipped=%u | "
           "audio=%u samples | video total=%llu sectors\n",
           pl->sectors_fed, pl->sectors_skipped,
           pl->audio_samples_out,
           (unsigned long long)pl->total_sectors);
}

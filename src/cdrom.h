#pragma once
#include "zip_stream.h"
#include "playdia.h"
#include <stdio.h>

// ─────────────────────────────────────────────────────────────
//  CD-ROM Subsystem — Sanyo LC89515 + Toshiba TC9263F
//
//  Supports:
//    • Raw 2352-byte sectors (Mode 1 and Mode 2)
//    • Cooked 2048-byte sectors
//    • ISO 9660 filesystem (directory listing, file lookup)
//    • Mode 2 Form 1 (2048 data bytes) and Form 2 (2336 data bytes)
//    • Continuous streaming → AK8000
//    • TOC parsing
// ─────────────────────────────────────────────────────────────

#define CDROM_STAT_READY    0x01
#define CDROM_STAT_PLAYING  0x02
#define CDROM_STAT_BUSY     0x04
#define CDROM_STAT_ERROR    0x80

#define CDROM_CMD_SEEK      0x01
#define CDROM_CMD_READ      0x02
#define CDROM_CMD_STOP      0x03
#define CDROM_CMD_PLAY      0x04
#define CDROM_CMD_PAUSE     0x05
#define CDROM_CMD_RESUME    0x06
#define CDROM_CMD_STREAM    0x07   // start continuous streaming

// Raw sector layout (2352 bytes)
#define RAW_SYNC_OFF    0          // 12 bytes sync
#define RAW_HDR_OFF     12         // 4 bytes: M S F mode
#define RAW_SUBHDR_OFF  16         // 8 bytes: Mode 2 subheader (repeated)
#define RAW_DATA1_OFF   16         // Mode 1 data starts here (2048 bytes)
#define RAW_DATA2_OFF   24         // Mode 2 data starts here (up to 2336 bytes)

#define MODE2_FORM1_DATA 2048
#define MODE2_FORM2_DATA 2336

// Maximum files in ISO 9660 directory listing
#define ISO_MAX_FILES 256
#define ISO_MAX_NAME  32

typedef struct { uint8_t m, s, f; } MSF;

typedef struct {
    char     name[ISO_MAX_NAME];
    uint32_t lba;        // start sector
    uint32_t size;       // bytes
    bool     is_dir;
} ISOEntry;

// Multi-track source: maps LBA ranges to separate files
#define MAX_TRACK_SRCS 4
typedef struct {
    CDSource src;
    uint32_t lba_start;    // first LBA this source covers
    uint32_t n_sectors;    // number of sectors in this source
    bool     raw_mode;     // true=2352, false=2048
} TrackSource;

typedef struct CDROM {
    // Primary source (single-file mode, backwards compat)
    CDSource src;
    bool     disc_present;
    bool     motor_on;
    bool     streaming;      // continuous stream mode active

    // sector position
    uint32_t lba;
    uint32_t stream_end;     // end LBA for streaming (0 = unlimited)
    uint32_t total_sectors;

    // ISO geometry
    bool     raw_mode;       // true=2352 bytes/sector, false=2048
    uint32_t pvd_lba;        // Primary Volume Descriptor LBA (usually 16)
    uint32_t root_lba;       // root directory LBA
    uint32_t root_size;      // root directory size

    // Data buffer (2352 bytes max)
    uint8_t  sector_buf[SECTOR_SIZE];
    uint8_t *data_ptr;       // points inside sector_buf to data payload
    int      data_len;       // length of data payload
    bool     sector_ready;

    // File table (flat listing of root dir)
    ISOEntry files[ISO_MAX_FILES];
    int      n_files;

    // Status
    uint8_t  status;
    uint8_t  cmd;
    uint8_t  cmd_args[4];

    // IRQ callback (called when sector is ready)
    void    (*irq_cb)(void *ctx);
    void    *irq_ctx;

    // CUE/BIN: byte offset of data track start in BIN file
    uint32_t cue_data_offset;

    // Multi-track support
    TrackSource tracks[MAX_TRACK_SRCS];
    int         n_track_srcs;  // 0 = use legacy single src
} CDROM;

// ── API ───────────────────────────────────────────────────────
int      cdrom_init       (CDROM *cd);
void     cdrom_reset      (CDROM *cd);
int      cdrom_load_iso   (CDROM *cd, const char *path);
void     cdrom_eject      (CDROM *cd);
int      cdrom_seek       (CDROM *cd, uint32_t lba);
int      cdrom_read_sector(CDROM *cd);    // read one sector → sector_buf/data_ptr
int      cdrom_stream_tick(CDROM *cd);   // call once/frame; reads next sector if streaming
void     cdrom_write_cmd  (CDROM *cd, uint8_t cmd, uint8_t *args);
uint8_t  cdrom_read_status(CDROM *cd);

// ISO 9660
int      cdrom_parse_iso  (CDROM *cd);
ISOEntry *cdrom_find_file (CDROM *cd, const char *name);  // case-insensitive
void     cdrom_list_files (CDROM *cd);

// Utility
MSF      lba_to_msf       (uint32_t lba);
uint32_t msf_to_lba       (MSF msf);

// CUE/BIN support
#define CUE_MAX_TRACKS 20

typedef enum { TRACK_MODE1_2352=1, TRACK_MODE2_2352, TRACK_AUDIO } TrackType;

typedef struct {
    TrackType type;
    uint32_t  lba_start;   // absolute LBA on disc
    uint32_t  file_offset; // byte offset in .bin file
    uint32_t  n_sectors;
} CueTrack;

typedef struct {
    CueTrack tracks[CUE_MAX_TRACKS];
    int      n_tracks;
    int      data_track;  // index of first data track
} CueSheet;

// Load disc from CUE sheet (opens the referenced BIN automatically)
int cdrom_load_cue(CDROM *cd, const char *cue_path);

// Load disc image directly from a .zip archive (no extraction)
int cdrom_load_zip(CDROM *cd, const char *zip_path);

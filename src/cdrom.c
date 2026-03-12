#include "cdrom.h"
#include "zip_stream.h"
#include <zip.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define LBA_OFFSET 150   // LBA 0 = MSF 00:02:00

MSF lba_to_msf(uint32_t lba) {
    lba += LBA_OFFSET;
    MSF m = { lba/(75*60), (lba/75)%60, lba%75 };
    return m;
}
uint32_t msf_to_lba(MSF msf) {
    return (uint32_t)(msf.m*75*60 + msf.s*75 + msf.f) - LBA_OFFSET;
}

// ─── Little-endian helpers for ISO 9660 ──────────────────────
static inline uint16_t r16le(const uint8_t *p){ return p[0]|(p[1]<<8); }
static inline uint32_t r32le(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }

int cdrom_init(CDROM *cd) { memset(cd,0,sizeof*cd); cd->status=CDROM_STAT_READY; return 0; }
void cdrom_reset(CDROM *cd) {
    cd->lba=0; cd->sector_ready=false; cd->streaming=false;
    cd->status=CDROM_STAT_READY; cd->motor_on=false;
}

// ─────────────────────────────────────────────────────────────
//  Load ISO file
// ─────────────────────────────────────────────────────────────
int cdrom_load_iso(CDROM *cd, const char *path) {
    cds_close(&cd->src);
    cd->src.fp = fopen(path,"rb");
    if (!cds_valid(&cd->src)) { fprintf(stderr,"[CDROM] Cannot open: %s\n",path); return -1; }
    long sz = cds_size(&cd->src);

    if (sz % SECTOR_SIZE == 0) {
        cd->raw_mode = true;
        cd->total_sectors = (uint32_t)(sz / SECTOR_SIZE);
        printf("[CDROM] Raw ISO: %u sectors (%ld MB)\n", cd->total_sectors, sz/1048576);
    } else {
        cd->raw_mode = false;
        cd->total_sectors = (uint32_t)(sz / SECTOR_DATA);
        printf("[CDROM] Cooked ISO: %u sectors (%ld MB)\n", cd->total_sectors, sz/1048576);
    }

    cd->disc_present = true;
    cd->motor_on     = true;
    cd->status       = CDROM_STAT_READY;

    // Try to parse ISO 9660 filesystem
    cdrom_parse_iso(cd);
    return 0;
}

void cdrom_eject(CDROM *cd) {
    cds_close(&cd->src);
    cd->disc_present=false; cd->motor_on=false; cd->streaming=false; cd->status=0;
    printf("[CDROM] Ejected\n");
}

// ─────────────────────────────────────────────────────────────
//  Low-level sector read
//
//  Fills sector_buf[SECTOR_SIZE] and sets data_ptr/data_len:
//    Mode 1        : data_ptr = buf+16,  data_len = 2048
//    Mode 2 Form 1 : data_ptr = buf+24,  data_len = 2048
//    Mode 2 Form 2 : data_ptr = buf+24,  data_len = 2336
//    Cooked        : data_ptr = buf,     data_len = 2048
// ─────────────────────────────────────────────────────────────
// Find which track source covers a given LBA (multi-track mode)
static TrackSource *find_track_src(CDROM *cd, uint32_t lba) {
    for (int i = 0; i < cd->n_track_srcs; i++) {
        TrackSource *ts = &cd->tracks[i];
        if (lba >= ts->lba_start && lba < ts->lba_start + ts->n_sectors)
            return ts;
    }
    return NULL;
}

int cdrom_seek(CDROM *cd, uint32_t lba) {
    if (!cd->disc_present) return -1;
    if (lba >= cd->total_sectors) {
        fprintf(stderr,"[CDROM] Seek OOB: %u / %u\n", lba, cd->total_sectors);
        cd->status |= CDROM_STAT_ERROR;
        return -1;
    }
    cd->lba = lba; cd->sector_ready = false;
    return 0;
}

int cdrom_read_sector(CDROM *cd) {
    if (!cd->disc_present) return -1;
    if (cd->lba >= cd->total_sectors) return -1;

    CDSource *src;
    int bps;
    long offset;

    if (cd->n_track_srcs > 0) {
        // Multi-track mode: find the right source
        TrackSource *ts = find_track_src(cd, cd->lba);
        if (!ts || !cds_valid(&ts->src)) {
            fprintf(stderr, "[CDROM] No track source for LBA %u\n", cd->lba);
            cd->status |= CDROM_STAT_ERROR;
            return -1;
        }
        src = &ts->src;
        bps = ts->raw_mode ? SECTOR_SIZE : SECTOR_DATA;
        offset = (long)(cd->lba - ts->lba_start) * bps;
        cd->raw_mode = ts->raw_mode; // update for sector parsing below
    } else {
        // Legacy single-source mode
        if (!cds_valid(&cd->src)) return -1;
        src = &cd->src;
        bps = cd->raw_mode ? SECTOR_SIZE : SECTOR_DATA;
        offset = (long)cd->cue_data_offset + (long)cd->lba * bps;
    }

    if (cds_seek(src, offset, SEEK_SET) != 0) return -1;
    if (cds_read(src, cd->sector_buf, (size_t)bps) != (size_t)bps) {
        cd->status |= CDROM_STAT_ERROR; return -1;
    }

    if (cd->raw_mode) {
        // sector_buf[15] = mode byte
        uint8_t mode = cd->sector_buf[15];
        if (mode == 1) {
            // Mode 1: 16 bytes header + 2048 bytes data
            cd->data_ptr = cd->sector_buf + 16;
            cd->data_len = 2048;
        } else if (mode == 2) {
            // Mode 2: subheader at [16..23], then data
            // Form determined by subheader byte 2 bit 5
            uint8_t subhdr_flags = cd->sector_buf[18]; // flags (form byte)
            if (subhdr_flags & 0x20) {
                // Form 2: 2336 bytes data (used for XA/FMV)
                cd->data_ptr = cd->sector_buf + 24;
                cd->data_len = 2336;
            } else {
                // Form 1: 2048 bytes data
                cd->data_ptr = cd->sector_buf + 24;
                cd->data_len = 2048;
            }
        } else {
            // Unknown — treat as Mode 1
            cd->data_ptr = cd->sector_buf + 16;
            cd->data_len = 2048;
        }
    } else {
        // Cooked: pure 2048-byte data
        cd->data_ptr = cd->sector_buf;
        cd->data_len = 2048;
    }

    cd->sector_ready = true;
    cd->lba++;

    // Fire IRQ if callback set
    if (cd->irq_cb) cd->irq_cb(cd->irq_ctx);

    return 0;
}

// ─────────────────────────────────────────────────────────────
//  Streaming tick — call once per frame
//  Reads next sector if streaming mode active.
//  Returns 1 if a new sector was read, 0 otherwise.
// ─────────────────────────────────────────────────────────────
int cdrom_stream_tick(CDROM *cd) {
    if (!cd->streaming || !cd->disc_present) return 0;
    if (cd->stream_end > 0 && cd->lba >= cd->stream_end) {
        cd->streaming = false; return 0;
    }
    return (cdrom_read_sector(cd) == 0) ? 1 : 0;
}

// ─────────────────────────────────────────────────────────────
//  Command interface (used by NEC CPU via I/O registers)
// ─────────────────────────────────────────────────────────────
void cdrom_write_cmd(CDROM *cd, uint8_t cmd, uint8_t *args) {
    cd->cmd = cmd;
    if (args) memcpy(cd->cmd_args, args, 4);

    switch (cmd) {
    case CDROM_CMD_SEEK: {
        uint32_t lba = ((uint32_t)args[0]<<16)|((uint32_t)args[1]<<8)|args[2];
        cdrom_seek(cd, lba);
        printf("[CDROM] SEEK → LBA %u (MSF %02u:%02u:%02u)\n", lba,
               lba_to_msf(lba).m, lba_to_msf(lba).s, lba_to_msf(lba).f);
        break;
    }
    case CDROM_CMD_READ:
        cdrom_read_sector(cd);
        break;
    case CDROM_CMD_STREAM:
        cd->streaming = true;
        cd->stream_end = args ? ((uint32_t)args[0]<<16)|((uint32_t)args[1]<<8)|args[2] : 0;
        printf("[CDROM] STREAM from LBA %u\n", cd->lba);
        break;
    case CDROM_CMD_STOP:
        cd->motor_on=false; cd->streaming=false;
        cd->status &= ~CDROM_STAT_PLAYING;
        break;
    case CDROM_CMD_PLAY:
        cd->motor_on=true;
        cd->status |= CDROM_STAT_PLAYING;
        break;
    case CDROM_CMD_PAUSE:
        cd->streaming=false;
        break;
    case CDROM_CMD_RESUME:
        cd->streaming=true;
        break;
    default:
        printf("[CDROM] Unknown cmd: 0x%02X\n", cmd);
    }
}

uint8_t cdrom_read_status(CDROM *cd) { return cd->status; }

// ─────────────────────────────────────────────────────────────
//  ISO 9660 Parser
// ─────────────────────────────────────────────────────────────

// Read a specific sector's DATA portion into a 2048-byte buffer
static int iso_read_data(CDROM *cd, uint32_t lba, uint8_t *buf) {
    uint32_t saved_lba = cd->lba;
    cdrom_seek(cd, lba);
    int r = cdrom_read_sector(cd);
    if (r == 0 && cd->data_len >= 2048)
        memcpy(buf, cd->data_ptr, 2048);
    cd->lba = saved_lba;
    return r;
}

// Parse directory records from a directory sector
static void parse_directory(CDROM *cd, uint32_t dir_lba, uint32_t dir_size) {
    uint8_t buf[2048];
    uint32_t bytes_read = 0;

    while (bytes_read < dir_size && cd->n_files < ISO_MAX_FILES) {
        if (iso_read_data(cd, dir_lba, buf) != 0) break;
        dir_lba++;

        int pos = 0;
        while (pos < 2048) {
            uint8_t rec_len = buf[pos];
            if (rec_len == 0) { pos = (pos + 2047) & ~2047; break; } // skip to next sector

            // Directory Record layout:
            // [0]  = record length
            // [1]  = extended attribute length
            // [2-5] = LBA (LE)
            // [6-9] = LBA (BE) — ignored
            // [10-13] = data length (LE)
            // [25] = flags (0x02 = directory)
            // [32] = file name length
            // [33+] = file name

            if (pos + rec_len > 2048) break;

            uint32_t file_lba  = r32le(buf + pos + 2);
            uint32_t file_size = r32le(buf + pos + 10);
            uint8_t  flags     = buf[pos + 25];
            uint8_t  name_len  = buf[pos + 32];

            // Skip "." and ".." (name_len==1 with byte 0x00 or 0x01)
            if (name_len == 1 && (buf[pos+33] == 0x00 || buf[pos+33] == 0x01)) {
                pos += rec_len;
                continue;
            }

            ISOEntry *e = &cd->files[cd->n_files++];
            e->lba    = file_lba;
            e->size   = file_size;
            e->is_dir = (flags & 0x02) != 0;

            // Copy name, strip ";1" version suffix
            int n = name_len < ISO_MAX_NAME-1 ? name_len : ISO_MAX_NAME-1;
            memcpy(e->name, buf + pos + 33, n);
            e->name[n] = '\0';
            // Remove ";1"
            char *semi = strchr(e->name, ';');
            if (semi) *semi = '\0';

            pos += rec_len;
        }
        bytes_read += 2048;
    }
}

int cdrom_parse_iso(CDROM *cd) {
    if (!cd->disc_present) return -1;

    uint8_t buf[2048];

    // Primary Volume Descriptor is at LBA 16
    if (iso_read_data(cd, 16, buf) != 0) {
        printf("[CDROM] Cannot read PVD\n"); return -1;
    }

    // Check PVD signature: byte[0]=1, bytes[1-5]="CD001"
    if (buf[0] != 0x01 || memcmp(buf+1,"CD001",5) != 0) {
        printf("[CDROM] Not ISO 9660 (PVD signature missing)\n"); return -1;
    }

    // Volume name at offset 40, 32 bytes
    char volname[33]; memcpy(volname, buf+40, 32); volname[32]='\0';
    // Trim trailing spaces
    for (int i=31; i>=0 && volname[i]==' '; i--) volname[i]='\0';
    printf("[CDROM] Volume: \"%s\"\n", volname);

    // Root directory record at offset 156 (34 bytes)
    cd->root_lba  = r32le(buf + 156 + 2);
    cd->root_size = r32le(buf + 156 + 10);
    cd->pvd_lba   = 16;

    printf("[CDROM] Root dir: LBA=%u size=%u bytes\n", cd->root_lba, cd->root_size);

    // Parse root directory
    cd->n_files = 0;
    parse_directory(cd, cd->root_lba, cd->root_size);

    printf("[CDROM] Found %d entries in root directory\n", cd->n_files);
    return 0;
}

ISOEntry *cdrom_find_file(CDROM *cd, const char *name) {
    for (int i = 0; i < cd->n_files; i++) {
        // Case-insensitive comparison
        const char *a = cd->files[i].name, *b = name;
        while (*a && *b && tolower((uint8_t)*a) == tolower((uint8_t)*b)) { a++; b++; }
        if (*a == '\0' && *b == '\0') return &cd->files[i];
    }
    return NULL;
}

void cdrom_list_files(CDROM *cd) {
    printf("[CDROM] ── Root directory ─────────────────────\n");
    for (int i = 0; i < cd->n_files; i++) {
        ISOEntry *e = &cd->files[i];
        printf("  %s  %-24s  LBA=%6u  %u bytes\n",
               e->is_dir ? "DIR" : "   ", e->name, e->lba, e->size);
    }
    printf("[CDROM] ─────────────────────────────────────\n");
}

// ─────────────────────────────────────────────────────────────
//  CUE/BIN loader
// ─────────────────────────────────────────────────────────────
#include <ctype.h>

static void str_trim(char *s) {
    // Remove trailing whitespace + CR/LF
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1]=='\r'||s[n-1]=='\n'||s[n-1]==' '||s[n-1]=='\t'))
        s[--n] = '\0';
    // Remove leading whitespace
    int i = 0;
    while (s[i]==' '||s[i]=='\t') i++;
    if (i > 0) memmove(s, s+i, strlen(s+i)+1);
}

// Parse "MM:SS:FF" timecode → LBA
static uint32_t msf_parse(const char *s) {
    int m=0,sec=0,f=0;
    sscanf(s, "%d:%d:%d", &m, &sec, &f);
    return (uint32_t)(m*60*75 + sec*75 + f);
}

// Resolve BIN path relative to CUE directory
static void resolve_path(const char *cue_path, const char *bin_name,
                         char *out, int out_sz) {
    // Copy cue_path, replace filename part with bin_name
    const char *last_sep = strrchr(cue_path, '/');
#ifdef _WIN32
    const char *last_bs  = strrchr(cue_path, '\\');
    if (!last_sep || (last_bs && last_bs > last_sep)) last_sep = last_bs;
#endif
    if (last_sep) {
        int dir_len = (int)(last_sep - cue_path) + 1;
        snprintf(out, out_sz, "%.*s%s", dir_len, cue_path, bin_name);
    } else {
        snprintf(out, out_sz, "%s", bin_name);
    }
}

int cdrom_load_cue(CDROM *cd, const char *cue_path) {
    FILE *cue = fopen(cue_path, "r");
    if (!cue) {
        fprintf(stderr, "[CDROM] Cannot open CUE: %s\n", cue_path);
        return -1;
    }

    // Parse CUE: collect all FILE entries with their associated track modes
    #define MAX_CUE_BIN_FILES 8
    char  bin_paths[MAX_CUE_BIN_FILES][1024];
    bool  bin_raw[MAX_CUE_BIN_FILES];
    int   n_bin_files = 0;

    int  cur_file_idx = -1;  // index into bin_paths for current FILE
    int  n_tracks  = 0;
    int  data_track = -1;

    char line[512];
    while (fgets(line, sizeof line, cue)) {
        str_trim(line);
        if (!line[0]) continue;

        // FILE "name.bin" BINARY
        if (strncasecmp(line, "FILE", 4) == 0) {
            char *q1 = strchr(line, '"');
            char *q2 = q1 ? strchr(q1+1, '"') : NULL;
            if (q1 && q2 && n_bin_files < MAX_CUE_BIN_FILES) {
                char bin_name[512] = {0};
                int n = (int)(q2-q1-1);
                if (n >= (int)sizeof bin_name) n = (int)sizeof bin_name - 1;
                memcpy(bin_name, q1+1, n); bin_name[n] = '\0';
                cur_file_idx = n_bin_files;
                resolve_path(cue_path, bin_name,
                             bin_paths[cur_file_idx],
                             sizeof(bin_paths[0]));
                bin_raw[cur_file_idx] = true;
                n_bin_files++;
            }
            continue;
        }

        // TRACK nn MODE1/2352 | MODE2/2352 | AUDIO
        if (strncasecmp(line, "TRACK", 5) == 0) {
            char tmode[32] = {0};
            int tnum = 0;
            sscanf(line+5, " %d %31s", &tnum, tmode);
            int cur_track = tnum - 1;
            if (cur_track >= 0 && cur_track + 1 > n_tracks)
                n_tracks = cur_track + 1;

            if (strncasecmp(tmode,"MODE1",5)==0 ||
                strncasecmp(tmode,"MODE2",5)==0) {
                if (data_track < 0) data_track = cur_track;
            }
            continue;
        }
    }
    fclose(cue);

    if (n_bin_files == 0) {
        fprintf(stderr, "[CDROM] No FILE entries in CUE\n");
        return -1;
    }

    printf("[CDROM] CUE parsed: %d tracks, %d BIN files\n",
           n_tracks, n_bin_files);

    // If only one BIN file, use legacy single-source mode
    if (n_bin_files == 1) {
        cds_close(&cd->src);
        cd->src.fp = fopen(bin_paths[0], "rb");
        if (!cd->src.fp) {
            fprintf(stderr, "[CDROM] Cannot open BIN: %s\n", bin_paths[0]);
            return -1;
        }

        fseek(cd->src.fp, 0, SEEK_END);
        long bin_size = ftell(cd->src.fp);
        fseek(cd->src.fp, 0, SEEK_SET);

        cd->raw_mode        = true;
        cd->total_sectors   = (uint32_t)(bin_size / SECTOR_SIZE);
        cd->cue_data_offset = 0;
        cd->lba             = 0;
        cd->disc_present    = true;
        cd->motor_on        = true;
        cd->status          = CDROM_STAT_READY;
        cd->n_track_srcs    = 0; // use legacy single src

        printf("[CDROM] BIN: %s  (%u sectors, %ld MB)\n",
               bin_paths[0], cd->total_sectors, bin_size / 1048576);

        cdrom_parse_iso(cd);
        return 0;
    }

    // Multiple BIN files → use multi-track TrackSource array
    cd->n_track_srcs = 0;
    uint32_t lba_cursor = 0;

    for (int i = 0; i < n_bin_files && cd->n_track_srcs < MAX_TRACK_SRCS; i++) {
        FILE *fp = fopen(bin_paths[i], "rb");
        if (!fp) {
            fprintf(stderr, "[CDROM] Cannot open BIN: %s\n", bin_paths[i]);
            continue;
        }

        fseek(fp, 0, SEEK_END);
        long bin_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        TrackSource *ts = &cd->tracks[cd->n_track_srcs];
        memset(ts, 0, sizeof(*ts));
        ts->src.fp    = fp;
        ts->raw_mode  = bin_raw[i];
        ts->lba_start = lba_cursor;

        int bps = ts->raw_mode ? SECTOR_SIZE : SECTOR_DATA;
        ts->n_sectors = (uint32_t)(bin_size / bps);
        lba_cursor += ts->n_sectors;

        printf("[CDROM] Track %d: \"%s\"  LBA %u–%u (%u sectors, %ld MB)\n",
               cd->n_track_srcs + 1, bin_paths[i],
               ts->lba_start, ts->lba_start + ts->n_sectors - 1,
               ts->n_sectors, bin_size / 1048576);

        cd->n_track_srcs++;
    }

    if (cd->n_track_srcs == 0) {
        fprintf(stderr, "[CDROM] No BIN files could be opened\n");
        return -1;
    }

    cd->raw_mode        = true;
    cd->cue_data_offset = 0;
    cd->total_sectors   = lba_cursor;
    cd->disc_present    = true;
    cd->motor_on        = true;
    cd->status          = CDROM_STAT_READY;
    cd->lba             = 0;

    printf("[CDROM] Total: %u sectors across %d tracks\n",
           cd->total_sectors, cd->n_track_srcs);

    cdrom_parse_iso(cd);
    return 0;
}

// ─────────────────────────────────────────────────────────────
//  cdrom_load_zip — load ISO/BIN directly from a .zip archive
//
//  Priority order for entry selection:
//    1. .cue file → parse CUE sheet, open BIN entry by name
//    2. .bin file (largest, > 10 MB) → treat as raw BIN
//    3. .iso file → treat as cooked ISO
// ─────────────────────────────────────────────────────────────

// Find entry index by filename (case-insensitive)
static int zip_find_name(const char *zip_path, const char *name) {
    int err = 0;
    zip_t *za = zip_open(zip_path, ZIP_RDONLY, &err);
    if (!za) return -1;
    int n = (int)zip_get_num_entries(za, 0);
    int found = -1;
    for (int i = 0; i < n; i++) {
        const char *en = zip_get_name(za, (zip_uint64_t)i, 0);
        if (!en) continue;
        // Strip directory component
        const char *base = strrchr(en, '/');
        base = base ? base+1 : en;
        if (strcasecmp(base, name) == 0) { found = i; break; }
    }
    zip_close(za);
    return found;
}

// Find the largest .bin entry (likely the disc image)
static int zip_find_largest_bin(const char *zip_path) {
    int err = 0;
    zip_t *za = zip_open(zip_path, ZIP_RDONLY, &err);
    if (!za) return -1;
    int n = (int)zip_get_num_entries(za, 0);
    int best = -1; zip_uint64_t best_sz = 0;
    for (int i = 0; i < n; i++) {
        zip_stat_t st; zip_stat_index(za, (zip_uint64_t)i, 0, &st);
        if (!st.name) continue;
        size_t nl = strlen(st.name);
        if (nl >= 4 && strcasecmp(st.name + nl - 4, ".bin") == 0
            && st.size > best_sz && st.size > 2352) {
            best = i; best_sz = st.size;
        }
    }
    zip_close(za);
    return best;
}

// Read CUE text from a zip entry into a malloc'd buffer
static char *zip_read_text_entry(const char *zip_path, int idx) {
    int err = 0;
    zip_t *za = zip_open(zip_path, ZIP_RDONLY, &err);
    if (!za) return NULL;
    zip_stat_t st; zip_stat_index(za, (zip_uint64_t)idx, 0, &st);
    if (st.size > 65536) { zip_close(za); return NULL; }
    char *buf = malloc(st.size + 1);
    zip_file_t *zf = zip_fopen_index(za, (zip_uint64_t)idx, 0);
    if (!zf) { free(buf); zip_close(za); return NULL; }
    zip_int64_t got = zip_fread(zf, buf, st.size);
    buf[got > 0 ? got : 0] = '\0';
    zip_fclose(zf); zip_close(za);
    return buf;
}

// Parse CUE text, open BIN(s) from zip, set up multi-track if needed
static int load_cue_from_zip(CDROM *cd, const char *zip_path,
                              int cue_idx) {
    char *cue_text = zip_read_text_entry(zip_path, cue_idx);
    if (!cue_text) return -1;

    // Parse ALL FILE entries and their track types from CUE text
    // Format: FILE "name.bin" BINARY\n  TRACK nn MODEx/2352\n ...
    #define MAX_CUE_FILES 8
    char  file_names[MAX_CUE_FILES][512];
    bool  file_raw[MAX_CUE_FILES]; // true if MODE1/MODE2, false if AUDIO (still 2352)
    int   n_files = 0;

    char *p = cue_text;
    while (*p && n_files < MAX_CUE_FILES) {
        // Find next FILE line
        char *file_line = NULL;
        while (*p) {
            // Skip whitespace
            while (*p == ' ' || *p == '\t') p++;
            if (strncasecmp(p, "FILE", 4) == 0) { file_line = p; break; }
            // Skip to next line
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
        if (!file_line) break;

        // Extract filename
        char *q1 = strchr(p, '"');
        char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (q1 && q2) {
            int n = (int)(q2 - q1 - 1);
            if (n >= 512) n = 511;
            memcpy(file_names[n_files], q1 + 1, n);
            file_names[n_files][n] = '\0';
        }
        // Skip past this FILE line
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        // Look for TRACK line to determine mode (default: raw)
        file_raw[n_files] = true;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (strncasecmp(p, "FILE", 4) == 0) break; // next file
            if (strncasecmp(p, "TRACK", 5) == 0) {
                // All Playdia tracks are MODE2/2352, so raw=true
                file_raw[n_files] = true;
                break;
            }
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }

        n_files++;
    }
    free(cue_text);

    if (n_files == 0) {
        fprintf(stderr, "[ZIP/CUE] No FILE entries in CUE\n");
        return -1;
    }

    // Open all BIN files as track sources
    cd->n_track_srcs = 0;
    uint32_t lba_cursor = 0;

    for (int i = 0; i < n_files && cd->n_track_srcs < MAX_TRACK_SRCS; i++) {
        int bin_idx = zip_find_name(zip_path, file_names[i]);
        if (bin_idx < 0) {
            fprintf(stderr, "[ZIP/CUE] BIN not found: %s\n", file_names[i]);
            continue;
        }

        ZipStream *zs = zs_open_index(zip_path, bin_idx);
        if (!zs) continue;

        TrackSource *ts = &cd->tracks[cd->n_track_srcs];
        memset(ts, 0, sizeof(*ts));
        ts->src.zs    = zs;
        ts->raw_mode  = file_raw[i];
        ts->lba_start = lba_cursor;

        int bps = ts->raw_mode ? SECTOR_SIZE : SECTOR_DATA;
        long bin_size = zs_size(zs);
        ts->n_sectors = (uint32_t)(bin_size / bps);
        lba_cursor += ts->n_sectors;

        printf("[ZIP/CUE] Track %d: \"%s\"  LBA %u–%u (%u sectors, %ld MB)\n",
               cd->n_track_srcs + 1, file_names[i],
               ts->lba_start, ts->lba_start + ts->n_sectors - 1,
               ts->n_sectors, bin_size / 1048576);

        cd->n_track_srcs++;
    }

    if (cd->n_track_srcs == 0) {
        fprintf(stderr, "[ZIP/CUE] No BIN files could be opened\n");
        return -1;
    }

    // Also set primary src to track 1 for backwards compat (ISO parsing)
    // Actually, multi-track read_sector handles it, just set the totals
    cd->raw_mode        = true;
    cd->cue_data_offset = 0;
    cd->total_sectors   = lba_cursor;
    cd->disc_present    = true;
    cd->motor_on        = true;
    cd->status          = CDROM_STAT_READY;
    cd->lba             = 0;

    printf("[ZIP/CUE] Total: %u sectors across %d tracks\n",
           cd->total_sectors, cd->n_track_srcs);

    cdrom_parse_iso(cd);
    return 0;
}

int cdrom_load_zip(CDROM *cd, const char *zip_path) {
    printf("[ZIP] Loading from: %s\n", zip_path);
    zs_list(zip_path);

    // ── Strategy 1: find a .cue entry ────────────────────────
    int cue_idx = -1;
    {
        int err = 0;
        zip_t *za = zip_open(zip_path, ZIP_RDONLY, &err);
        if (za) {
            int n = (int)zip_get_num_entries(za, 0);
            for (int i = 0; i < n; i++) {
                const char *name = zip_get_name(za, (zip_uint64_t)i, 0);
                if (!name) continue;
                size_t nl = strlen(name);
                if (nl >= 4 && strcasecmp(name + nl - 4, ".cue") == 0)
                    { cue_idx = i; break; }
            }
            zip_close(za);
        }
    }
    if (cue_idx >= 0) {
        printf("[ZIP] Found CUE at entry %d — loading CUE/BIN pair\n", cue_idx);
        return load_cue_from_zip(cd, zip_path, cue_idx);
    }

    // ── Strategy 2: largest .bin (raw 2352) ──────────────────
    int bin_idx = zip_find_largest_bin(zip_path);
    if (bin_idx >= 0) {
        printf("[ZIP] No CUE found — opening largest .bin directly\n");
        cds_close(&cd->src);
        cd->src.zs = zs_open_index(zip_path, bin_idx);
        if (!cd->src.zs) return -1;
        cd->raw_mode      = true;
        cd->cue_data_offset = 0;
        long sz = zs_size(cd->src.zs);
        cd->total_sectors = (uint32_t)(sz / SECTOR_SIZE);
        cd->disc_present  = true; cd->motor_on = true;
        cd->status = CDROM_STAT_READY; cd->lba = 0;
        cdrom_parse_iso(cd);
        return 0;
    }

    // ── Strategy 3: .iso file ─────────────────────────────────
    {
        ZipStream *zs = zs_open_suffix(zip_path, ".iso");
        if (zs) {
            printf("[ZIP] Opening .iso entry directly\n");
            cds_close(&cd->src);
            cd->src.zs    = zs;
            cd->raw_mode  = false;
            cd->cue_data_offset = 0;
            long sz = zs_size(zs);
            cd->total_sectors = (uint32_t)(sz / SECTOR_DATA);
            cd->disc_present = true; cd->motor_on = true;
            cd->status = CDROM_STAT_READY; cd->lba = 0;
            cdrom_parse_iso(cd);
            return 0;
        }
    }

    fprintf(stderr, "[ZIP] No usable disc image found in %s\n", zip_path);
    return -1;
}

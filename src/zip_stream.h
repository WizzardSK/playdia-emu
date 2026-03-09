#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// ═══════════════════════════════════════════════════════════════
//  zip_stream — seekable FILE-like wrapper over a libzip entry
//
//  Allows reading a single file inside a .zip directly without
//  extracting to disk. Used by cdrom.c to read .bin/.iso entries.
//
//  API mirrors fopen/fread/fseek/ftell/fclose so callers are
//  interchangeable between real files and zip entries.
// ═══════════════════════════════════════════════════════════════

typedef struct ZipStream ZipStream;

// Open a specific entry by index inside zip_path.
// Returns NULL on failure.
ZipStream *zs_open_index(const char *zip_path, int entry_index);

// Open first entry matching suffix (case-insensitive), e.g. ".bin", ".iso"
ZipStream *zs_open_suffix(const char *zip_path, const char *suffix);

// List all entries in a zip to stdout
void zs_list(const char *zip_path);

// Read up to n bytes into buf. Returns bytes read (like fread).
size_t zs_read(ZipStream *zs, void *buf, size_t n);

// Seek. whence: SEEK_SET, SEEK_CUR, SEEK_END
int zs_seek(ZipStream *zs, long offset, int whence);

// Tell current position
long zs_tell(ZipStream *zs);

// Size of the entry in bytes
long zs_size(ZipStream *zs);

// Entry filename (inside zip)
const char *zs_name(ZipStream *zs);

// Close and free
void zs_close(ZipStream *zs);

// ── CDROM integration ──────────────────────────────────────────
// A CDROM-compatible source that can be either a FILE* or ZipStream*
typedef struct CDSource {
    FILE      *fp;   // set if regular file
    ZipStream *zs;   // set if zip entry
} CDSource;

static inline size_t cds_read(CDSource *s, void *buf, size_t n) {
    if (s->fp) return fread(buf, 1, n, s->fp);
    if (s->zs) return zs_read(s->zs, buf, n);
    return 0;
}
static inline int cds_seek(CDSource *s, long off, int whence) {
    if (s->fp) return fseek(s->fp, off, whence);
    if (s->zs) return zs_seek(s->zs, off, whence);
    return -1;
}
static inline long cds_tell(CDSource *s) {
    if (s->fp) return ftell(s->fp);
    if (s->zs) return zs_tell(s->zs);
    return -1;
}
static inline long cds_size(CDSource *s) {
    if (s->fp) {
        long cur = ftell(s->fp);
        fseek(s->fp, 0, SEEK_END);
        long sz = ftell(s->fp);
        fseek(s->fp, cur, SEEK_SET);
        return sz;
    }
    if (s->zs) return zs_size(s->zs);
    return -1;
}
static inline void cds_close(CDSource *s) {
    if (s->fp) { fclose(s->fp); s->fp = NULL; }
    if (s->zs) { zs_close(s->zs); s->zs = NULL; }
}
static inline bool cds_valid(CDSource *s) {
    return s->fp != NULL || s->zs != NULL;
}

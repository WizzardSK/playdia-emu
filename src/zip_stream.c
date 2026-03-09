#include "zip_stream.h"
#include <zip.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strcasecmp
#include <stdio.h>

// ─────────────────────────────────────────────────────────────
//  ZipStream internals
//
//  libzip's zip_file_t is forward-only (no seek). To support
//  seeking we use two strategies:
//
//  Small files (< ZIP_CACHE_THRESHOLD): cache entire entry in RAM,
//  serve reads from cache → full random-access, minimal memory.
//
//  Large files (CD images, typically 100-700 MB): we keep the
//  zip_file_t open and simulate seeking by:
//    - Forward seek: skip bytes by reading and discarding
//    - Backward seek: reopen the zip_file_t from the start
//
//  For CD images backward seeks are rare (only during ISO parse
//  at startup) so performance is acceptable.
// ─────────────────────────────────────────────────────────────

#define ZIP_CACHE_THRESHOLD  (4 * 1024 * 1024)  // 4 MB

struct ZipStream {
    zip_t      *za;          // zip archive handle
    zip_file_t *zf;          // current file handle
    int         entry_index; // entry index inside archive
    char        entry_name[512];
    long        entry_size;  // uncompressed size in bytes
    long        pos;         // current logical position

    // Cache for small entries
    uint8_t    *cache;
    long        cache_size;

    // Path to zip (needed to reopen zf on backward seek)
    char        zip_path[1024];
};

// Open a zip_file_t at entry_index positioned at byte 0
static zip_file_t *open_entry(zip_t *za, int idx) {
    return zip_fopen_index(za, (zip_uint64_t)idx, 0);
}

// Skip `n` bytes forward in zf by reading and discarding
static int skip_forward(zip_file_t *zf, long n) {
    char buf[4096];
    while (n > 0) {
        long chunk = n < (long)sizeof buf ? n : (long)sizeof buf;
        zip_int64_t got = zip_fread(zf, buf, (zip_uint64_t)chunk);
        if (got <= 0) return -1;
        n -= got;
    }
    return 0;
}

ZipStream *zs_open_index(const char *zip_path, int entry_index) {
    int err = 0;
    zip_t *za = zip_open(zip_path, ZIP_RDONLY, &err);
    if (!za) {
        fprintf(stderr, "[ZIP] Cannot open %s (err %d)\n", zip_path, err);
        return NULL;
    }

    zip_int64_t n_entries = zip_get_num_entries(za, 0);
    if (entry_index < 0 || entry_index >= (int)n_entries) {
        fprintf(stderr, "[ZIP] Entry index %d out of range (%lld entries)\n",
                entry_index, (long long)n_entries);
        zip_close(za);
        return NULL;
    }

    zip_stat_t st;
    if (zip_stat_index(za, (zip_uint64_t)entry_index, 0, &st) != 0) {
        fprintf(stderr, "[ZIP] stat failed for entry %d\n", entry_index);
        zip_close(za);
        return NULL;
    }

    zip_file_t *zf = open_entry(za, entry_index);
    if (!zf) {
        fprintf(stderr, "[ZIP] Cannot open entry %d: %s\n",
                entry_index, zip_strerror(za));
        zip_close(za);
        return NULL;
    }

    ZipStream *zs = calloc(1, sizeof *zs);
    zs->za          = za;
    zs->zf          = zf;
    zs->entry_index = entry_index;
    zs->entry_size  = (long)st.size;
    zs->pos         = 0;
    snprintf(zs->entry_name, sizeof zs->entry_name, "%s",
             st.name ? st.name : "(unknown)");
    snprintf(zs->zip_path, sizeof zs->zip_path, "%s", zip_path);

    // Cache small entries
    if (zs->entry_size <= ZIP_CACHE_THRESHOLD) {
        zs->cache = malloc((size_t)zs->entry_size);
        if (zs->cache) {
            zip_int64_t got = zip_fread(zf, zs->cache,
                                        (zip_uint64_t)zs->entry_size);
            zs->cache_size = (long)got;
        }
    }

    printf("[ZIP] Opened entry [%d] \"%s\" (%ld MB) %s\n",
           entry_index, zs->entry_name,
           zs->entry_size / (1024*1024),
           zs->cache ? "(cached)" : "(streaming)");
    return zs;
}

ZipStream *zs_open_suffix(const char *zip_path, const char *suffix) {
    int err = 0;
    zip_t *za = zip_open(zip_path, ZIP_RDONLY, &err);
    if (!za) {
        fprintf(stderr, "[ZIP] Cannot open %s\n", zip_path);
        return NULL;
    }

    int n = (int)zip_get_num_entries(za, 0);
    int best = -1;
    size_t suf_len = strlen(suffix);

    for (int i = 0; i < n; i++) {
        const char *name = zip_get_name(za, (zip_uint64_t)i, 0);
        if (!name) continue;
        size_t nl = strlen(name);
        if (nl >= suf_len &&
            strcasecmp(name + nl - suf_len, suffix) == 0) {
            best = i;
            break;
        }
    }
    zip_close(za);

    if (best < 0) {
        fprintf(stderr, "[ZIP] No entry with suffix \"%s\" in %s\n",
                suffix, zip_path);
        return NULL;
    }
    return zs_open_index(zip_path, best);
}

void zs_list(const char *zip_path) {
    int err = 0;
    zip_t *za = zip_open(zip_path, ZIP_RDONLY, &err);
    if (!za) { fprintf(stderr, "[ZIP] Cannot open %s\n", zip_path); return; }
    int n = (int)zip_get_num_entries(za, 0);
    printf("[ZIP] %s — %d entries:\n", zip_path, n);
    for (int i = 0; i < n; i++) {
        zip_stat_t st; zip_stat_index(za, (zip_uint64_t)i, 0, &st);
        printf("  [%2d] %-40s  %7ld KB\n",
               i, st.name ? st.name : "?", (long)st.size/1024);
    }
    zip_close(za);
}

const char *zs_name(ZipStream *zs) { return zs->entry_name; }
long        zs_size(ZipStream *zs) { return zs->entry_size; }
long        zs_tell(ZipStream *zs) { return zs->pos; }

size_t zs_read(ZipStream *zs, void *buf, size_t n) {
    if (zs->pos >= zs->entry_size) return 0;
    size_t avail = (size_t)(zs->entry_size - zs->pos);
    if (n > avail) n = avail;

    if (zs->cache) {
        memcpy(buf, zs->cache + zs->pos, n);
        zs->pos += (long)n;
        return n;
    }

    zip_int64_t got = zip_fread(zs->zf, buf, (zip_uint64_t)n);
    if (got > 0) zs->pos += (long)got;
    return got > 0 ? (size_t)got : 0;
}

int zs_seek(ZipStream *zs, long offset, int whence) {
    long new_pos;
    switch (whence) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = zs->pos + offset; break;
        case SEEK_END: new_pos = zs->entry_size + offset; break;
        default: return -1;
    }
    if (new_pos < 0) new_pos = 0;
    if (new_pos > zs->entry_size) new_pos = zs->entry_size;

    // Cached: just move pointer
    if (zs->cache) { zs->pos = new_pos; return 0; }

    // Streaming: forward seek = skip bytes
    if (new_pos >= zs->pos) {
        if (skip_forward(zs->zf, new_pos - zs->pos) != 0) return -1;
        zs->pos = new_pos;
        return 0;
    }

    // Backward seek: reopen entry from start, then skip forward
    zip_fclose(zs->zf);
    zs->zf = open_entry(zs->za, zs->entry_index);
    if (!zs->zf) return -1;
    zs->pos = 0;
    if (new_pos > 0 && skip_forward(zs->zf, new_pos) != 0) return -1;
    zs->pos = new_pos;
    return 0;
}

void zs_close(ZipStream *zs) {
    if (!zs) return;
    if (zs->zf)    zip_fclose(zs->zf);
    if (zs->za)    zip_close(zs->za);
    if (zs->cache) free(zs->cache);
    free(zs);
}

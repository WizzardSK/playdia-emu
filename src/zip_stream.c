#include "zip_stream.h"
#include "miniz/miniz.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strcasecmp
#include <stdio.h>

// ─────────────────────────────────────────────────────────────
//  ZipStream internals
//
//  Reads a zip entry through miniz, which is vendored in src/miniz so the
//  libretro core does not need libzip on every platform it is built for.
//
//  miniz's extraction iterator is forward-only, like libzip's file handle
//  was, so seeking works the same way as before:
//
//  Small files (< ZIP_CACHE_THRESHOLD): cache the whole entry in RAM and
//  serve reads from there → full random access, little memory.
//
//  Large files (CD images, typically 100-700 MB): keep the iterator open
//  and simulate seeking by
//    - forward seek: read and discard
//    - backward seek: restart the iterator from the beginning
//
//  For CD images backward seeks are rare (the ISO parse at startup), so
//  that is fast enough.
// ─────────────────────────────────────────────────────────────

#define ZIP_CACHE_THRESHOLD  (4 * 1024 * 1024)  // 4 MB

struct ZipStream {
    mz_zip_archive  za;          // zip archive
    mz_zip_reader_extract_iter_state *iter;  // current entry reader
    int         entry_index; // entry index inside archive
    char        entry_name[512];
    long        entry_size;  // uncompressed size in bytes
    long        pos;         // current logical position

    // Cache for small entries
    uint8_t    *cache;
    long        cache_size;

    // Path to zip (kept for messages)
    char        zip_path[1024];
};

static const char *za_error(mz_zip_archive *za)
{
    return mz_zip_get_error_string(mz_zip_get_last_error(za));
}

// Open an archive read-only, or NULL
static int open_archive(mz_zip_archive *za, const char *path)
{
    mz_zip_zero_struct(za);

    if (!mz_zip_reader_init_file(za, path, 0)) {
        fprintf(stderr, "[ZIP] Cannot open %s: %s\n", path, za_error(za));
        return -1;
    }

    return 0;
}

// Start reading entry idx from byte 0
static mz_zip_reader_extract_iter_state *open_entry(mz_zip_archive *za, int idx)
{
    return mz_zip_reader_extract_iter_new(za, (mz_uint)idx, 0);
}

// Skip `n` bytes forward by reading and discarding
static int skip_forward(mz_zip_reader_extract_iter_state *iter, long n)
{
    char buf[4096];
    while (n > 0) {
        size_t chunk = n < (long)sizeof buf ? (size_t)n : sizeof buf;
        size_t got = mz_zip_reader_extract_iter_read(iter, buf, chunk);
        if (got == 0) return -1;
        n -= (long)got;
    }
    return 0;
}

ZipStream *zs_open_index(const char *zip_path, int entry_index) {
    ZipStream *zs = calloc(1, sizeof *zs);
    if (!zs) return NULL;

    if (open_archive(&zs->za, zip_path) != 0) {
        free(zs);
        return NULL;
    }

    const int n_entries = (int)mz_zip_reader_get_num_files(&zs->za);
    if (entry_index < 0 || entry_index >= n_entries) {
        fprintf(stderr, "[ZIP] Entry index %d out of range (%d entries)\n",
                entry_index, n_entries);
        mz_zip_reader_end(&zs->za);
        free(zs);
        return NULL;
    }

    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zs->za, (mz_uint)entry_index, &st)) {
        fprintf(stderr, "[ZIP] stat failed for entry %d: %s\n",
                entry_index, za_error(&zs->za));
        mz_zip_reader_end(&zs->za);
        free(zs);
        return NULL;
    }

    zs->iter = open_entry(&zs->za, entry_index);
    if (!zs->iter) {
        fprintf(stderr, "[ZIP] Cannot open entry %d: %s\n",
                entry_index, za_error(&zs->za));
        mz_zip_reader_end(&zs->za);
        free(zs);
        return NULL;
    }

    zs->entry_index = entry_index;
    zs->entry_size  = (long)st.m_uncomp_size;
    zs->pos         = 0;
    snprintf(zs->entry_name, sizeof zs->entry_name, "%s", st.m_filename);
    snprintf(zs->zip_path, sizeof zs->zip_path, "%s", zip_path);

    // Cache small entries
    if (zs->entry_size <= ZIP_CACHE_THRESHOLD) {
        zs->cache = malloc((size_t)zs->entry_size);
        if (zs->cache) {
            const size_t got = mz_zip_reader_extract_iter_read(
                zs->iter, zs->cache, (size_t)zs->entry_size);
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
    mz_zip_archive za;

    if (open_archive(&za, zip_path) != 0)
        return NULL;

    const int n = (int)mz_zip_reader_get_num_files(&za);
    const size_t suf_len = strlen(suffix);
    int best = -1;

    for (int i = 0; i < n; i++) {
        char name[512];
        if (!mz_zip_reader_get_filename(&za, (mz_uint)i, name, sizeof name))
            continue;

        const size_t nl = strlen(name);
        if (nl >= suf_len && strcasecmp(name + nl - suf_len, suffix) == 0) {
            best = i;
            break;
        }
    }

    mz_zip_reader_end(&za);

    if (best < 0) {
        fprintf(stderr, "[ZIP] No entry with suffix \"%s\" in %s\n",
                suffix, zip_path);
        return NULL;
    }
    return zs_open_index(zip_path, best);
}

void zs_list(const char *zip_path) {
    mz_zip_archive za;

    if (open_archive(&za, zip_path) != 0)
        return;

    const int n = (int)mz_zip_reader_get_num_files(&za);
    printf("[ZIP] %s — %d entries:\n", zip_path, n);

    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, (mz_uint)i, &st)) continue;
        printf("  [%2d] %-40s  %7ld KB\n",
               i, st.m_filename, (long)(st.m_uncomp_size / 1024));
    }

    mz_zip_reader_end(&za);
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

    const size_t got = mz_zip_reader_extract_iter_read(zs->iter, buf, n);
    zs->pos += (long)got;
    return got;
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
        if (skip_forward(zs->iter, new_pos - zs->pos) != 0) return -1;
        zs->pos = new_pos;
        return 0;
    }

    // Backward seek: restart the entry, then skip forward
    mz_zip_reader_extract_iter_free(zs->iter);
    zs->iter = open_entry(&zs->za, zs->entry_index);
    if (!zs->iter) return -1;
    zs->pos = 0;
    if (new_pos > 0 && skip_forward(zs->iter, new_pos) != 0) return -1;
    zs->pos = new_pos;
    return 0;
}

void zs_close(ZipStream *zs) {
    if (!zs) return;
    if (zs->iter) mz_zip_reader_extract_iter_free(zs->iter);
    mz_zip_reader_end(&zs->za);
    free(zs->cache);
    free(zs);
}

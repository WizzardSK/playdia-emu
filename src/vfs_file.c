#define _GNU_SOURCE
#include "vfs_file.h"
#include "libretro.h"

#include <stdlib.h>
#include <string.h>

// fopencookie is glibc, funopen64 is bionic; without either there is nothing
// to wrap a VFS handle in, and the core falls back to plain fopen.
#if defined(__GLIBC__)
#define PD_HAVE_FOPENCOOKIE 1
#elif defined(__ANDROID__)
#define PD_HAVE_FUNOPEN64 1
#endif

static struct retro_vfs_interface *g_vfs;
static unsigned g_vfs_version;

void pd_vfs_set_interface(struct retro_vfs_interface *vfs, unsigned version)
{
    g_vfs = vfs;
    g_vfs_version = version;
}

// A path the C library cannot resolve carries a scheme, e.g. content://
static bool has_scheme(const char *path)
{
    return path && strstr(path, "://") != NULL;
}

#if defined(PD_HAVE_FOPENCOOKIE) || defined(PD_HAVE_FUNOPEN64)

static ssize_t vfs_read_cb(void *cookie, char *buf, size_t size)
{
    const int64_t got = g_vfs->read((struct retro_vfs_file_handle *)cookie, buf, (uint64_t)size);
    return got < 0 ? -1 : (ssize_t)got;
}

static ssize_t vfs_write_cb(void *cookie, const char *buf, size_t size)
{
    const int64_t put = g_vfs->write((struct retro_vfs_file_handle *)cookie, buf, (uint64_t)size);
    return put < 0 ? -1 : (ssize_t)put;
}

static int vfs_close_cb(void *cookie)
{
    return g_vfs->close((struct retro_vfs_file_handle *)cookie);
}

// RetroArch's seek() answers 0 on success rather than the new offset. Handing
// that back would have the C library subtract its read-ahead from it and end
// up with a negative position, so the position comes from tell().
static int64_t vfs_seek_to(void *cookie, int64_t offset, int whence)
{
    struct retro_vfs_file_handle *h = (struct retro_vfs_file_handle *)cookie;

    int vfs_whence = RETRO_VFS_SEEK_POSITION_START;

    switch (whence)
    {
    case SEEK_SET: vfs_whence = RETRO_VFS_SEEK_POSITION_START;   break;
    case SEEK_CUR: vfs_whence = RETRO_VFS_SEEK_POSITION_CURRENT; break;
    case SEEK_END: vfs_whence = RETRO_VFS_SEEK_POSITION_END;     break;
    default: return -1;
    }

    if (g_vfs->seek(h, offset, vfs_whence) < 0)
        return -1;

    return g_vfs->tell(h);
}
#endif

#if defined(PD_HAVE_FOPENCOOKIE)
static int cookie_seek(void *cookie, off64_t *offset, int whence)
{
    const int64_t pos = vfs_seek_to(cookie, *offset, whence);
    if (pos < 0) return -1;
    *offset = (off64_t)pos;
    return 0;
}
#elif defined(PD_HAVE_FUNOPEN64)
static int funopen_read(void *cookie, char *buf, int size)
{
    return (int)vfs_read_cb(cookie, buf, (size_t)size);
}
static int funopen_write(void *cookie, const char *buf, int size)
{
    return (int)vfs_write_cb(cookie, buf, (size_t)size);
}
static off64_t funopen_seek(void *cookie, off64_t offset, int whence)
{
    return (off64_t)vfs_seek_to(cookie, offset, whence);
}
#endif

FILE *pd_fopen(const char *path, const char *mode)
{
    if (!g_vfs || !has_scheme(path))
        return fopen(path, mode);

#if defined(PD_HAVE_FOPENCOOKIE) || defined(PD_HAVE_FUNOPEN64)
    unsigned access = RETRO_VFS_FILE_ACCESS_READ;

    if (strchr(mode, 'w'))
        access = RETRO_VFS_FILE_ACCESS_WRITE;
    else if (strchr(mode, 'a') || strchr(mode, '+'))
        access = RETRO_VFS_FILE_ACCESS_READ_WRITE;

    struct retro_vfs_file_handle *h =
        g_vfs->open(path, access, RETRO_VFS_FILE_ACCESS_HINT_NONE);

    if (!h)
        return NULL;

#if defined(PD_HAVE_FOPENCOOKIE)
    const cookie_io_functions_t io = {
        .read  = vfs_read_cb,
        .write = vfs_write_cb,
        .seek  = cookie_seek,
        .close = vfs_close_cb,
    };

    FILE *fp = fopencookie(h, mode, io);
#else
    FILE *fp = funopen64(h, funopen_read, funopen_write, funopen_seek, vfs_close_cb);
#endif

    if (!fp)
        g_vfs->close(h);

    return fp;
#else
    // Nothing to wrap the handle in on this platform.
    return NULL;
#endif
}

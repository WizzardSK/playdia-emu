#pragma once
#include <stdio.h>

// ─────────────────────────────────────────────────────────────
//  Opening content through the frontend
//
//  The core sets need_fullpath, so it opens the disc itself. On Android the
//  path the frontend hands over is a Storage Access Framework content:// URI,
//  which no C library can resolve - only the frontend can. Paths carrying a
//  URI scheme therefore go through the libretro VFS, wrapped back into a
//  FILE* so the rest of the emulator does not have to change.
//
//  Without a VFS interface, or for an ordinary local path, pd_fopen is fopen.
// ─────────────────────────────────────────────────────────────

struct retro_vfs_interface;

void  pd_vfs_set_interface(struct retro_vfs_interface *vfs, unsigned version);
FILE *pd_fopen(const char *path, const char *mode);

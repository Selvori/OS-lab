#ifndef OSLAB_FS_H
#define OSLAB_FS_H

#include <kernel/types.h>

#define FS_ROOT 1U
#define FS_NAME_MAX 27
#define FS_PATH_MAX 255
#define FS_MAX_FILE (12 * 512)

enum fs_type { FS_FILE = 1, FS_DIR = 2 };
enum fs_error {
    FS_NOENT = -2, FS_IO = -5, FS_NODEV = -19, FS_NOTDIR = -20,
    FS_ISDIR = -21, FS_INVAL = -22, FS_NOSPC = -28, FS_NAMETOOLONG = -36,
    FS_NOTEMPTY = -39, FS_EXIST = -17,
};

struct fs_info {
    uint32 ino;
    uint32 type;
    uint32 size;
};

struct fs_dirent {
    uint32 ino;
    char name[FS_NAME_MAX + 1];
};

int fs_mount(void);
int fs_lookup(uint32 cwd, const char *path);
int fs_stat(uint32 ino, struct fs_info *info);
int fs_read(uint32 ino, uint32 offset, void *data, size_t size);
int fs_write(uint32 ino, uint32 offset, const void *data, size_t size);
int fs_truncate(uint32 ino);
/* A nonnegative readdir result is zero at EOF or the next cursor. */
int fs_readdir(uint32 ino, uint32 cursor, struct fs_dirent *entry);
int fs_create(uint32 cwd, const char *path, enum fs_type type);
int fs_remove(uint32 cwd, const char *path);
int fs_sync(void);

#endif

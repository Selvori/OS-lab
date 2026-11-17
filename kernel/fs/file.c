#include <kernel/console.h>
#include <kernel/file_abi.h>
#include <kernel/fs.h>
#include <kernel/print.h>
#include <kernel/proc.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/user.h>

#define NFILE 48

enum file_kind { DISK_FILE, CONSOLE_IN, CONSOLE_OUT };

struct kfile {
    unsigned refs;
    enum file_kind kind;
    uint32 ino;
    uint32 cursor;
    unsigned flags;
};

/* The boot hart owns this table, just as it owns the process table. */
static struct kfile open_files[NFILE];
static struct kfile console_in = { .kind = CONSOLE_IN, .flags = OPEN_RDONLY };
static struct kfile console_out = { .kind = CONSOLE_OUT, .flags = OPEN_WRONLY };

void files_attach_console(struct proc *p)
{
    console_in.refs++;
    console_out.refs += 2;
    p->files[0] = &console_in;
    p->files[1] = &console_out;
    p->files[2] = &console_out;
}

void files_fork(struct proc *child, const struct proc *parent)
{
    child->cwd = parent->cwd;
    for (unsigned i = 0; i < NFD; ++i) {
        child->files[i] = parent->files[i];
        if (child->files[i] != NULL)
            child->files[i]->refs++;
    }
}

static void close_file(struct proc *p, unsigned fd)
{
    assert(p->files[fd] != NULL && p->files[fd]->refs != 0, "close file reference");
    p->files[fd]->refs--;
    p->files[fd] = NULL;
}

void files_drop(struct proc *p)
{
    for (unsigned i = 0; i < NFD; ++i) {
        if (p->files[i] != NULL)
            close_file(p, i);
    }
}

static int free_fd(const struct proc *p)
{
    for (unsigned i = 0; i < NFD; ++i) {
        if (p->files[i] == NULL)
            return (int)i;
    }
    return -24;
}

static int copy_path(const struct proc *p, uintptr_t address, char *path)
{
    for (unsigned i = 0; i <= FS_PATH_MAX; ++i) {
        if (user_copyin(p->pgtbl, &path[i], address + i, 1) != 0)
            return -14;
        if (path[i] == '\0')
            return 0;
    }
    return FS_NAMETOOLONG;
}

static int sys_open(struct proc *p, const char *path, uint64 flags)
{
    const uint64 supported = OPEN_WRONLY | OPEN_RDWR | OPEN_CREATE | OPEN_TRUNC | OPEN_APPEND;
    unsigned mode = flags & 3;
    if ((flags & ~supported) != 0 || mode == 3 ||
        (mode == OPEN_RDONLY && (flags & (OPEN_TRUNC | OPEN_APPEND)) != 0))
        return FS_INVAL;

    int fd = free_fd(p);
    if (fd < 0)
        return fd;
    struct kfile *file = NULL;
    for (unsigned i = 0; i < NFILE; ++i) {
        if (open_files[i].refs == 0) {
            file = &open_files[i];
            break;
        }
    }
    if (file == NULL)
        return -23;

    int ino = fs_lookup(p->cwd, path);
    bool create = (flags & OPEN_CREATE) != 0 && ino == FS_NOENT;
    if (create)
        ino = fs_create(p->cwd, path, FS_FILE);
    if (ino < 0)
        return ino;

    if (!create) {
        struct fs_info info;
        int rc = fs_stat(ino, &info);
        if (rc != 0)
            return rc;
        if (info.type == FS_DIR && (mode != OPEN_RDONLY || (flags & OPEN_CREATE) != 0))
            return FS_ISDIR;
        if (flags & OPEN_TRUNC) {
            rc = fs_truncate(ino);
            if (rc != 0)
                return rc;
        }
    }
    *file = (struct kfile){ .refs = 1, .kind = DISK_FILE, .ino = ino, .flags = flags };
    p->files[fd] = file;
    return fd;
}

static long sys_read(struct proc *p, struct kfile *file, uintptr_t address, size_t count)
{
    if ((file->flags & 3) == OPEN_WRONLY)
        return -9;
    uint8 data[FILE_IO_MAX];
    if (count > sizeof(data))
        return FS_INVAL;
    if (!user_writable(p->pgtbl, address, count))
        return -14;
    int n = file->kind == CONSOLE_IN ? console_read(data, count) :
            fs_read(file->ino, file->cursor, data, count);
    if (n > 0) {
        if (user_copyout(p->pgtbl, address, data, (size_t)n) != 0)
            return -14;
        if (file->kind == DISK_FILE)
            file->cursor += n;
    }
    return n;
}

static long sys_write(struct proc *p, struct kfile *file, uintptr_t address, size_t count)
{
    if ((file->flags & 3) == OPEN_RDONLY)
        return -9;
    char data[FILE_IO_MAX];
    if (count > sizeof(data))
        return FS_INVAL;
    if (user_copyin(p->pgtbl, data, address, count) != 0)
        return -14;
    if (file->kind == CONSOLE_OUT) {
        for (size_t i = 0; i < count; ++i)
            printf("%c", data[i]);
        return (long)count;
    }
    uint32 offset = file->cursor;
    if (file->flags & OPEN_APPEND) {
        struct fs_info info;
        int rc = fs_stat(file->ino, &info);
        if (rc != 0)
            return rc;
        offset = info.size;
    }
    int n = fs_write(file->ino, offset, data, count);
    if (n > 0)
        file->cursor = offset + n;
    return n;
}

static int sys_getcwd(struct proc *p, uintptr_t address, size_t size)
{
    char path[FS_PATH_MAX + 1];
    size_t pos = FS_PATH_MAX;
    path[pos] = '\0';

    uint32 child = p->cwd;
    while (child != FS_ROOT) {
        int parent = fs_lookup(child, "..");
        if (parent < 0)
            return parent;
        uint32 cursor = 0;
        struct fs_dirent entry;
        for (;;) {
            int next = fs_readdir(parent, cursor, &entry);
            if (next <= 0)
                return next == 0 ? FS_NOENT : next;
            cursor = next;
            if (entry.ino == child && !(entry.name[0] == '.' &&
                (entry.name[1] == '\0' || (entry.name[1] == '.' && entry.name[2] == '\0'))))
                break;
        }
        size_t n = 0;
        while (entry.name[n] != '\0')
            ++n;
        if (n + 1 > pos)
            return FS_NAMETOOLONG;
        pos -= n;
        memcpy(path + pos, entry.name, n);
        path[--pos] = '/';
        child = parent;
    }

    if (pos == FS_PATH_MAX)
        path[--pos] = '/';
    size_t count = sizeof(path) - pos;
    if (count > size)
        return -34;
    return user_copyout(p->pgtbl, address, path + pos, count) == 0 ? (int)(count - 1) : -14;
}

static long path_operation(struct proc *p, uint64 op, uintptr_t address, uint64 arg)
{
    char path[FS_PATH_MAX + 1];
    int status = copy_path(p, address, path);
    if (status != 0)
        return status;
    switch (op) {
    case SYS_OPEN:
        return sys_open(p, path, arg);
    case SYS_EXEC:
        return proc_exec_image(p, path, arg);
    case SYS_MKDIR: {
        int ino = fs_create(p->cwd, path, FS_DIR);
        return ino < 0 ? ino : 0;
    }
    case SYS_CHDIR: {
        int ino = fs_lookup(p->cwd, path);
        if (ino < 0)
            return ino;
        struct fs_info info;
        status = fs_stat(ino, &info);
        if (status != 0)
            return status;
        if (info.type != FS_DIR)
            return FS_NOTDIR;
        p->cwd = ino;
        return 0;
    }
    case SYS_UNLINK: {
        int ino = fs_lookup(p->cwd, path);
        if (ino < 0)
            return ino;
        if (ino == FS_ROOT || proc_cwd_busy(ino))
            return -16;
        for (unsigned i = 0; i < NFILE; ++i) {
            if (open_files[i].refs != 0 && open_files[i].ino == (uint32)ino)
                return -16;
        }
        return fs_remove(p->cwd, path);
    }
    default:
        return -38;
    }
}

long file_syscall(struct proc *p)
{
    struct user_trapframe *tf = p->tf;
    uint64 op = tf->x[17];
    uint64 arg0 = tf->x[10], arg1 = tf->x[11], arg2 = tf->x[12];
    switch (op) {
    case SYS_OPEN:
    case SYS_CHDIR:
    case SYS_MKDIR:
    case SYS_UNLINK:
    case SYS_EXEC:
        return path_operation(p, op, arg0, arg1);
    case SYS_SYNC:
        return fs_sync();
    case SYS_GETCWD:
        return sys_getcwd(p, arg0, arg1);
    case SYS_READ:
    case SYS_WRITE:
    case SYS_CLOSE:
    case SYS_DUP:
    case SYS_FSTAT:
    case SYS_READDIR:
        break;
    default:
        return -38;
    }

    if (arg0 >= NFD || p->files[arg0] == NULL)
        return -9;
    struct kfile *file = p->files[arg0];

    switch (op) {
    case SYS_READ:
        return sys_read(p, file, arg1, arg2);
    case SYS_WRITE:
        return sys_write(p, file, arg1, arg2);
    case SYS_CLOSE:
        close_file(p, arg0);
        return 0;
    case SYS_DUP: {
        int fd = free_fd(p);
        if (fd < 0)
            return fd;
        file->refs++;
        p->files[fd] = file;
        return fd;
    }
    case SYS_FSTAT: {
        struct fs_info info = { .type = FS_DEVICE };
        int status = file->kind == DISK_FILE ? fs_stat(file->ino, &info) : 0;
        if (status != 0)
            return status;
        return user_copyout(p->pgtbl, arg1, &info, sizeof(info)) == 0 ? 0 : -14;
    }
    case SYS_READDIR: {
        if (file->kind != DISK_FILE)
            return FS_NOTDIR;
        struct fs_dirent entry;
        if (!user_writable(p->pgtbl, arg1, sizeof(entry)))
            return -14;
        int next = fs_readdir(file->ino, file->cursor, &entry);
        if (next <= 0)
            return next;
        if (user_copyout(p->pgtbl, arg1, &entry, sizeof(entry)) != 0)
            return -14;
        file->cursor = next;
        return 1;
    }
    default:
        return -38;
    }
}

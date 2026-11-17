#include <kernel/file_abi.h>
#include <kernel/fs.h>
#include <kernel/page.h>
#include <kernel/proc.h>
#include <kernel/string.h>
#include <kernel/user.h>

struct executable {
    uint32 magic;
    uint32 length;
};

static int prepare_arguments(const struct proc *p, const char *path,
                             uintptr_t argv, uint8 *stack, uintptr_t *sp)
{
    uintptr_t pointers[EXEC_MAX_ARGS + 1] = {0};
    size_t top = PAGE_SIZE;
    unsigned argc = 0;
    if (argv == 0) {
        size_t len = 0;
        while (path[len] != '\0')
            ++len;
        if (len > EXEC_ARG_MAX)
            return -7;
        top -= len + 1;
        memcpy(stack + top, path, len + 1);
        pointers[argc++] = USER_STACK + top;
    } else {
        for (;;) {
            uintptr_t arg;
            if (user_copyin(p->pgtbl, &arg, argv + argc * sizeof(arg),
                            sizeof(arg)) != 0)
                return -14;
            if (arg == 0)
                break;
            if (argc == EXEC_MAX_ARGS)
                return -7;

            char text[EXEC_ARG_MAX + 1];
            size_t n = 0;
            do {
                if (n == sizeof(text))
                    return -7;
                if (user_copyin(p->pgtbl, &text[n], arg + n, 1) != 0)
                    return -14;
            } while (text[n++] != '\0');
            top -= n;
            memcpy(stack + top, text, n);
            pointers[argc++] = USER_STACK + top;
        }
    }
    size_t bytes = (argc + 1) * sizeof(pointers[0]);
    top = (top - bytes) & ~(size_t)15;
    memcpy(stack + top, pointers, bytes);
    *sp = USER_STACK + top;
    return (int)argc;
}

int proc_exec_image(struct proc *p, const char *path, uintptr_t argv)
{
    int ino = fs_lookup(p->cwd, path);
    if (ino < 0)
        return ino;
    struct fs_info info;
    int rc = fs_stat(ino, &info);
    if (rc != 0)
        return rc;
    struct executable header;
    if (info.type != FS_FILE || info.size < sizeof(header))
        return -8;
    if (fs_read(ino, 0, &header, sizeof(header)) != (int)sizeof(header))
        return FS_IO;
    if (header.magic != EXEC_MAGIC || header.length == 0 ||
        header.length > PAGE_SIZE || info.size != sizeof(header) + header.length)
        return -8;

    uint8 *code = page_alloc(PAGE_ZONE_USER);
    uint8 *stack = page_alloc(PAGE_ZONE_USER);
    uint8 *data = page_alloc(PAGE_ZONE_USER);
    rc = -12;
    if (code == NULL || stack == NULL || data == NULL)
        goto done;
    rc = fs_read(ino, sizeof(header), code, header.length);
    if (rc != (int)header.length) {
        rc = FS_IO;
        goto done;
    }
    uintptr_t sp;
    int argc = prepare_arguments(p, path, argv, stack, &sp);
    if (argc < 0) {
        rc = argc;
        goto done;
    }
    struct proc heap = { .owner = p->owner };
    rc = user_heap_init(&heap);
    if (rc != 0)
        goto done;

    user_heap_destroy(p);
    p->mmap = heap.mmap;
    p->heap_top = heap.heap_top;
    user_replace_image(p, code, data, stack);
    memset(p->tf, 0, sizeof(*p->tf));
    p->tf->epc = USER_BASE;
    p->tf->x[2] = sp;
    p->tf->x[11] = sp;
    /* SYS_EXEC returns argc into saved a0. Boot has no syscall dispatcher. */
    p->tf->x[10] = argc;
    __asm__ volatile("fence.i" ::: "memory");
    return argc;
done:
    if (data != NULL)
        page_free(data);
    if (stack != NULL)
        page_free(stack);
    if (code != NULL)
        page_free(code);
    return rc;
}

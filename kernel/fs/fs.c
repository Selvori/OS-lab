#include <kernel/block.h>
#include <kernel/fs.h>
#include <kernel/lock.h>
#include <kernel/string.h>

#define FS_MAGIC 0x5345454cU
#define FS_VERSION 1
#define FS_SUPER_SECTOR 1
#define FS_INODES 128
#define FS_DIRECT 12
#define BITS_PER_SECTOR (8 * BLOCK_SIZE)
#define FS_MAX_SECTORS (2 * BITS_PER_SECTOR)

struct superblock {
    uint32 magic;
    uint32 version;
    uint32 sectors;
    uint32 bitmap;
    uint32 bitmap_count;
    uint32 inodes;
    uint32 data;
    uint32 root;
};

struct disk_inode {
    uint32 type;
    uint32 size;
    uint32 blocks[FS_DIRECT];
    uint32 reserved[2];
};

#define INODES_PER_SECTOR (BLOCK_SIZE / sizeof(struct disk_inode))

_Static_assert(sizeof(struct disk_inode) == 64, "on-disk inode size");
_Static_assert(sizeof(struct fs_dirent) == 32, "on-disk directory entry size");
_Static_assert(FS_MAX_FILE == FS_DIRECT * BLOCK_SIZE, "maximum file size");

static spinlock_t fs_lock;
static struct superblock super;
static bool mounted;

/* Internal helpers run under fs_lock, except mount before boot publication. */
static int write_sector(uint32 sector, const void *data)
{
    if (block_write(sector, data) == 0)
        return 0;
    mounted = false;
    return FS_IO;
}

static int load_inode(uint32 ino, struct disk_inode *inode)
{
    if (ino == 0 || ino >= FS_INODES)
        return FS_NOENT;
    struct disk_inode records[INODES_PER_SECTOR];
    if (block_read(super.inodes + ino / INODES_PER_SECTOR, records) != 0)
        return FS_IO;
    *inode = records[ino % INODES_PER_SECTOR];
    if (inode->type == 0)
        return 0;
    if ((inode->type != FS_FILE && inode->type != FS_DIR) ||
        inode->size > FS_MAX_FILE ||
        (inode->type == FS_DIR && inode->size % sizeof(struct fs_dirent) != 0))
        return FS_IO;
    for (uint32 i = 0; i < FS_DIRECT; ++i) {
        uint32 address = inode->blocks[i];
        if (i * BLOCK_SIZE < inode->size) {
            if (address < super.data || address >= super.sectors)
                return FS_IO;
        } else if (address != 0) {
            return FS_IO;
        }
    }
    return 0;
}

static int live_inode(uint32 ino, struct disk_inode *inode)
{
    int rc = load_inode(ino, inode);
    return rc == 0 && inode->type == 0 ? FS_NOENT : rc;
}

static int store_inode(uint32 ino, const struct disk_inode *inode)
{
    struct disk_inode records[INODES_PER_SECTOR];
    uint32 sector = super.inodes + ino / INODES_PER_SECTOR;
    if (block_read(sector, records) != 0)
        return FS_IO;
    records[ino % INODES_PER_SECTOR] = *inode;
    return write_sector(sector, records);
}

static int allocate_sector(void)
{
    uint8 bitmap[BLOCK_SIZE];
    for (uint32 i = 0; i < super.bitmap_count; ++i) {
        if (block_read(super.bitmap + i, bitmap) != 0)
            return FS_IO;
        uint32 start = i * BITS_PER_SECTOR;
        for (uint32 bit = 0; bit < BITS_PER_SECTOR; ++bit) {
            uint32 sector = start + bit;
            if (sector >= super.sectors)
                break;
            if (sector < super.data || (bitmap[bit / 8] & (1U << (bit % 8))) != 0)
                continue;
            bitmap[bit / 8] |= 1U << (bit % 8);
            int rc = write_sector(super.bitmap + i, bitmap);
            return rc == 0 ? (int)sector : rc;
        }
    }
    return FS_NOSPC;
}

static int free_sector(uint32 sector)
{
    uint8 bitmap[BLOCK_SIZE];
    uint32 location = super.bitmap + sector / BITS_PER_SECTOR;
    uint32 bit = sector % BITS_PER_SECTOR;
    if (block_read(location, bitmap) != 0)
        return FS_IO;
    uint8 mask = 1U << (bit % 8);
    if ((bitmap[bit / 8] & mask) == 0)
        return FS_IO;
    bitmap[bit / 8] &= (uint8)~mask;
    return write_sector(location, bitmap);
}

static int clear_inode(uint32 ino, const struct disk_inode *inode, uint32 type)
{
    struct disk_inode empty = { .type = type };
    int rc = store_inode(ino, &empty);
    for (unsigned i = 0; rc == 0 && i < FS_DIRECT && inode->blocks[i] != 0; ++i)
        rc = free_sector(inode->blocks[i]);
    if (rc != 0)
        mounted = false;
    return rc;
}

static int read_data(const struct disk_inode *inode, uint32 offset,
                     void *data, size_t size)
{
    uint8 sector[BLOCK_SIZE];
    if (offset >= inode->size)
        return 0;
    if (size > inode->size - offset)
        size = inode->size - offset;
    size_t done = 0;
    while (done < size) {
        uint32 pos = offset + done;
        unsigned within = pos % BLOCK_SIZE;
        size_t count = BLOCK_SIZE - within;
        if (count > size - done)
            count = size - done;
        if (block_read(inode->blocks[pos / BLOCK_SIZE], sector) != 0)
            return done != 0 ? (int)done : FS_IO;
        memcpy((uint8 *)data + done, sector + within, count);
        done += count;
    }
    return (int)done;
}

static int write_data(uint32 ino, struct disk_inode *inode, uint32 offset,
                      const void *data, size_t size)
{
    uint8 sector[BLOCK_SIZE];
    if (offset > inode->size)
        return FS_INVAL;
    if (size == 0)
        return 0;
    if (offset == FS_MAX_FILE)
        return FS_NOSPC;
    if (size > FS_MAX_FILE - offset)
        size = FS_MAX_FILE - offset;

    uint32 old_size = inode->size;
    size_t done = 0;
    int rc = 0;
    while (done < size) {
        uint32 pos = offset + done;
        uint32 index = pos / BLOCK_SIZE;
        unsigned within = pos % BLOCK_SIZE;
        size_t count = BLOCK_SIZE - within;
        if (count > size - done)
            count = size - done;

        uint32 address = inode->blocks[index];
        if (address == 0) {
            rc = allocate_sector();
            if (rc < 0)
                break;
            address = rc;
            memset(sector, 0, sizeof(sector));
        } else if (within != 0 || count != BLOCK_SIZE) {
            rc = block_read(address, sector) == 0 ? 0 : FS_IO;
            if (rc != 0)
                break;
        }

        memcpy(sector + within, (const uint8 *)data + done, count);
        rc = write_sector(address, sector);
        if (rc != 0)
            break;
        inode->blocks[index] = address;
        done += count;
        if (offset + done > inode->size)
            inode->size = offset + done;
    }
    if (!mounted)
        return FS_IO;
    if (inode->size != old_size && store_inode(ino, inode) != 0) {
        mounted = false;
        return FS_IO;
    }
    return done != 0 ? (int)done : rc;
}

static bool equal_name(const char *a, const char *b)
{
    for (unsigned i = 0; i <= FS_NAME_MAX; ++i) {
        if (a[i] != b[i])
            return false;
        if (a[i] == '\0')
            return true;
    }
    return false;
}

static int directory_entry(const struct disk_inode *dir, uint32 offset,
                           struct fs_dirent *entry)
{
    if (read_data(dir, offset, entry, sizeof(*entry)) != (int)sizeof(*entry))
        return FS_IO;
    if (entry->ino >= FS_INODES ||
        (entry->ino != 0 && (entry->name[0] == '\0' || entry->name[FS_NAME_MAX] != '\0')))
        return FS_IO;
    return 0;
}

/* On a miss, slot is the first free record or the append offset. */
static int find_entry(const struct disk_inode *dir, const char *name, uint32 *slot)
{
    if (slot != NULL)
        *slot = dir->size;
    for (uint32 offset = 0; offset < dir->size; offset += sizeof(struct fs_dirent)) {
        struct fs_dirent entry;
        int rc = directory_entry(dir, offset, &entry);
        if (rc != 0)
            return rc;
        if (entry.ino == 0) {
            if (slot != NULL && *slot == dir->size)
                *slot = offset;
        } else if (equal_name(entry.name, name)) {
            if (slot != NULL)
                *slot = offset;
            return (int)entry.ino;
        }
    }
    return FS_NOENT;
}

static int walk(uint32 cwd, const char *path, char *leaf, bool *trailing)
{
    if (path == NULL || *path == '\0')
        return FS_NOENT;
    size_t length = 0;
    while (path[length] != '\0') {
        if (++length > FS_PATH_MAX)
            return FS_NAMETOOLONG;
    }
    uint32 ino = *path == '/' ? FS_ROOT : cwd;
    while (*path == '/')
        ++path;
    if (*path == '\0')
        return leaf == NULL ? (int)ino : FS_INVAL;

    for (;;) {
        char name[FS_NAME_MAX + 1];
        unsigned n = 0;
        while (*path != '/' && *path != '\0') {
            if (n == FS_NAME_MAX)
                return FS_NAMETOOLONG;
            name[n++] = *path++;
        }
        name[n] = '\0';
        bool slash = *path == '/';
        while (*path == '/')
            ++path;
        struct disk_inode inode;
        int rc = live_inode(ino, &inode);
        if (rc != 0)
            return rc;
        if (inode.type != FS_DIR)
            return FS_NOTDIR;
        if (*path == '\0' && leaf != NULL) {
            memcpy(leaf, name, n + 1);
            *trailing = slash;
            return (int)ino;
        }
        rc = find_entry(&inode, name, NULL);
        if (rc < 0)
            return rc;
        ino = (uint32)rc;
        if (*path == '\0') {
            rc = live_inode(ino, &inode);
            if (rc != 0)
                return rc;
            return slash && inode.type != FS_DIR ? FS_NOTDIR : (int)ino;
        }
    }
}

int fs_mount(void)
{
    spinlock_init(&fs_lock, "filesystem");
    if (block_capacity() == 0)
        return FS_NODEV;
    uint8 sector[BLOCK_SIZE];
    if (block_read(FS_SUPER_SECTOR, sector) != 0)
        return FS_IO;
    memcpy(&super, sector, sizeof(super));
    if (super.magic != FS_MAGIC || super.version != FS_VERSION ||
        super.sectors > FS_MAX_SECTORS || super.sectors > block_capacity() ||
        super.bitmap != 2 ||
        super.bitmap_count != (super.sectors + BITS_PER_SECTOR - 1) / BITS_PER_SECTOR ||
        super.inodes != super.bitmap + super.bitmap_count ||
        super.data != super.inodes + FS_INODES / INODES_PER_SECTOR ||
        super.data >= super.sectors || super.root != FS_ROOT)
        return FS_INVAL;
    struct disk_inode root;
    if (live_inode(FS_ROOT, &root) != 0 || root.type != FS_DIR ||
        find_entry(&root, ".", NULL) != FS_ROOT || find_entry(&root, "..", NULL) != FS_ROOT)
        return FS_IO;
    mounted = true;
    return 0;
}

int fs_lookup(uint32 cwd, const char *path)
{
    spinlock_acquire(&fs_lock);
    int rc = mounted ? walk(cwd, path, NULL, NULL) : FS_NODEV;
    spinlock_release(&fs_lock);
    return rc;
}

int fs_stat(uint32 ino, struct fs_info *info)
{
    spinlock_acquire(&fs_lock);
    struct disk_inode inode;
    int rc = mounted ? live_inode(ino, &inode) : FS_NODEV;
    if (rc == 0)
        *info = (struct fs_info){ ino, inode.type, inode.size };
    spinlock_release(&fs_lock);
    return rc;
}

int fs_read(uint32 ino, uint32 offset, void *data, size_t size)
{
    spinlock_acquire(&fs_lock);
    struct disk_inode inode;
    int rc = mounted ? live_inode(ino, &inode) : FS_NODEV;
    if (rc == 0)
        rc = inode.type == FS_FILE ? read_data(&inode, offset, data, size) : FS_ISDIR;
    spinlock_release(&fs_lock);
    return rc;
}

int fs_write(uint32 ino, uint32 offset, const void *data, size_t size)
{
    spinlock_acquire(&fs_lock);
    struct disk_inode inode;
    int rc = mounted ? live_inode(ino, &inode) : FS_NODEV;
    if (rc == 0)
        rc = inode.type == FS_FILE ? write_data(ino, &inode, offset, data, size) : FS_ISDIR;
    spinlock_release(&fs_lock);
    return rc;
}

int fs_truncate(uint32 ino)
{
    spinlock_acquire(&fs_lock);
    struct disk_inode inode;
    int rc = mounted ? live_inode(ino, &inode) : FS_NODEV;
    if (rc == 0)
        rc = inode.type == FS_FILE ? clear_inode(ino, &inode, FS_FILE) : FS_ISDIR;
    spinlock_release(&fs_lock);
    return rc;
}

int fs_readdir(uint32 ino, uint32 cursor, struct fs_dirent *entry)
{
    if (cursor % sizeof(*entry) != 0)
        return FS_INVAL;
    spinlock_acquire(&fs_lock);
    struct disk_inode inode;
    int rc = mounted ? live_inode(ino, &inode) : FS_NODEV;
    if (rc == 0 && inode.type != FS_DIR)
        rc = FS_NOTDIR;
    while (rc == 0 && cursor < inode.size) {
        rc = directory_entry(&inode, cursor, entry);
        cursor += sizeof(*entry);
        if (rc == 0 && entry->ino != 0)
            rc = (int)cursor;
    }
    spinlock_release(&fs_lock);
    return rc;
}

static int parent_dir(uint32 cwd, const char *path, char *name, bool *trailing,
                      struct disk_inode *parent)
{
    int ino = walk(cwd, path, name, trailing);
    if (ino < 0)
        return ino;
    if (equal_name(name, ".") || equal_name(name, ".."))
        return FS_INVAL;
    int rc = live_inode(ino, parent);
    return rc == 0 ? ino : rc;
}

int fs_create(uint32 cwd, const char *path, enum fs_type type)
{
    if (type != FS_FILE && type != FS_DIR)
        return FS_INVAL;
    spinlock_acquire(&fs_lock);
    struct disk_inode parent;
    struct fs_dirent entry = {0};
    bool trailing;
    uint32 slot;
    int pino = mounted ? parent_dir(cwd, path, entry.name, &trailing, &parent) : FS_NODEV;
    int rc = pino < 0 ? pino : find_entry(&parent, entry.name, &slot);
    if (rc >= 0)
        rc = FS_EXIST;
    if (pino < 0 || rc != FS_NOENT)
        goto out;
    if (trailing && type != FS_DIR) {
        rc = FS_NOTDIR;
        goto out;
    }

    struct disk_inode inode;
    uint32 ino;
    for (ino = FS_ROOT + 1; ino < FS_INODES; ++ino) {
        rc = load_inode(ino, &inode);
        if (rc != 0)
            goto out;
        if (inode.type == 0)
            break;
    }
    if (ino == FS_INODES) {
        rc = FS_NOSPC;
        goto out;
    }

    inode = (struct disk_inode){ .type = type };
    if (type == FS_DIR) {
        const struct fs_dirent dots[] = {
            { .ino = ino, .name = "." }, { .ino = (uint32)pino, .name = ".." },
        };
        rc = write_data(ino, &inode, 0, dots, sizeof(dots));
        if (rc < 0)
            goto out;
    } else {
        rc = store_inode(ino, &inode);
        if (rc < 0)
            goto out;
    }

    entry.ino = ino;
    rc = write_data(pino, &parent, slot, &entry, sizeof(entry));
    if (rc == (int)sizeof(entry))
        rc = (int)ino;
    else if (mounted && clear_inode(ino, &inode, 0) != 0)
        rc = FS_IO;
out:
    spinlock_release(&fs_lock);
    return rc;
}

int fs_remove(uint32 cwd, const char *path)
{
    spinlock_acquire(&fs_lock);
    struct disk_inode parent;
    char name[FS_NAME_MAX + 1];
    bool trailing;
    uint32 slot;
    int pino = mounted ? parent_dir(cwd, path, name, &trailing, &parent) : FS_NODEV;
    int ino = pino < 0 ? pino : find_entry(&parent, name, &slot);
    int rc = ino;
    if (ino < 0)
        goto out;
    if (ino == FS_ROOT) {
        rc = FS_INVAL;
        goto out;
    }

    struct disk_inode inode;
    rc = live_inode(ino, &inode);
    if (rc != 0)
        goto out;
    if (trailing && inode.type != FS_DIR) {
        rc = FS_NOTDIR;
        goto out;
    }
    if (inode.type == FS_DIR) {
        for (uint32 offset = 0; offset < inode.size; offset += sizeof(struct fs_dirent)) {
            struct fs_dirent entry;
            rc = directory_entry(&inode, offset, &entry);
            if (rc != 0)
                goto out;
            if (entry.ino != 0 && !equal_name(entry.name, ".") && !equal_name(entry.name, "..")) {
                rc = FS_NOTEMPTY;
                goto out;
            }
        }
    }

    const struct fs_dirent vacant = {0};
    rc = write_data(pino, &parent, slot, &vacant, sizeof(vacant));
    if (rc == (int)sizeof(vacant))
        rc = clear_inode(ino, &inode, 0);
out:
    spinlock_release(&fs_lock);
    return rc;
}

int fs_sync(void)
{
    spinlock_acquire(&fs_lock);
    int rc = mounted ? block_flush() : FS_NODEV;
    spinlock_release(&fs_lock);
    return rc;
}

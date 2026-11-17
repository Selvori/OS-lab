#include <kernel/block.h>
#include <kernel/lock.h>
#include <kernel/string.h>
#include <drivers/block/virtio.h>

struct cache_entry {
    bool valid;
    uint64 sector;
    uint64 last_use;
    uint8 data[BLOCK_SIZE];
};

static struct cache_entry cache[BLOCK_CACHE_SIZE];
static struct block_cache_stats stats;
static spinlock_t block_lock;
static uint64 capacity;
static uint64 sequence;

void block_init(void)
{
    spinlock_init(&block_lock, "block");
    capacity = virtio_blk_init();
}

uint64 block_capacity(void)
{
    return capacity;
}

static struct cache_entry *find(uint64 sector)
{
    for (size_t i = 0; i < BLOCK_CACHE_SIZE; ++i) {
        if (cache[i].valid && cache[i].sector == sector)
            return &cache[i];
    }
    return NULL;
}

static struct cache_entry *replace(uint64 sector)
{
    struct cache_entry *oldest = &cache[0];
    for (size_t i = 0; i < BLOCK_CACHE_SIZE; ++i) {
        struct cache_entry *entry = &cache[i];
        if (!entry->valid) {
            oldest = entry;
            break;
        }
        if (entry->last_use < oldest->last_use)
            oldest = entry;
    }
    if (oldest->valid)
        ++stats.evictions;
    oldest->valid = true;
    oldest->sector = sector;
    return oldest;
}

int block_read(uint64 sector, void *data)
{
    if (capacity == 0)
        return -19;
    if (sector >= capacity || data == NULL)
        return -22;

    spinlock_acquire(&block_lock);
    struct cache_entry *entry = find(sector);
    int result = 0;
    if (entry != NULL) {
        ++stats.hits;
        memcpy(data, entry->data, BLOCK_SIZE);
    } else {
        ++stats.misses;
        result = virtio_blk_read(sector, data);
        if (result == 0) {
            entry = replace(sector);
            memcpy(entry->data, data, BLOCK_SIZE);
        }
    }
    if (entry != NULL)
        entry->last_use = ++sequence;
    spinlock_release(&block_lock);
    return result;
}

int block_write(uint64 sector, const void *data)
{
    if (capacity == 0)
        return -19;
    if (sector >= capacity || data == NULL)
        return -22;

    spinlock_acquire(&block_lock);
    struct cache_entry *entry = find(sector);
    int result = virtio_blk_write(sector, data);
    if (result == 0) {
        if (entry == NULL)
            entry = replace(sector);
        memcpy(entry->data, data, BLOCK_SIZE);
        entry->last_use = ++sequence;
    } else if (entry != NULL) {
        entry->valid = false;
    }
    spinlock_release(&block_lock);
    return result;
}

int block_flush(void)
{
    if (capacity == 0)
        return -19;
    spinlock_acquire(&block_lock);
    int result = virtio_blk_flush();
    spinlock_release(&block_lock);
    return result;
}

struct block_cache_stats block_stats(void)
{
    spinlock_acquire(&block_lock);
    struct block_cache_stats result = stats;
    spinlock_release(&block_lock);
    return result;
}

#ifndef OSLAB_BLOCK_H
#define OSLAB_BLOCK_H

#include <kernel/types.h>

#define BLOCK_SIZE 512
#define BLOCK_CACHE_SIZE 16

struct block_cache_stats {
    uint64 hits;
    uint64 misses;
    uint64 evictions;
};

void block_init(void);
uint64 block_capacity(void);
int block_read(uint64 sector, void *data);
int block_write(uint64 sector, const void *data);
int block_flush(void);
struct block_cache_stats block_stats(void);

#endif

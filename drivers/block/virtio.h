#ifndef OSLAB_VIRTIO_BLK_H
#define OSLAB_VIRTIO_BLK_H

#include <kernel/types.h>

uint64 virtio_blk_init(void);
/* Calls are serialized by the block layer. */
int virtio_blk_read(uint64 sector, void *data);
int virtio_blk_write(uint64 sector, const void *data);
int virtio_blk_flush(void);

#endif

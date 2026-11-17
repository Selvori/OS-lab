#ifndef SEELE_BLOCK_BACKEND_H
#define SEELE_BLOCK_BACKEND_H
#include <kernel/types.h>
/* Only the block layer calls these functions, with one I/O in flight. */
uint64 disk_init(void);
int disk_read(uint64 sector, void *data);
int disk_write(uint64 sector, const void *data);
int disk_flush(void);
#endif

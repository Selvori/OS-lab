#ifndef SEELE_GPT_H
#define SEELE_GPT_H
#include <kernel/types.h>

#define SEELE_TEST_SECTORS 8192U

/* Find the unique, non-overlapping SeeleFS test partition. */
int gpt_seele_partition(int (*read)(uint64 sector, void *buf),
                        uint64 sectors, uint64 *first_sector);
#endif

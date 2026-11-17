#include <drivers/block/gpt.h>
#include <kernel/block.h>
#include <kernel/string.h>

#define ENTRY_SIZE 128
#define ENTRY_LIMIT 128

struct partition_range {
    uint64 first;
    uint64 last;
};

/* 9ea193f1-66ec-4307-a879-474c515e4e45, encoded in GPT byte order. */
static const uint8 partition_type[] = {
    0xf1, 0x93, 0xa1, 0x9e, 0xec, 0x66, 0x07, 0x43,
    0xa8, 0x79, 0x47, 0x4c, 0x51, 0x5e, 0x4e, 0x45,
};
static const uint8 zeros[16];

static bool same(const uint8 *left, const uint8 *right, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        if (left[i] != right[i])
            return false;
    return true;
}

static uint32 le32(const uint8 *p)
{
    return p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static uint64 le64(const uint8 *p)
{
    return le32(p) | ((uint64)le32(p + 4) << 32);
}

static uint32 crc_step(uint32 crc, const uint8 *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return crc;
}

static int parse_table(int (*read)(uint64, void *), uint64 table, uint32 entries,
                       uint64 usable_start, uint64 usable_end, uint32 expected_crc,
                       uint64 *first_sector)
{
    static struct partition_range ranges[ENTRY_LIMIT];
    uint8 sector[BLOCK_SIZE];
    unsigned used = 0;
    int candidate = -1;
    uint32 crc = UINT32_MAX;
    for (uint32 i = 0; i < entries; ++i) {
        size_t offset = i * ENTRY_SIZE % BLOCK_SIZE;
        if (offset == 0 && read(table + i * ENTRY_SIZE / BLOCK_SIZE, sector) != 0)
            return -5;
        const uint8 *entry = sector + offset;
        crc = crc_step(crc, entry, ENTRY_SIZE);
        if (same(entry, zeros, sizeof(zeros)))
            continue;
        uint64 first = le64(entry + 32);
        uint64 last = le64(entry + 40);
        if (first < usable_start || last > usable_end || last < first)
            return -22;
        for (unsigned j = 0; j < used; ++j)
            if (first <= ranges[j].last && last >= ranges[j].first)
                return -22;
        if (same(entry, partition_type, sizeof(partition_type))) {
            static const uint8 label[] = { 's', 0, 'e', 0, 'e', 0, 'l', 0, 'e', 0, 'f', 0, 's', 0, 0, 0 };
            if (candidate >= 0 || last - first + 1 != SEELE_TEST_SECTORS ||
                !same(entry + 56, label, sizeof(label)))
                return -22;
            candidate = (int)used;
        }
        ranges[used++] = (struct partition_range){ first, last };
    }
    if (~crc != expected_crc)
        return -22;
    if (candidate < 0)
        return -2;
    *first_sector = ranges[candidate].first;
    return 0;
}

struct gpt_header {
    uint8 id[16];
    uint64 usable_start;
    uint64 usable_end;
    uint64 table;
    uint32 entries;
    uint32 table_crc;
};

static int read_header(int (*read)(uint64, void *), uint64 sectors, bool primary,
                       struct gpt_header *header)
{
    uint8 sector[BLOCK_SIZE];
    uint64 current = primary ? 1 : sectors - 1;
    uint64 alternate = primary ? sectors - 1 : 1;
    if (read(current, sector) != 0)
        return -5;
    if (!same(sector, (const uint8 *)"EFI PART", 8) || le32(sector + 8) != 0x10000)
        return -22;
    uint32 header_len = le32(sector + 12);
    uint32 header_crc = le32(sector + 16);
    if (header_len < 92 || header_len > BLOCK_SIZE)
        return -22;
    memset(sector + 16, 0, 4);
    if (~crc_step(UINT32_MAX, sector, header_len) != header_crc)
        return -22;

    memcpy(header->id, sector + 56, sizeof(header->id));
    header->usable_start = le64(sector + 40);
    header->usable_end = le64(sector + 48);
    header->table = le64(sector + 72);
    header->entries = le32(sector + 80);
    header->table_crc = le32(sector + 88);
    if (le64(sector + 24) != current || le64(sector + 32) != alternate ||
        header->usable_start > header->usable_end || header->usable_end >= sectors - 1 ||
        le32(sector + 84) != ENTRY_SIZE || header->entries == 0 || header->entries > ENTRY_LIMIT)
        return -22;

    uint64 count = (header->entries * ENTRY_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (primary) {
        if (header->table < 2 || header->table >= header->usable_start ||
            count > header->usable_start - header->table)
            return -22;
    } else if (header->table <= header->usable_end || header->table >= current ||
               count > current - header->table) {
        return -22;
    }
    return 0;
}

int gpt_seele_partition(int (*read)(uint64 sector, void *buf),
                        uint64 sectors, uint64 *first_sector)
{
    if (sectors < 2 * SEELE_TEST_SECTORS)
        return -22;
    struct gpt_header primary, backup;
    int rc = read_header(read, sectors, true, &primary);
    if (rc == 0)
        rc = read_header(read, sectors, false, &backup);
    if (rc != 0)
        return rc;
    if (!same(primary.id, backup.id, sizeof(primary.id)) ||
        primary.usable_start != backup.usable_start || primary.usable_end != backup.usable_end ||
        primary.entries != backup.entries || primary.table_crc != backup.table_crc)
        return -22;

    uint64 first, alternate;
    rc = parse_table(read, primary.table, primary.entries, primary.usable_start,
                     primary.usable_end, primary.table_crc, &first);
    int copy_rc = parse_table(read, backup.table, backup.entries, backup.usable_start,
                              backup.usable_end, backup.table_crc, &alternate);
    if (rc != copy_rc || (rc == 0 && first != alternate))
        return -22;
    if (rc == 0)
        *first_sector = first;
    return rc;
}

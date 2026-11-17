#include <asm/io.h>
#include <kernel/block.h>
#include <kernel/print.h>
#include <kernel/string.h>
#include <platform.h>
#include <drivers/block/backend.h>

enum {
    MAGIC = 0x000, VERSION = 0x004, DEVICE_ID = 0x008,
    DEVICE_FEATURES = 0x010, DEVICE_FEATURES_SEL = 0x014,
    DRIVER_FEATURES = 0x020, DRIVER_FEATURES_SEL = 0x024,
    QUEUE_SEL = 0x030, QUEUE_NUM_MAX = 0x034, QUEUE_NUM = 0x038,
    QUEUE_READY = 0x044, QUEUE_NOTIFY = 0x050,
    INTERRUPT_STATUS = 0x060, INTERRUPT_ACK = 0x064, DEVICE_STATUS = 0x070,
    QUEUE_DESC = 0x080, QUEUE_DRIVER = 0x090, QUEUE_DEVICE = 0x0a0,
    CONFIG_GENERATION = 0x0fc, CAPACITY = 0x100,
};

enum {
    ACKNOWLEDGE = 1, DRIVER = 2, DRIVER_OK = 4, FEATURES_OK = 8,
    DEVICE_NEEDS_RESET = 64, FAILED = 128,
    FEATURE_RO = 1 << 5, FEATURE_FLUSH = 1 << 9,
    DESC_NEXT = 1, DESC_WRITE = 2, AVAIL_NO_INTERRUPT = 1,
    REQ_READ = 0, REQ_WRITE = 1, REQ_FLUSH = 4,
    QUEUE_SIZE = 4,
};

struct descriptor {
    uint64 addr;
    uint32 len;
    uint16_t flags;
    uint16_t next;
};

struct avail_ring {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
};

struct used_entry {
    uint32 id;
    uint32 len;
};

struct used_ring {
    uint16_t flags;
    uint16_t idx;
    struct used_entry ring[QUEUE_SIZE];
};

struct request {
    uint32 type;
    uint32 reserved;
    uint64 sector;
};

_Static_assert(sizeof(struct descriptor) == 16, "virtio descriptor size");
_Static_assert(sizeof(struct request) == 16, "virtio block header size");

static uintptr_t base;
static struct descriptor desc[QUEUE_SIZE] __attribute__((aligned(16)));
static struct avail_ring avail __attribute__((aligned(2)));
static volatile struct used_ring used __attribute__((aligned(4)));
static struct request header;
static volatile uint8 response;
static uint16_t last_used;
static uint8 bounce[BLOCK_SIZE];

static uint64 now(void)
{
    uint64 value;
    __asm__ volatile("rdtime %0" : "=r"(value));
    return value;
}

static uint32 read_reg(uint32 reg)
{
    return mmio_read32(base + reg);
}

static void write_reg(uint32 reg, uint32 value)
{
    mmio_write32(base + reg, value);
}

static void write_address(uint32 reg, uintptr_t address)
{
    write_reg(reg, (uint32)address);
    write_reg(reg + 4, (uint32)(address >> 32));
}

static uint64 reject(void)
{
    write_reg(DEVICE_STATUS, read_reg(DEVICE_STATUS) | FAILED);
    return 0;
}

uint64 disk_init(void)
{
    for (uintptr_t addr = VIRTIO_BASE; addr < VIRTIO_BASE + VIRTIO_SIZE;
         addr += VIRTIO_STRIDE) {
        if (mmio_read32(addr + MAGIC) == 0x74726976 &&
            mmio_read32(addr + VERSION) == 2 &&
            mmio_read32(addr + DEVICE_ID) == 2) {
            base = addr;
            break;
        }
    }
    if (base == 0)
        return 0;

    write_reg(DEVICE_STATUS, 0);
    uint64 start = now();
    while (read_reg(DEVICE_STATUS) != 0) {
        if (now() - start >= TIMEBASE_HZ)
            return 0;
    }
    uint32 status = ACKNOWLEDGE;
    write_reg(DEVICE_STATUS, status);
    status |= DRIVER;
    write_reg(DEVICE_STATUS, status);

    write_reg(DEVICE_FEATURES_SEL, 0);
    uint32 features = read_reg(DEVICE_FEATURES);
    write_reg(DEVICE_FEATURES_SEL, 1);
    /* Bit 32 is VIRTIO_F_VERSION_1. */
    if ((read_reg(DEVICE_FEATURES) & 1) == 0 ||
        (features & (FEATURE_RO | FEATURE_FLUSH)) != FEATURE_FLUSH)
        return reject();

    write_reg(DRIVER_FEATURES_SEL, 0);
    write_reg(DRIVER_FEATURES, FEATURE_FLUSH);
    write_reg(DRIVER_FEATURES_SEL, 1);
    write_reg(DRIVER_FEATURES, 1);
    status |= FEATURES_OK;
    write_reg(DEVICE_STATUS, status);
    if ((read_reg(DEVICE_STATUS) & FEATURES_OK) == 0)
        return reject();

    write_reg(QUEUE_SEL, 0);
    if (read_reg(QUEUE_READY) != 0 || read_reg(QUEUE_NUM_MAX) < QUEUE_SIZE)
        return reject();
    avail.flags = AVAIL_NO_INTERRUPT;
    write_reg(QUEUE_NUM, QUEUE_SIZE);
    write_address(QUEUE_DESC, (uintptr_t)desc);
    write_address(QUEUE_DRIVER, (uintptr_t)&avail);
    write_address(QUEUE_DEVICE, (uintptr_t)&used);
    write_reg(QUEUE_READY, 1);
    write_reg(DEVICE_STATUS, status | DRIVER_OK);

    start = now();
    do {
        uint32 generation = read_reg(CONFIG_GENERATION);
        uint64 low = read_reg(CAPACITY);
        uint64 high = read_reg(CAPACITY + 4);
        if (read_reg(CONFIG_GENERATION) == generation)
            return (high << 32) | low;
    } while (now() - start < TIMEBASE_HZ);
    return reject();
}

static int submit(uint32 type, uint64 sector)
{
    header = (struct request){ .type = type, .sector = sector };
    response = 0xff;
    int tail = type == REQ_FLUSH ? 1 : 2;
    desc[0] = (struct descriptor){ (uintptr_t)&header, sizeof(header), DESC_NEXT, 1 };
    if (tail == 2) {
        desc[1] = (struct descriptor){
            (uintptr_t)bounce, sizeof(bounce),
            DESC_NEXT | (type == REQ_READ ? DESC_WRITE : 0), 2,
        };
    }
    desc[tail] = (struct descriptor){ (uintptr_t)&response, 1, DESC_WRITE, 0 };

    avail.ring[avail.idx % QUEUE_SIZE] = 0;
    io_fence();
    ++avail.idx;
    write_reg(QUEUE_NOTIFY, 0);

    uint64 start = now();
    uint16_t expected = last_used + 1;
    while (used.idx == last_used) {
        if (now() - start >= 2 * TIMEBASE_HZ)
            panic("virtio block request timed out");
    }
    io_fence();
    if (used.idx != expected || used.ring[last_used % QUEUE_SIZE].id != 0)
        panic("virtio block completion invalid");
    uint32 written = used.ring[last_used % QUEUE_SIZE].len;
    last_used = expected;
    uint32 irq = read_reg(INTERRUPT_STATUS);
    if (irq != 0)
        write_reg(INTERRUPT_ACK, irq);
    if (read_reg(DEVICE_STATUS) & DEVICE_NEEDS_RESET)
        return -5;
    uint32 expected_written = type == REQ_READ ? BLOCK_SIZE + 1 : 1;
    return response == 0 && written == expected_written ? 0 : -5;
}

int disk_read(uint64 sector, void *data)
{
    int result = submit(REQ_READ, sector);
    if (result == 0)
        memcpy(data, bounce, sizeof(bounce));
    return result;
}

int disk_write(uint64 sector, const void *data)
{
    memcpy(bounce, data, sizeof(bounce));
    return submit(REQ_WRITE, sector);
}

int disk_flush(void)
{
    return submit(REQ_FLUSH, 0);
}

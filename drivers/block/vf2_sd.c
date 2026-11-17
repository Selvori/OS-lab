#include <asm/io.h>
#include <drivers/block/backend.h>
#include <drivers/block/gpt.h>
#include <kernel/block.h>
#include <kernel/print.h>
#include <kernel/string.h>
#include <platform.h>

enum {
    CTRL = 0x00, PWREN = 0x04, CLKDIV = 0x08, CLKSRC = 0x0c, CLKENA = 0x10,
    TMOUT = 0x14, CTYPE = 0x18, BLKSIZ = 0x1c, BYTCNT = 0x20, INTMASK = 0x24,
    CMDARG = 0x28, CMD = 0x2c, RESP0 = 0x30, RESP1 = 0x34, RESP2 = 0x38,
    RESP3 = 0x3c, RINTSTS = 0x44, STATUS = 0x48, VERID = 0x6c, HCON = 0x70,
    UHS = 0x74, BMOD = 0x80, FIFO = 0x200,
};
enum {
    RESET_ALL = 7,
    RESP = 1U << 6, LONG_RESP = 1U << 7, RESP_CRC = 1U << 8,
    DATA = 1U << 9, WRITE_DATA = 1U << 10, WAIT_DATA = 1U << 13,
    INIT_CLOCKS = 1U << 15, UPDATE_CLOCK = 1U << 21, USE_HOLD = 1U << 29,
    START = 1U << 31,
    CMD_DONE = 1U << 2, DATA_DONE = 1U << 3, CRC_ERROR = 1U << 6,
    LOCK_ERROR = 1U << 12,
    RESP_ERRORS = (1U << 1) | (1U << 8) | LOCK_ERROR,
    DATA_ERRORS = (1U << 7) | (1U << 9) | (1U << 10) | (1U << 11) |
                  LOCK_ERROR | (1U << 13) | (1U << 15),
    FIFO_EMPTY = 1U << 2, FIFO_FULL = 1U << 3, CARD_BUSY = 1U << 9,
    R1_ERRORS = 0xfdffe008U, R1_READY = 1U << 8, R1_STATE_MASK = 15U << 9,
    R1_TRANSFER = 4U << 9,
    OCR_READY = 1U << 31, OCR_BLOCK_ADDRESS = 1U << 30,
};

static uint32 rca;
static uint64 card_sectors;
static uint64 partition_start;
static bool healthy;

static uint64 ticks(void)
{
    uint64 value;
    __asm__ volatile("rdtime %0" : "=r"(value));
    return value;
}

static uint32 reg(uint32 offset)
{
    return mmio_read32(SD_MMC_BASE + offset);
}

static void put(uint32 offset, uint32 value)
{
    mmio_write32(SD_MMC_BASE + offset, value);
}

static bool wait_clear(uint32 offset, uint32 bits)
{
    uint64 start = ticks();
    while (reg(offset) & bits) {
        if (ticks() - start >= TIMEBASE_HZ)
            return false;
    }
    return true;
}

static bool update_clock(void)
{
    put(CMD, START | UPDATE_CLOCK | WAIT_DATA);
    return wait_clear(CMD, START) && (reg(RINTSTS) & LOCK_ERROR) == 0;
}

static bool set_clock(uint32 divider)
{
    put(RINTSTS, UINT32_MAX);
    put(CLKENA, 0);
    if (!update_clock())
        return false;
    put(CLKSRC, 0);
    put(CLKDIV, divider);
    if (!update_clock())
        return false;
    put(CLKENA, 1);
    return update_clock();
}

static bool command(unsigned index, uint32 arg, uint32 flags)
{
    put(RINTSTS, UINT32_MAX);
    put(CMDARG, arg);
    put(CMD, START | USE_HOLD | WAIT_DATA | flags | index);
    uint32 errors = RESP_ERRORS | (flags & RESP_CRC ? CRC_ERROR : 0);
    uint64 start = ticks();
    for (;;) {
        uint32 status = reg(RINTSTS);
        if ((status & errors) || ticks() - start >= TIMEBASE_HZ / 4) {
            printf("SDIO1: CMD%d failed, RINTSTS=%x\n", (int)index, status);
            return false;
        }
        if (status & CMD_DONE) {
            put(RINTSTS, CMD_DONE);
            return true;
        }
    }
}

static bool card_ready(void)
{
    uint64 start = ticks();
    do {
        if (!wait_clear(STATUS, CARD_BUSY) || !command(13, rca, RESP | RESP_CRC))
            return false;
        uint32 result = reg(RESP0);
        if (result & R1_ERRORS)
            return false;
        if ((result & (R1_READY | R1_STATE_MASK)) == (R1_READY | R1_TRANSFER))
            return true;
    } while (ticks() - start < TIMEBASE_HZ);
    return false;
}

static uint64 card_init(void)
{
    uint32 ver = reg(VERID) & 0xffff;
    uint32 width = (reg(HCON) >> 7) & 7;
    printf("SDIO1: DW-MSHC version=%x hdata=%d\n", ver, (int)width);
    if (ver < 0x240a || ver == 0xffff || width != 1)
        return 0;

    /* U-Boot enables JH7110 clocks, mux and the 3.3 V rail via mmc dev 1. */
    put(PWREN, 1);
    put(CTRL, RESET_ALL);
    if (!wait_clear(CTRL, RESET_ALL))
        return 0;
    put(BMOD, 0);
    put(INTMASK, 0);
    put(TMOUT, UINT32_MAX);
    put(CTYPE, 0);
    put(UHS, 0);
    if (!set_clock(255) || !command(0, 0, INIT_CLOCKS) ||
        !command(8, 0x1aa, RESP | RESP_CRC) || (reg(RESP0) & 0xfff) != 0x1aa)
        return 0;

    uint64 start = ticks();
    uint32 ocr;
    do {
        if (!command(55, 0, RESP | RESP_CRC) ||
            !command(41, OCR_BLOCK_ADDRESS | 0x00ff8000, RESP))
            return 0;
        ocr = reg(RESP0);
        if (ocr & OCR_READY)
            break;
    } while (ticks() - start < 2 * TIMEBASE_HZ);
    if ((ocr & (OCR_READY | OCR_BLOCK_ADDRESS)) != (OCR_READY | OCR_BLOCK_ADDRESS) ||
        !command(2, 0, RESP | LONG_RESP | RESP_CRC) ||
        !command(3, 0, RESP | RESP_CRC))
        return 0;
    rca = reg(RESP0) & 0xffff0000U;
    if (rca == 0 || !command(9, rca, RESP | LONG_RESP | RESP_CRC))
        return 0;

    if ((reg(RESP3) >> 30) != 1)
        return 0;
    uint32 c_size = ((reg(RESP2) & 0x3fU) << 16) | (reg(RESP1) >> 16);
    if (!command(7, rca, RESP | RESP_CRC) || (reg(RESP0) & R1_ERRORS) != 0 ||
        !card_ready() || !set_clock(8))
        return 0;
    return ((uint64)c_size + 1) * 1024;
}

static int transfer(uint64 sector, uint32 *words, bool write)
{
    if (sector >= card_sectors || sector > UINT32_MAX || !healthy)
        return -5;
    put(BLKSIZ, BLOCK_SIZE);
    put(BYTCNT, BLOCK_SIZE);
    unsigned index = write ? 24 : 17;
    if (!wait_clear(STATUS, CARD_BUSY) ||
        !command(index, (uint32)sector, RESP | RESP_CRC | DATA | (write ? WRITE_DATA : 0)) ||
        (reg(RESP0) & R1_ERRORS) != 0)
        goto failed;

    unsigned done = 0;
    uint32 blocked = write ? FIFO_FULL : FIFO_EMPTY;
    uint64 start = ticks();
    for (;;) {
        uint32 interrupt = reg(RINTSTS);
        if ((interrupt & DATA_ERRORS) || ticks() - start >= TIMEBASE_HZ)
            goto failed;
        while (done < BLOCK_SIZE / sizeof(*words) && (reg(STATUS) & blocked) == 0) {
            if (write)
                put(FIFO, words[done]);
            else
                words[done] = reg(FIFO);
            ++done;
        }
        if (interrupt & DATA_DONE) {
            if (done != BLOCK_SIZE / sizeof(*words))
                goto failed;
            put(RINTSTS, DATA_DONE);
            if (!write || card_ready())
                return 0;
            goto failed;
        }
    }
failed:
    printf("SDIO1: sector I/O stopped; CMD%d RINTSTS=%x STATUS=%x\n",
           (int)index, reg(RINTSTS), reg(STATUS));
    healthy = false;
    return -5;
}

static int read_physical(uint64 sector, void *data)
{
    uint32 words[BLOCK_SIZE / sizeof(uint32)];
    int rc = transfer(sector, words, false);
    if (rc == 0)
        memcpy(data, words, BLOCK_SIZE);
    return rc;
}

uint64 disk_init(void)
{
    card_sectors = card_init();
    if (card_sectors == 0) {
        printf("SDIO1: SD initialization failed\n");
        return 0;
    }
    healthy = true;
    printf("SDIO1: card %d MiB\n", (int)(card_sectors / 2048));
    int rc = gpt_seele_partition(read_physical, card_sectors, &partition_start);
    if (rc != 0) {
        printf("SDIO1: SeeleFS GPT partition unavailable (%d)\n", rc);
        return 0;
    }
    printf("SDIO1: SeeleFS first LBA=%d length=%d\n",
           (int)partition_start, (int)SEELE_TEST_SECTORS);
    return SEELE_TEST_SECTORS;
}

int disk_read(uint64 sector, void *data)
{
    if (partition_start == 0 || sector >= SEELE_TEST_SECTORS)
        return -22;
    return read_physical(partition_start + sector, data);
}

int disk_write(uint64 sector, const void *data)
{
    if (partition_start == 0 || sector >= SEELE_TEST_SECTORS)
        return -22;
    uint32 words[BLOCK_SIZE / sizeof(uint32)];
    memcpy(words, data, BLOCK_SIZE);
    return transfer(partition_start + sector, words, true);
}

int disk_flush(void)
{
    return partition_start != 0 && healthy && card_ready() ? 0 : -5;
}

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "block_device.h"
#include "ata_block_device.h"

/* ATA I/O ports (primary controller) */
#define ATA_DATA          0x1F0
#define ATA_SECTOR_COUNT  0x1F2
#define ATA_LBA_LOW       0x1F3
#define ATA_LBA_MID       0x1F4
#define ATA_LBA_HIGH      0x1F5
#define ATA_DRIVE         0x1F6
#define ATA_STATUS        0x1F7
#define ATA_COMMAND       0x1F7

/* ATA status register bits */
#define ATA_STATUS_BUSY          0x80
#define ATA_STATUS_DATA_REQUEST  0x08

/* ATA commands */
#define ATA_COMMAND_READ          0x20
#define ATA_COMMAND_WRITE         0x30
#define ATA_COMMAND_CACHE_FLUSH   0xE7
#define ATA_COMMAND_IDENTIFY      0xEC

static inline void outb(uint16_t port, uint8_t val)
{
    /* OUT only ever moves a byte through AL */
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;

    /* IN is the mirror image: a byte read always lands in AL */
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void ata_select_drive(uint8_t value)
{
    outb(ATA_DRIVE, value);

    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);
}

static inline void insw(uint16_t port, void* addr, uint32_t count)
{
    uint16_t* buf = (uint16_t*)addr;

    __asm__ volatile (
        "cld\n\t"       /* EDI must count up, not down */
        "rep insw"      /* read `count` words from `port` into [EDI], advancing EDI each time */
        : "+D"(buf), "+c"(count)
        : "d"(port)
        : "memory"
    );
}

static inline void outsw(uint16_t port, const void* addr, uint32_t count)
{
    const uint16_t* buf = (const uint16_t*)addr;

    __asm__ volatile (
        "cld\n\t"       /* same direction guarantee, this time for ESI */
        "rep outsw"     /* write `count` words from [ESI] to `port`, advancing ESI each time */
        : "+S"(buf), "+c"(count)
        : "d"(port)
        : "memory"
    );
}

static int ata_wait_bsy_clear(void)
{
    for (int i = 0; i < 1000000; i++)
    {
        if (!(inb(ATA_STATUS) & ATA_STATUS_BUSY))
            return 0;
    }
    return -1;
}

static int ata_wait_drq(void)
{
    for (int i = 0; i < 1000000; i++)
    {
        uint8_t st = inb(ATA_STATUS);

        if (st & 0x01)
            return -2;

        if (st & ATA_STATUS_DATA_REQUEST)
            return 0;
    }

    return -1;
}

struct BlockDevice
{
    uint32_t sector_size;
    uint64_t sector_count;
};

static int ata_identify(uint16_t* buf, uint32_t* out_sectors)
{
    ata_wait_bsy_clear();
    ata_select_drive(0xE0);
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW,  0);
    outb(ATA_LBA_MID,  0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_COMMAND_IDENTIFY);

    if (ata_wait_bsy_clear() != 0) return -1;
    if (ata_wait_drq() != 0)       return -1;

    insw(ATA_DATA, buf, 256);

    *out_sectors = (uint32_t)buf[60] | ((uint32_t)buf[61] << 16);
    return 0;
}

BlockDevice* ata_block_device_open(void)
{
    uint16_t identify_buf[256];
    uint32_t sectors;

    if (ata_identify(identify_buf, &sectors) != 0 || sectors == 0)
        return NULL;

    BlockDevice* device = (BlockDevice*)malloc(sizeof(BlockDevice));
    device->sector_size = 512;
    device->sector_count = sectors;

    return device;
}

uint32_t block_device_sector_size(BlockDevice* device)
{
    return device->sector_size;
}

uint64_t block_device_sector_count(BlockDevice* device)
{
    return device->sector_count;
}

void block_device_close(BlockDevice* device)
{
    free(device);
}

int block_device_read(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    void* buffer
)
{
    (void)device;
    uint8_t* out = (uint8_t*)buffer;

    for (uint32_t i = 0; i < sector_count; i++)
    {
        uint32_t sector = (uint32_t)(lba + i);

        if (ata_wait_bsy_clear() != 0) return -1;
        ata_select_drive(0xE0 | ((sector >> 24) & 0x0F));
        outb(ATA_SECTOR_COUNT, 1);
        outb(ATA_LBA_LOW,  (uint8_t)sector);
        outb(ATA_LBA_MID,  (uint8_t)(sector >> 8));
        outb(ATA_LBA_HIGH, (uint8_t)(sector >> 16));
        outb(ATA_COMMAND, ATA_COMMAND_READ);
        if (ata_wait_bsy_clear() != 0) return -1;
        if (ata_wait_drq() != 0)       return -1;
        insw(ATA_DATA, out + (i * 512), 256);
    }

    return 0;
}

int block_device_write(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    const void* buffer
)
{
    (void)device;
    const uint8_t* in = (const uint8_t*)buffer;

    for (uint32_t i = 0; i < sector_count; i++)
    {
        uint32_t sector = (uint32_t)(lba + i);

        if (ata_wait_bsy_clear() != 0) return -1;
        ata_select_drive(0xE0 | ((sector >> 24) & 0x0F));
        outb(ATA_SECTOR_COUNT, 1);
        outb(ATA_LBA_LOW,  (uint8_t)sector);
        outb(ATA_LBA_MID,  (uint8_t)(sector >> 8));
        outb(ATA_LBA_HIGH, (uint8_t)(sector >> 16));
        outb(ATA_COMMAND, ATA_COMMAND_WRITE);
        if (ata_wait_bsy_clear() != 0) return -1;
        if (ata_wait_drq() != 0)       return -1;
        outsw(ATA_DATA, in + (i * 512), 256);
        if (ata_wait_bsy_clear() != 0) return -1;

        outb(ATA_COMMAND, ATA_COMMAND_CACHE_FLUSH);
        if (ata_wait_bsy_clear() != 0) return -1;
    }

    return 0;
}

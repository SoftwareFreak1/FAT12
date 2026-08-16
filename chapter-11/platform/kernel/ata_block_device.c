#include <stdint.h>
#include <stddef.h>
#include "block_device.h"
#include "ata_block_device.h"

struct BlockDevice
{
    uint32_t sector_size;
    uint64_t sector_count;
};

/* ATA I/O ports (primary controller) */
#define ATA_DATA      0x1F0
#define ATA_ERROR     0x1F1
#define ATA_SECCOUNT  0x1F2
#define ATA_LBA_LOW   0x1F3
#define ATA_LBA_MID   0x1F4
#define ATA_LBA_HIGH  0x1F5
#define ATA_DRIVE     0x1F6
#define ATA_STATUS    0x1F7
#define ATA_COMMAND   0x1F7

/* ATA status register bits */
#define ATA_SR_BSY    0x80
#define ATA_SR_DRQ    0x08

/* ATA commands */
#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_IDENTIFY  0xEC

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void ata_io_delay(void)
{
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);
}

static inline void insw(uint16_t port, void* addr, uint32_t count)
{
    uint16_t* buf = (uint16_t*)addr;

    __asm__ volatile (
        "cld\n\t"
        "rep insw"
        : "+D"(buf), "+c"(count)
        : "d"(port)
        : "memory"
    );
}

static inline void outsw(uint16_t port, const void* addr, uint32_t count)
{
    const uint16_t* buf = (const uint16_t*)addr;

    __asm__ volatile (
        "cld\n\t"
        "rep outsw"
        : "+D"(buf), "+c"(count)
        : "d"(port)
        : "memory"
    );
}

static int ata_wait_bsy_clear(void)
{
    for (int i = 0; i < 1000000; i++)
    {
        if (!(inb(ATA_STATUS) & ATA_SR_BSY))
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

        if (st & ATA_SR_DRQ)
            return 0;
    }

    return -1;
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
        outb(ATA_DRIVE, 0xE0 | ((sector >> 24) & 0x0F));
        ata_io_delay();
        outb(ATA_SECCOUNT, 1);
        outb(ATA_LBA_LOW,  (uint8_t)sector);
        outb(ATA_LBA_MID,  (uint8_t)(sector >> 8));
        outb(ATA_LBA_HIGH, (uint8_t)(sector >> 16));
        outb(ATA_COMMAND, ATA_CMD_READ);
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
    (void)lba;
    (void)sector_count;
    (void)buffer;

    return -1;
}

typedef struct
{
    BlockDevice base;
} ATADevice;

void block_device_close(BlockDevice* device)
{
    (void)device;
}

uint32_t block_device_sector_size(BlockDevice* device)
{
    return device->sector_size;
}

uint64_t block_device_sector_count(BlockDevice* device)
{
    return device->sector_count;
}

static int ata_identify(uint16_t* buf, uint32_t* out_sectors)
{
    ata_wait_bsy_clear();
    outb(ATA_DRIVE, 0xE0);
    ata_io_delay();
    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LOW,  0);
    outb(ATA_LBA_MID,  0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    if (ata_wait_bsy_clear() != 0) return -1;
    if (ata_wait_drq() != 0)       return -1;

    insw(ATA_DATA, buf, 256);

    *out_sectors = (uint32_t)buf[60] | ((uint32_t)buf[61] << 16);
    return 0;
}

static ATADevice g_device;

BlockDevice* ata_block_device_open(void)
{
    g_device.base.sector_size = 512;

    uint16_t identify_buf[256];
    uint32_t sectors;

    if (ata_identify(identify_buf, &sectors) == 0 && sectors > 0)
        g_device.base.sector_count = sectors;
    else
        g_device.base.sector_count = (64UL * 1024 * 1024) / 512;

    return (BlockDevice*)&g_device;
}

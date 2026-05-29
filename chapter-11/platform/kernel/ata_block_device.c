#include <stdint.h>
#include <stddef.h>
#include "block_device.h"
#include "ata_block_device.h"

struct BlockDevice {
    uint32_t block_size;
    uint64_t block_count;
};

#define ATA_DATA     0x1F0
#define ATA_SECCOUNT 0x1F2
#define ATA_LBA_LOW  0x1F3
#define ATA_LBA_MID  0x1F4
#define ATA_LBA_HIGH 0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_COMMAND  0x1F7
#define ATA_STATUS   0x1F7

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_CMD_READ 0x20

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

typedef struct {
    struct BlockDevice base;
} ATADevice;

int block_device_read(
    BlockDevice* device,
    uint64_t lba,
    uint32_t block_count,
    void* buffer
)
{
    (void)device;
    uint8_t* out = (uint8_t*)buffer;

    for (uint32_t i = 0; i < block_count; i++)
    {
        uint32_t sector = (uint32_t)(lba + i);

        ata_wait_bsy_clear();
        outb(ATA_DRIVE, 0xE0 | ((sector >> 24) & 0x0F));
        ata_io_delay();
        outb(ATA_SECCOUNT, 1);
        outb(ATA_LBA_LOW,  (uint8_t)sector);
        outb(ATA_LBA_MID,  (uint8_t)(sector >> 8));
        outb(ATA_LBA_HIGH, (uint8_t)(sector >> 16));
        outb(ATA_COMMAND, ATA_CMD_READ);
        ata_wait_bsy_clear();
        ata_wait_drq();
        insw(ATA_DATA, out + (i * 512), 256);
    }

    return 0;
}

int block_device_write(
    BlockDevice* device,
    uint64_t lba,
    uint32_t block_count,
    const void* buffer
)
{
    (void)device;
    (void)lba;
    (void)block_count;
    (void)buffer;
    return 0;
}

void block_device_close(BlockDevice* device)
{
    (void)device;
}

uint32_t block_device_block_size(BlockDevice* device)
{
    return device->block_size;
}

uint64_t block_device_block_count(BlockDevice* device)
{
    return device->block_count;
}

static ATADevice g_device;

BlockDevice* ata_block_device_open(void)
{
    g_device.base.block_size = 512;
    g_device.base.block_count = (64 * 1024 * 1024) / 512;
    return (BlockDevice*)&g_device;
}

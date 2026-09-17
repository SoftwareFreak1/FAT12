#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "block_device.h"
#include "ata_block_device.h"

#define ATA_DATA          0x1F0
#define ATA_SECTOR_COUNT  0x1F2
#define ATA_LBA_LOW       0x1F3
#define ATA_LBA_MID       0x1F4
#define ATA_LBA_HIGH      0x1F5
#define ATA_DRIVE         0x1F6
#define ATA_STATUS        0x1F7
#define ATA_COMMAND       0x1F7

static inline void outb(uint16_t port, uint8_t val)
{
    /* Generates:
           outb %al, %dx

       val  -> AL  ("a")
       port -> DX  ("Nd" would allow an 8-bit immediate instead of DX,
                    but every port here is > 0xFF, so DX always wins) */
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;

    /* Generates:
            inb %dx, %al

       port -> DX  ("Nd", same reasoning as outb: always DX here)
       ret  <- AL  ("=a") */
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));

    return ret;
}

static inline void insw(uint16_t port, void* addr, uint32_t byte_count)
{
    uint16_t* buf = (uint16_t*)addr;
    uint32_t word_count = byte_count / 2;  /* rep insw moves whole 16-bit words, not bytes */

    /* Generates:
            cld        # EDI must count up, not down
            rep insw   # read `word_count` words from `port` into [EDI], advancing EDI each time

       buf        -> EDI  ("+D", INS's fixed destination register)
       word_count -> ECX  ("+c", REP's fixed counter register)
       port       -> DX   ("d", INS's fixed port register - no immediate form exists)
       memory clobber: buf's contents change, but the compiler only sees
       the EDI register value change, not what it points to */
    __asm__ volatile (
        "cld\n\t"
        "rep insw"
        : "+D"(buf), "+c"(word_count)
        : "d"(port)
        : "memory"
    );
}

static inline void outsw(uint16_t port, const void* addr, uint32_t byte_count)
{
    const uint16_t* buf = (const uint16_t*)addr;
    uint32_t word_count = byte_count / 2;  /* rep outsw moves whole 16-bit words, not bytes */

    /* Generates:
            cld         # ESI must count up, not down
            rep outsw   # write `word_count` words from [ESI] to `port`, advancing ESI each time

       buf        -> ESI  ("+S", OUTS's fixed source register)
       word_count -> ECX  ("+c", REP's fixed counter register)
       port       -> DX   ("d", OUTS's fixed port register - no immediate form exists)
       memory clobber: the compiler only sees the ESI register value, not
       that this instruction reads whatever it points to */
    __asm__ volatile (
        "cld\n\t"
        "rep outsw"
        : "+S"(buf), "+c"(word_count)
        : "d"(port)
        : "memory"
    );
}

#define ATA_STATUS_BUSY  0x80

static inline void ata_wait_busy_clear(void)
{
    while (inb(ATA_STATUS) & ATA_STATUS_BUSY)
        ;
}

static inline void ata_issue_command(uint8_t command, uint32_t lba, uint8_t sector_count)
{
    ata_wait_busy_clear();

    /* master drive, LBA mode, plus the LBA's top 4 bits (27:24) */
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));

    /* four dummy reads: lets the status register settle before it's trustworthy.
       QEMU's IDE emulation never needs this delay, but real hardware often does */
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);
    inb(ATA_STATUS);

    outb(ATA_SECTOR_COUNT, sector_count);
    outb(ATA_LBA_LOW, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));

    /* triggers execution — must be last */
    outb(ATA_COMMAND, command);

    ata_wait_busy_clear();
}

#define ATA_COMMAND_IDENTIFY  0xEC
#define ATA_STATUS_DATA_REQUEST  0x08

struct BlockDevice
{
    uint32_t sector_size;
    uint64_t sector_count;
};

static inline void ata_wait_data_request(void)
{
    while (!(inb(ATA_STATUS) & ATA_STATUS_DATA_REQUEST))
        ;
}

BlockDevice* ata_block_device_open(void)
{
    ata_issue_command(ATA_COMMAND_IDENTIFY, 0, 0);

    ata_wait_data_request();
    uint16_t buf[256];
    insw(ATA_DATA, buf, sizeof(buf));

    uint32_t sectors = (uint32_t)buf[60] | ((uint32_t)buf[61] << 16);
    if (sectors == 0) return NULL;

    BlockDevice* device = (BlockDevice*)malloc(sizeof(BlockDevice));
    device->sector_count = sectors;

    /* 512 is the ATA default logical sector size; IDENTIFY only reports
       an actual size when a drive's is larger. Classic ATA/IDE drives,
       and QEMU's IDE emulation, never are, so we hardcode it */
    device->sector_size = 512;

    return device;
}

#define ATA_COMMAND_READ  0x20

int block_device_read(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    void* buffer
)
{
    ata_issue_command(ATA_COMMAND_READ, (uint32_t)lba, (uint8_t)sector_count);

    for (uint32_t i = 0; i < sector_count; i++)
    {
        ata_wait_data_request();
        insw(ATA_DATA, (uint8_t*)buffer + (i * device->sector_size), device->sector_size);
    }

    return 0;
}

#define ATA_COMMAND_WRITE        0x30
#define ATA_COMMAND_CACHE_FLUSH  0xE7

int block_device_write(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    const void* buffer
)
{
    ata_issue_command(ATA_COMMAND_WRITE, (uint32_t)lba, (uint8_t)sector_count);

    for (uint32_t i = 0; i < sector_count; i++)
    {
        ata_wait_data_request();
        outsw(ATA_DATA, (const uint8_t*)buffer + (i * device->sector_size), device->sector_size);
    }

    /* the drive may only have buffered these sectors in its write cache;
       CACHE_FLUSH forces them out to persistent media before we return */
    ata_issue_command(ATA_COMMAND_CACHE_FLUSH, 0, 0);

    return 0;
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

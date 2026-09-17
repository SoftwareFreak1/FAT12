#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "block_device.h"
#include "ram_disk_block_device.h"

#define SECTOR_SIZE    512
#define RAM_DISK_SIZE  (4 * 1024 * 1024)  /* 4 MiB */

struct BlockDevice
{
    uint8_t data[RAM_DISK_SIZE];
};

int block_device_read(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    void* buffer
)
{
    size_t offset = (size_t)(lba * SECTOR_SIZE);
    size_t total_bytes = sector_count * SECTOR_SIZE;
    memcpy(buffer, device->data + offset, total_bytes);
    return 0;
}

int block_device_write(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    const void* buffer
)
{
    size_t offset = (size_t)(lba * SECTOR_SIZE);
    size_t total_bytes = sector_count * SECTOR_SIZE;
    memcpy(device->data + offset, buffer, total_bytes);
    return 0;
}

void block_device_close(BlockDevice* device)
{
    free(device);
}

uint32_t block_device_sector_size(BlockDevice* device)
{
    (void)device;
    return SECTOR_SIZE;
}

uint64_t block_device_sector_count(BlockDevice* device)
{
    (void)device;
    return RAM_DISK_SIZE / SECTOR_SIZE;
}

BlockDevice* ram_disk_block_device_open(void)
{
    return (BlockDevice*)malloc(sizeof(BlockDevice));
}

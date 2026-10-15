#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "block_device.h"
#include "ram_disk_block_device.h"

struct BlockDevice {
    uint8_t* data;
    uint32_t sector_size;
    uint64_t sector_count;
};

BlockDevice* ram_disk_block_device_open(void)
{
    BlockDevice* device = (BlockDevice*)malloc(sizeof(BlockDevice));

    device->sector_size = 512;
    device->sector_count = 8192;  /* 4 MiB, like every disk image since Chapter 1 */
    device->data = (uint8_t*)malloc((size_t)(device->sector_count * device->sector_size));

    return device;
}

uintptr_t ram_disk_block_device_address(BlockDevice* device)
{
    return (uintptr_t)device->data;
}

int block_device_read(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    void* buffer
)
{
    /* Never copy past the end of the volume */
    if (lba + sector_count > device->sector_count) return -1;

    size_t offset = (size_t)(lba * device->sector_size);
    size_t total_bytes = sector_count * device->sector_size;
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
    /* Never copy past the end of the volume */
    if (lba + sector_count > device->sector_count) return -1;

    size_t offset = (size_t)(lba * device->sector_size);
    size_t total_bytes = sector_count * device->sector_size;
    memcpy(device->data + offset, buffer, total_bytes);
    return 0;
}

int block_device_close(BlockDevice* device)
{
    free(device->data);
    free(device);
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

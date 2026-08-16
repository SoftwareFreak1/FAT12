#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include <stdint.h>

typedef struct BlockDevice BlockDevice;

int block_device_read(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    void* buffer
);

int block_device_write(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    const void* buffer
);

uint32_t block_device_sector_size(BlockDevice* device);
uint64_t block_device_sector_count(BlockDevice* device);
void block_device_close(BlockDevice* device);

#endif

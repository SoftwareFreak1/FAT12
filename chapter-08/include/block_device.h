#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include <stdint.h>

/*
 * Opening a block device is platform-specific — each platform provides
 * its own function (e.g. file_block_device_open, ata_block_device_open)
 * with the signature appropriate for that environment.
 * This header only defines the common operations on an already-opened device.
 */

/*
 * All operations return 0 on success, -1 on error.
 * This enables production error handling patterns,
 * but the book suppresses checks for readability.
 * The focus is on FAT12, not on raw block device
 * communication or error handling.
 */

typedef struct BlockDevice BlockDevice;

int block_device_read(
    BlockDevice* device,
    uint64_t lba,
    uint32_t block_count,
    void* buffer
);

int block_device_write(
    BlockDevice* device,
    uint64_t lba,
    uint32_t block_count,
    const void* buffer
);

void block_device_close(BlockDevice* device);
uint32_t block_device_block_size(BlockDevice* device);
uint64_t block_device_block_count(BlockDevice* device);

#endif

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "debug.h"
#include "block_device.h"

struct BlockDevice {
    FILE* file;
    uint32_t sector_size;
    uint64_t sector_count;
};

BlockDevice* file_block_device_open(const char* path) {
    BlockDevice* device = (BlockDevice*)malloc(sizeof(BlockDevice));
    device->file = fopen(path, "r+");
    if (device->file == NULL) {
        free(device);
        return NULL;
    }

    device->sector_size = 512;

    fseeko(device->file, 0, SEEK_END);
    device->sector_count = (uint64_t)(ftello(device->file) / device->sector_size);
    return device;
}

int block_device_read(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    void* buffer
) {
    DBG_PRINT("[ block_device ] read %" PRIu32 " sector(s) starting at LBA %" PRIu64 "\n", sector_count, lba);
    size_t total_bytes = sector_count * device->sector_size;
    off_t offset = lba * device->sector_size;
    DBG_PRINT("[ file_io      ] read %zu bytes at 0x%" PRIx64 "\n", total_bytes, (uint64_t)offset);
    fseeko(device->file, offset, SEEK_SET);
    fread(buffer, 1, total_bytes, device->file);
    return 0;
}

int block_device_write(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    const void* buffer
) {
    DBG_PRINT("[ block_device ] write %" PRIu32 " sector(s) starting at LBA %" PRIu64 "\n", sector_count, lba);
    size_t total_bytes = sector_count * device->sector_size;
    off_t offset = lba * device->sector_size;
    DBG_PRINT("[ file_io      ] write %zu bytes at 0x%" PRIx64 "\n", total_bytes, (uint64_t)offset);
    fseeko(device->file, offset, SEEK_SET);
    fwrite(buffer, 1, total_bytes, device->file);
    return 0;
}

int block_device_close(BlockDevice* device) {
    fclose(device->file);
    free(device);
    return 0;
}

uint32_t block_device_sector_size(BlockDevice* device) {
    return device->sector_size;
}

uint64_t block_device_sector_count(BlockDevice* device) {
    return device->sector_count;
}

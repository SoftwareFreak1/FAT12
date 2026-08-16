#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "debug.h"
#include "block_device.h"

#define SECTOR_SIZE 512

struct BlockDevice {
    FILE* file;
};

BlockDevice* file_block_device_open(const char* path) {
    BlockDevice* device = (BlockDevice*)malloc(sizeof(BlockDevice));
    device->file = fopen(path, "r+");
    if (device->file == NULL) {
        free(device);
        return NULL;
    }
    return device;
}

int block_device_read(
    BlockDevice* device,
    uint64_t lba,
    uint32_t sector_count,
    void* buffer
) {
    DBG_PRINT("[ block_device ] read %" PRIu32 " sector(s) starting at LBA %" PRIu64 "\n", sector_count, lba);
    size_t total_bytes = sector_count * SECTOR_SIZE;
    off_t offset = lba * SECTOR_SIZE;
    DBG_PRINT("[ file         ] read %zu bytes at 0x%" PRIx64 "\n", total_bytes, (uint64_t)offset);
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
    size_t total_bytes = sector_count * SECTOR_SIZE;
    off_t offset = lba * SECTOR_SIZE;
    DBG_PRINT("[ file         ] write %zu bytes at 0x%" PRIx64 "\n", total_bytes, (uint64_t)offset);
    fseeko(device->file, offset, SEEK_SET);
    fwrite(buffer, 1, total_bytes, device->file);
    return 0;
}

void block_device_close(BlockDevice* device) {
    fclose(device->file);
    free(device);
}

uint32_t block_device_sector_size(BlockDevice* device) {
    (void)device;
    return SECTOR_SIZE;
}

uint64_t block_device_sector_count(BlockDevice* device) {
    off_t saved = ftello(device->file);
    fseeko(device->file, 0, SEEK_END);
    off_t size = ftello(device->file);
    fseeko(device->file, saved, SEEK_SET);
    return (uint64_t)(size / SECTOR_SIZE);
}

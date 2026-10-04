#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "block_device.h"
#include "file_block_device.h"

/* same offset as in the xxd dump earlier; Chapter 2 explains why */
#define VOLUME_LABEL_OFFSET 0x2b

int main(void) {
    /* Hardcoded so every command stays short */
    BlockDevice* device = file_block_device_open("disk.img");
    if (device == NULL) {
        fprintf(stderr, "error: could not open disk.img\n");
        return 1;
    }

    uint32_t sector_size = block_device_sector_size(device);
    printf("Sector size (bytes): %u\n", sector_size);
    printf("Sector count: %" PRIu64 "\n", block_device_sector_count(device));

    uint8_t sector[sector_size];
    block_device_read(device, 0, 1, sector);
    printf("Volume label: %.11s\n", (const char*)&sector[VOLUME_LABEL_OFFSET]);

    memcpy(&sector[VOLUME_LABEL_OFFSET], "HELLO DISK!", 11);
    block_device_write(device, 0, 1, sector);

    block_device_close(device);

    return 0;
}

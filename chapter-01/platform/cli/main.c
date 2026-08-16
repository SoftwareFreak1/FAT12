#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "block_device.h"
#include "file_block_device.h"

int main(void) {
    BlockDevice* device = file_block_device_open("disk.img");
    if (device == NULL) {
        fprintf(stderr, "error: could not open disk.img\n");
        return 1;
    }

    uint8_t sector[512];
    block_device_read(device, 0, 1, sector);
    printf("Volume label: %.11s\n", (const char*)&sector[43]);
    block_device_close(device);

    return 0;
}

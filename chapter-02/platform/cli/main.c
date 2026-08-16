#include <stdio.h>
#include <stdlib.h>
#include "block_device.h"
#include "fat12.h"
#include "file_block_device.h"

int main(void)
{
    BlockDevice *device = file_block_device_open("disk.img");
    if (device == NULL)
    {
        fprintf(stderr, "error: could not open disk.img\n");
        return 1;
    }

    FAT12FS *fs = fat12_mount(device);
    fat12_umount(fs);
    block_device_close(device);

    return 0;
}

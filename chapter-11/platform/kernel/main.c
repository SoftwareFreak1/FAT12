#include <stdint.h>
#include "vga.h"
#include "ata_block_device.h"
#include "block_device.h"
#include "fat12.h"

void kernel_main(void)
{
    vga_clear();

    BlockDevice* device = ata_block_device_open();

    VolumeInfo info = fat12_volume_info(device);
    vga_print("Root of '");
    vga_print(info.volume_label);
    vga_print("':\n\n");

    Directory* dir = fat12_opendir(device, "/");
    DirEntry entry;

    while (fat12_readdir(dir, &entry) != -1)
    {
        vga_print(entry.name);
        vga_print("\n");
    }

    fat12_closedir(dir);

    while (1)
    {
        __asm__ volatile ("hlt");
    }
}

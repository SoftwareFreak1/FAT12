#include <stddef.h>
#include <stdint.h>
#include "vga.h"
#include "ata_block_device.h"
#include "block_device.h"
#include "fat12.h"

static BlockDevice* g_device;
static FAT12FS* g_fs;

void kernel_main(void)
{
    vga_clear();
    g_device = ata_block_device_open();

    g_fs = fat12_mount(g_device);
    if (g_fs == NULL)
    {
        vga_print("Mount failed\n");
        return;
    }

    vga_print("FAT12 Kernel Boot\n");

    /* 1. List root directory */
    vga_print("=== Root Directory ===\n");
    Directory* dir = fat12_opendir(g_fs, "/");
    if (dir != NULL)
    {
        DirEntry entry;

        while (fat12_readdir(dir, &entry) != -1)
        {
            vga_print(entry.name);
            vga_print("\n");
        }

        fat12_closedir(dir);
    }

    /* 2. Read /HELLO.TXT from root */
    vga_print("\n=== /HELLO.TXT ===\n");
    File* f = fat12_open(g_fs, "/HELLO.TXT", "r");
    if (f != NULL)
    {
        char buf[256];
        uint32_t bytes;

        while ((bytes = fat12_read(f, buf, sizeof(buf))) > 0)
        {
            buf[bytes < sizeof(buf) ? bytes : sizeof(buf) - 1] = '\0';
            vga_print(buf);
        }

        fat12_close(f);
    }

    /* 3. Read /DATA/NOTES.TXT from a subdirectory */
    vga_print("\n=== /DATA/NOTES.TXT ===\n");
    f = fat12_open(g_fs, "/DATA/NOTES.TXT", "r");
    if (f != NULL)
    {
        char buf[256];
        uint32_t bytes;

        while ((bytes = fat12_read(f, buf, sizeof(buf))) > 0)
        {
            buf[bytes < sizeof(buf) ? bytes : sizeof(buf) - 1] = '\0';
            vga_print(buf);
        }

        fat12_close(f);
    }

    fat12_umount(g_fs);
    vga_print("\nDone.\n");
}

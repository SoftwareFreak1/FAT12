#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "vga.h"
#include "ram_disk_block_device.h"
#include "block_device.h"
#include "fat12.h"

void kernel_main(void)
{
    vga_clear();
    BlockDevice* device = ram_disk_block_device_open();

    /* Format a fresh volume */
    FormatParams params = { "KERNEL" };
    if (fat12_format(device, params) != 0)
    {
        vga_print("Format failed\n");
        return;
    }

    FAT12FS* fs = fat12_mount(device);
    if (fs == NULL)
    {
        vga_print("Mount failed\n");
        return;
    }

    /* Create a file in the root directory */
    File* f = fat12_open(fs, "/HELLO.TXT", 'w');
    if (f != NULL)
    {
        const char* text = "Hello, FAT12!\n";
        fat12_write(f, text, strlen(text));
        fat12_close(f);
    }

    /* List root directory */
    vga_print("=== Root Directory ===\n");
    Directory* dir = fat12_opendir(fs, "/");
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

    /* Read /HELLO.TXT back from root */
    vga_print("\n=== /HELLO.TXT ===\n");
    f = fat12_open(fs, "/HELLO.TXT", 'r');
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

    fat12_umount(fs);
}

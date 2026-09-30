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
    vga_print("FAT12 kernel booted\n\n");
    BlockDevice* device = ram_disk_block_device_open();

    /* Format a fresh volume */
    vga_print("Formatting volume ........ ");
    FormatParams params = { "KERNEL" };
    if (fat12_format(device, params) != 0)
    {
        vga_print("FAILED\n");
        return;
    }
    vga_print("OK\n");

    vga_print("Mounting volume .......... ");
    FAT12FS* fs = fat12_mount(device);
    if (fs == NULL)
    {
        vga_print("FAILED\n");
        return;
    }
    vga_print("OK\n");

    /* Create a file in the root directory */
    vga_print("Writing /HELLO.TXT ....... ");
    File* f = fat12_open(fs, "/HELLO.TXT", 'w');
    if (f != NULL)
    {
        const char* text = "Hello, FAT12!\n";
        fat12_write(f, text, strlen(text));
        fat12_close(f);
        vga_print("OK\n");
    }
    else
    {
        vga_print("FAILED\n");
    }

    /* List root directory */
    vga_print("Listing / ................ ");
    Directory* dir = fat12_opendir(fs, "/");
    if (dir != NULL)
    {
        DirEntry entry;

        vga_print("OK\n\n");
        while (fat12_readdir(dir, &entry) != -1)
        {
            vga_print("    ");
            vga_print(entry.name);
            vga_print("\n");
        }
        vga_print("\n");

        fat12_closedir(dir);
    }
    else
    {
        vga_print("FAILED\n");
    }

    /* Read /HELLO.TXT back from root */
    vga_print("Reading /HELLO.TXT ....... ");
    f = fat12_open(fs, "/HELLO.TXT", 'r');
    if (f != NULL)
    {
        char buf[256];
        uint32_t bytes;

        vga_print("OK\n\n    ");
        while ((bytes = fat12_read(f, buf, sizeof(buf))) > 0)
        {
            buf[bytes < sizeof(buf) ? bytes : sizeof(buf) - 1] = '\0';
            vga_print(buf);
        }
        vga_print("\n");

        fat12_close(f);
    }
    else
    {
        vga_print("FAILED\n");
    }

    vga_print("Unmounting volume ........ ");
    fat12_umount(fs);
    vga_print("OK\n");
}

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "vga.h"
#include "ram_disk_block_device.h"
#include "block_device.h"
#include "fat12.h"

/* There is no printf, so we format hex numbers ourselves */
static void print_hex(uint32_t value)
{
    char str[11] = "0x";

    /* One hex digit per nibble, most significant first */
    for (int i = 0; i < 8; i++)
        str[2 + i] = "0123456789ABCDEF"[(value >> (28 - i * 4)) & 0xF];
    str[10] = '\0';

    vga_print(str);
}

void kernel_main(void)
{
    vga_clear();
    vga_print("FAT12 kernel booted\n\n");
    vga_print("Opening block device ..... ");
    BlockDevice* device = ram_disk_block_device_open();
    /* Also show where the volume lives in RAM, for inspecting it from outside */
    vga_print("OK (RAM disk at ");
    print_hex((uint32_t)ram_disk_block_device_address(device));
    vga_print(")\n");

    /* Format a fresh volume */
    vga_print("Formatting volume ........ ");
    FormatParams params = { "MYKERNEL" };
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
        uint32_t written = fat12_write(f, text, strlen(text));

        /* Closing writes the data to the volume, so it can fail too */
        if (fat12_close(f) == 0 && written == strlen(text))
            vga_print("OK\n");
        else
            vga_print("FAILED\n");
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
        /* Leave room for the '\0' vga_print needs */
        while ((bytes = fat12_read(f, buf, sizeof(buf) - 1)) > 0)
        {
            buf[bytes] = '\0';
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

    vga_print("Closing block device ..... ");
    if (block_device_close(device) != 0)
    {
        vga_print("FAILED\n");
        return;
    }
    vga_print("OK\n");
}

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "block_device.h"
#include "fat12.h"
#include "file_block_device.h"

static int cmd_ls(FAT12FS *fs, int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: ls <path>\n");
        return 1;
    }

    const char *path = argv[2];
    Directory *dir = fat12_opendir(fs, path);
    if (dir == NULL)
    {
        fprintf(stderr, "error: directory not found\n");
        return 1;
    }

    printf("%-12s  %-9s  %-19s  %-19s\n", "NAME", "TYPE/SIZE", "CREATED", "MODIFIED");

    DirEntry entry;
    while (fat12_readdir(dir, &entry) == 0)
    {
        char type_str[16];

        if (entry.kind == ENTRY_VOLUME_LABEL)
            snprintf(type_str, sizeof(type_str), "<VOL>");
        else if (entry.kind == ENTRY_DIRECTORY)
            snprintf(type_str, sizeof(type_str), "<DIR>");
        else
            snprintf(type_str, sizeof(type_str), "%u B", entry.size);

        printf(
            "%-12s  %-9s  %04u-%02u-%02u %02u:%02u:%02u  %04u-%02u-%02u %02u:%02u:%02u\n",
            entry.name, type_str,
            entry.create_time.year, entry.create_time.month, entry.create_time.day,
            entry.create_time.hours, entry.create_time.minutes, entry.create_time.seconds,
            entry.modify_time.year, entry.modify_time.month, entry.modify_time.day,
            entry.modify_time.hours, entry.modify_time.minutes, entry.modify_time.seconds
        );
    }

    fat12_closedir(dir);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: <command>\n");
        return 1;
    }

    BlockDevice *device = file_block_device_open("disk.img");
    if (device == NULL)
    {
        fprintf(stderr, "error: could not open disk image\n");
        return 1;
    }

    FAT12FS *fs = fat12_mount(device);

    char *command = argv[1];
    int ret = 0;

    if (strcmp(command, "ls") == 0)
        ret = cmd_ls(fs, argc, argv);
    else
    {
        fprintf(stderr, "unknown command: %s\n", command);
        ret = 1;
    }

    fat12_umount(fs);
    block_device_close(device);

    return ret;
}

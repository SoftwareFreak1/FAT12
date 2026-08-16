#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "block_device.h"
#include "fat12.h"
#include "file_block_device.h"

static void format_datetime(char *buf, size_t size, Timestamp ts)
{
    snprintf(buf, size, "%04u-%02u-%02u %02u:%02u:%02u",
             ts.year, ts.month, ts.day,
             ts.hours, ts.minutes, ts.seconds);
}

static int cmd_ls(FAT12FS *fs, int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s ls <path>\n", argv[0]);
        return 1;
    }

    const char *path = argv[2];
    Directory *dir = fat12_opendir(fs, path);
    if (dir == NULL)
    {
        fprintf(stderr, "error: directory not found\n");
        return 1;
    }

    DirEntry entry;
    while (fat12_readdir(dir, &entry) == 0)
    {
        char time_str[20];
        char type_str[16];
        format_datetime(time_str, sizeof(time_str), entry.modify_time);

        if (entry.attr == FAT12_ATTR_VOLUME_ID)
            snprintf(type_str, sizeof(type_str), "<VOL>");
        else if (entry.attr & FAT12_ATTR_DIRECTORY)
            snprintf(type_str, sizeof(type_str), "<DIR>");
        else
            snprintf(type_str, sizeof(type_str), "%u B", entry.size);

        printf("%-12s  %-8s  %s\n", entry.name, type_str, time_str);
    }

    fat12_closedir(dir);
    return 0;
}

static int cmd_cat(FAT12FS *fs, int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: fat12-cli cat <path>\n");
        return 1;
    }
    const char *path = argv[2];
    File *file = fat12_open(fs, path, "r");
    if (file == NULL)
    {
        fprintf(stderr, "error: file not found\n");
        return 1;
    }

    uint8_t buf[512];
    uint32_t bytes;
    while ((bytes = fat12_read(file, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, bytes, stdout);

    fat12_close(file);
    return 0;
}

static int cmd_write(FAT12FS *fs, int argc, char *argv[])
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: fat12-cli write <fat_path> <local_path>\n");
        return 1;
    }

    const char *fat_path = argv[2];
    const char *local_path = argv[3];
    FILE *local = fopen(local_path, "rb");
    if (local == NULL)
    {
        fprintf(stderr, "error: cannot open '%s'\n", local_path);
        return 1;
    }

    File *file = fat12_open(fs, fat_path, "w");
    if (file == NULL)
    {
        fprintf(stderr, "error: '%s' already exists or cannot be created\n", fat_path);
        fclose(local);
        return 1;
    }

    uint8_t buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), local)) > 0)
        fat12_write(file, buf, n);

    fat12_close(file);
    fclose(local);

    return 0;
}

static int cmd_mkdir(FAT12FS *fs, int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: fat12-cli mkdir <path>\n");
        return 1;
    }

    const char *path = argv[2];
    if (fat12_mkdir(fs, path) != 0)
    {
        fprintf(stderr, "error: could not create directory\n");
        return 1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <command>\n", argv[0]);
        return 1;
    }

    BlockDevice *device = file_block_device_open("disk.img");
    if (device == NULL)
    {
        fprintf(stderr, "error: could not open disk image\n");
        return 1;
    }

    FAT12FS *fs = fat12_mount(device);
    if (fs == NULL)
    {
        fprintf(stderr, "error: could not mount filesystem\n");
        block_device_close(device);
        return 1;
    }

    char *command = argv[1];
    int ret = 0;

    if (strcmp(command, "ls") == 0)
        ret = cmd_ls(fs, argc, argv);
    else if (strcmp(command, "cat") == 0)
        ret = cmd_cat(fs, argc, argv);
    else if (strcmp(command, "write") == 0)
        ret = cmd_write(fs, argc, argv);
    else if (strcmp(command, "mkdir") == 0)
        ret = cmd_mkdir(fs, argc, argv);
    else
    {
        fprintf(stderr, "unknown command: %s\n", command);
        ret = 1;
    }

    fat12_umount(fs);
    block_device_close(device);
    return ret;
}

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

static int cmd_cat(FAT12FS *fs, int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: cat <path>\n");
        return 1;
    }

    const char *path = argv[2];
    File *file = fat12_open(fs, path, 'r');
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

static int cmd_create(FAT12FS *fs, int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: create <fat_path>  (reads file content from stdin)\n");
        return 1;
    }

    const char *fat_path = argv[2];

    File *file = fat12_open(fs, fat_path, 'w');
    if (file == NULL)
    {
        fprintf(stderr, "error: '%s' already exists or cannot be created\n", fat_path);
        return 1;
    }

    uint8_t buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0)
        fat12_write(file, buf, n);

    if (fat12_close(file) != 0)
    {
        fprintf(stderr, "error: not enough free space to write '%s'\n", fat_path);
        return 1;
    }

    return 0;
}

static int cmd_mkdir(FAT12FS *fs, int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: mkdir <path>\n");
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

static int cmd_rm(FAT12FS *fs, int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: rm <path>\n");
        return 1;
    }

    const char *path = argv[2];
    if (fat12_remove(fs, path) != 0)
    {
        fprintf(stderr, "error: could not delete\n");
        return 1;
    }

    return 0;
}

static int cmd_mv(FAT12FS *fs, int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "usage: mv <old> <new>\n");
        return 1;
    }

    const char *old_path = argv[2];
    const char *new_path = argv[3];
    if (fat12_move(fs, old_path, new_path) != 0)
    {
        fprintf(stderr, "error: could not move\n");
        return 1;
    }

    return 0;
}

static int cmd_format(BlockDevice *device, int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: format <label>\n");
        return 1;
    }

    FormatParams params = { 0 };
    strncpy(params.volume_label, argv[2], sizeof(params.volume_label) - 1);
    params.volume_label[sizeof(params.volume_label) - 1] = '\0';

    if (fat12_format(device, params) != 0)
    {
        fprintf(stderr, "error: format failed\n");
        return 1;
    }

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

    char *command = argv[1];

    /* format works on the raw disk, so it runs before anything is mounted */
    if (strcmp(command, "format") == 0)
    {
        int ret = cmd_format(device, argc, argv);
        block_device_close(device);
        return ret;
    }

    FAT12FS *fs = fat12_mount(device);

    int ret = 0;

    if (strcmp(command, "ls") == 0)
        ret = cmd_ls(fs, argc, argv);
    else if (strcmp(command, "cat") == 0)
        ret = cmd_cat(fs, argc, argv);
    else if (strcmp(command, "create") == 0)
        ret = cmd_create(fs, argc, argv);
    else if (strcmp(command, "mkdir") == 0)
        ret = cmd_mkdir(fs, argc, argv);
    else if (strcmp(command, "rm") == 0)
        ret = cmd_rm(fs, argc, argv);
    else if (strcmp(command, "mv") == 0)
        ret = cmd_mv(fs, argc, argv);
    else
    {
        fprintf(stderr, "unknown command: %s\n", command);
        ret = 1;
    }

    fat12_umount(fs);
    block_device_close(device);

    return ret;
}

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "block_device.h"
#include "fat12.h"
#include "file_block_device.h"

static void format_datetime(char* buf, size_t size,
                            DosTimestamp ts)
{
    snprintf(buf, size, "%04u-%02u-%02u %02u:%02u:%02u",
             ts.year, ts.month, ts.day,
             ts.hours, ts.minutes, ts.seconds);
}

int main(int argc, char *argv[]) {
    const char* disk_path = "disk.img";

    if (argc < 2) {
        fprintf(stderr, "usage: %s <command>\n", argv[0]);
        return 1;
    }

    BlockDevice *disk = file_block_device_open(disk_path);
    if (disk == NULL) {
        fprintf(stderr, "error: could not open disk.img\n");
        return 1;
    }

    char* command = argv[1];

    if (strcmp(command, "info") == 0) {
        VolumeInfo info = fat12_volume_info(disk);

        printf("OEM Name: %s\n", info.oem_name);
        printf("Volume Label: %s\n", info.volume_label);
        printf("File System Type: %s\n", info.file_system_type);
        printf("Bytes Per Sector: %u\n", info.bytes_per_sector);
        printf("Sectors Per Cluster: %u\n", info.sectors_per_cluster);
        printf("Reserved Sector Count: %u\n", info.reserved_sector_count);
        printf("Number of FATs: %u\n", info.num_fats);
        printf("Root Entry Count: %u\n", info.root_entry_count);
        printf("Total Sectors: %u\n", info.total_sectors);
        printf("Media Descriptor: 0x%02x\n", info.media_descriptor);
        printf("FAT Size (sectors): %u\n", info.sectors_per_fat);
        printf("Sectors Per Track: %u\n", info.sectors_per_track);
        printf("Number of Heads: %u\n", info.number_of_heads);
        printf("Hidden Sectors: %u\n", info.hidden_sectors);
        printf("Drive Number: 0x%02x\n", info.drive_number);
        printf("Extended Boot Signature: 0x%02x\n", info.boot_signature);
        printf("Volume ID: 0x%08x\n", info.volume_id);
    }
    else if (strcmp(command, "ls") == 0) {
        Directory* dir = fat12_opendir(disk, (argc >= 3) ? argv[2] : "/");
        if (dir == NULL)
        {
            fprintf(stderr, "error: directory not found\n");
            block_device_close(disk);
            return 1;
        }

        DirEntry entry;
        while (fat12_readdir(dir, &entry) == 0)
        {
            char time_str[20];
            format_datetime(time_str, sizeof(time_str),
                            entry.modify_time);

            char type_str[16];

            if (entry.attr == FAT12_ATTR_VOLUME_ID)
            {
                snprintf(type_str, sizeof(type_str), "<VOL>");
            }
            else if (entry.attr & FAT12_ATTR_DIRECTORY)
            {
                snprintf(type_str, sizeof(type_str), "<DIR>");
            }
            else
            {
                snprintf(type_str, sizeof(type_str), "%u B", entry.size);
            }

            printf("%-12s  %-8s  %s\n",
                   entry.name, type_str, time_str);
        }

        fat12_closedir(dir);
    }
    else if (strcmp(command, "cat") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "usage: fat12-cli cat <path>\n");
            block_device_close(disk);
            return 1;
        }

        File* file = fat12_open(disk, argv[2], "r");
        if (file == NULL)
        {
            fprintf(stderr, "error: file not found\n");
            block_device_close(disk);
            return 1;
        }

        uint8_t buf[512];
        uint32_t bytes;
        while ((bytes = fat12_read(file, buf, sizeof(buf))) > 0)
            fwrite(buf, 1, bytes, stdout);

        fat12_close(file);
    }
    else if (strcmp(command, "write") == 0)
    {
        if (argc < 4)
        {
            fprintf(stderr,
                "usage: fat12-cli write <path> <data>\n");
            block_device_close(disk);
            return 1;
        }

        File* file = fat12_open(disk, argv[2], "w");
        if (file == NULL)
        {
            fprintf(stderr,
                "error: could not create file\n");
            block_device_close(disk);
            return 1;
        }

        fat12_write(file, argv[3], strlen(argv[3]));
        fat12_close(file);
    }
    else if (strcmp(command, "mkdir") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "usage: fat12-cli mkdir <path>\n");
            block_device_close(disk);
            return 1;
        }

        if (fat12_mkdir(disk, argv[2]) != 0)
        {
            fprintf(stderr, "error: could not create directory\n");
        }
    }
    else if (strcmp(command, "rm") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "usage: fat12-cli rm <path>\n");
            block_device_close(disk);
            return 1;
        }

        if (fat12_remove(disk, argv[2]) != 0)
            fprintf(stderr, "error: could not delete\n");
    }

    block_device_close(disk);
    return 0;
}

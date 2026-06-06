#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "block_device.h"
#include "fat12.h"
#include "file_block_device.h"

static void format_datetime(char* buf, size_t size,
                            uint16_t time, uint16_t date)
{
    unsigned hours = (time >> 11) & 0x1F;
    unsigned mins  = (time >> 5)  & 0x3F;
    unsigned secs  = (time & 0x1F) * 2;

    unsigned day   = date & 0x1F;
    unsigned month = (date >> 5) & 0x0F;
    unsigned year  = ((date >> 9) & 0x7F) + 1980;

    snprintf(buf, size, "%04u-%02u-%02u %02u:%02u:%02u",
             year, month, day, hours, mins, secs);
}

int main(int argc, char *argv[]) {
    const char* disk_path = "./disk.img";

    if (argc < 2) return -1;

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
        char dir_path[256];
        strncpy(dir_path, (argc >= 3) ? argv[2] : "/", 255);
        dir_path[255] = '\0';
        for (int i = 0; dir_path[i]; i++)
        {
            if (dir_path[i] >= 'a' && dir_path[i] <= 'z')
                dir_path[i] -= 32;
        }

        Directory* dir = fat12_opendir(disk, dir_path);
        if (dir == NULL)
        {
            fprintf(stderr, "error: directory not found\n");
            block_device_close(disk);
            return 1;
        }

        printf("Directory: %s\n\n", dir_path);

        DirEntry entry;
        while (fat12_readdir(dir, &entry) == 0)
        {
            char time_str[20];
            format_datetime(time_str, sizeof(time_str),
                            entry.modify_time, entry.modify_date);

            if (entry.attr == FAT12_ATTR_VOLUME_ID)
            {
                printf("%-12s  %-8s  %s\n",
                       entry.name, "<VOL>", time_str);
            }
            else if (entry.attr & FAT12_ATTR_DIRECTORY)
            {
                printf("%-12s  %-8s  %s\n",
                       entry.name, "<DIR>", time_str);
            }
            else
            {
                char size_str[16];
                snprintf(size_str, sizeof(size_str), "%u B", entry.size);
                printf("%-12s  %-8s  %s\n",
                       entry.name, size_str, time_str);
            }
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

        char name_upper[256];
        strncpy(name_upper, argv[2], 255);
        name_upper[255] = '\0';
        for (int i = 0; name_upper[i]; i++)
        {
            if (name_upper[i] >= 'a' && name_upper[i] <= 'z')
                name_upper[i] -= 32;
        }

        File* file = fat12_open(disk, name_upper, "r");
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

        char path_upper[256];
        strncpy(path_upper, argv[2], 255);
        path_upper[255] = '\0';
        for (int i = 0; path_upper[i]; i++)
        {
            if (path_upper[i] >= 'a' && path_upper[i] <= 'z')
                path_upper[i] -= 32;
        }

        File* file = fat12_open(disk, path_upper, "w");
        if (file == NULL)
        {
            fprintf(stderr,
                "error: could not create file\n");
            block_device_close(disk);
            return 1;
        }

        fat12_write(file, argv[3], strlen(argv[3]));
        fat12_close(file);

        printf("ok\n");
    }
    else if (strcmp(command, "mkdir") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "usage: fat12-cli mkdir <path>\n");
            block_device_close(disk);
            return 1;
        }

        char path_upper[256];
        strncpy(path_upper, argv[2], 255);
        path_upper[255] = '\0';
        for (int i = 0; path_upper[i]; i++)
        {
            if (path_upper[i] >= 'a' && path_upper[i] <= 'z')
                path_upper[i] -= 32;
        }

        if (fat12_mkdir(disk, path_upper) == 0)
            printf("Directory created\n");
        else
        {
            fprintf(stderr, "error: could not create directory\n");
            block_device_close(disk);
            return 1;
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

        char path_upper[256];
        strncpy(path_upper, argv[2], 255);
        path_upper[255] = '\0';
        for (int i = 0; path_upper[i]; i++)
        {
            if (path_upper[i] >= 'a' && path_upper[i] <= 'z')
                path_upper[i] -= 32;
        }

        if (fat12_remove(disk, path_upper) == 0)
            printf("File deleted\n");
        else
        {
            fprintf(stderr, "error: could not delete file\n");
            block_device_close(disk);
            return 1;
        }
    }
    else if (strcmp(command, "rmdir") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "usage: fat12-cli rmdir <path>\n");
            block_device_close(disk);
            return 1;
        }

        char path_upper[256];
        strncpy(path_upper, argv[2], 255);
        path_upper[255] = '\0';
        for (int i = 0; path_upper[i]; i++)
        {
            if (path_upper[i] >= 'a' && path_upper[i] <= 'z')
                path_upper[i] -= 32;
        }

        if (fat12_rmdir(disk, path_upper) == 0)
            printf("Directory removed\n");
        else
        {
            fprintf(stderr, "error: could not remove directory\n");
            block_device_close(disk);
            return 1;
        }
    }
    else if (strcmp(command, "format") == 0)
    {
        fat12_format(disk);
        printf("Formatted\n");
    }
    else if (strcmp(command, "mv") == 0)
    {
        if (argc < 4)
        {
            fprintf(stderr, "usage: fat12-cli mv <old> <new>\n");
            block_device_close(disk);
            return 1;
        }

        char old_upper[256];
        strncpy(old_upper, argv[2], 255);
        old_upper[255] = '\0';
        for (int i = 0; old_upper[i]; i++)
        {
            if (old_upper[i] >= 'a' && old_upper[i] <= 'z')
                old_upper[i] -= 32;
        }

        char new_upper[256];
        strncpy(new_upper, argv[3], 255);
        new_upper[255] = '\0';
        for (int i = 0; new_upper[i]; i++)
        {
            if (new_upper[i] >= 'a' && new_upper[i] <= 'z')
                new_upper[i] -= 32;
        }

        if (fat12_rename(disk, old_upper, new_upper) == 0)
            printf("Renamed\n");
        else
        {
            fprintf(stderr, "error: could not rename\n");
            block_device_close(disk);
            return 1;
        }
    }

    block_device_close(disk);
    return 0;
}

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
        printf("Boot Signature: 0x%02x\n", info.boot_signature);
        printf("Volume ID: 0x%08x\n", info.volume_id);
    }
    else if (strcmp(command, "ls") == 0) {
        Directory* dir = fat12_opendir(disk, "/");

        printf("Directory: \\\n\n");

        DirEntry entry;
        while (fat12_readdir(dir, &entry) == 0)
        {
            char time_str[20];
            format_datetime(time_str, sizeof(time_str),
                            entry.modify_time, entry.modify_date);

            if (entry.attr == 0x08)
            {
                printf("%-12s  %-8s  %s\n",
                       entry.name, "<VOL>", time_str);
            }
            else if (entry.attr & 0x10)
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

    block_device_close(disk);
    return 0;
}

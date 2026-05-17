#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "block_device.h"
#include "fat12.h"
#include "file_block_device.h"

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

    block_device_close(disk);
    return 0;
}

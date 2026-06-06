#include <stdlib.h>
#include <string.h>
#include "block_device.h"
#include "layout.h"
#include "fat12.h"

static BootSector fat12_read_boot_sector(BlockDevice* disk)
{
    uint32_t sector_size = block_device_sector_size(disk);

    void* buffer = malloc(sector_size);
    block_device_read(disk, 0, 1, buffer);

    BootSector result;
    memcpy(&result, buffer, sizeof(BootSector));

    free(buffer);

    return result;
}

VolumeInfo fat12_volume_info(BlockDevice* disk)
{
    BootSector s = fat12_read_boot_sector(disk);
    VolumeInfo info = {0};

    memcpy(info.oem_name, s.oem_name, 8);
    info.oem_name[8] = '\0';

    memcpy(info.volume_label, s.extended_bpb.volume_label, 11);
    info.volume_label[11] = '\0';

    memcpy(info.file_system_type, s.extended_bpb.file_system_type, 8);
    info.file_system_type[8] = '\0';

    info.bytes_per_sector = s.bpb.bytes_per_sector;
    info.sectors_per_cluster = s.bpb.sectors_per_cluster;
    info.reserved_sector_count = s.bpb.reserved_sector_count;
    info.num_fats = s.bpb.num_fats;
    info.root_entry_count = s.bpb.root_entry_count;
    info.total_sectors = s.bpb.total_sectors_16
        ? s.bpb.total_sectors_16
        : s.bpb.total_sectors_32;
    info.media_descriptor = s.bpb.media;
    info.sectors_per_fat = s.bpb.fat_size_16;
    info.sectors_per_track = s.bpb.sectors_per_track;
    info.number_of_heads = s.bpb.number_of_heads;
    info.hidden_sectors = s.bpb.hidden_sectors;
    info.drive_number = s.extended_bpb.drive_number;
    info.boot_signature = s.extended_bpb.boot_signature;
    info.volume_id = s.extended_bpb.volume_id;

    return info;
}

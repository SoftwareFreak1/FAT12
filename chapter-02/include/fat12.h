#ifndef FAT12_H
#define FAT12_H
#include <stddef.h>
#include <stdint.h>
#include "block_device.h"

typedef struct {
    char oem_name[9];
    char volume_label[12];
    char file_system_type[9];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint32_t total_sectors;
    uint8_t media_descriptor;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t number_of_heads;
    uint32_t hidden_sectors;
    uint8_t drive_number;
    uint8_t boot_signature;
    uint32_t volume_id;
} VolumeInfo;

VolumeInfo fat12_volume_info(BlockDevice* disk);

#endif

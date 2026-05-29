#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdint.h>

#pragma pack(push, 1)

typedef struct {
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t number_of_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
} BPB;

typedef struct {
    uint8_t  drive_number;
    uint8_t  reserved_nt;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char file_system_type[8];
} ExtendedBPB;

typedef struct {
    uint8_t jump[3];
    char oem_name[8];
    BPB bpb;
    ExtendedBPB extended_bpb;
    uint8_t  boot_code[448];
    uint16_t signature;
} BootSector;

typedef struct
{
    char name[8];
    char ext[3];
    uint8_t attr;
    uint8_t reserved;
    uint8_t create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t last_write_time;
    uint16_t last_write_date;
    uint16_t first_cluster;
    uint32_t file_size;
} DirectoryEntry;

#pragma pack(pop)

#endif

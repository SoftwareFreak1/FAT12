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

/* File attribute flags */
#define FAT12_ATTR_READ_ONLY   0x01
#define FAT12_ATTR_HIDDEN      0x02
#define FAT12_ATTR_SYSTEM      0x04
#define FAT12_ATTR_VOLUME_ID   0x08
#define FAT12_ATTR_LONG_NAME   0x0F
#define FAT12_ATTR_DIRECTORY   0x10
#define FAT12_ATTR_ARCHIVE     0x20

/* Directory entry (user-facing) */
typedef struct {
    char name[12];
    uint32_t size;
    uint8_t attr;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t modify_time;
    uint16_t modify_date;
} DirEntry;

typedef struct Directory Directory;

/* Directory iteration */
Directory* fat12_opendir(BlockDevice* disk, const char* path);
int fat12_readdir(Directory* dir, DirEntry* out);
void fat12_closedir(Directory* dir);

/* File I/O */
typedef struct File File;

/* Path must be uppercase (FAT stores names in uppercase) */
File* fat12_open(BlockDevice* disk, const char* path, const char* mode);
uint32_t fat12_read(File* file, void* buffer, uint32_t size);
uint32_t fat12_write(File* file, const void* buffer, uint32_t size);
void fat12_close(File* file);

int fat12_mkdir(BlockDevice* disk, const char* path);
int fat12_remove(BlockDevice* disk, const char* path);
int fat12_rmdir(BlockDevice* disk, const char* path);
int fat12_rename(BlockDevice* disk, const char* old_path, const char* new_path);
void fat12_format(BlockDevice* disk);

#endif

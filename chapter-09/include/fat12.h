#ifndef FAT12_H
#define FAT12_H
#include <stddef.h>
#include <stdint.h>
#include "block_device.h"

typedef struct FAT12FS FAT12FS;

FAT12FS* fat12_mount(BlockDevice* device);
void fat12_umount(FAT12FS* fs);

/* File attribute flags */
#define FAT12_ATTR_READ_ONLY   0x01
#define FAT12_ATTR_HIDDEN      0x02
#define FAT12_ATTR_SYSTEM      0x04
#define FAT12_ATTR_VOLUME_ID   0x08
#define FAT12_ATTR_LONG_NAME   0x0F
#define FAT12_ATTR_DIRECTORY   0x10
#define FAT12_ATTR_ARCHIVE     0x20


/* Decoded timestamp */
typedef struct {
    unsigned year;
    unsigned month;
    unsigned day;
    unsigned hours;
    unsigned minutes;
    unsigned seconds;
} Timestamp;

/* Directory entry (user-facing) */
typedef struct {
    char name[13];
    uint32_t size;
    uint8_t attr;
    Timestamp create_time;
    Timestamp modify_time;
} DirEntry;

typedef struct Directory Directory;

/* Directory iteration */
Directory* fat12_opendir(FAT12FS* fs, const char* path);
int fat12_readdir(Directory* dir, DirEntry* out);
void fat12_closedir(Directory* dir);

/* File I/O */
typedef struct File File;

File* fat12_open(FAT12FS* fs, const char* path, const char* mode);
uint32_t fat12_read(File* file, void* buffer, uint32_t size);
uint32_t fat12_write(File* file, const void* buffer, uint32_t size);
int fat12_close(File* file);

int fat12_mkdir(FAT12FS* fs, const char* path);
int fat12_remove(FAT12FS* fs, const char* path);
int fat12_move(FAT12FS* fs, const char* old_path, const char* new_path);

#endif

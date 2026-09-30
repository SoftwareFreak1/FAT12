#ifndef FAT12_H
#define FAT12_H
#include <stdint.h>
#include "block_device.h"

typedef struct FAT12FS FAT12FS;

FAT12FS* fat12_mount(BlockDevice* device);
void fat12_umount(FAT12FS* fs);

typedef struct {
    unsigned year;
    unsigned month;
    unsigned day;
    unsigned hours;
    unsigned minutes;
    unsigned seconds;
} Timestamp;

typedef enum {
    ENTRY_FILE,
    ENTRY_DIRECTORY,
    ENTRY_VOLUME_LABEL
} EntryKind;

typedef struct {
    char name[13];
    uint32_t size;
    EntryKind kind;
    Timestamp create_time;
    Timestamp modify_time;
} DirEntry;

typedef struct Directory Directory;

Directory* fat12_opendir(FAT12FS* fs, const char* path);
int fat12_readdir(Directory* dir, DirEntry* out);
void fat12_closedir(Directory* dir);

typedef struct File File;

File* fat12_open(FAT12FS* fs, const char* path, char mode);
uint32_t fat12_read(File* file, void* buffer, uint32_t size);
uint32_t fat12_write(File* file, const void* buffer, uint32_t size);
int fat12_close(File* file);

int fat12_mkdir(FAT12FS* fs, const char* path);
int fat12_remove(FAT12FS* fs, const char* path);
int fat12_move(FAT12FS* fs, const char* old_path, const char* new_path);

typedef struct {
    char volume_label[12];
    /* Any other customization/parameter we want to add can go here */
} FormatParams;

int fat12_format(BlockDevice* device, FormatParams params);

#endif

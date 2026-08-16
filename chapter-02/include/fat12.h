#ifndef FAT12_H
#define FAT12_H
#include <stdint.h>
#include "block_device.h"

typedef struct FAT12FS FAT12FS;

FAT12FS* fat12_mount(BlockDevice* device);
void fat12_umount(FAT12FS* fs);

#endif

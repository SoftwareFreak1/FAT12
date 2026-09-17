#ifndef FILE_BLOCK_DEVICE_H
#define FILE_BLOCK_DEVICE_H

#include "block_device.h"

BlockDevice* file_block_device_open(const char* path);

#endif

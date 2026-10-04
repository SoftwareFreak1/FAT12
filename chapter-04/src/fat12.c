#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "block_device.h"
#include "debug.h"
#include "layout.h"
#include "fat12.h"

#define DIRECTORY_ENTRY_SIZE 32

#define FAT12_ATTR_READ_ONLY   0x01
#define FAT12_ATTR_HIDDEN      0x02
#define FAT12_ATTR_SYSTEM      0x04
#define FAT12_ATTR_VOLUME_ID   0x08
#define FAT12_ATTR_DIRECTORY   0x10
#define FAT12_ATTR_ARCHIVE     0x20

#define NAME_FREE  0xE5
#define NAME_END   0x00

#define FIRST_DATA_CLUSTER  2

static BootSector read_boot_sector(BlockDevice* device)
{
    uint32_t sector_size = block_device_sector_size(device);
    void* buffer = malloc(sector_size);
    block_device_read(device, 0, 1, buffer);

    BootSector result;
    memcpy(&result, buffer, sizeof(BootSector));
    free(buffer);

    return result;
}

struct FAT12FS {
    BlockDevice* device;
    BootSector bs;
    uint32_t fat_lba;
    uint32_t fat_sectors;
    uint32_t root_dir_lba;
    uint32_t root_dir_sectors;
    uint32_t data_lba;
    uint32_t cluster_bytes;
};

FAT12FS* fat12_mount(BlockDevice* device)
{
    DBG_PRINT("[ fat12        ] --- fat12_mount() ---\n");
    FAT12FS* fs = malloc(sizeof(FAT12FS));
    fs->device = device;
    fs->bs = read_boot_sector(device);

    /* jump, boot_code, reserved_nt: not used by FAT itself, so we skip them */
    DBG_PRINT("[ fat12        ] OEM Name: %.8s\n", fs->bs.oem_name);
    DBG_PRINT("[ fat12        ] Bytes Per Sector: %u\n", fs->bs.bpb.bytes_per_sector);
    DBG_PRINT("[ fat12        ] Sectors Per Cluster: %u\n", fs->bs.bpb.sectors_per_cluster);
    DBG_PRINT("[ fat12        ] Reserved Sector Count: %u\n", fs->bs.bpb.reserved_sector_count);
    DBG_PRINT("[ fat12        ] Number of FATs: %u\n", fs->bs.bpb.num_fats);
    DBG_PRINT("[ fat12        ] Root Entry Count: %u\n", fs->bs.bpb.root_entry_count);
    DBG_PRINT("[ fat12        ] Total Sectors (16): %u\n", fs->bs.bpb.total_sectors_16);
    DBG_PRINT("[ fat12        ] Media Descriptor: 0x%02x\n", fs->bs.bpb.media);
    DBG_PRINT("[ fat12        ] FAT Size (sectors): %u\n", fs->bs.bpb.fat_size_16);
    DBG_PRINT("[ fat12        ] Sectors Per Track: %u\n", fs->bs.bpb.sectors_per_track);
    DBG_PRINT("[ fat12        ] Number of Heads: %u\n", fs->bs.bpb.number_of_heads);
    DBG_PRINT("[ fat12        ] Hidden Sectors: %u\n", fs->bs.bpb.hidden_sectors);
    DBG_PRINT("[ fat12        ] Total Sectors (32): %u\n", fs->bs.bpb.total_sectors_32);
    DBG_PRINT("[ fat12        ] Drive Number: 0x%02x\n", fs->bs.extended_bpb.drive_number);
    DBG_PRINT("[ fat12        ] Boot Signature: 0x%02x\n", fs->bs.extended_bpb.boot_signature);
    DBG_PRINT("[ fat12        ] Volume ID: 0x%08x\n", fs->bs.extended_bpb.volume_id);
    DBG_PRINT("[ fat12        ] Volume Label: %.11s\n", fs->bs.extended_bpb.volume_label);
    DBG_PRINT("[ fat12        ] File System Type: %.8s\n", fs->bs.extended_bpb.file_system_type);
    DBG_PRINT("[ fat12        ] Signature: 0x%04x\n", fs->bs.signature);

    fs->fat_lba = fs->bs.bpb.reserved_sector_count;
    fs->fat_sectors = fs->bs.bpb.num_fats * fs->bs.bpb.fat_size_16;
    fs->root_dir_lba = fs->fat_lba + fs->fat_sectors;
    fs->root_dir_sectors = ((fs->bs.bpb.root_entry_count * DIRECTORY_ENTRY_SIZE) + (fs->bs.bpb.bytes_per_sector - 1)) / fs->bs.bpb.bytes_per_sector;
    fs->data_lba = fs->root_dir_lba + fs->root_dir_sectors;
    fs->cluster_bytes = fs->bs.bpb.sectors_per_cluster * fs->bs.bpb.bytes_per_sector;

    DBG_PRINT("[ fat12        ] FAT start LBA: %u\n", fs->fat_lba);
    DBG_PRINT("[ fat12        ] FAT size (sectors): %u\n", fs->fat_sectors);
    DBG_PRINT("[ fat12        ] Root dir start LBA: %u\n", fs->root_dir_lba);
    DBG_PRINT("[ fat12        ] Root dir size (sectors): %u\n", fs->root_dir_sectors);
    DBG_PRINT("[ fat12        ] Data region start LBA: %u\n", fs->data_lba);
    DBG_PRINT("[ fat12        ] Cluster size (bytes): %u\n", fs->cluster_bytes);

    return fs;
}

void fat12_umount(FAT12FS* fs)
{
    free(fs);
}

static void decode_8_3_name(const DirectoryEntry* entry, char* out)
{
    int i = 0;

    /* 0x05 escape — real first byte is 0xE5 */
    if (entry->name[0] == 0x05)
        out[i++] = (char)0xE5;

    /* Copy name bytes until padding or end */
    for (; i < 8 && entry->name[i] != ' '; i++)
        out[i] = entry->name[i];

    /* If extension exists, insert dot and copy non-space bytes */
    const char* extension = entry->name + 8;
    int has_extension = extension[0] != ' ';
    if (has_extension)
    {
        out[i] = '.';
        i++;
        for (int k = 0; k < 3; k++)
        {
            if (extension[k] != ' ')
            {
                out[i] = extension[k];
                i++;
            }
        }
    }

    out[i] = '\0';
}

static Timestamp decode_timestamp(uint16_t time, uint16_t date)
{
    Timestamp ts;

    ts.hours   = (time >> 11) & 0x1F;
    ts.minutes = (time >> 5)  & 0x3F;
    ts.seconds = (time & 0x1F) * 2;

    ts.day     = date & 0x1F;
    ts.month   = (date >> 5) & 0x0F;
    ts.year    = ((date >> 9) & 0x7F) + 1980;

    return ts;
}

static DirectoryEntry* read_root_directory(FAT12FS* fs)
{
    DBG_PRINT("[ fat12        ] read root directory\n");
    uint32_t bytes = fs->root_dir_sectors * fs->bs.bpb.bytes_per_sector;
    DirectoryEntry* entries = (DirectoryEntry*)malloc(bytes);
    block_device_read(fs->device, fs->root_dir_lba, fs->root_dir_sectors, entries);

    return entries;
}

static bool is_free_entry(const DirectoryEntry* entry)
{
    // Cast to unsigned char: 0xE5 doesn't fit a signed char (-128 to 127),
    // so without the cast, sign-extension turns it into 0xFFFFFFE5 and the
    // comparison fails.
    return (unsigned char)entry->name[0] == NAME_FREE;
}

static bool is_end_of_directory(const DirectoryEntry* entry)
{
    return entry->name[0] == NAME_END;
}

static bool is_lfn_fragment(const DirectoryEntry* entry)
{
    /* Bits 6 and 7 of the attribute byte are reserved, so mask them
       off (0x3F) before comparing instead of testing for an exact match. */
    return (entry->attributes & 0x3F) == (FAT12_ATTR_READ_ONLY | FAT12_ATTR_HIDDEN | FAT12_ATTR_SYSTEM | FAT12_ATTR_VOLUME_ID);
}

static bool is_invalid_entry(const DirectoryEntry* entry)
{
    /* Directory and Volume Label together mean neither — the FAT spec's
       own validation algorithm treats this combination as corruption,
       never something a well-formed volume produces. */
    uint8_t mask = FAT12_ATTR_DIRECTORY | FAT12_ATTR_VOLUME_ID;
    return (entry->attributes & mask) == mask;
}

static DirectoryEntry* next_valid_entry(
    DirectoryEntry* entries,
    uint32_t count,
    uint32_t* offset
)
{
    while (*offset < count)
    {
        DirectoryEntry* entry = &entries[*offset];
        if (is_end_of_directory(entry)) return NULL;

        (*offset)++;

        if (is_free_entry(entry)) continue;
        if (is_lfn_fragment(entry)) continue;
        if (is_invalid_entry(entry)) continue;

        return entry;
    }

    return NULL;
}

struct Directory {
    DirectoryEntry* entries;
    uint32_t count;
    uint32_t offset;
};

Directory* fat12_opendir(FAT12FS* fs, const char* path)
{
    DBG_PRINT("[ fat12        ] --- fat12_opendir(\"%s\") ---\n", path);

    /* Guards against anything but the root directory for now */
    if (strcmp(path, "/") != 0) return NULL;

    Directory* dir = (Directory*)malloc(sizeof(Directory));
    dir->entries = read_root_directory(fs);
    dir->count = fs->bs.bpb.root_entry_count;
    dir->offset = 0;

    return dir;
}

int fat12_readdir(Directory* dir, DirEntry* out)
{
    DBG_PRINT("[ fat12        ] --- fat12_readdir() ---\n");
    DirectoryEntry* entry = next_valid_entry(dir->entries, dir->count, &dir->offset);
    if (entry == NULL) return -1;

    decode_8_3_name(entry, out->name);
    out->size = entry->file_size;
    out->create_time = decode_timestamp(entry->create_time, entry->create_date);
    out->modify_time = decode_timestamp(entry->last_write_time, entry->last_write_date);

    if (entry->attributes & FAT12_ATTR_VOLUME_ID)
        out->kind = ENTRY_VOLUME_LABEL;
    else if (entry->attributes & FAT12_ATTR_DIRECTORY)
        out->kind = ENTRY_DIRECTORY;
    else
        out->kind = ENTRY_FILE;

    return 0;
}

void fat12_closedir(Directory* dir)
{
    free(dir->entries);
    free(dir);
}

static uint32_t data_cluster_to_lba(FAT12FS* fs, uint16_t cluster)
{
    return fs->data_lba + ((cluster - FIRST_DATA_CLUSTER) * fs->bs.bpb.sectors_per_cluster);
}

static void str_upper(char* s)
{
    // char may be signed, so bytes >= 0x80 could arrive as negative values,
    // which toupper does not accept; the cast keeps them in 0-255
    for (int i = 0; s[i] != '\0'; i++) {
        s[i] = (char)toupper((unsigned char)s[i]);
    }
}

static DirectoryEntry* find_entry_by_name(
    DirectoryEntry* entries,
    uint32_t count,
    const char* name
)
{
    /* Uppercase the name for comparison: names on disk are always uppercase */
    char target[13];
    strncpy(target, name, 12);
    target[12] = '\0';
    str_upper(target);

    uint32_t offset = 0;
    DirectoryEntry* entry;

    while ((entry = next_valid_entry(entries, count, &offset)) != NULL)
    {
        /* Skip the volume label: it has a name, but it is not a file or directory */
        if (entry->attributes & FAT12_ATTR_VOLUME_ID) continue;

        char entry_name[13];
        decode_8_3_name(entry, entry_name);

        if (strcmp(entry_name, target) == 0) return entry;
    }

    return NULL;
}

static int resolve_path(
    FAT12FS* fs,
    const char* path,
    DirectoryEntry* out
)
{
    /* Only absolute paths are supported: every path must start at "/" */
    if (path[0] != '/') return -1;

    DirectoryEntry* entries = read_root_directory(fs);

    DirectoryEntry* entry = find_entry_by_name(
        entries,
        fs->bs.bpb.root_entry_count,
        path + 1 // skip the leading "/"
    );

    if (entry != NULL)
    {
        memcpy(out, entry, sizeof(DirectoryEntry));
    }

    free(entries);

    return entry != NULL ? 0 : -1;
}

struct File {
    uint8_t* data;
    uint32_t size;
    uint32_t position;
    char mode;
};

File* fat12_open(FAT12FS* fs, const char* path, char mode)
{
    DBG_PRINT("[ fat12        ] --- fat12_open(\"%s\", mode='%c') ---\n", path, mode);

    if (mode == 'r')
    {
        DirectoryEntry resolved;
        /* Fail if the path does not resolve to an entry */
        if (resolve_path(fs, path, &resolved) != 0) return NULL;
        /* Only files can be opened, not directories */
        if (resolved.attributes & FAT12_ATTR_DIRECTORY) return NULL;

        uint8_t* data = NULL;
        uint32_t size = resolved.file_size;

        /* Empty files own no cluster (first_cluster is 0), so there is nothing to read */
        if (size > 0)
        {
            /* One cluster only: anything past it is cut off for now */
            if (size > fs->cluster_bytes) size = fs->cluster_bytes;

            DBG_PRINT("[ fat12        ] read cluster %u\n", resolved.first_cluster);
            data = (uint8_t*)malloc(fs->cluster_bytes);
            block_device_read(fs->device, data_cluster_to_lba(fs, resolved.first_cluster), fs->bs.bpb.sectors_per_cluster, data);
        }

        File* file = (File*)malloc(sizeof(File));
        file->data = data;
        file->size = size;
        file->position = 0;
        file->mode = 'r';
        return file;
    }

    return NULL;
}

uint32_t fat12_read(File* file, void* buffer, uint32_t size)
{
    if (file->mode != 'r') return 0;
    /* Empty files have no data to copy from */
    if (file->data == NULL) return 0;

    uint32_t remaining = file->size - file->position;
    if (size < remaining) remaining = size;

    memcpy(buffer, file->data + file->position, remaining);
    file->position += remaining;

    return remaining;
}

int fat12_close(File* file)
{
    DBG_PRINT("[ fat12        ] --- fat12_close() ---\n");

    /* free(NULL) is a no-op, so empty files are fine */
    free(file->data);
    free(file);

    return 0;
}

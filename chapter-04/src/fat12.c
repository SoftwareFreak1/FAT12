#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "block_device.h"
#include "layout.h"
#include "fat12.h"
#include "debug.h"

/* Directory entry name[0] sentinels */
#define NAME_DELETED  0xE5
#define NAME_END      0x00
#define FAT12_ATTR_LONG_NAME   0x0F

// Sentinel meaning "the root directory"
#define ROOT_DIR_CLUSTER 0

struct FAT12FS {
    BlockDevice* device;
    BootSector bs;
    uint32_t fat_lba;
    uint32_t fat_sectors;
    uint32_t root_dir_lba;
    uint32_t root_dir_sectors;
    /* --- NEW: first data-region LBA, in-memory FAT copy, and cached data cluster count --- */
    uint32_t first_data_lba;
    uint8_t* fat;
    uint32_t data_cluster_count;
};

static BootSector read_boot_sector(BlockDevice* device);
static DirectoryEntry* read_root_directory(FAT12FS* fs, uint32_t* count);
static void decode_8_3_name(const DirectoryEntry* raw, char* out);
static Timestamp decode_dos_timestamp(uint16_t time, uint16_t date);
static int next_active_entry(DirectoryEntry* entries, uint32_t count, uint32_t* offset, DirectoryEntry** out);
static int is_deleted_entry(const DirectoryEntry* entry);
static uint32_t data_cluster_to_lba(FAT12FS* fs, uint16_t cluster);
static uint16_t find_next_cluster(FAT12FS* fs, uint16_t cluster);
static uint8_t* read_cluster_chain(FAT12FS* fs, uint16_t first_cluster, uint32_t* out_bytes);
static void str_upper(char* s);
static int resolve_path(FAT12FS* fs, const char* path, DirectoryEntry* out);
static DirectoryEntry* find_by_name_in_entries(DirectoryEntry* entries, uint32_t count, const char* name);

static uint32_t total_data_clusters(FAT12FS* fs)
{
    uint32_t total_sectors = fs->bs.bpb.total_sectors_16
        ? fs->bs.bpb.total_sectors_16
        : fs->bs.bpb.total_sectors_32;

    uint32_t data_sectors = total_sectors - fs->first_data_lba;

    return data_sectors / fs->bs.bpb.sectors_per_cluster;
}

FAT12FS* fat12_mount(BlockDevice* device)
{
    FAT12FS* fs = malloc(sizeof(FAT12FS));
    fs->device = device;
    fs->bs = read_boot_sector(device);
    fs->fat_lba = fs->bs.bpb.reserved_sector_count;
    fs->fat_sectors = fs->bs.bpb.num_fats * fs->bs.bpb.fat_size_16;
    fs->root_dir_lba = fs->fat_lba + fs->fat_sectors;
    fs->root_dir_sectors = ((fs->bs.bpb.root_entry_count * sizeof(DirectoryEntry))
        + (fs->bs.bpb.bytes_per_sector - 1))
        / fs->bs.bpb.bytes_per_sector;
    fs->first_data_lba = fs->root_dir_lba + fs->root_dir_sectors;
    /* --- NEW: total data cluster count, cached once so chain walks never recompute it --- */
    fs->data_cluster_count = total_data_clusters(fs);

    DBG_PRINT("[ fat12 ] FAT start LBA: %u\n", fs->fat_lba);
    DBG_PRINT("[ fat12 ] FAT size (sectors): %u\n", fs->fat_sectors);
    DBG_PRINT("[ fat12 ] Root dir start LBA: %u\n", fs->root_dir_lba);
    DBG_PRINT("[ fat12 ] Root dir size (sectors): %u\n", fs->root_dir_sectors);
    DBG_PRINT("[ fat12 ] First data LBA: %u\n", fs->first_data_lba);
    /* --- NEW --- */
    DBG_PRINT("[ fat12 ] Data cluster count: %u\n", fs->data_cluster_count);

    /* --- NEW: read the whole FAT into memory once --- */
    uint32_t fat_bytes = fs->bs.bpb.fat_size_16 * fs->bs.bpb.bytes_per_sector;
    fs->fat = (uint8_t*)malloc(fat_bytes);
    block_device_read(device, fs->fat_lba, fs->bs.bpb.fat_size_16, fs->fat);

    return fs;
}

void fat12_umount(FAT12FS* fs)
{
    free(fs->fat);
    free(fs);
}

struct Directory {
    DirectoryEntry* entries;
    uint32_t count;
    uint32_t offset;
};

Directory* fat12_opendir(FAT12FS* fs, const char* path)
{
    DirectoryEntry resolved;
    if (resolve_path(fs, path, &resolved) != 0)
        return NULL;
    if (!(resolved.attr & FAT12_ATTR_DIRECTORY))
        return NULL;

    uint32_t count;
    DirectoryEntry* entries = read_root_directory(fs, &count);
    Directory* dir = (Directory*)malloc(sizeof(Directory));
    dir->entries = entries;
    dir->count = count;
    dir->offset = 0;

    return dir;
}

int fat12_readdir(Directory* dir, DirEntry* out)
{
    DirectoryEntry* raw;
    if (!next_active_entry(dir->entries, dir->count, &dir->offset, &raw))
        return -1;

    memset(out->name, 0, sizeof(out->name));
    decode_8_3_name(raw, out->name);

    out->size = raw->file_size;
    out->attr = raw->attr;
    out->create_time = decode_dos_timestamp(raw->create_time, raw->create_date);
    out->modify_time = decode_dos_timestamp(raw->last_write_time, raw->last_write_date);

    return 0;
}

void fat12_closedir(Directory* dir)
{
    free(dir->entries);
    free(dir);
}

struct File
{
    FAT12FS* fs;
    uint8_t* data;
    uint32_t size;
    uint32_t position;
};

File* fat12_open(FAT12FS* fs, const char* path, const char* mode)
{
    if (mode == NULL || strcmp(mode, "r") != 0)
        return NULL;

    DirectoryEntry resolved;
    if (resolve_path(fs, path, &resolved) != 0)
        return NULL;
    if (resolved.attr & FAT12_ATTR_DIRECTORY)
        return NULL;

    uint32_t chain_bytes;
    uint8_t* data = NULL;

    if (resolved.file_size > 0)
    {
        data = read_cluster_chain(fs, resolved.first_cluster, &chain_bytes);
        if (data == NULL)
        {
            return NULL;
        }
    }

    File* file = (File*)malloc(sizeof(File));
    file->fs = fs;
    file->data = data;
    file->size = resolved.file_size;
    file->position = 0;
    return file;
}

uint32_t fat12_read(File* file, void* buffer, uint32_t size)
{
    if (file->position >= file->size) return 0;

    uint32_t remaining = file->size - file->position;
    if (size < remaining) remaining = size;

    memcpy(buffer, file->data + file->position, remaining);
    file->position += remaining;

    return remaining;
}

void fat12_close(File* file)
{
    free(file->data);
    free(file);
}

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

static DirectoryEntry* read_root_directory(
    FAT12FS* fs,
    uint32_t* count
)
{
    uint32_t bytes = fs->root_dir_sectors * fs->bs.bpb.bytes_per_sector;
    *count = bytes / sizeof(DirectoryEntry);
    DirectoryEntry* entries = (DirectoryEntry*)malloc(bytes);
    block_device_read(fs->device, fs->root_dir_lba, fs->root_dir_sectors, entries);

    return entries;
}

static void decode_8_3_name(const DirectoryEntry* raw, char* out)
{
    int i = 0;

    /* 0x05 escape — real first byte is 0xE5 */
    if (raw->name[0] == 0x05)
        out[i++] = (char)0xE5;

    /* Copy name bytes until padding or end */
    for (; i < 8 && raw->name[i] != ' '; i++)
        out[i] = raw->name[i];

    /* If extension exists, insert dot and copy non-space bytes */
    int has_ext = raw->ext[0] != ' ';
    if (has_ext)
    {
        out[i] = '.';
        i++;
        for (int k = 0; k < 3; k++)
        {
            if (raw->ext[k] != ' ')
            {
                out[i] = raw->ext[k];
                i++;
            }
        }
    }

    out[i] = '\0';
}

static Timestamp decode_dos_timestamp(uint16_t time, uint16_t date)
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

static int is_deleted_entry(const DirectoryEntry* entry)
{
    // Cast to unsigned char: 0xE5 doesn't fit a signed char (-128 to 127),
    // so without the cast, sign-extension turns it into 0xFFFFFFE5 and the
    // comparison fails.
    return (unsigned char)entry->name[0] == NAME_DELETED;
}

static int next_active_entry(
    DirectoryEntry* entries,
    uint32_t count,
    uint32_t* offset,
    DirectoryEntry** out
)
{
    while (*offset < count)
    {
        DirectoryEntry* raw = &entries[*offset];

        /* End-of-directory marker -- no more entries after this */
        if (raw->name[0] == NAME_END) return 0;

        (*offset)++;

        /* Deleted entry */
        if (is_deleted_entry(raw)) continue;
        /* Long filename fragment -- not a real entry */
        if (raw->attr == FAT12_ATTR_LONG_NAME) continue;

        *out = raw;
        return 1;
    }

    return 0;
}

static uint32_t data_cluster_to_lba(FAT12FS* fs, uint16_t cluster)
{
    uint32_t lba = fs->first_data_lba + ((cluster - 2) * fs->bs.bpb.sectors_per_cluster);
    DBG_PRINT("[ fat12 ] cluster %u -> LBA %u\n", cluster, lba);
    return lba;
}

static uint16_t find_next_cluster(
    FAT12FS* fs,
    uint16_t cluster
)
{
    uint32_t offset = cluster + (cluster / 2);

    if (cluster & 1)
    {
        /* odd cluster: shift right by 4 */
        return ((fs->fat[offset + 1] << 8) | fs->fat[offset]) >> 4;
    }
    else
    {
        /* even cluster: mask low 12 bits */
        return ((fs->fat[offset + 1] << 8) | fs->fat[offset]) & 0x0FFF;
    }
}

/* FAT entry special values */
#define CLUSTER_FIRST            0x002

static uint8_t* read_cluster_chain(
    FAT12FS* fs,
    uint16_t first_cluster,
    uint32_t* out_bytes
)
{
    uint32_t cluster_bytes = fs->bs.bpb.sectors_per_cluster * fs->bs.bpb.bytes_per_sector;
    uint32_t max_bytes = 0;
    uint32_t capacity = 0;
    uint8_t* all = NULL;
    uint16_t cluster = first_cluster;

    while (cluster >= CLUSTER_FIRST && (uint32_t)(cluster - 2) < fs->data_cluster_count)
    {
        uint32_t lba = data_cluster_to_lba(fs, cluster);
        DBG_PRINT("[ fat12 ] read: cluster %u\n", cluster);

        uint8_t* buf = (uint8_t*)malloc(cluster_bytes);
        block_device_read(fs->device, lba, fs->bs.bpb.sectors_per_cluster, buf);

        uint32_t needed = max_bytes + cluster_bytes;
        if (needed > capacity)
        {
            capacity = capacity ? capacity * 2 : 4096;
            if (capacity < needed) capacity = needed;
            all = (uint8_t*)realloc(all, capacity);
        }

        memcpy(all + max_bytes, buf, cluster_bytes);
        max_bytes += cluster_bytes;
        free(buf);

        cluster = find_next_cluster(fs, cluster);
    }

    *out_bytes = max_bytes;
    return all;
}

static void str_upper(char* s)
{
    for (int i = 0; s[i] != '\0'; i++)
    {
        if (s[i] >= 'a' && s[i] <= 'z')
            s[i] -= 32; // ASCII: lowercase letters are 32 above uppercase
    }
}

static int resolve_path(
    FAT12FS* fs,
    const char* path,
    DirectoryEntry* out
)
{
    const char* p = path;
    if (*p == '/') p++;

    if (*p == '\0')
    {
        out->attr = FAT12_ATTR_DIRECTORY;
        out->first_cluster = ROOT_DIR_CLUSTER;
        return 0;
    }

    char name[13];
    strncpy(name, p, 12);
    name[12] = '\0';
    str_upper(name);

    uint32_t count;
    DirectoryEntry* entries = read_root_directory(fs, &count);
    if (entries == NULL) return -1;

    DirectoryEntry* raw = find_by_name_in_entries(entries, count, name);
    if (raw != NULL)
    {
        memcpy(out, raw, sizeof(DirectoryEntry));
    }

    free(entries);
    return raw != NULL ? 0 : -1;
}

static DirectoryEntry* find_by_name_in_entries(
    DirectoryEntry* entries,
    uint32_t count,
    const char* name
)
{
    uint32_t i = 0;
    DirectoryEntry* raw;
    while (next_active_entry(entries, count, &i, &raw))
    {
        if (raw->attr == FAT12_ATTR_VOLUME_ID) continue;

        char entry_name[13];
        decode_8_3_name(raw, entry_name);
        str_upper(entry_name);

        if (strcmp(entry_name, name) == 0)
            return raw;
    }
    return NULL;
}

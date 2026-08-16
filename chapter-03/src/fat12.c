#include <stdlib.h>
#include <string.h>
#include "block_device.h"
#include "debug.h"
#include "layout.h"
#include "fat12.h"

/* Directory entry name[0] sentinels */
#define NAME_END      0x00
#define NAME_DELETED  0xE5
#define FAT12_ATTR_LONG_NAME   0x0F

struct FAT12FS {
    BlockDevice* device;
    BootSector bs;
    /* --- NEW: cached position and size, computed once on mount --- */
    uint32_t fat_lba;
    uint32_t fat_sectors;
    uint32_t root_dir_lba;
    uint32_t root_dir_sectors;
};

// Sentinel meaning "the root directory"
#define ROOT_DIR_CLUSTER 0

static BootSector read_boot_sector(BlockDevice* device);
static DirectoryEntry* read_root_directory(FAT12FS* fs, uint32_t* count);
static void decode_8_3_name(const DirectoryEntry* raw, char* out);
static Timestamp decode_dos_timestamp(uint16_t time, uint16_t date);
static int next_active_entry(DirectoryEntry* entries, uint32_t count, uint32_t* offset, DirectoryEntry** out);
static int is_deleted_entry(const DirectoryEntry* entry);
static int resolve_path(FAT12FS* fs, const char* path, DirectoryEntry* out);

FAT12FS* fat12_mount(BlockDevice* device)
{
    FAT12FS* fs = (FAT12FS*)malloc(sizeof(FAT12FS));
    fs->device = device;
    fs->bs = read_boot_sector(device);
    fs->fat_lba = fs->bs.bpb.reserved_sector_count;
    fs->fat_sectors = fs->bs.bpb.num_fats * fs->bs.bpb.fat_size_16;
    fs->root_dir_lba = fs->fat_lba + fs->fat_sectors;
    fs->root_dir_sectors = ((fs->bs.bpb.root_entry_count * sizeof(DirectoryEntry))
        + (fs->bs.bpb.bytes_per_sector - 1))
        / fs->bs.bpb.bytes_per_sector;

    DBG_PRINT("[ fat12 ] FAT start LBA: %u\n", fs->fat_lba);
    DBG_PRINT("[ fat12 ] FAT size (sectors): %u\n", fs->fat_sectors);
    DBG_PRINT("[ fat12 ] Root dir start LBA: %u\n", fs->root_dir_lba);
    DBG_PRINT("[ fat12 ] Root dir size (sectors): %u\n", fs->root_dir_sectors);

    return fs;
}

void fat12_umount(FAT12FS* fs)
{
    free(fs);
}

struct Directory {
    DirectoryEntry* entries;
    uint32_t count;
    uint32_t offset;
};

static int resolve_path(
    FAT12FS* fs,
    const char* path,
    DirectoryEntry* out
)
{
    (void)fs;
    const char* p = path;
    if (*p == '/') p++;

    if (*p == '\0')
    {
        out->attr = FAT12_ATTR_DIRECTORY;
        out->first_cluster = ROOT_DIR_CLUSTER;
        return 0;
    }

    return -1;
}

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

        /* End-of-directory marker — no more entries after this */
        if (raw->name[0] == NAME_END) return 0;

        (*offset)++;

        /* Deleted entry */
        if (is_deleted_entry(raw)) continue;
        /* Long filename fragment — not a real entry */
        if (raw->attr == FAT12_ATTR_LONG_NAME) continue;

        *out = raw;
        return 1;
    }

    return 0;
}

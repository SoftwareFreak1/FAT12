#include <stdlib.h>
#include <string.h>
#include "block_device.h"
#include "layout.h"
#include "fat12.h"
#include "debug.h"

static BootSector fat12_read_boot_sector(BlockDevice* disk)
{
    uint32_t sector_size = block_device_sector_size(disk);

    void* buffer = malloc(sector_size);
    block_device_read(disk, 0, 1, buffer);

    BootSector result;
    memcpy(&result, buffer, sizeof(BootSector));

    free(buffer);

    return result;
}

VolumeInfo fat12_volume_info(BlockDevice* disk)
{
    BootSector s = fat12_read_boot_sector(disk);
    VolumeInfo info = {0};

    memcpy(info.oem_name, s.oem_name, 8);
    info.oem_name[8] = '\0';

    memcpy(info.volume_label, s.extended_bpb.volume_label, 11);
    info.volume_label[11] = '\0';

    memcpy(info.file_system_type, s.extended_bpb.file_system_type, 8);
    info.file_system_type[8] = '\0';

    info.bytes_per_sector = s.bpb.bytes_per_sector;
    info.sectors_per_cluster = s.bpb.sectors_per_cluster;
    info.reserved_sector_count = s.bpb.reserved_sector_count;
    info.num_fats = s.bpb.num_fats;
    info.root_entry_count = s.bpb.root_entry_count;
    info.total_sectors = s.bpb.total_sectors_16
        ? s.bpb.total_sectors_16
        : s.bpb.total_sectors_32;
    info.media_descriptor = s.bpb.media;
    info.sectors_per_fat = s.bpb.fat_size_16;
    info.sectors_per_track = s.bpb.sectors_per_track;
    info.number_of_heads = s.bpb.number_of_heads;
    info.hidden_sectors = s.bpb.hidden_sectors;
    info.drive_number = s.extended_bpb.drive_number;
    info.boot_signature = s.extended_bpb.boot_signature;
    info.volume_id = s.extended_bpb.volume_id;

    return info;
}

static uint32_t root_dir_sector_count(BootSector bs)
{
    return ((bs.bpb.root_entry_count * 32)
        + (bs.bpb.bytes_per_sector - 1))
        / bs.bpb.bytes_per_sector;
}

static uint32_t calculate_root_dir_lba(BootSector bs)
{
    return bs.bpb.reserved_sector_count +
           (bs.bpb.num_fats * bs.bpb.fat_size_16);
}

static DirectoryEntry* fat12_root_directory(
    BlockDevice* disk,
    BootSector bs,
    uint32_t* count
)
{
    uint32_t sectors = root_dir_sector_count(bs);
    uint32_t lba = calculate_root_dir_lba(bs);
    uint32_t bytes = sectors * bs.bpb.bytes_per_sector;

    *count = bytes / sizeof(DirectoryEntry);

    DirectoryEntry* entries =
        (DirectoryEntry*)malloc(bytes);

    block_device_read(disk, lba, sectors, entries);

    return entries;
}

static void format_8_3_name(const DirectoryEntry* raw, char* out)
{
    int i = 0;

    if ((unsigned char)raw->name[0] == 0x05)
        out[i++] = (char)0xE5;

    for (; i < 8 && raw->name[i] != ' '; i++)
        out[i] = raw->name[i];

    int has_ext = 0;
    for (int k = 0; k < 3; k++)
    {
        if (raw->ext[k] != ' ')
        {
            has_ext = 1;
            break;
        }
    }

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

static DosTimestamp decode_dos_timestamp(uint16_t time, uint16_t date)
{
    DosTimestamp ts;
    ts.hours   = (time >> 11) & 0x1F;
    ts.minutes = (time >> 5)  & 0x3F;
    ts.seconds = (time & 0x1F) * 2;
    ts.day     = date & 0x1F;
    ts.month   = (date >> 5) & 0x0F;
    ts.year    = ((date >> 9) & 0x7F) + 1980;
    return ts;
}

struct Directory {
    DirectoryEntry* entries;
    uint32_t count;
    uint32_t offset;
};

static int next_live_entry(
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
        if (raw->name[0] == 0x00) return 0;

        (*offset)++;

        /* Deleted entry */
        if ((unsigned char)raw->name[0] == 0xE5) continue;
        /* Long filename fragment — not a real entry */
        if (raw->attr == FAT12_ATTR_LONG_NAME) continue;

        *out = raw;
        return 1;
    }

    return 0;
}

Directory* fat12_opendir(BlockDevice* disk, const char* path)
{
    (void)path; /* path resolution added in Chapter 5 — always reads root for now */

    BootSector bs = fat12_read_boot_sector(disk);

    uint32_t count;
    DirectoryEntry* entries =
        fat12_root_directory(disk, bs, &count);

    Directory* dir = (Directory*)malloc(sizeof(Directory));
    dir->entries = entries;
    dir->count = count;
    dir->offset = 0;

    return dir;
}

int fat12_readdir(Directory* dir, DirEntry* out)
{
    DirectoryEntry* raw;
    if (!next_live_entry(dir->entries, dir->count, &dir->offset, &raw))
        return -1;

    memset(out->name, 0, sizeof(out->name));
    format_8_3_name(raw, out->name);

    out->size = raw->file_size;
    out->attr = raw->attr;
    out->create_time = decode_dos_timestamp(
        raw->create_time, raw->create_date);
    out->modify_time = decode_dos_timestamp(
        raw->last_write_time, raw->last_write_date);

    return 0;
}

void fat12_closedir(Directory* dir)
{
    free(dir->entries);
    free(dir);
}

static uint32_t first_data_sector(BootSector bs)
{
    uint32_t lba = bs.bpb.reserved_sector_count +
           (bs.bpb.num_fats * bs.bpb.fat_size_16) +
           root_dir_sector_count(bs);
    DBG_PRINT("[ fat12 ] first data sector: %u\n", lba);
    return lba;
}

static uint32_t data_cluster_to_lba(BootSector bs, uint16_t cluster)
{
    return ((cluster - 2) * bs.bpb.sectors_per_cluster)
           + first_data_sector(bs);
}

static uint16_t read_fat12_entry(
    const uint8_t* fat,
    uint16_t cluster
)
{
    uint32_t offset = cluster + (cluster / 2);

    if (cluster & 1)
    {
        /* odd cluster: shift right by 4 */
        return
            ((fat[offset + 1] << 8) |
              fat[offset]) >> 4;
    }
    else
    {
        /* even cluster: mask low 12 bits */
        return
            ((fat[offset + 1] << 8) |
              fat[offset]) & 0x0FFF;
    }
}

static uint8_t* fat_load(BlockDevice* disk, BootSector bs)
{
    uint32_t fat_bytes =
        bs.bpb.fat_size_16 *
        bs.bpb.bytes_per_sector;

    uint8_t* fat = (uint8_t*)malloc(fat_bytes);

    block_device_read(
        disk,
        bs.bpb.reserved_sector_count,
        bs.bpb.fat_size_16,
        fat
    );

    return fat;
}

static uint16_t total_data_clusters(BootSector bs)
{
    uint32_t total_sectors = bs.bpb.total_sectors_16
        ? bs.bpb.total_sectors_16
        : bs.bpb.total_sectors_32;

    uint32_t data_sectors = total_sectors - first_data_sector(bs);

    return data_sectors / bs.bpb.sectors_per_cluster;
}

static uint8_t* read_cluster_chain(
    BlockDevice* disk,
    BootSector bs,
    const uint8_t* fat,
    uint16_t first_cluster,
    uint32_t* out_bytes
)
{
    uint32_t cluster_bytes =
        bs.bpb.sectors_per_cluster *
        bs.bpb.bytes_per_sector;
    uint16_t max_cluster = total_data_clusters(bs);
    uint32_t max_bytes = 0;
    uint32_t capacity = 0;
    uint8_t* all = NULL;
    uint16_t cluster = first_cluster;

    while (cluster >= 0x002 && cluster < 0xFF0 && (cluster - 2) < max_cluster)
    {
        uint32_t lba = data_cluster_to_lba(bs, cluster);
        DBG_PRINT("[ fat12 ] read: cluster %u\n", cluster);

        uint8_t* buf = (uint8_t*)malloc(cluster_bytes);
        block_device_read(disk, lba, bs.bpb.sectors_per_cluster, buf);

        uint32_t needed = max_bytes + cluster_bytes;
        if (needed > capacity)
        {
            capacity = capacity ? capacity * 2 : 4096;
            if (capacity < needed) capacity = needed;
            all = (uint8_t*)realloc(
                all, capacity);
        }

        memcpy(all + max_bytes, buf, cluster_bytes);
        max_bytes += cluster_bytes;
        free(buf);

        cluster = read_fat12_entry(fat, cluster);
    }

    *out_bytes = max_bytes;
    return all;
}

static int find_file_in_root(
    BlockDevice* disk,
    BootSector bs,
    const char* name,
    uint16_t* out_cluster,
    uint32_t* out_size
)
{
    uint32_t count;
    DirectoryEntry* entries = fat12_root_directory(disk, bs, &count);

    uint32_t i = 0;
    DirectoryEntry* raw;
    while (next_live_entry(entries, count, &i, &raw))
    {
        if (raw->attr & FAT12_ATTR_DIRECTORY) continue;

        char entry_name[13];
        format_8_3_name(raw, entry_name);

        if (strcmp(entry_name, name) == 0)
        {
            *out_cluster = raw->first_cluster;
            *out_size = raw->file_size;
            free(entries);
            return 1;
        }
    }

    free(entries);
    return 0;
}

static void str_upper(char* s)
{
    for (int i = 0; s[i] != '\0'; i++)
    {
        if (s[i] >= 'a' && s[i] <= 'z')
            s[i] -= 32;
    }
}

struct File
{
    BlockDevice* disk;
    uint8_t* data;
    uint8_t* fat;
    BootSector bs;
    uint32_t size;
    uint32_t position;
};

File* fat12_open(BlockDevice* disk, const char* path, const char* mode)
{
    (void)mode; /* reserved for write support in Chapter 6 */

    char name[13];
    strncpy(name, path, 12);
    name[12] = '\0';
    str_upper(name);

    BootSector bs = fat12_read_boot_sector(disk);

    uint16_t first_cluster;
    uint32_t file_size;
    if (!find_file_in_root(disk, bs, name, &first_cluster, &file_size))
        return NULL;

    uint8_t* fat = fat_load(disk, bs);

    uint32_t chain_bytes;
    File* file = (File*)malloc(sizeof(File));
    file->disk = disk;
    file->bs = bs;
    file->fat = fat;
    file->data = read_cluster_chain(disk, bs, fat, first_cluster, &chain_bytes);
    if (file->data == NULL)
    {
        free(fat);
        free(file);
        return NULL;
    }
    file->size = file_size;
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
    free(file->fat);
    free(file);
}

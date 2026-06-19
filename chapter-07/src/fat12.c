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

/* Internal sentinel: resolved path is root */
#define ROOT_DIR_CLUSTER 0

static uint8_t* fat_load(BlockDevice* disk, BootSector bs);

static DirectoryEntry* read_directory_entries(
    BlockDevice* disk,
    BootSector bs,
    uint8_t* fat,
    uint16_t first_cluster,
    uint32_t* count
);

static int fat12_resolve_path(
    BlockDevice* disk,
    const char* path,
    int last_is_dir,
    DirectoryEntry* out
);

static int split_parent_name(
    const char* path,
    char* parent,
    int parent_size,
    char* name,
    int name_size
);

Directory* fat12_opendir(BlockDevice* disk, const char* path)
{
    DirectoryEntry resolved;
    if (fat12_resolve_path(disk, path, 1, &resolved) != 0)
        return NULL;

    BootSector bs = fat12_read_boot_sector(disk);

    uint32_t count;
    DirectoryEntry* entries;

    if (resolved.first_cluster == ROOT_DIR_CLUSTER)
    {
        entries = fat12_root_directory(disk, bs, &count);
    }
    else
    {
        uint8_t* fat = fat_load(disk, bs);
        if (fat == NULL) return NULL;

        entries = read_directory_entries(
            disk, bs, fat,
            resolved.first_cluster, &count);
        free(fat);
    }

    if (entries == NULL) return NULL;

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
    uint16_t first_cluster;
    int mode;
    char parent_path[256];
    char name[13];
};

File* fat12_open(BlockDevice* disk, const char* path, const char* mode)
{
    BootSector bs = fat12_read_boot_sector(disk);
    uint8_t* fat = fat_load(disk, bs);
    if (fat == NULL) return NULL;

    if (mode[0] == 'w')
    {
        DirectoryEntry resolved;
        if (fat12_resolve_path(disk, path, 0, &resolved) == 0 ||
            fat12_resolve_path(disk, path, 1, &resolved) == 0)
        {
            free(fat);
            return NULL;
        }

        char parent_path[256];
        char name[13];

        if (split_parent_name(path, parent_path, sizeof(parent_path),
                              name, sizeof(name)) < 0)
        {
            free(fat);
            return NULL;
        }

        File* file = (File*)malloc(sizeof(File));
        file->disk = disk;
        file->bs = bs;
        file->fat = fat;
        file->data = NULL;
        file->size = 0;
        file->position = 0;
        file->first_cluster = 0;
        file->mode = 'w';
        memcpy(file->parent_path, parent_path, 256);
        memcpy(file->name, name, 13);
        return file;
    }

    DirectoryEntry resolved;
    if (fat12_resolve_path(disk, path, 0, &resolved) != 0 ||
        (resolved.attr & FAT12_ATTR_DIRECTORY))
    {
        free(fat);
        return NULL;
    }

    uint32_t chain_bytes;
    uint8_t* data = read_cluster_chain(
        disk, bs, fat,
        resolved.first_cluster, &chain_bytes);
    if (data == NULL)
    {
        free(fat);
        return NULL;
    }

    File* file = (File*)malloc(sizeof(File));
    file->disk = disk;
    file->bs = bs;
    file->fat = fat;
    file->data = data;
    file->size = resolved.file_size;
    file->position = 0;
    file->first_cluster = resolved.first_cluster;
    file->mode = 'r';
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

static uint16_t allocate_chain(
    uint8_t* fat,
    BootSector bs,
    uint32_t needed_bytes,
    uint32_t* out_chain_bytes
);

static void write_clusters(
    BlockDevice* disk,
    BootSector bs,
    const uint8_t* fat,
    uint16_t first_cluster,
    const uint8_t* data,
    uint32_t size
);

static int create_dir_entry(
    BlockDevice* disk,
    BootSector bs,
    uint8_t* fat,
    const char* parent_path,
    const char* name,
    uint16_t first_cluster,
    uint32_t file_size
);

static void fat_sync(BlockDevice* disk, BootSector bs, uint8_t* fat);

static int create_directory_contents(
    BlockDevice* disk,
    BootSector bs,
    const uint8_t* fat,
    uint16_t cluster,
    uint16_t parent_cluster
);

void fat12_close(File* file)
{
    if (file->mode == 'w')
    {
        if (file->size > 0)
        {
            uint32_t chain_bytes;
            uint16_t first = allocate_chain(
                file->fat, file->bs, file->size, &chain_bytes);

            if (first != 0 && chain_bytes >= file->size)
            {
                file->first_cluster = first;

                write_clusters(file->disk, file->bs, file->fat,
                    first, file->data, file->size);

                create_dir_entry(file->disk, file->bs, file->fat,
                    file->parent_path, file->name,
                    first, file->size);

                fat_sync(file->disk, file->bs, file->fat);
            }
        }
        else
        {
            create_dir_entry(file->disk, file->bs, file->fat,
                file->parent_path, file->name, 0, 0);
        }
    }

    free(file->data);
    free(file->fat);
    free(file);
}

static DirectoryEntry* read_directory_entries(
    BlockDevice* disk,
    BootSector bs,
    uint8_t* fat,
    uint16_t first_cluster,
    uint32_t* count
)
{
    uint32_t bytes;
    uint8_t* raw = read_cluster_chain(
        disk, bs, fat, first_cluster, &bytes);

    if (raw == NULL)
    {
        *count = 0;
        return NULL;
    }

    *count = bytes / sizeof(DirectoryEntry);
    return (DirectoryEntry*)raw;
}

static int split_path(
    const char* path,
    char components[][13],
    int max
)
{
    char* copy = strdup(path);
    if (copy == NULL) return -1;
    str_upper(copy);

    int count = 0;
    char* saveptr;
    char* token = strtok_r(copy, "/", &saveptr);
    while (token != NULL && count < max)
    {
        strncpy(components[count], token, 12);
        components[count][12] = '\0';
        count++;
        token = strtok_r(NULL, "/", &saveptr);
    }

    if (token != NULL)
    {
        free(copy);
        return -1;
    }

    free(copy);
    return count;
}

static DirectoryEntry* find_by_name_in_entries(
    DirectoryEntry* entries,
    uint32_t count,
    const char* name
)
{
    uint32_t i = 0;
    DirectoryEntry* raw;
    while (next_live_entry(entries, count, &i, &raw))
    {
        if (raw->attr == FAT12_ATTR_VOLUME_ID) continue;

        char entry_name[13];
        format_8_3_name(raw, entry_name);

        if (strcmp(entry_name, name) == 0)
            return raw;
    }
    return NULL;
}

static int fat12_resolve_path(
    BlockDevice* disk,
    const char* path,
    int last_is_dir,
    DirectoryEntry* out
)
{
    BootSector bs = fat12_read_boot_sector(disk);

    uint8_t* fat = fat_load(disk, bs);
    if (fat == NULL) return -1;

    /* ---- Step 1: Tokenize the path ---- */
    char components[32][13];
    int depth = split_path(path, components, 32);
    if (depth < 0)
    {
        free(fat);
        return -1;
    }

    /* Root path shortcut — no tokens means "/" */
    if (depth == 0)
    {
        free(fat);
        out->attr = FAT12_ATTR_DIRECTORY;
        out->first_cluster = ROOT_DIR_CLUSTER;
        return 0;
    }

    /* ---- Step 2: Start at root ---- */
    uint32_t current_count;
    DirectoryEntry* current_entries =
        fat12_root_directory(disk, bs, &current_count);
    if (current_entries == NULL)
    {
        free(fat);
        return -1;
    }

    /* ---- Step 3: Walk each component ---- */
    for (int i = 0; i < depth; i++)
    {
        DirectoryEntry* raw = find_by_name_in_entries(
            current_entries, current_count, components[i]);

        if (raw == NULL)
        {
            free(current_entries);
            free(fat);
            return -1;
        }

        if (i == depth - 1)
        {
            /* Last component — save it after verifying the type matches */
            if ((last_is_dir && !(raw->attr & FAT12_ATTR_DIRECTORY)) ||
                (!last_is_dir && (raw->attr & FAT12_ATTR_DIRECTORY)))
            {
                free(current_entries);
                free(fat);
                return -1;
            }

            *out = *raw;
        }
        else
        {
            /* Intermediate component — must be a directory, descend into it */
            if (!(raw->attr & FAT12_ATTR_DIRECTORY))
            {
                free(current_entries);
                free(fat);
                return -1;
            }

            uint32_t new_count;
            DirectoryEntry* new_entries =
                read_directory_entries(
                    disk, bs, fat,
                    raw->first_cluster,
                    &new_count);

            free(current_entries);
            current_entries = new_entries;
            current_count = new_count;
        }
    }

    /* ---- Step 4: Cleanup ---- */
    free(current_entries);
    free(fat);
    return 0;
}

static void write_fat12_entry(
    uint8_t* fat,
    uint16_t cluster,
    uint16_t value
)
{
    uint32_t offset = cluster + (cluster / 2);

    if (cluster & 1)
    {
        /* odd cluster: write high nibble, write next byte */
        fat[offset]     = (fat[offset] & 0x0F) | ((value & 0x0F) << 4);
        fat[offset + 1] = (value >> 4) & 0xFF;
    }
    else
    {
        /* even cluster: write low byte, write low nibble of next byte */
        fat[offset]     = value & 0xFF;
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F);
    }
}

static uint16_t find_free_cluster(const uint8_t* fat, BootSector bs)
{
    uint32_t total_sectors = bs.bpb.total_sectors_16
        ? bs.bpb.total_sectors_16
        : bs.bpb.total_sectors_32;
    uint32_t data_sectors = total_sectors - first_data_sector(bs);
    uint16_t total_clusters =
        data_sectors / bs.bpb.sectors_per_cluster;

    for (uint16_t i = 2; i < total_clusters + 2; i++)
    {
        if (read_fat12_entry(fat, i) == 0x000)
            return i;
    }

    return 0;
}

static uint16_t allocate_chain(
    uint8_t* fat,
    BootSector bs,
    uint32_t needed_bytes,
    uint32_t* out_chain_bytes
)
{
    uint32_t cluster_bytes = bs.bpb.sectors_per_cluster *
                             bs.bpb.bytes_per_sector;
    uint32_t needed_clusters =
        (needed_bytes + cluster_bytes - 1) / cluster_bytes;
    if (needed_clusters == 0) return 0;

    uint16_t first = 0;
    uint16_t prev = 0;

    for (uint32_t i = 0; i < needed_clusters; i++)
    {
        uint16_t curr = find_free_cluster(fat, bs);
        if (curr == 0)
        {
            *out_chain_bytes = i * cluster_bytes;
            return first;
        }

        write_fat12_entry(fat, curr, 0xFFF);

        if (i == 0)
            first = curr;
        else
            write_fat12_entry(fat, prev, curr);

        prev = curr;
    }
    *out_chain_bytes = needed_clusters * cluster_bytes;
    return first;
}

static void write_clusters(
    BlockDevice* disk,
    BootSector bs,
    const uint8_t* fat,
    uint16_t first_cluster,
    const uint8_t* data,
    uint32_t size
)
{
    uint32_t cluster_bytes = bs.bpb.sectors_per_cluster *
                             bs.bpb.bytes_per_sector;
    uint32_t remaining = size;
    uint16_t cluster = first_cluster;

    while (cluster >= 0x002 && cluster < 0xFF0 && remaining > 0)
    {
        uint32_t lba = data_cluster_to_lba(bs, cluster);
        uint32_t chunk = remaining < cluster_bytes
                       ? remaining : cluster_bytes;

        if (chunk < cluster_bytes)
        {
            uint8_t* pad = calloc(1, cluster_bytes);
            memcpy(pad, data + (size - remaining), chunk);
            block_device_write(disk, lba,
                bs.bpb.sectors_per_cluster, pad);
            free(pad);
        }
        else
        {
            block_device_write(disk, lba,
                bs.bpb.sectors_per_cluster, data + (size - remaining));
        }

        remaining -= chunk;
        cluster = read_fat12_entry(fat, cluster);
    }
}

static void fat_sync(BlockDevice* disk, BootSector bs, uint8_t* fat)
{
    for (int i = 0; i < bs.bpb.num_fats; i++)
    {
        block_device_write(
            disk,
            bs.bpb.reserved_sector_count + (i * bs.bpb.fat_size_16),
            bs.bpb.fat_size_16,
            fat
        );
    }
}

static DirectoryEntry* load_parent_directory(
    BlockDevice* disk,
    BootSector bs,
    uint8_t* fat,
    const char* parent_path,
    uint32_t* out_count,
    uint16_t* out_first_cluster
)
{
    if (parent_path[0] == '/' && parent_path[1] == '\0')
    {
        *out_first_cluster = ROOT_DIR_CLUSTER;
        return fat12_root_directory(disk, bs, out_count);
    }

    DirectoryEntry resolved;
    if (fat12_resolve_path(disk, parent_path, 1, &resolved) != 0)
        return NULL;

    *out_first_cluster = resolved.first_cluster;

    return read_directory_entries(disk, bs, fat, *out_first_cluster, out_count);
}

static int find_free_slot(DirectoryEntry* entries, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if (entries[i].name[0] == 0x00 ||
            (unsigned char)entries[i].name[0] == 0xE5)
        {
            return i;
        }
    }
    return -1;
}

static void set_entry_name(DirectoryEntry* entry, const char* name)
{
    char name_copy[64];
    strncpy(name_copy, name, sizeof(name_copy) - 1);
    name_copy[sizeof(name_copy) - 1] = '\0';

    char* dot = strchr(name_copy, '.');
    if (dot)
    {
        *dot = '\0';
        strncpy(entry->ext, dot + 1, 3);
    }

    strncpy(entry->name, name_copy, 8);

    for (int i = 0; i < 8; i++)
        if (entry->name[i] == '\0')
            entry->name[i] = ' ';
    for (int i = 0; i < 3; i++)
        if (entry->ext[i] == '\0')
            entry->ext[i] = ' ';
}

static int extend_directory_chain(
    BlockDevice* disk,
    BootSector bs,
    uint8_t* fat,
    uint16_t parent_first_cluster
)
{
    uint16_t cluster = parent_first_cluster;
    while (cluster >= 0x002 && cluster < 0xFF0)
    {
        uint16_t next = read_fat12_entry(fat, cluster);
        if (next >= 0xFF0) break;
        cluster = next;
    }

    uint16_t new_cluster = find_free_cluster(fat, bs);
    if (new_cluster == 0) return -1;

    write_fat12_entry(fat, cluster, new_cluster);
    write_fat12_entry(fat, new_cluster, 0xFFF);

    uint32_t cluster_bytes =
        bs.bpb.sectors_per_cluster *
        bs.bpb.bytes_per_sector;
    uint32_t lba = data_cluster_to_lba(bs, new_cluster);
    uint8_t* zb = calloc(1, cluster_bytes);
    block_device_write(disk, lba,
        bs.bpb.sectors_per_cluster, zb);
    free(zb);

    return 0;
}

static int ensure_directory_space(
    BlockDevice* disk,
    BootSector bs,
    uint8_t* fat,
    uint16_t parent_first_cluster,
    DirectoryEntry** entries,
    uint32_t* count
)
{
    int slot = find_free_slot(*entries, *count);
    if (slot >= 0) return slot;

    if (parent_first_cluster == ROOT_DIR_CLUSTER) return -1;

    if (extend_directory_chain(disk, bs, fat,
            parent_first_cluster) != 0)
        return -1;

    free(*entries);
    *entries = read_directory_entries(
        disk, bs, fat, parent_first_cluster, count);
    if (*entries == NULL) return -1;

    return find_free_slot(*entries, *count);
}

static void write_directory(
    BlockDevice* disk,
    BootSector bs,
    const uint8_t* fat,
    uint16_t parent_cluster,
    DirectoryEntry* entries,
    uint32_t count
)
{
    if (parent_cluster == ROOT_DIR_CLUSTER)
    {
        uint32_t lba = calculate_root_dir_lba(bs);
        block_device_write(disk, lba, root_dir_sector_count(bs), entries);
    }
    else
    {
        write_clusters(disk, bs, fat, parent_cluster, (uint8_t*)entries, count * sizeof(DirectoryEntry));
    }
}

typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} DateTime;

static uint16_t dos_time_encode(const DateTime* dt)
{
    return (dt->hour << 11) | (dt->minute << 5) | (dt->second / 2);
}

static uint16_t dos_date_encode(const DateTime* dt)
{
    return ((dt->year - 1980) << 9) | (dt->month << 5) | dt->day;
}

static void set_file_timestamps(DirectoryEntry* entry)
{
    DateTime dt = { .year = 2024, .month = 1, .day = 1,
                    .hour = 12, .minute = 0, .second = 0 };
    entry->create_date      = dos_date_encode(&dt);
    entry->create_time      = dos_time_encode(&dt);
    entry->last_write_date  = dos_date_encode(&dt);
    entry->last_write_time  = dos_time_encode(&dt);
    entry->last_access_date = dos_date_encode(&dt);
}

static int create_dir_entry(
    BlockDevice* disk,
    BootSector bs,
    uint8_t* fat,
    const char* parent_path,
    const char* name,
    uint16_t first_cluster,
    uint32_t file_size
)
{
    uint32_t count;
    uint16_t parent_first_cluster;
    DirectoryEntry* entries = load_parent_directory(
        disk, bs, fat, parent_path,
        &count, &parent_first_cluster);
    if (entries == NULL) return -1;

    int slot = ensure_directory_space(
        disk, bs, fat, parent_first_cluster,
        &entries, &count);
    if (slot < 0)
    {
        free(entries);
        return -1;
    }

    DirectoryEntry* entry = &entries[slot];
    memset(entry, 0, sizeof(DirectoryEntry));

    set_entry_name(entry, name);

    entry->attr = FAT12_ATTR_ARCHIVE;
    entry->first_cluster = first_cluster;
    entry->file_size = file_size;
    set_file_timestamps(entry);

    write_directory(disk, bs, fat,
        parent_first_cluster, entries, count);

    free(entries);
    return 0;
}

static int split_parent_name(
    const char* path,
    char* parent,
    int parent_size,
    char* name,
    int name_size
)
{
    const char* slash = strrchr(path, '/');
    if (slash == NULL) return -1;

    int parent_len = slash - path;
    if (parent_len >= parent_size) return -1;

    if (parent_len == 0)
    {
        strncpy(parent, "/", parent_size);
    }
    else
    {
        strncpy(parent, path, parent_size - 1);
        parent[parent_len] = '\0';
    }

    const char* name_start = slash + 1;
    if (strlen(name_start) >= (size_t)name_size) return -1;
    strncpy(name, name_start, name_size - 1);
    name[name_size - 1] = '\0';
    str_upper(name);

    return 0;
}

uint32_t fat12_write(File* file, const void* buffer, uint32_t size)
{
    if (file->mode != 'w') return 0;

    uint8_t* new_data = (uint8_t*)realloc(
        file->data, file->size + size);
    if (new_data == NULL) return 0;

    memcpy(new_data + file->size, buffer, size);
    file->data = new_data;
    file->size += size;
    file->position = file->size;

    return size;
}

static int create_directory_contents(
    BlockDevice* disk,
    BootSector bs,
    const uint8_t* fat,
    uint16_t cluster,
    uint16_t parent_cluster
)
{
    uint32_t cluster_bytes =
        bs.bpb.sectors_per_cluster *
        bs.bpb.bytes_per_sector;

    uint8_t* buf = calloc(1, cluster_bytes);
    if (buf == NULL) return -1;

    DirectoryEntry* d = (DirectoryEntry*)buf;

    memcpy(d[0].name, ".          ", 11);
    d[0].attr = FAT12_ATTR_DIRECTORY;
    d[0].first_cluster = cluster;

    memcpy(d[1].name, "..         ", 11);
    d[1].attr = FAT12_ATTR_DIRECTORY;
    d[1].first_cluster = parent_cluster;

    write_clusters(disk, bs, fat, cluster, buf, cluster_bytes);
    free(buf);
    return 0;
}

int fat12_mkdir(BlockDevice* disk, const char* path)
{
    /* --- Check target does not exist --- */
    DirectoryEntry resolved;

    if (fat12_resolve_path(disk, path, 0, &resolved) == 0)
        return -1;
    if (fat12_resolve_path(disk, path, 1, &resolved) == 0)
        return -1;

    /* --- Read BPB and load FAT --- */
    BootSector bs = fat12_read_boot_sector(disk);

    uint8_t* fat = fat_load(disk, bs);
    if (fat == NULL) return -1;

    /* --- Split path into parent path and leaf name --- */
    char parent_path[256];
    char dirname[64];

    if (split_parent_name(path, parent_path, sizeof(parent_path),
                          dirname, sizeof(dirname)) < 0)
    {
        free(fat);
        return -1;
    }

    /* --- Load parent directory entries --- */
    uint32_t count;
    uint16_t parent_cluster;
    DirectoryEntry* entries = load_parent_directory(
        disk, bs, fat, parent_path, &count, &parent_cluster);
    if (entries == NULL)
    {
        free(fat);
        return -1;
    }

    /* --- Find or extend a free slot in the parent --- */
    int slot = ensure_directory_space(
        disk, bs, fat, parent_cluster,
        &entries, &count);
    if (slot < 0)
    {
        free(entries);
        free(fat);
        return -1;
    }

    /* --- Allocate a cluster and mark it end-of-chain (0xFFF) --- */
    uint16_t cluster = find_free_cluster(fat, bs);
    if (cluster == 0)
    {
        free(entries);
        free(fat);
        return -1;
    }
    write_fat12_entry(fat, cluster, 0xFFF);

    /* --- Stamp . and .. into the new cluster --- */
    if (create_directory_contents(
            disk, bs, fat, cluster, parent_cluster) != 0)
    {
        free(entries);
        free(fat);
        return -1;
    }

    /* --- Fill the directory entry in the parent --- */
    DirectoryEntry* entry = &entries[slot];
    memset(entry, 0, sizeof(DirectoryEntry));

    set_entry_name(entry, dirname);

    entry->attr = FAT12_ATTR_DIRECTORY;
    entry->first_cluster = cluster;
    entry->file_size = 0;

    set_file_timestamps(entry);

    /* --- Write parent directory + flush FAT --- */
    write_directory(disk, bs, fat,
        parent_cluster, entries, count);

    fat_sync(disk, bs, fat);

    /* --- Cleanup --- */
    free(entries);
    free(fat);
    return 0;
}

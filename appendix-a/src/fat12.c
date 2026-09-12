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

#define ROOT_DIR_CLUSTER 0

/* Directory entry indices — guaranteed by the FAT spec */
#define SELF_ENTRY    0
#define PARENT_ENTRY  1

struct FAT12FS {
    BlockDevice* device;
    BootSector bs;
    uint32_t fat_lba;
    uint32_t fat_sectors;
    uint32_t root_dir_lba;
    uint32_t root_dir_sectors;
    uint32_t first_data_lba;
    uint8_t* fat;
    uint32_t data_cluster_count;
};

/* ---- Internal helpers (defined before use so no forward declarations) ---- */

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

static void decode_8_3_name(const DirectoryEntry* raw, char* out)
{
    int i = 0;

    if (raw->name[0] == 0x05)
        out[i++] = (char)NAME_DELETED;

    for (; i < 8 && raw->name[i] != ' '; i++)
        out[i] = raw->name[i];

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

        if (raw->name[0] == NAME_END) return 0;

        (*offset)++;

        if (is_deleted_entry(raw)) continue;
        if (raw->attr == FAT12_ATTR_LONG_NAME) continue;

        *out = raw;
        return 1;
    }

    return 0;
}

/* FAT entry special values */
#define CLUSTER_FIRST             0x002

static uint32_t data_cluster_to_lba(FAT12FS* fs, uint16_t cluster)
{
    return ((cluster - CLUSTER_FIRST) * fs->bs.bpb.sectors_per_cluster)
          + fs->first_data_lba;
}

static uint16_t find_next_cluster(
    FAT12FS* fs,
    uint16_t cluster
)
{
    uint32_t offset = cluster + (cluster / 2);

    if (cluster & 1)
    {
        return
            ((fs->fat[offset + 1] << 8) |
              fs->fat[offset]) >> 4;
    }
    else
    {
        return
            ((fs->fat[offset + 1] << 8) |
              fs->fat[offset]) & 0x0FFF;
    }
}

static void str_upper(char* s)
{
    for (int i = 0; s[i] != '\0'; i++)
    {
        if (s[i] >= 'a' && s[i] <= 'z')
            s[i] -= 32;
    }
}

static void set_next_cluster(
    uint8_t* fat,
    uint16_t cluster,
    uint16_t value
)
{
    uint32_t offset = cluster + (cluster / 2);

    if (cluster & 1)
    {
        fat[offset]     = (fat[offset] & 0x0F) | ((value & 0x0F) << 4);
        fat[offset + 1] = (value >> 4) & 0xFF;
    }
    else
    {
        fat[offset]     = value & 0xFF;
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F);
    }
}

static uint16_t time_encode(const Timestamp* dt)
{
    return (dt->hours << 11) | (dt->minutes << 5) | (dt->seconds / 2);
}

static uint16_t date_encode(const Timestamp* dt)
{
    return ((dt->year - 1980) << 9) | (dt->month << 5) | dt->day;
}

static int find_free_entry(DirectoryEntry* entries, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if (entries[i].name[0] == NAME_END ||
            is_deleted_entry(&entries[i]))
        {
            return i;
        }
    }
    return -1;
}

static void encode_8_3_name(DirectoryEntry* entry, const char* name)
{
    char name_copy[64];
    strncpy(name_copy, name, sizeof(name_copy) - 1);
    name_copy[sizeof(name_copy) - 1] = '\0';

    memset(entry->name, 0, 8);
    memset(entry->ext, 0, 3);

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

    /* 0x05 escape: if the first character is NAME_DELETED (0xE5),
       store 0x05 instead so it is not mistaken for a deleted entry on disk */
    if (is_deleted_entry(entry))
        entry->name[0] = 0x05;
}

static DirectoryEntry* read_root_directory(
    FAT12FS* fs,
    uint32_t* count
)
{
    uint32_t bytes = fs->root_dir_sectors * fs->bs.bpb.bytes_per_sector;
    *count = bytes / sizeof(DirectoryEntry);

    DirectoryEntry* entries =
        (DirectoryEntry*)malloc(bytes);

    block_device_read(fs->device, fs->root_dir_lba, fs->root_dir_sectors, entries);

    return entries;
}

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

    while (cluster >= CLUSTER_FIRST && (uint32_t)(cluster - CLUSTER_FIRST) < fs->data_cluster_count)
    {
        uint32_t lba = data_cluster_to_lba(fs, cluster);
        DBG_PRINT("[ fat12        ] read cluster %u\n", cluster);

        uint8_t* buf = (uint8_t*)malloc(cluster_bytes);
        block_device_read(fs->device, lba, fs->bs.bpb.sectors_per_cluster, buf);

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

        cluster = find_next_cluster(fs, cluster);
    }

    *out_bytes = max_bytes;
    return all;
}

static DirectoryEntry* read_directory_entries(
    FAT12FS* fs,
    uint16_t first_cluster,
    uint32_t* count
)
{
    uint32_t bytes;
    uint8_t* raw = read_cluster_chain(
        fs, first_cluster, &bytes);

    if (raw == NULL)
    {
        *count = 0;
        return NULL;
    }

    *count = bytes / sizeof(DirectoryEntry);
    return (DirectoryEntry*)raw;
}

#define CLUSTER_FREE              0x000

#define CLUSTER_END               0xFFF

static uint16_t allocate_cluster_chain(
    FAT12FS* fs,
    uint32_t needed_bytes,
    uint32_t* out_chain_bytes
)
{
    uint32_t cluster_bytes = fs->bs.bpb.sectors_per_cluster * fs->bs.bpb.bytes_per_sector;
    uint32_t needed_clusters = (needed_bytes + cluster_bytes - 1) / cluster_bytes;

    DBG_PRINT("[ fat12        ] allocating %u cluster(s) for %u bytes\n", needed_clusters, needed_bytes);

    uint16_t* clusters = (uint16_t*)malloc(needed_clusters * sizeof(uint16_t));

    /* Phase 1: scan the FAT and collect free cluster numbers. */
    uint32_t found = 0;
    for (uint16_t c = CLUSTER_FIRST; (uint32_t)(c - CLUSTER_FIRST) < fs->data_cluster_count && found < needed_clusters; c++)
    {
        if (find_next_cluster(fs, c) == CLUSTER_FREE)
            clusters[found++] = c;
    }

    /* Not enough free clusters — nothing touched the FAT. */
    if (found < needed_clusters)
    {
        free(clusters);
        *out_chain_bytes = 0;
        return 0;
    }

    /* Phase 2: link the collected clusters into a chain. */
    for (uint32_t i = 0; i < needed_clusters; i++)
    {
        uint16_t next = (i == needed_clusters - 1) ? CLUSTER_END : clusters[i + 1];
        DBG_PRINT("[ fat12        ] allocate cluster %u%s\n", clusters[i], i == 0 ? " (first)" : "");
        set_next_cluster(fs->fat, clusters[i], next);
    }

    *out_chain_bytes = needed_clusters * cluster_bytes;
    uint16_t first = clusters[0];
    free(clusters);

    return first;
}

static void write_cluster_chain(
    FAT12FS* fs,
    uint16_t first_cluster,
    const uint8_t* data,
    uint32_t size
)
{
    uint32_t cluster_bytes = fs->bs.bpb.sectors_per_cluster * fs->bs.bpb.bytes_per_sector;
    uint32_t remaining = size;
    uint16_t cluster = first_cluster;

    while (cluster >= CLUSTER_FIRST && (uint32_t)(cluster - CLUSTER_FIRST) < fs->data_cluster_count && remaining > 0)
    {
        uint32_t lba = data_cluster_to_lba(fs, cluster);
        DBG_PRINT("[ fat12        ] write cluster %u\n", cluster);
        uint32_t chunk = remaining < cluster_bytes ? remaining : cluster_bytes;

        uint8_t* buf = (uint8_t*)calloc(1, cluster_bytes);
        memcpy(buf, data + (size - remaining), chunk);
        block_device_write(fs->device, lba, fs->bs.bpb.sectors_per_cluster, buf);
        free(buf);

        remaining -= chunk;
        cluster = find_next_cluster(fs, cluster);
    }
}

static void flush_fats(FAT12FS* fs)
{
    for (int i = 0; i < fs->bs.bpb.num_fats; i++)
    {
        block_device_write(
            fs->device,
            fs->fat_lba + (i * fs->bs.bpb.fat_size_16),
            fs->bs.bpb.fat_size_16,
            fs->fat
        );
    }
}

static void set_entry_timestamps(DirectoryEntry* entry)
{
    Timestamp dt = { .year = 2024, .month = 1, .day = 1,
                     .hours = 12, .minutes = 0, .seconds = 0 };
    entry->create_date      = date_encode(&dt);
    entry->create_time      = time_encode(&dt);
    entry->last_write_date  = date_encode(&dt);
    entry->last_write_time  = time_encode(&dt);
    entry->last_access_date = date_encode(&dt);
}

static DirectoryEntry* find_entry_by_name(
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

        if (strcmp(entry_name, name) == 0)
            return raw;
    }
    return NULL;
}

static void write_directory(
    FAT12FS* fs,
    uint16_t parent_cluster,
    DirectoryEntry* entries,
    uint32_t count
)
{
    if (parent_cluster == ROOT_DIR_CLUSTER)
    {
        block_device_write(
            fs->device, fs->root_dir_lba,
            fs->root_dir_sectors, entries);
    }
    else
    {
        write_cluster_chain(fs, parent_cluster,
            (uint8_t*)entries,
            count * sizeof(DirectoryEntry));
    }
}

static int resolve_path(
    FAT12FS* fs,
    const char* path,
    DirectoryEntry* out
)
{
    /* All paths must be absolute, starting with "/" (root); the slash is removed after the check */
    if (path[0] != '/') return -1;
    const char* p = path + 1;

    /* Return the Root Directory if the path contains only "/" */
    if (*p == '\0')
    {
        out->attr = FAT12_ATTR_DIRECTORY;
        out->first_cluster = ROOT_DIR_CLUSTER;
        return 0;
    }

    /* ---- Step 1: Load Root Directory as Current Directory ---- */
    uint32_t dir_count;
    DirectoryEntry* dir_entries = read_root_directory(fs, &dir_count);

    while (1)
    {
        /* ---- Step 2: Extract next component from path string ---- */
        char name[13];
        int i = 0;
        while (*p && *p != '/' && i < 12)
        {
            name[i++] = *p++;
        }
        name[i] = '\0';
        str_upper(name);

        /* ---- Step 3: Found in Current Directory? ---- */
        DirectoryEntry* found = find_entry_by_name(dir_entries, dir_count, name);

        if (found == NULL)
        {
            /* Error: Not Found */
            free(dir_entries);
            return -1;
        }

        /* ---- Step 4: Is last component? ---- */
        int is_last = (*p == '\0');
        if (*p == '/') p++;
        if (is_last)
        {
            /* Step 7: Return entry */
            *out = *found;
            free(dir_entries);
            return 0;
        }
        else
        {
            /* ---- Step 5: Is entry a directory? ---- */
            if (!(found->attr & FAT12_ATTR_DIRECTORY))
            {
                /* Error: Not Found */
                free(dir_entries);
                return -1;
            }

            /* Step 6: Load subdirectory as Current Directory */
            uint32_t sub_count;
            DirectoryEntry* sub_entries = read_directory_entries(fs, found->first_cluster, &sub_count);
            free(dir_entries);
            dir_entries = sub_entries;
            dir_count = sub_count;
        }
    }
}

static DirectoryEntry* read_directory(
    FAT12FS* fs,
    uint16_t cluster,
    uint32_t* out_count
)
{
    if (cluster == ROOT_DIR_CLUSTER)
    {
        return read_root_directory(fs, out_count);
    }
    /* --- NEW --- */
    return read_directory_entries(fs, cluster, out_count);
}

static void free_cluster_chain(FAT12FS* fs, uint16_t cluster)
{
    while (cluster >= CLUSTER_FIRST && (uint32_t)(cluster - CLUSTER_FIRST) < fs->data_cluster_count)
    {
        uint16_t next = find_next_cluster(fs, cluster);
        DBG_PRINT("[ fat12        ] free cluster %u\n", cluster);
        set_next_cluster(fs->fat, cluster, CLUSTER_FREE);
        cluster = next;
    }
}

static int is_dot_entry(const char* name)
{
    return strcmp(name, ".") == 0 ||
           strcmp(name, "..") == 0;
}

static void mark_entry_deleted(DirectoryEntry* entry)
{
    entry->name[0] = (char)NAME_DELETED;
}

static int is_directory_empty(FAT12FS* fs, uint16_t cluster)
{
    uint32_t count;
    DirectoryEntry* entries = read_directory_entries(fs, cluster, &count);

    int result = 1;
    uint32_t offset = 0;
    DirectoryEntry* entry;
    while (next_active_entry(entries, count, &offset, &entry))
    {
        char entry_name[13];
        decode_8_3_name(entry, entry_name);
        if (is_dot_entry(entry_name))
            continue;

        result = 0;
        break;
    }

    free(entries);
    return result;
}

static int get_parent_and_name(
    const char* path,
    char* out_parent,
    char* out_name
)
{
    /* Only absolute paths are supported: every path must start at "/" */
    if (path[0] != '/') return -1;

    const char* slash = strrchr(path, '/');
    int parent_len = slash - path;

    int copy_len = parent_len == 0 ? 1 : parent_len;
    memcpy(out_parent, path, copy_len);
    out_parent[copy_len] = '\0';

    strcpy(out_name, slash + 1);
    str_upper(out_name);

    return 0;
}

static int create_dir_entry(
    FAT12FS* fs,
    const char* path,
    int* out_slot,
    uint16_t* out_parent_cluster
)
{
    char parent_dir[256];
    char name[13];

    if (get_parent_and_name(path, parent_dir, name) != 0)
    {
        return -1;
    }

    DirectoryEntry resolved;
    if (resolve_path(fs, parent_dir, &resolved) != 0)
        return -1;
    *out_parent_cluster = resolved.first_cluster;

    uint32_t count;
    DirectoryEntry* entries = read_directory(fs, *out_parent_cluster, &count);
    if (entries == NULL) return -1;

    if (find_entry_by_name(entries, count, name) != NULL)
    {
        free(entries);
        return -1;
    }

    int slot = find_free_entry(entries, count);
    if (slot < 0)
    {
        free(entries);
        return -1;
    }

    DirectoryEntry* entry = &entries[slot];
    memset(entry, 0, sizeof(DirectoryEntry));

    encode_8_3_name(entry, name);
    entry->attr = FAT12_ATTR_ARCHIVE;
    set_entry_timestamps(entry);

    write_directory(fs,
        *out_parent_cluster, entries, count);

    free(entries);
    *out_slot = slot;
    return 0;
}

static int update_dir_entry(
    FAT12FS* fs,
    uint16_t parent_cluster,
    int slot,
    uint16_t first_cluster,
    uint32_t file_size
)
{
    uint32_t count;
    DirectoryEntry* entries = read_directory(fs, parent_cluster, &count);
    if (entries == NULL) return -1;

    entries[slot].first_cluster = first_cluster;
    entries[slot].file_size = file_size;

    write_directory(fs, parent_cluster, entries, count);

    free(entries);
    return 0;
}

static void initialize_directory_data(
    FAT12FS* fs,
    uint16_t first_cluster,
    uint16_t parent_first_cluster
)
{
    uint32_t cluster_bytes =
        fs->bs.bpb.sectors_per_cluster *
        fs->bs.bpb.bytes_per_sector;

    uint8_t* buf = calloc(1, cluster_bytes);

    DirectoryEntry* entries = (DirectoryEntry*)buf;

    /* ---- Step 6: Stamp . and .. into the buffer, then write it to disk ---- */
    memcpy(entries[SELF_ENTRY].name, ".          ", 11);
    entries[SELF_ENTRY].attr = FAT12_ATTR_DIRECTORY;
    entries[SELF_ENTRY].first_cluster = first_cluster;
    set_entry_timestamps(&entries[SELF_ENTRY]);

    memcpy(entries[PARENT_ENTRY].name, "..         ", 11);
    entries[PARENT_ENTRY].attr = FAT12_ATTR_DIRECTORY;
    entries[PARENT_ENTRY].first_cluster = parent_first_cluster;
    set_entry_timestamps(&entries[PARENT_ENTRY]);

    write_cluster_chain(fs, first_cluster, buf, cluster_bytes);
    free(buf);
}

/* ---- Format helpers ---- */

static uint16_t compute_sectors_per_fat(
    uint32_t total_sectors,
    uint16_t reserved_sector_count,
    uint8_t num_fats,
    uint16_t root_dir_sectors,
    uint8_t sectors_per_cluster,
    uint32_t bytes_per_sector
)
{
    uint16_t sectors_per_fat;

    for (sectors_per_fat = 1; sectors_per_fat < 255; sectors_per_fat++)
    {
        uint32_t data_sectors =
            total_sectors - reserved_sector_count - (num_fats * sectors_per_fat) - root_dir_sectors;
        uint32_t total_clusters =
            data_sectors / sectors_per_cluster;
        uint32_t fat_capacity =
            (bytes_per_sector * 8 * sectors_per_fat) / 12;

        if (total_clusters + CLUSTER_FIRST <= fat_capacity)
            break;
    }

    return sectors_per_fat;
}

static int format_boot_sector(
    BlockDevice* device,
    const char volume_label[12],
    BootSector* bs
)
{
    /* Start from an empty boot sector, filled with zeros */
    memset(bs, 0, sizeof(*bs));

    /* Ask the device for its own geometry */
    uint32_t total_sectors = (uint32_t)block_device_sector_count(device);
    uint32_t bytes_per_sector = block_device_sector_size(device);

    /* Fixed BPB fields — the book's one-disk convention — plus the one
       field that has to be computed: how many sectors the FAT needs */
    bs->bpb.bytes_per_sector = bytes_per_sector;
    bs->bpb.sectors_per_cluster = 2;  /* Hard-coded — matches the book's convention */
    bs->bpb.root_entry_count = 512;
    bs->bpb.reserved_sector_count = 1;
    bs->bpb.num_fats = 2;

    uint32_t root_dir_sectors = ((bs->bpb.root_entry_count * sizeof(DirectoryEntry)) + (bytes_per_sector - 1)) / bytes_per_sector;

    /* Bail out before computing the FAT size (or writing anything) if the
       disk can't even fit the reserved region, one sector-sized FAT per
       copy, and the root directory */
    if (total_sectors < bs->bpb.reserved_sector_count + bs->bpb.num_fats + root_dir_sectors)
        return -1;

    bs->bpb.fat_size_16 = compute_sectors_per_fat(
        total_sectors, bs->bpb.reserved_sector_count, bs->bpb.num_fats, root_dir_sectors, bs->bpb.sectors_per_cluster, bytes_per_sector);

    /* Only one of the two total-sector fields is ever valid — mount reads
       total_sectors_16 first and falls back to total_sectors_32 when it's
       zero. The unused field already reads 0 from the memset above. */
    if (total_sectors < 65536)
        bs->bpb.total_sectors_16 = (uint16_t)total_sectors;
    else
        bs->bpb.total_sectors_32 = total_sectors;

    /* Legacy/reference fields the library itself never reads — see
       Appendix C: The Boot Sector's Leftovers for what each one meant */
    memcpy(bs->oem_name, "FAT12LIB", 8);
    bs->bpb.media = 0xF8; /* Fixed disk */
    bs->bpb.sectors_per_track = 32;
    bs->bpb.number_of_heads = 2;
    bs->bpb.hidden_sectors = 0;
    bs->extended_bpb.drive_number = 0x80;
    bs->extended_bpb.volume_id = 0x07E80101;
    memcpy(bs->extended_bpb.file_system_type, "FAT12   ", 8);

    /* Extended boot signature — required for the volume label and volume
       ID fields to be considered valid */
    bs->extended_bpb.boot_signature = 0x29;

    /* Set the volume label — blank the field with spaces, then copy the
       caller's name over it, truncated to the field's 11-byte width */
    size_t label_len = strlen(volume_label);
    if (label_len > 11) label_len = 11;
    memset(bs->extended_bpb.volume_label, ' ', 11);
    memcpy(bs->extended_bpb.volume_label, volume_label, label_len);

    /* Write the boot sector to disk */
    block_device_write(device, 0, 1, bs);

    return 0;
}

static void format_fats(
    BlockDevice* device,
    BootSector bs
)
{
    /* Allocate a FAT-sized buffer, zeroed — every cluster starts free */
    uint32_t fat_sectors = bs.bpb.fat_size_16;
    uint64_t fat_bytes = fat_sectors * bs.bpb.bytes_per_sector;
    uint8_t* fat = (uint8_t*)malloc(fat_bytes);
    memset(fat, 0, fat_bytes);

    /* Entries 0 and 1 are reserved: media descriptor, then end-of-chain marker */
    set_next_cluster(fat, 0, 0xF00 | bs.bpb.media);
    set_next_cluster(fat, 1, CLUSTER_END);

    /* Write the same buffer to every FAT copy */
    for (uint32_t copy = 0; copy < bs.bpb.num_fats; copy++)
    {
        uint32_t lba = bs.bpb.reserved_sector_count + (copy * fat_sectors);
        block_device_write(device, lba, fat_sectors, fat);
    }

    free(fat);
}

static void format_root_dir(BlockDevice* device, BootSector* bs)
{
    /* Locate the root directory — same formula fat12_mount uses */
    uint32_t fat_lba = bs->bpb.reserved_sector_count;
    uint32_t fat_sectors = bs->bpb.num_fats * bs->bpb.fat_size_16;
    uint32_t root_dir_lba = fat_lba + fat_sectors;

    /* Size it in whole sectors and allocate a zero-filled buffer */
    uint32_t count = ((bs->bpb.root_entry_count * sizeof(DirectoryEntry)
        + bs->bpb.bytes_per_sector - 1)
        / bs->bpb.bytes_per_sector);
    uint64_t total_bytes = count * bs->bpb.bytes_per_sector;
    uint8_t* zeros = (uint8_t*)malloc(total_bytes);
    memset(zeros, 0, total_bytes);

    /* Volume label entry — matches the BPB label */
    DirectoryEntry* label = (DirectoryEntry*)zeros;
    memcpy(label->name, bs->extended_bpb.volume_label, 11);
    label->attr = FAT12_ATTR_VOLUME_ID;

    /* Write the root directory to disk — everything after the label
       stays zeroed, so readdir stops there */
    block_device_write(device, root_dir_lba, count, zeros);
    free(zeros);
}

static uint32_t total_data_clusters(FAT12FS* fs)
{
    uint32_t total_sectors = fs->bs.bpb.total_sectors_16
        ? fs->bs.bpb.total_sectors_16
        : fs->bs.bpb.total_sectors_32;

    uint32_t data_sectors = total_sectors - fs->first_data_lba;

    return data_sectors / fs->bs.bpb.sectors_per_cluster;
}

/* ---- Public API ---- */

FAT12FS* fat12_mount(BlockDevice* device)
{
    FAT12FS* fs = (FAT12FS*)malloc(sizeof(FAT12FS));
    fs->device = device;
    fs->bs = read_boot_sector(device);

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

    fs->fat_lba = fs->bs.bpb.reserved_sector_count;
    fs->fat_sectors = fs->bs.bpb.num_fats * fs->bs.bpb.fat_size_16;
    fs->root_dir_lba = fs->fat_lba + fs->fat_sectors;
    fs->root_dir_sectors = ((fs->bs.bpb.root_entry_count * sizeof(DirectoryEntry)
                           + fs->bs.bpb.bytes_per_sector - 1)
                          / fs->bs.bpb.bytes_per_sector);
    fs->first_data_lba = fs->root_dir_lba + fs->root_dir_sectors;
    fs->data_cluster_count = total_data_clusters(fs);

    DBG_PRINT("[ fat12        ] FAT start LBA: %u\n", fs->fat_lba);
    DBG_PRINT("[ fat12        ] FAT size (sectors): %u\n", fs->fat_sectors);
    DBG_PRINT("[ fat12        ] Root dir start LBA: %u\n", fs->root_dir_lba);
    DBG_PRINT("[ fat12        ] Root dir size (sectors): %u\n", fs->root_dir_sectors);
    DBG_PRINT("[ fat12        ] First data LBA: %u\n", fs->first_data_lba);
    DBG_PRINT("[ fat12        ] Data cluster count: %u\n", fs->data_cluster_count);

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

    uint32_t count;
    DirectoryEntry* entries = read_directory(fs, resolved.first_cluster, &count);

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
    if (!next_active_entry(dir->entries, dir->count, &dir->offset, &raw))
        return -1;

    decode_8_3_name(raw, out->name);

    out->size = raw->file_size;
    out->attr = raw->attr;
    out->create_time = decode_timestamp(
        raw->create_time, raw->create_date);
    out->modify_time = decode_timestamp(
        raw->last_write_time, raw->last_write_date);

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
    char mode;
    /* --- NEW --- */
    uint16_t dir_cluster;
    int dir_slot;
};

File* fat12_open(FAT12FS* fs, const char* path, char mode)
{
    /* Only absolute paths are supported: every path must start at "/" */
    if (path[0] != '/') return NULL;

    if (mode == 'w')
    {
        int slot;
        /* --- NEW --- */
        uint16_t parent_cluster;
        if (create_dir_entry(fs, path, &slot, &parent_cluster) != 0) return NULL;
        /* --- END NEW --- */

        File* file = (File*)malloc(sizeof(File));
        file->fs = fs;
        file->data = NULL;
        file->size = 0;
        file->position = 0;
        file->mode = 'w';
        /* --- NEW --- */
        file->dir_cluster = parent_cluster;
        /* --- END NEW --- */
        file->dir_slot = slot;
        return file;
    }

    if (mode == 'r')
    {
        DirectoryEntry resolved;
        if (resolve_path(fs, path, &resolved) != 0 ||
            (resolved.attr & FAT12_ATTR_DIRECTORY))
        {
            return NULL;
        }

        uint32_t chain_bytes;
        uint8_t* data = NULL;

        if (resolved.file_size > 0)
        {
            data = read_cluster_chain(
                fs, resolved.first_cluster, &chain_bytes);
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
        file->mode = 'r';
        return file;
    }

    return NULL;
}

uint32_t fat12_read(File* file, void* buffer, uint32_t size)
{
    if (file->mode != 'r') return 0;
    if (file->position >= file->size) return 0;

    uint32_t remaining = file->size - file->position;
    if (size < remaining) remaining = size;

    memcpy(buffer, file->data + file->position, remaining);
    file->position += remaining;

    return remaining;
}

uint32_t fat12_write(File* file, const void* buffer, uint32_t size)
{
    if (file->mode != 'w') return 0;

    uint8_t* new_data = (uint8_t*)realloc(
        file->data, file->size + size);
    memcpy(new_data + file->size, buffer, size);
    file->data = new_data;
    file->size += size;
    file->position = file->size;

    return size;
}

int fat12_close(File* file)
{
    int result = 0;

    if (file->mode == 'w')
    {
        if (file->size > 0)
        {
            uint32_t chain_bytes;
            uint16_t first = allocate_cluster_chain(
                file->fs, file->size, &chain_bytes);

            if (first != 0)
            {
                write_cluster_chain(file->fs,
                    first, file->data, file->size);

                flush_fats(file->fs);

                /* --- NEW --- */
                update_dir_entry(file->fs, file->dir_cluster,
                    file->dir_slot, first, file->size);
                /* --- END NEW --- */
            }
            else
            {
                result = -1;
            }
        }
    }

    free(file->data);
    free(file);
    return result;
}

int fat12_mkdir(FAT12FS* fs, const char* path)
{
    /* ---- Step 1: Split path into parent path and leaf name ---- */
    char parent_path[256];
    char dirname[64];

    if (get_parent_and_name(path, parent_path, dirname) < 0)
    {
        return -1;
    }

    /* ---- Step 2: Load parent directory entries ---- */
    DirectoryEntry parent;
    if (resolve_path(fs, parent_path, &parent) != 0)
    {
        return -1;
    }

    uint32_t count;
    DirectoryEntry* entries = read_directory(fs, parent.first_cluster, &count);
    if (entries == NULL)
    {
        return -1;
    }

    /* ---- Step 3: Check target does not already exist ---- */
    if (find_entry_by_name(entries, count, dirname) != NULL)
    {
        free(entries);
        return -1;
    }

    /* ---- Step 4: Find a free slot in the parent ---- */
    int slot = find_free_entry(entries, count);
    if (slot < 0)
    {
        free(entries);
        return -1;
    }

    /* ---- Step 5: Allocate a cluster and mark it end-of-chain ---- */
    uint32_t cluster_bytes =
        fs->bs.bpb.sectors_per_cluster *
        fs->bs.bpb.bytes_per_sector;
    uint32_t chain_bytes;
    uint16_t cluster = allocate_cluster_chain(fs, cluster_bytes, &chain_bytes);
    if (cluster == 0)
    {
        free(entries);
        return -1;
    }

    /* ---- Step 6: Build and write the new directory's contents ---- */
    initialize_directory_data(fs, cluster, parent.first_cluster);

    /* ---- Step 7: Fill the parent's entry, then commit to disk in order ---- */
    DirectoryEntry* entry = &entries[slot];
    memset(entry, 0, sizeof(DirectoryEntry));

    encode_8_3_name(entry, dirname);

    entry->attr = FAT12_ATTR_DIRECTORY;
    entry->first_cluster = cluster;
    entry->file_size = 0;

    set_entry_timestamps(entry);

    flush_fats(fs);

    write_directory(fs,
        parent.first_cluster, entries, count);

    free(entries);
    return 0;
}

int fat12_remove(FAT12FS* fs, const char* path)
{
    /* ---- Step 1: Reject root ---- */
    if (strcmp(path, "/") == 0) return -1;

    /* ---- Step 2: Split path into parent path and filename ---- */
    char parent_path[256];
    char filename[64];
    if (get_parent_and_name(path, parent_path, filename) < 0) return -1;

    /* ---- Step 3: Reject . and .. ---- */
    if (is_dot_entry(filename)) return -1;

    /* ---- Step 4: Load parent directory entries ---- */
    DirectoryEntry parent;
    if (resolve_path(fs, parent_path, &parent) != 0) return -1;

    uint32_t count;
    DirectoryEntry* entries = read_directory(fs, parent.first_cluster, &count);
    if (entries == NULL) return -1;

    /* ---- Step 5: Find the entry ---- */
    DirectoryEntry* entry = find_entry_by_name(entries, count, filename);
    if (entry == NULL)
    {
        free(entries);
        return -1;
    }

    /* ---- Step 6: Reject read-only entries ---- */
    if (entry->attr & FAT12_ATTR_READ_ONLY)
    {
        free(entries);
        return -1;
    }

    /* ---- Step 7: If it is a directory, verify it is empty ---- */
    if (entry->attr & FAT12_ATTR_DIRECTORY)
    {
        if (is_directory_empty(fs, entry->first_cluster) == 0)
        {
            free(entries);
            return -1;
        }
    }

    /* ---- Step 8: Mark the entry deleted, then write the parent ---- */
    mark_entry_deleted(entry);
    write_directory(fs, parent.first_cluster, entries, count);

    /* ---- Step 9: Free its cluster chain, then flush the FAT ---- */
    free_cluster_chain(fs, entry->first_cluster);
    flush_fats(fs);

    free(entries);
    return 0;
}

static int is_subpath(const char* parent, const char* child)
{
    char* parent_up = strdup(parent);
    char* child_up = strdup(child);
    str_upper(parent_up);
    str_upper(child_up);

    size_t parent_len = strlen(parent_up);
    int result = (strncmp(parent_up, child_up, parent_len) == 0 &&
                  child_up[parent_len] == '/');

    free(parent_up);
    free(child_up);
    return result;
}

static void update_dotdot(
    FAT12FS* fs,
    uint16_t dir_cluster,
    uint16_t new_parent_cluster
)
{
    uint32_t count;
    DirectoryEntry* entries = read_directory(fs, dir_cluster, &count);

    entries[PARENT_ENTRY].first_cluster = new_parent_cluster;

    write_directory(fs, dir_cluster, entries, count);
    free(entries);
}

int fat12_move(
    FAT12FS* fs,
    const char* old_path,
    const char* new_path
)
{
    /* --- Guard: reject root, no-op if same path --- */
    if (strcmp(old_path, "/") == 0) return -1;
    if (strcmp(old_path, new_path) == 0) return 0;

    /* --- Split both paths into parent path and leaf name --- */
    char src_parent_path[256];
    char src_name[64];
    char dst_parent_path[256];
    char dst_name[64];

    if (get_parent_and_name(old_path, src_parent_path, src_name) < 0)
    {
        return -1;
    }

    if (get_parent_and_name(new_path, dst_parent_path, dst_name) < 0)
    {
        return -1;
    }

    /* --- Guard: the dot entries cannot be moved or renamed --- */
    if (is_dot_entry(src_name) || is_dot_entry(dst_name))
    {
        return -1;
    }

    /* --- Load source parent directory --- */
    DirectoryEntry src_resolved;
    if (resolve_path(fs, src_parent_path, &src_resolved) != 0)
    {
        return -1;
    }
    uint16_t src_parent_cluster = src_resolved.first_cluster;

    uint32_t src_count;
    DirectoryEntry* src_entries = read_directory(
        fs, src_parent_cluster, &src_count);
    if (src_entries == NULL)
    {
        return -1;
    }

    /* --- Find source entry --- */
    DirectoryEntry* src_entry = find_entry_by_name(
        src_entries, src_count, src_name);
    if (src_entry == NULL)
    {
        free(src_entries);
        return -1;
    }

    /* --- Guard: read-only files cannot be renamed or moved --- */
    if (src_entry->attr & FAT12_ATTR_READ_ONLY)
    {
        free(src_entries);
        return -1;
    }

    /* --- Cycle detection: moving a directory into itself --- */
    if ((src_entry->attr & FAT12_ATTR_DIRECTORY) && is_subpath(old_path, new_path))
    {
        free(src_entries);
        return -1;
    }

    /* --- Load destination parent directory --- */
    DirectoryEntry dst_resolved;
    if (resolve_path(fs, dst_parent_path, &dst_resolved) != 0)
    {
        free(src_entries);
        return -1;
    }
    uint16_t dst_parent_cluster = dst_resolved.first_cluster;

    uint32_t dst_count;
    DirectoryEntry* dst_entries = read_directory(
        fs, dst_parent_cluster, &dst_count);
    if (dst_entries == NULL)
    {
        free(src_entries);
        return -1;
    }

    /* --- Same-parent shortcut: reuse one buffer --- */
    int same_parent =
        (src_parent_cluster == dst_parent_cluster);

    if (same_parent)
    {
        free(dst_entries);
        dst_entries = NULL;
    }

    DirectoryEntry* target_entries =
        same_parent ? src_entries : dst_entries;
    uint32_t target_count =
        same_parent ? src_count : dst_count;

    /* --- Check destination does not already exist --- */
    if (find_entry_by_name(
            target_entries, target_count, dst_name) != NULL)
    {
        free(dst_entries);
        free(src_entries);
        return -1;
    }

    /* --- Ensure space and find free slot in destination --- */
    int dst_slot = find_free_entry(target_entries, target_count);
    if (dst_slot < 0)
    {
        free(dst_entries);
        free(src_entries);
        return -1;
    }

    /* --- Copy entry, set new name --- */
    memcpy(&target_entries[dst_slot], src_entry,
           sizeof(DirectoryEntry));

    encode_8_3_name(&target_entries[dst_slot], dst_name);

    /* --- Update .. for cross-directory directory moves --- */
    if (src_entry->attr & FAT12_ATTR_DIRECTORY)
    {
        update_dotdot(fs,
            src_entry->first_cluster,
            dst_parent_cluster);
    }

    /* --- Mark source deleted, flush directories and FAT --- */
    mark_entry_deleted(src_entry);

    write_directory(fs, src_parent_cluster,
        src_entries, src_count);

    if (!same_parent)
    {
        write_directory(fs, dst_parent_cluster,
            dst_entries, dst_count);
    }

    flush_fats(fs);

    /* --- Cleanup --- */
    free(src_entries);
    free(dst_entries);
    return 0;
}

int fat12_format(BlockDevice* device, FormatParams params)
{
    /* Write the boot sector, FATs, and root directory */
    BootSector bs;
    if (format_boot_sector(device, params.volume_label, &bs) != 0) return -1;
    format_fats(device, bs);
    format_root_dir(device, &bs);

    return 0;
}

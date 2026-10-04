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

/* Directory entry indices — guaranteed by the FAT spec */
#define SELF_ENTRY    0
#define PARENT_ENTRY  1

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
    uint8_t* fat;
    uint32_t data_cluster_count;
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

    uint32_t total_sectors = fs->bs.bpb.total_sectors_16
        ? fs->bs.bpb.total_sectors_16
        : fs->bs.bpb.total_sectors_32;

    fs->data_cluster_count = (total_sectors - fs->data_lba) / fs->bs.bpb.sectors_per_cluster;

    DBG_PRINT("[ fat12        ] FAT start LBA: %u\n", fs->fat_lba);
    DBG_PRINT("[ fat12        ] FAT size (sectors): %u\n", fs->fat_sectors);
    DBG_PRINT("[ fat12        ] Root dir start LBA: %u\n", fs->root_dir_lba);
    DBG_PRINT("[ fat12        ] Root dir size (sectors): %u\n", fs->root_dir_sectors);
    DBG_PRINT("[ fat12        ] Data region start LBA: %u\n", fs->data_lba);
    DBG_PRINT("[ fat12        ] Cluster size (bytes): %u\n", fs->cluster_bytes);
    DBG_PRINT("[ fat12        ] Data cluster count: %u\n", fs->data_cluster_count);

    DBG_PRINT("[ fat12        ] load FAT (first copy)\n");
    fs->fat = (uint8_t*)malloc(fs->bs.bpb.fat_size_16 * fs->bs.bpb.bytes_per_sector);
    block_device_read(device, fs->fat_lba, fs->bs.bpb.fat_size_16, fs->fat);

    return fs;
}

void fat12_umount(FAT12FS* fs)
{
    free(fs->fat);
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

/* FAT entry special values */
#define FAT_ENTRY_FREE           0x000
#define FAT_ENTRY_END            0xFFF

static uint32_t data_cluster_to_lba(FAT12FS* fs, uint16_t cluster)
{
    return fs->data_lba + ((cluster - FIRST_DATA_CLUSTER) * fs->bs.bpb.sectors_per_cluster);
}

static uint16_t get_fat_entry(
    FAT12FS* fs,
    uint16_t cluster
)
{
    /* byte where this cluster's 12-bit entry starts (1.5 bytes per entry);
       integer division drops the .5 for odd clusters: 7 / 2 = 3 */
    uint32_t offset = cluster + (cluster / 2);

    /* read the two bytes the entry spans as one little-endian 16-bit value
       (second byte on top); this closes the gap: on-disk bytes 07 f0 become
       0xF007, the entry's three nibbles side by side and the neighbor's
       nibble at one end, so all that is left is to cut that one off */
    uint16_t value = (fs->fat[offset + 1] << 8) | fs->fat[offset];

    /* lowest bit set = odd cluster, clear = even */
    if (cluster & 1)
    {
        /* odd cluster: drop the last nibble (it belongs to the even entry) */
        return value >> 4;
    }
    else
    {
        /* even cluster: clear the first nibble (it belongs to the odd entry) */
        return value & 0x0FFF;
    }
}

static void set_fat_entry(
    uint8_t* fat,
    uint16_t cluster,
    uint16_t value
)
{
    uint32_t offset = cluster + (cluster / 2);

    if (cluster & 1)
    {
        /* odd cluster: write high nibble, write next byte */
        fat[offset] = (fat[offset] & 0x0F) | ((value & 0x0F) << 4);
        fat[offset + 1] = (value >> 4) & 0xFF;
    }
    else
    {
        /* even cluster: write low byte, write low nibble of next byte */
        fat[offset] = value & 0xFF;
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F);
    }
}

static int is_data_cluster(FAT12FS* fs, uint16_t cluster)
{
    /* real data clusters run from 2 up to the volume's last one; free (0) lies
       below that range, and since FAT12 caps a volume at 4084 data clusters
       (highest cluster 0xFF5), bad (0xFF7) and end-of-chain (0xFF8+) lie above */
    return cluster >= FIRST_DATA_CLUSTER && (uint32_t)(cluster - FIRST_DATA_CLUSTER) < fs->data_cluster_count;
}

static uint8_t* read_cluster_chain(
    FAT12FS* fs,
    uint16_t first_cluster,
    uint32_t* out_bytes
)
{
    /* first pass: count the chain's clusters, so we know how much to allocate */
    uint32_t cluster_count = 0;
    for (uint16_t cluster = first_cluster; is_data_cluster(fs, cluster); cluster = get_fat_entry(fs, cluster))
    {
        /* no chain can be longer than the volume has clusters: if this one is,
           a corrupt FAT has looped it back onto itself, so give up */
        if (cluster_count == fs->data_cluster_count)
            return NULL;

        cluster_count++;
    }

    uint8_t* data = (uint8_t*)malloc(cluster_count * fs->cluster_bytes);

    /* second pass: the first one already checked these clusters, so we just
       read all of them, in chain order, each into its slot */
    uint16_t cluster = first_cluster;
    for (uint32_t i = 0; i < cluster_count; i++)
    {
        DBG_PRINT("[ fat12        ] read cluster %u\n", cluster);
        block_device_read(fs->device, data_cluster_to_lba(fs, cluster), fs->bs.bpb.sectors_per_cluster, data + i * fs->cluster_bytes);
        cluster = get_fat_entry(fs, cluster);
    }

    *out_bytes = cluster_count * fs->cluster_bytes;
    return data;
}

static void write_cluster_chain(
    FAT12FS* fs,
    uint16_t first_cluster,
    const uint8_t* data,
    uint32_t size
)
{
    uint32_t remaining = size;
    uint16_t cluster = first_cluster;

    while (is_data_cluster(fs, cluster) && remaining > 0)
    {
        uint32_t lba = data_cluster_to_lba(fs, cluster);
        DBG_PRINT("[ fat12        ] write cluster %u\n", cluster);
        uint32_t chunk = remaining < fs->cluster_bytes ? remaining : fs->cluster_bytes;

        uint8_t* buf = (uint8_t*)calloc(1, fs->cluster_bytes);
        memcpy(buf, data + (size - remaining), chunk);
        block_device_write(fs->device, lba, fs->bs.bpb.sectors_per_cluster, buf);
        free(buf);

        remaining -= chunk;
        cluster = get_fat_entry(fs, cluster);
    }
}

static uint16_t allocate_cluster_chain(
    FAT12FS* fs,
    uint32_t needed_bytes,
    uint32_t* out_chain_bytes
)
{
    uint32_t needed_clusters = (needed_bytes + fs->cluster_bytes - 1) / fs->cluster_bytes;

    DBG_PRINT("[ fat12        ] allocating %u cluster(s) for %u bytes\n", needed_clusters, needed_bytes);

    uint16_t* clusters = (uint16_t*)malloc(needed_clusters * sizeof(uint16_t));

    /* Phase 1: scan the FAT and collect free cluster numbers. */
    uint32_t found = 0;
    for (uint16_t c = FIRST_DATA_CLUSTER; (uint32_t)(c - FIRST_DATA_CLUSTER) < fs->data_cluster_count && found < needed_clusters; c++)
    {
        if (get_fat_entry(fs, c) == FAT_ENTRY_FREE)
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
        uint16_t next = (i == needed_clusters - 1) ? FAT_ENTRY_END : clusters[i + 1];
        DBG_PRINT("[ fat12        ] allocate cluster %u%s\n", clusters[i], i == 0 ? " (first)" : "");
        set_fat_entry(fs->fat, clusters[i], next);
    }

    *out_chain_bytes = needed_clusters * fs->cluster_bytes;
    uint16_t first = clusters[0];
    free(clusters);

    return first;
}

static DirectoryEntry* read_subdirectory(
    FAT12FS* fs,
    uint16_t first_cluster,
    uint32_t* count
)
{
    uint32_t bytes;
    uint8_t* data = read_cluster_chain(fs, first_cluster, &bytes);

    if (data == NULL)
    {
        *count = 0;
        return NULL;
    }

    *count = bytes / DIRECTORY_ENTRY_SIZE;
    return (DirectoryEntry*)data;
}

#define ROOT_DIR_CLUSTER_SENTINEL 0

static DirectoryEntry* read_directory(
    FAT12FS* fs,
    uint16_t first_cluster,
    uint32_t* out_count
)
{
    if (first_cluster == ROOT_DIR_CLUSTER_SENTINEL)
    {
        *out_count = fs->bs.bpb.root_entry_count;
        return read_root_directory(fs);
    }

    return read_subdirectory(fs, first_cluster, out_count);
}

static int resolve_path(
    FAT12FS* fs,
    const char* path,
    DirectoryEntry* out
)
{
    /* All paths must be absolute, starting with "/" (root); the slash is skipped after the check */
    if (path[0] != '/') return -1;
    const char* p = path + 1;

    /* Return the Root Directory if the path contains only "/" */
    if (*p == '\0')
    {
        out->attributes = FAT12_ATTR_DIRECTORY;
        out->first_cluster = ROOT_DIR_CLUSTER_SENTINEL;
        return 0;
    }

    /* ---- Set Root Directory as Current Directory ---- */
    uint16_t dir_cluster = ROOT_DIR_CLUSTER_SENTINEL;

    while (1)
    {
        /* ---- Read Current Directory ---- */
        uint32_t dir_count;
        DirectoryEntry* dir_entries = read_directory(fs, dir_cluster, &dir_count);
        if (dir_entries == NULL) return -1;

        /* ---- Extract next component from path string ---- */
        char name[13];
        int i = 0;
        while (*p && *p != '/' && i < 12)
        {
            name[i++] = *p++;
        }
        name[i] = '\0';

        /* ---- Found in Current Directory? ---- */
        DirectoryEntry* found = find_entry_by_name(dir_entries, dir_count, name);
        if (found == NULL)
        {
            /* Error: Not Found */
            free(dir_entries);
            return -1;
        }

        /* ---- Is last component? ---- */
        int is_last = (*p == '\0');
        if (*p == '/') p++;
        if (is_last)
        {
            /* ---- Return entry ---- */
            *out = *found;
            free(dir_entries);
            return 0;
        }

        /* ---- Is entry a directory? ---- */
        if (!(found->attributes & FAT12_ATTR_DIRECTORY))
        {
            /* Error: Not Found */
            free(dir_entries);
            return -1;
        }

        /* ---- Set subdirectory as Current Directory ---- */
        dir_cluster = found->first_cluster;
        free(dir_entries);
    }
}

static int find_free_entry(DirectoryEntry* entries, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if (entries[i].name[0] == NAME_END ||
            is_free_entry(&entries[i]))
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

    /* Blank-fill all 11 bytes up front: whatever we do not overwrite below
       is already the space padding the format calls for */
    memset(entry->name, ' ', 11);

    char* dot = strchr(name_copy, '.');
    if (dot)
    {
        *dot = '\0';

        /* The extension goes into the last 3 bytes, at offset 8 */
        size_t ext_len = strlen(dot + 1);
        if (ext_len > 3) ext_len = 3;
        memcpy(entry->name + 8, dot + 1, ext_len);
    }

    /* The base name goes into the first 8 bytes; anything longer is truncated */
    size_t base_len = strlen(name_copy);
    if (base_len > 8) base_len = 8;
    memcpy(entry->name, name_copy, base_len);

    /* 0x05 escape: if the first character is NAME_FREE (0xE5), store 0x05
       instead so the entry is not mistaken for a free slot on disk */
    if ((unsigned char)entry->name[0] == NAME_FREE)
        entry->name[0] = 0x05;
}

static uint16_t time_encode(const Timestamp* dt)
{
    return (dt->hours << 11) | (dt->minutes << 5) | (dt->seconds / 2);
}

static uint16_t date_encode(const Timestamp* dt)
{
    return ((dt->year - 1980) << 9) | (dt->month << 5) | dt->day;
}

static void set_entry_timestamps(DirectoryEntry* entry)
{
    /* In production, read the real-time clock and encode via
       time_encode/date_encode. We hard-code a reference
       timestamp to keep the focus on FAT12, not platform plumbing. */
    Timestamp dt = { .year = 2024, .month = 1, .day = 1,
                     .hours = 12, .minutes = 0, .seconds = 0 };
    entry->create_date      = date_encode(&dt);
    entry->create_time      = time_encode(&dt);
    entry->last_write_date  = date_encode(&dt);
    entry->last_write_time  = time_encode(&dt);
    entry->last_access_date = date_encode(&dt);
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

static void write_directory(
    FAT12FS* fs,
    uint16_t parent_cluster,
    DirectoryEntry* entries,
    uint32_t count
)
{
    if (parent_cluster == ROOT_DIR_CLUSTER_SENTINEL)
    {
        block_device_write(
            fs->device, fs->root_dir_lba,
            fs->root_dir_sectors, entries);
    }
    else
    {
        write_cluster_chain(fs, parent_cluster, (uint8_t*)entries, count * sizeof(DirectoryEntry));
    }
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
    entry->attributes = FAT12_ATTR_ARCHIVE;
    set_entry_timestamps(entry);

    write_directory(fs, *out_parent_cluster, entries, count);

    free(entries);
    *out_slot = slot;
    return 0;
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

struct Directory {
    DirectoryEntry* entries;
    uint32_t count;
    uint32_t offset;
};

Directory* fat12_opendir(FAT12FS* fs, const char* path)
{
    DBG_PRINT("[ fat12        ] --- fat12_opendir(\"%s\") ---\n", path);

    /* Resolve the path, rejecting anything that is not a directory */
    DirectoryEntry resolved;
    if (resolve_path(fs, path, &resolved) != 0) return NULL;
    if (!(resolved.attributes & FAT12_ATTR_DIRECTORY)) return NULL;

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

struct File {
    uint8_t* data;
    uint32_t size;
    uint32_t position;
    char mode;
    FAT12FS* fs;
    uint16_t dir_cluster;
    int dir_slot;
};

File* fat12_open(FAT12FS* fs, const char* path, char mode)
{
    DBG_PRINT("[ fat12        ] --- fat12_open(\"%s\", mode='%c') ---\n", path, mode);

    /* Only absolute paths are supported: every path must start at "/" */
    if (path[0] != '/') return NULL;

    if (mode == 'w')
    {
        int slot;
        uint16_t parent_cluster;
        if (create_dir_entry(fs, path, &slot, &parent_cluster) != 0) return NULL;

        File* file = (File*)malloc(sizeof(File));
        file->data = NULL;
        file->size = 0;
        file->position = 0;
        file->mode = 'w';
        file->fs = fs;
        file->dir_cluster = parent_cluster;
        file->dir_slot = slot;
        return file;
    }

    if (mode == 'r')
    {
        DirectoryEntry resolved;
        /* Fail if the path does not resolve to an entry */
        if (resolve_path(fs, path, &resolved) != 0) return NULL;
        /* Only files can be opened, not directories */
        if (resolved.attributes & FAT12_ATTR_DIRECTORY) return NULL;

        uint8_t* data = NULL;
        uint32_t chain_bytes = 0;

        if (resolved.file_size > 0)
        {
            data = read_cluster_chain(fs, resolved.first_cluster, &chain_bytes);
            if (data == NULL) return NULL;
        }

        File* file = (File*)malloc(sizeof(File));
        file->data = data;

        /* a chain shorter than file_size means a corrupt volume:
           never let fat12_read run past the bytes we actually loaded */
        file->size = resolved.file_size < chain_bytes ? resolved.file_size : chain_bytes;

        file->position = 0;
        file->mode = 'r';
        file->fs = fs;
        return file;
    }

    return NULL;
}

uint32_t fat12_write(File* file, const void* buffer, uint32_t size)
{
    if (file->mode != 'w') return 0;

    uint8_t* new_data = (uint8_t*)realloc(file->data, file->size + size);
    memcpy(new_data + file->size, buffer, size);
    file->data = new_data;
    file->size += size;
    file->position = file->size;

    return size;
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
    int result = 0;

    if (file->mode == 'w')
    {
        if (file->size > 0)
        {
            uint32_t chain_bytes;
            uint16_t first = allocate_cluster_chain(file->fs, file->size, &chain_bytes);

            if (first != 0)
            {
                write_cluster_chain(file->fs, first, file->data, file->size);
                flush_fats(file->fs);
                update_dir_entry(file->fs, file->dir_cluster, file->dir_slot, first, file->size);
            }
            else
            {
                result = -1;
            }
        }
    }

    /* free(NULL) is a no-op, so empty files are fine */
    free(file->data);
    free(file);

    return result;
}

static void initialize_directory_data(
    FAT12FS* fs,
    uint16_t first_cluster,
    uint16_t parent_first_cluster
)
{
    /* calloc zeros the whole cluster, so every entry stays 0x00 (end-of-directory) */
    uint8_t* buf = calloc(1, fs->cluster_bytes);
    DirectoryEntry* entries = (DirectoryEntry*)buf;

    memcpy(entries[SELF_ENTRY].name, ".          ", 11);
    entries[SELF_ENTRY].attributes = FAT12_ATTR_DIRECTORY;
    entries[SELF_ENTRY].first_cluster = first_cluster;
    set_entry_timestamps(&entries[SELF_ENTRY]);

    memcpy(entries[PARENT_ENTRY].name, "..         ", 11);
    entries[PARENT_ENTRY].attributes = FAT12_ATTR_DIRECTORY;
    entries[PARENT_ENTRY].first_cluster = parent_first_cluster;
    set_entry_timestamps(&entries[PARENT_ENTRY]);

    write_cluster_chain(fs, first_cluster, buf, fs->cluster_bytes);
    free(buf);
}

int fat12_mkdir(FAT12FS* fs, const char* path)
{
    DBG_PRINT("[ fat12        ] --- fat12_mkdir(\"%s\") ---\n", path);
    /* ---- Step 1: Split path into parent path and leaf name ---- */
    char parent_path[256];
    char dirname[64];
    if (get_parent_and_name(path, parent_path, dirname) < 0) return -1;

    /* ---- Step 2: Load parent directory entries ---- */
    DirectoryEntry parent;
    if (resolve_path(fs, parent_path, &parent) != 0) return -1;

    uint32_t count;
    DirectoryEntry* entries = read_directory(fs, parent.first_cluster, &count);
    if (entries == NULL) return -1;

    /* ---- Step 3: Make sure the name is not already taken ---- */
    if (find_entry_by_name(entries, count, dirname) != NULL)
    {
        free(entries);
        return -1;
    }

    /* ---- Step 4: Find an empty slot in the parent ---- */
    int slot = find_free_entry(entries, count);
    if (slot < 0)
    {
        free(entries);
        return -1;
    }

    /* ---- Step 5: Allocate a cluster and mark it end-of-chain ---- */
    uint32_t chain_bytes;
    uint16_t cluster = allocate_cluster_chain(fs, fs->cluster_bytes, &chain_bytes);
    if (cluster == 0)
    {
        free(entries);
        return -1;
    }

    /* ---- Step 6: Initialize its data, then write it to disk ---- */
    initialize_directory_data(fs, cluster, parent.first_cluster);

    /* ---- Step 7: Flush the FATs, then write the parent's new entry ---- */
    DirectoryEntry* entry = &entries[slot];
    memset(entry, 0, sizeof(DirectoryEntry));
    encode_8_3_name(entry, dirname);
    entry->attributes = FAT12_ATTR_DIRECTORY;
    entry->first_cluster = cluster;
    entry->file_size = 0;
    set_entry_timestamps(entry);

    flush_fats(fs);
    write_directory(fs, parent.first_cluster, entries, count);

    free(entries);
    return 0;
}

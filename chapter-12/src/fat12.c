#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
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
#define ROOT_DIR_CLUSTER_SENTINEL 0

#define FAT_ENTRY_FREE  0x000
#define FAT_ENTRY_END_OF_CHAIN   0xFFF

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
    DBG_PRINT("[ fat12        ] FAT region size (sectors): %u\n", fs->fat_sectors);
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
    /* Only trailing spaces are padding: scan each part from the right
       to find where it really ends */
    int base_len = 8;
    while (base_len > 0 && entry->name[base_len - 1] == ' ')
        base_len--;

    int ext_len = 3;
    while (ext_len > 0 && entry->name[8 + ext_len - 1] == ' ')
        ext_len--;

    /* Copy the base name, embedded spaces included */
    int i;
    for (i = 0; i < base_len; i++)
        out[i] = entry->name[i];

    /* 0x05 escape — real first byte is 0xE5 */
    if (entry->name[0] == 0x05)
        out[0] = (char)0xE5;

    /* If extension exists, insert dot and copy it */
    if (ext_len > 0)
    {
        out[i] = '.';
        i++;
        for (int k = 0; k < ext_len; k++)
        {
            out[i] = entry->name[8 + k];
            i++;
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
    /* No 8.3 name is longer than 12 characters, so a longer one can't match */
    if (strlen(name) > 12) return NULL;

    /* Uppercase the name for comparison: names on disk are always uppercase */
    char target[13];
    strcpy(target, name);
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

static uint32_t data_cluster_to_lba(FAT12FS* fs, uint16_t cluster)
{
    return fs->data_lba + ((cluster - FIRST_DATA_CLUSTER) * fs->bs.bpb.sectors_per_cluster);
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

        /* The copy stops at the end of the path, at a '/', or when name[]
           is full. Stopping anywhere else means the buffer filled up
           mid-component: it runs past 12 characters, too long for 8.3 */
        if (*p != '\0' && *p != '/')
        {
            free(dir_entries);
            return -1;
        }

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

typedef struct {
    const char* entry_name;
    uint16_t first_cluster;
    /* allocated by resolve_parent: the caller must free() it when done */
    DirectoryEntry* entries;
    uint32_t count;
} ParentDir;

static int resolve_parent(FAT12FS* fs, const char* path, ParentDir* out)
{
    /* Split at the last slash: the parent comes before it ("/" for root),
       the name after it. No slash means no parent, so the path is invalid. */
    const char* last_slash = strrchr(path, '/');
    if (last_slash == NULL) return -1;
    size_t parent_len = last_slash == path ? 1 : (size_t)(last_slash - path);

    /* Resolve the parent. resolve_path needs a '\0'-terminated string, so
       we resolve a copy of just the parent part and free it right after */
    char* parent_path = (char*)malloc(parent_len + 1);
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';
    DirectoryEntry parent_entry;
    bool found = resolve_path(fs, parent_path, &parent_entry) == 0;
    free(parent_path);

    /* The parent must exist, and it must be a directory */
    if (!found || !(parent_entry.attributes & FAT12_ATTR_DIRECTORY)) return -1;

    /* Load the parent's entries */
    uint32_t count;
    DirectoryEntry* entries = read_directory(fs, parent_entry.first_cluster, &count);
    if (entries == NULL) return -1;

    /* Fill in the result only once everything has succeeded */
    out->entry_name = last_slash + 1;
    out->first_cluster = parent_entry.first_cluster;
    out->entries = entries;
    out->count = count;

    return 0;
}

static bool is_dot_entry(const char* name)
{
    return strcmp(name, ".") == 0 ||
           strcmp(name, "..") == 0;
}

static int check_directory_empty(FAT12FS* fs, uint16_t first_cluster, bool* out_is_empty)
{
    uint32_t count;
    DirectoryEntry* entries = read_subdirectory(fs, first_cluster, &count);
    if (entries == NULL) return -1;

    bool is_empty = true;
    uint32_t offset = 0;
    DirectoryEntry* entry;
    while ((entry = next_valid_entry(entries, count, &offset)) != NULL)
    {
        char entry_name[13];
        decode_8_3_name(entry, entry_name);
        if (is_dot_entry(entry_name)) continue;

        is_empty = false;
        break;
    }

    free(entries);
    *out_is_empty = is_empty;

    return 0;
}

static void mark_entry_free(DirectoryEntry* entry)
{
    entry->name[0] = (char)NAME_FREE;
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
    uint8_t* buffer = (uint8_t*)malloc(fs->cluster_bytes);

    /* Follows the existing chain only and never allocates a cluster:
       data beyond the chain's end is not written */
    while (is_data_cluster(fs, cluster) && remaining > 0)
    {
        uint32_t lba = data_cluster_to_lba(fs, cluster);
        uint32_t chunk = remaining < fs->cluster_bytes ? remaining : fs->cluster_bytes;

        memcpy(buffer, data + (size - remaining), chunk);
        /* zero the unused rest of a short last chunk */
        memset(buffer + chunk, 0, fs->cluster_bytes - chunk);

        DBG_PRINT("[ fat12        ] write cluster %u\n", cluster);
        block_device_write(fs->device, lba, fs->bs.bpb.sectors_per_cluster, buffer);

        remaining -= chunk;
        cluster = get_fat_entry(fs, cluster);
    }

    free(buffer);
}

static void write_directory(
    FAT12FS* fs,
    uint16_t first_cluster,
    DirectoryEntry* entries,
    uint32_t count
)
{
    if (first_cluster == ROOT_DIR_CLUSTER_SENTINEL)
    {
        DBG_PRINT("[ fat12        ] write root directory\n");
        block_device_write(fs->device, fs->root_dir_lba, fs->root_dir_sectors, entries);
    }
    else
    {
        write_cluster_chain(fs, first_cluster, (uint8_t*)entries, count * DIRECTORY_ENTRY_SIZE);
    }
}

static void set_fat_entry(
    FAT12FS* fs,
    uint16_t cluster,
    uint16_t entry
)
{
    /* byte where this cluster's 12-bit entry starts, same as in get_fat_entry */
    uint32_t offset = cluster + (cluster / 2);

    /* read the two bytes the entry spans as one little-endian 16-bit value
       (second byte on top), exactly like get_fat_entry: our three nibbles
       side by side, plus one nibble of the neighbor at one end */
    uint16_t old_value = (fs->fat[offset + 1] << 8) | fs->fat[offset];

    uint16_t new_value;

    /* lowest bit set = odd cluster, clear = even */
    if (cluster & 1)
    {
        /* odd cluster: our entry is the top 12 bits; keep the low nibble
           (0x000F, it belongs to the even entry) */
        new_value = ((entry & 0x0FFF) << 4) | (old_value & 0x000F);
    }
    else
    {
        /* even cluster: our entry is the low 12 bits; keep the top nibble
           (0xF000, it belongs to the odd entry) */
        new_value = (old_value & 0xF000) | (entry & 0x0FFF);
    }

    /* split the 16-bit value back into its two bytes, little-endian:
       low byte first */
    fs->fat[offset] = new_value & 0xFF;
    fs->fat[offset + 1] = new_value >> 8;

    DBG_PRINT(
        "[ fat12        ] set FAT entry %u to 0x%03x (FAT bytes %u-%u: %02x %02x -> %02x %02x)\n",
        cluster, entry, offset, offset + 1,
        old_value & 0xFF, old_value >> 8,
        fs->fat[offset], fs->fat[offset + 1]
    );
}

static void free_cluster_chain(FAT12FS* fs, uint16_t first_cluster)
{
    uint16_t cluster = first_cluster;

    while (is_data_cluster(fs, cluster))
    {
        uint16_t next = get_fat_entry(fs, cluster);
        DBG_PRINT("[ fat12        ] free cluster %u\n", cluster);
        set_fat_entry(fs, cluster, FAT_ENTRY_FREE);
        cluster = next;
    }
}

static void flush_fats(FAT12FS* fs)
{
    for (int i = 0; i < fs->bs.bpb.num_fats; i++)
    {
        uint32_t lba = fs->fat_lba + (i * fs->bs.bpb.fat_size_16);
        DBG_PRINT("[ fat12        ] write FAT copy %d\n", i + 1);
        block_device_write(fs->device, lba, fs->bs.bpb.fat_size_16, fs->fat);
    }
}

int fat12_remove(FAT12FS* fs, const char* path)
{
    DBG_PRINT("[ fat12        ] --- fat12_remove(\"%s\") ---\n", path);

    /* ---- Step 1: Reject root ---- */
    /* Root must always be present, and it has no directory entry
       of its own that a delete could free */
    if (strcmp(path, "/") == 0) return -1;

    /* ---- Step 2: Load the parent directory's entries ---- */
    ParentDir parent;
    if (resolve_parent(fs, path, &parent) != 0) return -1;

    /* From here on, every exit goes through "done", which frees the
       parent's entries; result only becomes 0 once the delete is through */
    int result = -1;

    /* ---- Step 3: Reject . and .. ---- */
    /* They belong to the directory itself: they are created with it
       and only ever go away together with it, never on their own */
    if (is_dot_entry(parent.entry_name)) goto done;

    /* ---- Step 4: Find the entry ---- */
    DirectoryEntry* entry = find_entry_by_name(parent.entries, parent.count, parent.entry_name);
    if (entry == NULL) goto done;

    /* ---- Step 5: Reject read-only entries ---- */
    /* Read-only entries must not be written to; like most
       implementations, we protect them from deletion too */
    if (entry->attributes & FAT12_ATTR_READ_ONLY) goto done;

    /* ---- Step 6: If it is a directory, verify it is empty ---- */
    if (entry->attributes & FAT12_ATTR_DIRECTORY)
    {
        /* A directory we can't read can't be proven empty: refuse it too */
        bool is_empty;
        if (check_directory_empty(fs, entry->first_cluster, &is_empty) != 0 || !is_empty) goto done;
    }

    /* ---- Step 7: Mark the entry free, then write the parent ---- */
    mark_entry_free(entry);
    write_directory(fs, parent.first_cluster, parent.entries, parent.count);

    /* ---- Step 8: Free its cluster chain, then flush the FAT ---- */
    free_cluster_chain(fs, entry->first_cluster);
    flush_fats(fs);

    result = 0;

done:
    free(parent.entries);
    return result;
}

static int encode_8_3_name(const char* name, char out[11])
{
    /* Split at the last dot: base name before it, extension after it */
    size_t len = strlen(name);
    const char* separator = strrchr(name, '.');
    size_t base_len = separator ? (size_t)(separator - name) : len;
    size_t ext_len = separator ? len - base_len - 1 : 0;

    /* Checks on the whole name, before the per-character pass:
       base name 1-8 characters, extension at most 3, no leading space */
    if (base_len < 1 || base_len > 8) return -1;
    if (ext_len > 3) return -1;
    if (name[0] == ' ') return -1;

    /* Blank-fill all 11 bytes up front: whatever we do not overwrite below
       is already the space padding the format calls for */
    memset(out, ' ', 11);

    /* Copy the name in one character at a time, checking each on the way. */
    char* next = out;
    for (const char* p = name; *p; p++)
    {
        /* The dot itself is never stored: skip it, and write everything
           after it into the extension's slot */
        if (p == separator)
        {
            next = out + 8;
            continue;
        }

        unsigned char c = (unsigned char)*p;

        /* No control characters, and none of the characters FAT forbids */
        if (c < 0x20) return -1;
        if (strchr("\"*+,./:;<=>?[\\]|", c)) return -1;

        /* We accept lowercase input, but the volume stores only uppercase, so we convert it */
        *next++ = (char)toupper(c);
    }

    /* 0x05 escape: if the first character is 0xE5, store 0x05
       instead so the entry is not mistaken for a free slot on the volume */
    if ((unsigned char)out[0] == NAME_FREE)
        out[0] = 0x05;

    return 0;
}

static int find_free_entry(DirectoryEntry* entries, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if (is_end_of_directory(&entries[i]) || is_free_entry(&entries[i]))
        {
            return i;
        }
    }

    return -1;
}

static int check_directory_inside(
    FAT12FS* fs,
    uint16_t inner_first_cluster,
    uint16_t outer_first_cluster,
    bool* out_is_inside
)
{
    /* Climb from inner to root, one .. entry at a time */
    uint16_t cluster = inner_first_cluster;
    for (uint32_t depth = 0; cluster != ROOT_DIR_CLUSTER_SENTINEL; depth++)
    {
        /* No valid tree is deeper than the volume has clusters: a longer
           climb means a corrupt loop of .. entries */
        if (depth == fs->data_cluster_count) return -1;

        /* Met outer on the way up: inner lies inside it */
        if (cluster == outer_first_cluster)
        {
            *out_is_inside = true;
            return 0;
        }

        uint32_t count;
        DirectoryEntry* entries = read_directory(fs, cluster, &count);
        /* An unreadable directory, so we can't tell */
        if (entries == NULL) return -1;

        DirectoryEntry* dotdot = find_entry_by_name(entries, count, "..");
        /* No .. to climb: a corrupt directory, so we can't tell */
        if (dotdot == NULL)
        {
            free(entries);
            return -1;
        }

        /* Climb one level up: the parent is where we look next */
        cluster = dotdot->first_cluster;
        free(entries);
    }

    *out_is_inside = false;
    return 0;
}

int fat12_move(FAT12FS* fs, const char* src_path, const char* dst_path)
{
    DBG_PRINT("[ fat12        ] --- fat12_move(\"%s\" -> \"%s\") ---\n", src_path, dst_path);

    /* ---- Step 1: Reject root ---- */
    /* Root is the top of the tree: every destination lies inside it,
       so there is nowhere to move it */
    if (strcmp(src_path, "/") == 0) return -1;

    /* ---- Step 2: Load the source parent's entries ---- */
    ParentDir src;
    if (resolve_parent(fs, src_path, &src) != 0) return -1;

    /* From here on, every failure is a goto to the cleanup at the end,
       which frees both parents' entries. dst isn't loaded yet, so its
       entries start as NULL, which free() simply ignores */
    int result = -1;
    ParentDir dst;
    dst.entries = NULL;

    /* ---- Step 3: Reject . and .. as the source ---- */
    /* Every subdirectory must keep both as its first two entries, so
       moving one away would corrupt the directory */
    if (is_dot_entry(src.entry_name)) goto done;

    /* ---- Step 4: Find the entry ---- */
    DirectoryEntry* src_entry = find_entry_by_name(src.entries, src.count, src.entry_name);
    if (src_entry == NULL) goto done;

    /* ---- Step 5: Load the destination parent's entries ---- */
    if (resolve_parent(fs, dst_path, &dst) != 0) goto done;

    /* ---- Step 6: Encode the new name, rejecting it if it breaks a rule ---- */
    DirectoryEntry encoded;
    if (encode_8_3_name(dst.entry_name, encoded.name) != 0) goto done;

    /* ---- Step 7: Make sure the new name is not already taken ---- */
    /* Decoding the encoded name gives its canonical spelling, the same
       one find_entry_by_name compares against */
    char canonical[13];
    decode_8_3_name(&encoded, canonical);
    if (find_entry_by_name(dst.entries, dst.count, canonical) != NULL) goto done;

    /* ---- Step 8: Same parent directory? Then it's a rename ---- */
    /* Matching first clusters mean source and destination share one
       directory, so we only overwrite the name in place */
    if (src.first_cluster == dst.first_cluster)
    {
        memcpy(src_entry->name, encoded.name, 11);
        write_directory(fs, src.first_cluster, src.entries, src.count);
        result = 0;
        goto done;
    }

    /* ---- Step 9: If it is a directory, reject moving it into its own subtree ---- */
    if (src_entry->attributes & FAT12_ATTR_DIRECTORY)
    {
        bool is_inside;
        int status = check_directory_inside(fs, dst.first_cluster, src_entry->first_cluster, &is_inside);
        if (status != 0 || is_inside) goto done;
    }

    /* ---- Step 10: Find a free slot in the destination ---- */
    int dst_slot = find_free_entry(dst.entries, dst.count);
    if (dst_slot < 0) goto done;

    /* ---- Step 11: Move the entry (in memory): copy it into its new slot, update its name, mark the old slot free ---- */
    DirectoryEntry* new_entry = &dst.entries[dst_slot];
    memcpy(new_entry, src_entry, DIRECTORY_ENTRY_SIZE);
    memcpy(new_entry->name, encoded.name, 11);
    mark_entry_free(src_entry);

    /* ---- Step 12: If it is a directory, load it and point its .. at the new parent (in memory) ---- */
    DirectoryEntry* moved_entries = NULL;
    uint32_t moved_count = 0;
    if (new_entry->attributes & FAT12_ATTR_DIRECTORY)
    {
        moved_entries = read_directory(fs, new_entry->first_cluster, &moved_count);
        if (moved_entries == NULL) goto done;

        DirectoryEntry* dotdot = find_entry_by_name(moved_entries, moved_count, "..");
        if (dotdot == NULL)
        {
            free(moved_entries);
            goto done;
        }

        dotdot->first_cluster = dst.first_cluster;
    }

    /* ---- Step 13: Commit to the volume in the correct order ---- */
    /* First, the new entry in the destination */
    write_directory(fs, dst.first_cluster, dst.entries, dst.count);

    /* Then the moved directory, with its updated .. */
    if (moved_entries != NULL)
    {
        write_directory(fs, new_entry->first_cluster, moved_entries, moved_count);
        free(moved_entries);
    }

    /* Last, the old entry, now marked free, in the source */
    write_directory(fs, src.first_cluster, src.entries, src.count);

    result = 0;

done:
    free(dst.entries);
    free(src.entries);
    return result;
}

static uint16_t allocate_cluster(FAT12FS* fs)
{
    for (uint16_t cluster = FIRST_DATA_CLUSTER; is_data_cluster(fs, cluster); cluster++)
    {
        if (get_fat_entry(fs, cluster) == FAT_ENTRY_FREE)
        {
            DBG_PRINT("[ fat12        ] allocate cluster %u\n", cluster);
            set_fat_entry(fs, cluster, FAT_ENTRY_END_OF_CHAIN);
            return cluster;
        }
    }

    /* No free cluster left: the volume is full */
    return 0;
}

static void initialize_directory_data(
    FAT12FS* fs,
    const DirectoryEntry* dir_entry,
    uint16_t parent_first_cluster
)
{
    /* calloc zeros the whole cluster, so every entry stays 0x00 (end-of-directory) */
    uint8_t* buffer = calloc(1, fs->cluster_bytes);
    DirectoryEntry* entries = (DirectoryEntry*)buffer;

    /* . is a copy of the directory's own entry, pointing at itself */
    entries[0] = *dir_entry;
    memcpy(entries[0].name, ".          ", 11);
    entries[0].attributes = FAT12_ATTR_DIRECTORY;

    /* .. is also a copy of the directory's own entry, but pointing at the parent */
    entries[1] = *dir_entry;
    memcpy(entries[1].name, "..         ", 11);
    entries[1].attributes = FAT12_ATTR_DIRECTORY;
    entries[1].first_cluster = parent_first_cluster;

    write_cluster_chain(fs, dir_entry->first_cluster, buffer, fs->cluster_bytes);
    free(buffer);
}

static uint16_t encode_time(const Timestamp* dt)
{
    /* No masks: callers must pass in-range values. A bad one spills into the next field, so the bug shows */
    return (dt->hours << 11) | (dt->minutes << 5) | (dt->seconds / 2);
}

static uint16_t encode_date(const Timestamp* dt)
{
    /* Same contract here, and the year must stay within FAT's 1980-2107, or it wraps around */
    return ((dt->year - 1980) << 9) | (dt->month << 5) | dt->day;
}

static Timestamp current_time(void)
{
    /* FAT has no time zone field and by convention holds local time: localtime, not gmtime (UTC) */
    time_t seconds = time(NULL);
    struct tm* local = localtime(&seconds);

    /* struct tm counts years from 1900 and months from 0 */
    Timestamp now = {
        .year    = local->tm_year + 1900,
        .month   = local->tm_mon + 1,
        .day     = local->tm_mday,
        .hours   = local->tm_hour,
        .minutes = local->tm_min,
        .seconds = local->tm_sec
    };

    return now;
}

static void initialize_entry_timestamps(DirectoryEntry* entry)
{
    Timestamp now = current_time();
    uint16_t encoded_date = encode_date(&now);
    uint16_t encoded_time = encode_time(&now);

    entry->create_date      = encoded_date;
    entry->create_time      = encoded_time;
    entry->last_write_date  = encoded_date;
    entry->last_write_time  = encoded_time;
    entry->last_access_date = encoded_date;

    /* create_time only counts 2-second steps: an odd second goes here, as 100 centiseconds */
    entry->create_time_tenth = (now.seconds % 2) * 100;
}

int fat12_mkdir(FAT12FS* fs, const char* path)
{
    DBG_PRINT("[ fat12        ] --- fat12_mkdir(\"%s\") ---\n", path);

    /* ---- Step 1: Load the parent directory's entries ---- */
    ParentDir parent;
    if (resolve_parent(fs, path, &parent) != 0) return -1;

    int result = -1;

    /* ---- Step 2: Encode the new directory's name, rejecting it if it breaks a rule ---- */
    DirectoryEntry encoded;
    if (encode_8_3_name(parent.entry_name, encoded.name) != 0) goto done;

    /* ---- Step 3: Make sure the name is not already taken ---- */
    /* Compare its canonical spelling, as fat12_move does */
    char canonical[13];
    decode_8_3_name(&encoded, canonical);
    if (find_entry_by_name(parent.entries, parent.count, canonical) != NULL) goto done;

    /* ---- Step 4: Find an empty slot in the parent ---- */
    int slot = find_free_entry(parent.entries, parent.count);
    if (slot < 0) goto done;

    /* ---- Step 5: Allocate a cluster and mark it end-of-chain (in memory only) ---- */
    uint16_t cluster = allocate_cluster(fs);
    if (cluster == 0) goto done;

    /* ---- Step 6: Fill in the parent's new entry (in memory only) ---- */
    /* memset leaves file_size at 0, as a directory's own entry always needs */
    DirectoryEntry* entry = &parent.entries[slot];
    memset(entry, 0, DIRECTORY_ENTRY_SIZE);
    memcpy(entry->name, encoded.name, 11);
    /* No archive bit: drivers set it on new files only, never on directories */
    entry->attributes = FAT12_ATTR_DIRECTORY;
    entry->first_cluster = cluster;
    initialize_entry_timestamps(entry);

    /* ---- Step 7: Commit to the volume: the new directory's data, the FATs, then the parent directory ---- */
    initialize_directory_data(fs, entry, parent.first_cluster);
    flush_fats(fs);
    write_directory(fs, parent.first_cluster, parent.entries, parent.count);

    result = 0;

done:
    free(parent.entries);
    return result;
}

static int create_dir_entry(
    FAT12FS* fs,
    const char* path,
    int* out_slot,
    uint16_t* out_parent_cluster
)
{
    ParentDir parent;
    if (resolve_parent(fs, path, &parent) != 0) return -1;

    int result = -1;

    /* Compare the name's canonical spelling, as fat12_move does */
    DirectoryEntry encoded;
    char canonical[13];
    if (encode_8_3_name(parent.entry_name, encoded.name) != 0) goto done;
    decode_8_3_name(&encoded, canonical);
    if (find_entry_by_name(parent.entries, parent.count, canonical) != NULL) goto done;

    int slot = find_free_entry(parent.entries, parent.count);
    if (slot < 0) goto done;

    /* Zero the whole slot: a deleted entry still holds its old attributes,
       cluster, size, and timestamps. memset leaves first_cluster and
       file_size at 0: no data, no clusters yet */
    DirectoryEntry* entry = &parent.entries[slot];
    memset(entry, 0, DIRECTORY_ENTRY_SIZE);
    memcpy(entry->name, encoded.name, 11);
    entry->attributes = FAT12_ATTR_ARCHIVE;
    initialize_entry_timestamps(entry);

    write_directory(fs, parent.first_cluster, parent.entries, parent.count);

    *out_slot = slot;
    *out_parent_cluster = parent.first_cluster;
    result = 0;

done:
    free(parent.entries);
    return result;
}

File* fat12_open(FAT12FS* fs, const char* path, char mode)
{
    DBG_PRINT("[ fat12        ] --- fat12_open(\"%s\", mode='%c') ---\n", path, mode);

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

    return size;
}

static uint16_t allocate_cluster_chain(FAT12FS* fs, uint32_t needed_bytes)
{
    uint32_t needed_clusters = (needed_bytes + fs->cluster_bytes - 1) / fs->cluster_bytes;

    DBG_PRINT("[ fat12        ] allocating %u cluster(s) for %u bytes\n", needed_clusters, needed_bytes);

    uint16_t first = 0;
    uint16_t last = 0;

    for (uint32_t i = 0; i < needed_clusters; i++)
    {
        /* Each new cluster arrives already marked end-of-chain... */
        uint16_t cluster = allocate_cluster(fs);
        if (cluster == 0)
        {
            /* Volume full: release what we claimed so far (in memory only) */
            if (first != 0) free_cluster_chain(fs, first);
            return 0;
        }

        /* ...so linking it on is just pointing the old end at it */
        if (first == 0)
            first = cluster;
        else
            set_fat_entry(fs, last, cluster);
        last = cluster;
    }

    return first;
}

int fat12_close(File* file)
{
    DBG_PRINT("[ fat12        ] --- fat12_close() ---\n");
    int result = 0;

    if (file->mode == 'w' && file->size > 0)
    {
        uint16_t first = allocate_cluster_chain(file->fs, file->size);
        if (first == 0)
        {
            result = -1;
        }
        else
        {
            write_cluster_chain(file->fs, first, file->data, file->size);
            flush_fats(file->fs);

            /* Re-read the parent: the array open found the slot in is long gone.
               Entries never move within a directory, so dir_slot still names ours */
            uint32_t count;
            DirectoryEntry* entries = read_directory(file->fs, file->dir_cluster, &count);
            if (entries == NULL)
            {
                /* The data and FAT are already on the volume: those clusters
                   are now lost, so the caller must know */
                result = -1;
            }
            else
            {
                DirectoryEntry* entry = &entries[file->dir_slot];
                entry->first_cluster = first;
                entry->file_size = file->size;

                /* The content just changed: writing counts as accessing, too */
                Timestamp now = current_time();
                entry->last_write_date  = encode_date(&now);
                entry->last_write_time  = encode_time(&now);
                entry->last_access_date = encode_date(&now);

                write_directory(file->fs, file->dir_cluster, entries, count);
                free(entries);
            }
        }
    }

    /* free(NULL) is a no-op, so empty files are fine */
    free(file->data);
    free(file);

    return result;
}

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
        uint32_t data_sectors = total_sectors - reserved_sector_count - (num_fats * sectors_per_fat) - root_dir_sectors;
        uint32_t total_clusters = data_sectors / sectors_per_cluster;
        uint32_t fat_capacity = (sectors_per_fat * bytes_per_sector * 8) / 12;

        if (total_clusters + FIRST_DATA_CLUSTER <= fat_capacity) break;
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
    bs->bpb.sectors_per_cluster = 2;
    bs->bpb.root_entry_count = 512;
    bs->bpb.reserved_sector_count = 1;
    bs->bpb.num_fats = 2;

    uint32_t root_dir_sectors = ((bs->bpb.root_entry_count * DIRECTORY_ENTRY_SIZE) + (bytes_per_sector - 1)) / bytes_per_sector;

    /* Bail out before computing the FAT size, or writing anything, if the
       disk can't even fit the reserved region, one sector-sized FAT per
       copy, and the root directory */
    if (total_sectors < bs->bpb.reserved_sector_count + bs->bpb.num_fats + root_dir_sectors)
        return -1;

    bs->bpb.fat_size_16 = compute_sectors_per_fat(total_sectors, bs->bpb.reserved_sector_count, bs->bpb.num_fats, root_dir_sectors, bs->bpb.sectors_per_cluster, bytes_per_sector);

    /* Only one of the two total-sector fields is ever valid — mount reads
       total_sectors_16 first and falls back to total_sectors_32 when it's
       zero. The unused field already reads 0 from the memset above. */
    if (total_sectors < 65536)
        bs->bpb.total_sectors_16 = (uint16_t)total_sectors;
    else
        bs->bpb.total_sectors_32 = total_sectors;

    /* Legacy/reference fields we never read back — see
       Appendix C: The Boot Sector's Leftovers for what each one meant */
    memcpy(bs->oem_name, "MYFORMAT", 8);
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

    /* The spec requires this on every FAT boot sector, bootable or not.
       We still leave boot_code zeroed, so don't actually boot from this
       disk: a BIOS that tried would jump straight into that zeroed region
       and execute whatever garbage happens to be sitting there. */
    bs->signature = 0xAA55;

    /* Write the boot sector to disk */
    block_device_write(device, 0, 1, bs);

    return 0;
}

static void format_fats(
    BlockDevice* device,
    BootSector* bs
)
{
    /* Allocate a FAT-sized buffer, zeroed — every cluster starts free */
    uint32_t fat_sectors = bs->bpb.fat_size_16;
    uint64_t fat_bytes = fat_sectors * bs->bpb.bytes_per_sector;
    uint8_t* fat = (uint8_t*)malloc(fat_bytes);
    memset(fat, 0, fat_bytes);

    /* Entries 0 and 1 are reserved: media descriptor (0xF00 | media),
       then end-of-chain marker (0xFFF), packed into the first 3 bytes */
    fat[0] = bs->bpb.media;
    fat[1] = 0xFF;
    fat[2] = 0xFF;

    /* Write the same buffer to every FAT copy */
    for (uint32_t copy = 0; copy < bs->bpb.num_fats; copy++)
    {
        uint32_t lba = bs->bpb.reserved_sector_count + (copy * fat_sectors);
        block_device_write(device, lba, fat_sectors, fat);
    }

    free(fat);
}

static void format_root_dir(BlockDevice* device, BootSector* bs)
{
    /* Locate the root directory */
    uint32_t fat_lba = bs->bpb.reserved_sector_count;
    uint32_t fat_sectors = bs->bpb.num_fats * bs->bpb.fat_size_16;
    uint32_t root_dir_lba = fat_lba + fat_sectors;

    /* Size it in whole sectors and allocate a zero-filled buffer */
    uint32_t count = ((bs->bpb.root_entry_count * DIRECTORY_ENTRY_SIZE + bs->bpb.bytes_per_sector - 1) / bs->bpb.bytes_per_sector);
    uint64_t total_bytes = count * bs->bpb.bytes_per_sector;
    uint8_t* zeros = (uint8_t*)malloc(total_bytes);
    memset(zeros, 0, total_bytes);

    /* Volume label entry */
    DirectoryEntry* label = (DirectoryEntry*)zeros;
    memcpy(label->name, bs->extended_bpb.volume_label, 11);
    label->attributes = FAT12_ATTR_VOLUME_ID;
    initialize_entry_timestamps(label);

    /* Write the root directory to disk */
    block_device_write(device, root_dir_lba, count, zeros);
    free(zeros);
}

int fat12_format(BlockDevice* device, FormatParams params)
{
    DBG_PRINT("[ fat12        ] --- fat12_format(label=\"%.11s\") ---\n", params.volume_label);
    BootSector bs;
    if (format_boot_sector(device, params.volume_label, &bs) != 0) return -1;
    format_fats(device, &bs);
    format_root_dir(device, &bs);

    return 0;
}

#include <stdlib.h>
#include <string.h>
#include "block_device.h"
#include "layout.h"
#include "fat12.h"
#include "debug.h"

/* Directory entry name[0] sentinels */
#define NAME_DELETED  0xE5
#define NAME_END      0x00

#define ROOT_DIR_CLUSTER 0

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

typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} DateTime;

/* ---- Forward declarations for internal helpers ---- */

static BootSector read_boot_sector(BlockDevice* device);
static void decode_8_3_name(const DirectoryEntry* raw, char* out);
static Timestamp decode_dos_timestamp(uint16_t time, uint16_t date);
static int next_active_entry(DirectoryEntry* entries, uint32_t count, uint32_t* offset, DirectoryEntry** out);
static int is_deleted_entry(const DirectoryEntry* entry);
static uint32_t data_cluster_to_lba(FAT12FS* fs, uint16_t cluster);
static uint16_t find_next_cluster(FAT12FS* fs, uint16_t cluster);
static void str_upper(char* s);
static void write_fat12_entry(uint8_t* fat, uint16_t cluster, uint16_t value);
static uint16_t dos_time_encode(const DateTime* dt);
static uint16_t dos_date_encode(const DateTime* dt);
static int find_free_entry(DirectoryEntry* entries, uint32_t count);
static void set_entry_name(DirectoryEntry* entry, const char* name);
static DirectoryEntry* read_root_directory(FAT12FS* fs, uint32_t* count);
static uint8_t* read_cluster_chain(FAT12FS* fs, uint16_t first_cluster, uint32_t* out_bytes);
static DirectoryEntry* read_directory_entries(FAT12FS* fs, uint16_t first_cluster, uint32_t* count);
static uint16_t allocate_chain(FAT12FS* fs, uint32_t needed_bytes, uint32_t* out_chain_bytes);
static void write_cluster_chain(FAT12FS* fs, uint16_t first_cluster, const uint8_t* data, uint32_t size);
static void write_fats(FAT12FS* fs);
static void set_file_timestamps(DirectoryEntry* entry);
static DirectoryEntry* find_by_name_in_entries(DirectoryEntry* entries, uint32_t count, const char* name);
static int extend_directory_chain(FAT12FS* fs, uint16_t parent_first_cluster, DirectoryEntry** dir, uint32_t* total);
static int find_or_extend_slot(FAT12FS* fs, DirectoryEntry** entries, uint32_t* count, uint16_t parent_cluster);
static void write_directory(FAT12FS* fs, uint16_t parent_cluster, DirectoryEntry* entries, uint32_t count);
static int resolve_path(FAT12FS* fs, const char* path, DirectoryEntry* out);
static DirectoryEntry* load_parent_directory(FAT12FS* fs, const char* parent_path, uint32_t* out_count, uint16_t* out_first_cluster);
static int split_parent_name(const char* path, char* parent, int parent_size, char* name, int name_size);
static int is_dot_entry(const char* name);
static int create_dir_entry(FAT12FS* fs, const char* path, uint16_t first_cluster, uint32_t file_size);
static int create_directory_contents(FAT12FS* fs, uint16_t cluster, uint16_t parent_cluster);

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
    fs->fat_lba = fs->bs.bpb.reserved_sector_count;
    fs->fat_sectors = fs->bs.bpb.num_fats * fs->bs.bpb.fat_size_16;
    fs->root_dir_lba = fs->fat_lba + fs->fat_sectors;
    fs->root_dir_sectors = ((fs->bs.bpb.root_entry_count * sizeof(DirectoryEntry)
                           + fs->bs.bpb.bytes_per_sector - 1)
                          / fs->bs.bpb.bytes_per_sector);
    fs->first_data_lba = fs->root_dir_lba + fs->root_dir_sectors;
    fs->data_cluster_count = total_data_clusters(fs);

    DBG_PRINT("[ fat12 ] FAT start LBA: %u\n", fs->fat_lba);
    DBG_PRINT("[ fat12 ] FAT size (sectors): %u\n", fs->fat_sectors);
    DBG_PRINT("[ fat12 ] Root dir start LBA: %u\n", fs->root_dir_lba);
    DBG_PRINT("[ fat12 ] Root dir size (sectors): %u\n", fs->root_dir_sectors);
    DBG_PRINT("[ fat12 ] First data LBA: %u\n", fs->first_data_lba);
    DBG_PRINT("[ fat12 ] Data cluster count: %u\n", fs->data_cluster_count);

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
    DirectoryEntry* entries;
    /* --- NEW: branch on resolved entry --- */
    if (resolved.first_cluster == ROOT_DIR_CLUSTER)
    {
        entries = read_root_directory(fs, &count);
    }
    else
    {
        entries = read_directory_entries(
            fs, resolved.first_cluster, &count);
    }

    if (entries == NULL) return NULL;
    /* --- END NEW --- */

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

struct File
{
    FAT12FS* fs;
    uint8_t* data;
    uint32_t size;
    uint32_t position;
    uint16_t first_cluster;
    int mode;
    char* parent_path;
};

File* fat12_open(FAT12FS* fs, const char* path, const char* mode)
{
    /* Only absolute paths are supported: every path must start at "/" */
    if (path[0] != '/') return NULL;

    if (mode[0] == 'w')
    {
        DirectoryEntry resolved;
        if (resolve_path(fs, path, &resolved) == 0)
        {
            return NULL;
        }

        File* file = (File*)malloc(sizeof(File));
        file->fs = fs;
        file->data = NULL;
        file->size = 0;
        file->position = 0;
        file->first_cluster = 0;
        file->mode = 'w';
        file->parent_path = strdup(path);
        return file;
    }

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
            uint16_t first = allocate_chain(
                file->fs, file->size, &chain_bytes);

            if (first != 0 && chain_bytes >= file->size)
            {
                write_cluster_chain(file->fs,
                    first, file->data, file->size);

                write_fats(file->fs);

                create_dir_entry(file->fs,
                    file->parent_path,
                    first, file->size);
            }
            else
            {
                result = -1;
            }
        }
        else
        {
            create_dir_entry(file->fs,
                file->parent_path, 0, 0);

            write_fats(file->fs);
        }

        free(file->parent_path);
    }

    free(file->data);
    free(file);
    return result;
}

/* FAT entry special values */
#define CLUSTER_END               0xFFF

int fat12_mkdir(FAT12FS* fs, const char* path)
{
    /* --- Check target does not exist --- */
    DirectoryEntry resolved;

    if (resolve_path(fs, path, &resolved) == 0)
        return -1;

    /* --- Split path into parent path and leaf name --- */
    char parent_path[256];
    char dirname[64];

    if (split_parent_name(path, parent_path, sizeof(parent_path),
                          dirname, sizeof(dirname)) < 0)
    {
        return -1;
    }

    /* --- Load parent directory entries --- */
    uint32_t count;
    uint16_t parent_cluster;
    DirectoryEntry* entries = load_parent_directory(
        fs, parent_path, &count, &parent_cluster);
    if (entries == NULL)
    {
        return -1;
    }

    /* --- Find or extend a free slot in the parent --- */
    int slot = find_or_extend_slot(
        fs, &entries, &count, parent_cluster);
    if (slot < 0)
    {
        free(entries);
        return -1;
    }

    /* --- Allocate a cluster and mark it end-of-chain --- */
    uint32_t cluster_bytes =
        fs->bs.bpb.sectors_per_cluster *
        fs->bs.bpb.bytes_per_sector;
    uint32_t chain_bytes;
    uint16_t cluster = allocate_chain(fs, cluster_bytes, &chain_bytes);
    if (cluster == 0)
    {
        free(entries);
        return -1;
    }

    /* --- Stamp . and .. into the new cluster --- */
    if (create_directory_contents(
            fs, cluster, parent_cluster) != 0)
    {
        free(entries);
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
    write_directory(fs,
        parent_cluster, entries, count);

    write_fats(fs);

    /* --- Cleanup --- */
    free(entries);
    return 0;
}

#define CLUSTER_FIRST             0x002
#define CLUSTER_FREE              0x000

static void free_cluster_chain(FAT12FS* fs, uint16_t cluster)
{
    while (cluster >= CLUSTER_FIRST && (uint32_t)(cluster - 2) < fs->data_cluster_count)
    {
        uint16_t next = find_next_cluster(fs, cluster);
        DBG_PRINT("[ fat12 ] free cluster %u%s\n", cluster, (uint32_t)(next - 2) >= fs->data_cluster_count ? " (last)" : "");
        write_fat12_entry(fs->fat, cluster, CLUSTER_FREE);
        cluster = next;
    }
}

static void mark_entry_deleted(DirectoryEntry* entry)
{
    entry->name[0] = (char)NAME_DELETED;
}

static int is_directory_empty(FAT12FS* fs, uint16_t cluster)
{
    uint32_t count;
    DirectoryEntry* entries = read_directory_entries(fs, cluster, &count);
    if (entries == NULL)
        return -1;

    int result = 1;
    for (uint32_t i = 0; i < count; i++)
    {
        if (entries[i].name[0] == NAME_END)
            break;
        if (is_deleted_entry(&entries[i]))
            continue;
        if (entries[i].attr == FAT12_ATTR_LONG_NAME)
            continue;

        char entry_name[13];
        decode_8_3_name(&entries[i], entry_name);
        if (is_dot_entry(entry_name))
            continue;

        result = 0;
        break;
    }

    free(entries);
    return result;
}

int fat12_remove(FAT12FS* fs, const char* path)
{
    /* --- Guard: root cannot be deleted --- */
    if (strcmp(path, "/") == 0) return -1;

    /* --- Split path into parent path and filename --- */
    char parent_path[256];
    char filename[64];

    if (split_parent_name(path, parent_path, sizeof(parent_path),
                          filename, sizeof(filename)) < 0)
    {
        return -1;
    }

    /* --- Guard: the dot entries cannot be deleted --- */
    if (is_dot_entry(filename))
    {
        return -1;
    }

    /* --- Load parent directory entries --- */
    uint32_t count;
    uint16_t parent_cluster;
    DirectoryEntry* entries = load_parent_directory(fs, parent_path, &count, &parent_cluster);
    if (entries == NULL)
    {
        return -1;
    }

    /* --- Find the entry --- */
    DirectoryEntry* entry = find_by_name_in_entries(entries, count, filename);
    if (entry == NULL)
    {
        free(entries);
        return -1;
    }

    /* --- Guard: read-only files cannot be deleted --- */
    if (entry->attr & FAT12_ATTR_READ_ONLY)
    {
        free(entries);
        return -1;
    }

    /* --- If it is a directory, verify it is empty --- */
    if (entry->attr & FAT12_ATTR_DIRECTORY)
    {
        if (is_directory_empty(fs, entry->first_cluster) <= 0)
        {
            free(entries);
            return -1;
        }
    }

    mark_entry_deleted(entry);
    free_cluster_chain(fs, entry->first_cluster);

    /* Flush directory first, then FAT: if we crash between
       the two, we only leak clusters (repairable) instead of
       leaving a dangling reference (corruption) */
    write_directory(fs, parent_cluster, entries, count);
    write_fats(fs);

    /* --- Cleanup --- */
    free(entries);
    return 0;
}

static void update_dotdot(
    FAT12FS* fs,
    uint16_t dir_cluster,
    uint16_t new_parent_cluster
)
{
    uint32_t cluster_bytes =
        fs->bs.bpb.sectors_per_cluster *
        fs->bs.bpb.bytes_per_sector;
    uint32_t lba = data_cluster_to_lba(fs, dir_cluster);

    uint8_t* buf = malloc(cluster_bytes);
    block_device_read(fs->device, lba,
        fs->bs.bpb.sectors_per_cluster, buf);

    DirectoryEntry* entries =
        (DirectoryEntry*)buf;
    entries[1].first_cluster =
        new_parent_cluster;

    block_device_write(fs->device, lba,
        fs->bs.bpb.sectors_per_cluster, buf);
    free(buf);
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

    if (split_parent_name(old_path, src_parent_path,
                          sizeof(src_parent_path),
                          src_name, sizeof(src_name)) < 0)
    {
        return -1;
    }

    if (split_parent_name(new_path, dst_parent_path,
                          sizeof(dst_parent_path),
                          dst_name, sizeof(dst_name)) < 0)
    {
        return -1;
    }

    /* --- Guard: the dot entries cannot be moved or renamed --- */
    if (is_dot_entry(src_name) || is_dot_entry(dst_name))
    {
        return -1;
    }

    /* --- Load source parent directory and find source entry --- */
    uint32_t src_count;
    uint16_t src_parent_cluster;
    DirectoryEntry* src_entries = load_parent_directory(
        fs, src_parent_path,
        &src_count, &src_parent_cluster);
    if (src_entries == NULL)
    {
        return -1;
    }

    DirectoryEntry* src_entry = find_by_name_in_entries(
        src_entries, src_count, src_name);
    if (src_entry == NULL)
    {
        free(src_entries);
        return -1;
    }

    uint32_t src_index = (uint32_t)(src_entry - src_entries);

    /* --- Cycle detection: moving a directory into itself --- */
    if (src_entry->attr & FAT12_ATTR_DIRECTORY)
    {
        char* old_up = strdup(old_path);
        char* new_up = strdup(new_path);
        str_upper(old_up);
        str_upper(new_up);

        size_t old_len = strlen(old_up);
        int is_subdir = (strncmp(old_up, new_up, old_len) == 0 &&
                         new_up[old_len] == '/');

        free(old_up);
        free(new_up);

        if (is_subdir)
        {
            free(src_entries);
            return -1;
        }
    }

    /* --- Load destination parent directory --- */
    uint32_t dst_count;
    uint16_t dst_parent_cluster;
    DirectoryEntry* dst_entries = load_parent_directory(
        fs, dst_parent_path,
        &dst_count, &dst_parent_cluster);
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

    /* --- Check destination does not already exist --- */
    DirectoryEntry* check_entries =
        same_parent ? src_entries : dst_entries;
    uint32_t check_count =
        same_parent ? src_count : dst_count;

    if (find_by_name_in_entries(
            check_entries, check_count, dst_name) != NULL)
    {
        free(dst_entries);
        free(src_entries);
        return -1;
    }

    /* --- Ensure space and find free slot in destination --- */
    DirectoryEntry* dir_entries =
        same_parent ? src_entries : dst_entries;
    uint32_t dir_count =
        same_parent ? src_count : dst_count;

    int dst_slot = find_or_extend_slot(
        fs, &dir_entries, &dir_count, dst_parent_cluster);
    if (dst_slot < 0)
    {
        free(dst_entries);
        free(src_entries);
        return -1;
    }

    /* --- Re-sync pointers after find_or_extend_slot may have reallocated --- */
    if (same_parent)
    {
        src_entries = dir_entries;
        src_count = dir_count;
        src_entry = &src_entries[src_index];
    }
    else
    {
        dst_entries = dir_entries;
        dst_count = dir_count;
    }

    /* --- Copy entry, set new name and timestamps --- */
    memcpy(&dir_entries[dst_slot], src_entry,
           sizeof(DirectoryEntry));

    set_entry_name(&dir_entries[dst_slot], dst_name);

    set_file_timestamps(&dir_entries[dst_slot]);

    /* --- Mark source deleted --- */
    src_entry->name[0] = (char)NAME_DELETED;

    /* --- New reference first, then removal: a crash between the two
         leaves a safe duplicate instead of losing the entry --- */
    if (!same_parent)
    {
        write_directory(fs, dst_parent_cluster,
            dst_entries, dst_count);
    }

    /* --- Update .. for cross-directory directory moves --- */
    if ((src_entry->attr & FAT12_ATTR_DIRECTORY) && !same_parent)
    {
        update_dotdot(fs,
            src_entry->first_cluster,
            dst_parent_cluster);
    }

    write_directory(fs, src_parent_cluster,
        src_entries, src_count);

    write_fats(fs);

    /* --- Cleanup --- */
    free(src_entries);
    free(dst_entries);
    return 0;
}

/* ---- Internal helpers ---- */

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

        if (raw->name[0] == NAME_END) return 0;

        (*offset)++;

        if (is_deleted_entry(raw)) continue;
        if (raw->attr == FAT12_ATTR_LONG_NAME) continue;

        *out = raw;
        return 1;
    }

    return 0;
}

static uint32_t data_cluster_to_lba(FAT12FS* fs, uint16_t cluster)
{
    uint32_t lba = ((cluster - 2) * fs->bs.bpb.sectors_per_cluster)
                   + fs->first_data_lba;
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

static void write_fat12_entry(
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

static uint16_t dos_time_encode(const DateTime* dt)
{
    return (dt->hour << 11) | (dt->minute << 5) | (dt->second / 2);
}

static uint16_t dos_date_encode(const DateTime* dt)
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

static uint16_t allocate_chain(
    FAT12FS* fs,
    uint32_t needed_bytes,
    uint32_t* out_chain_bytes
)
{
    uint32_t cluster_bytes = fs->bs.bpb.sectors_per_cluster *
                             fs->bs.bpb.bytes_per_sector;
    uint32_t needed_clusters =
        (needed_bytes + cluster_bytes - 1) / cluster_bytes;

    /* Guard: needed_bytes can be 0 for empty files; skip allocation. */
    if (needed_clusters == 0) return 0;

    DBG_PRINT("[ fat12 ] allocating %u cluster(s) for %u bytes\n", needed_clusters, needed_bytes);

    /* Safe: a 4 MB disk has about 4067 clusters, well within uint32_t. */
    uint16_t* clusters = (uint16_t*)malloc(needed_clusters * sizeof(uint16_t));

    /* Only the volume's own data clusters are allocatable — the loop stops
       at the last one instead of scanning the whole FAT table. */
    uint32_t total_sectors = fs->bs.bpb.total_sectors_16
        ? fs->bs.bpb.total_sectors_16
        : fs->bs.bpb.total_sectors_32;
    uint16_t last_data_cluster =
        (total_sectors - fs->first_data_lba)
        / fs->bs.bpb.sectors_per_cluster + 1;

    /* Phase 1: scan the FAT and collect free cluster numbers. */
    uint32_t found = 0;
    for (uint16_t c = CLUSTER_FIRST; c <= last_data_cluster && found < needed_clusters; c++)
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
    for (uint32_t i = 0; i < needed_clusters - 1; i++)
    {
        DBG_PRINT("[ fat12 ] allocate cluster %u%s\n", clusters[i], i == 0 ? " (first)" : "");
        write_fat12_entry(fs->fat, clusters[i], clusters[i + 1]);
    }
    DBG_PRINT("[ fat12 ] allocate cluster %u\n", clusters[needed_clusters - 1]);
    write_fat12_entry(fs->fat, clusters[needed_clusters - 1], CLUSTER_END);

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
    uint32_t cluster_bytes = fs->bs.bpb.sectors_per_cluster *
                             fs->bs.bpb.bytes_per_sector;
    uint32_t remaining = size;
    uint16_t cluster = first_cluster;

    while (cluster >= CLUSTER_FIRST && (uint32_t)(cluster - 2) < fs->data_cluster_count && remaining > 0)
    {
        uint32_t lba = data_cluster_to_lba(fs, cluster);
        DBG_PRINT("[ fat12 ] write: cluster %u\n", cluster);
        uint32_t chunk = remaining < cluster_bytes
                       ? remaining : cluster_bytes;

        if (chunk < cluster_bytes)
        {
            uint8_t* pad = calloc(1, cluster_bytes);
            memcpy(pad, data + (size - remaining), chunk);
            block_device_write(fs->device, lba,
                fs->bs.bpb.sectors_per_cluster, pad);
            free(pad);
        }
        else
        {
            block_device_write(fs->device, lba,
                fs->bs.bpb.sectors_per_cluster, data + (size - remaining));
        }

        remaining -= chunk;
        cluster = find_next_cluster(fs, cluster);
    }
}

static void write_fats(FAT12FS* fs)
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

static int extend_directory_chain(
    FAT12FS* fs,
    uint16_t parent_first_cluster,
    DirectoryEntry** dir,
    uint32_t* total
)
{
    uint16_t cluster = parent_first_cluster;
    while (cluster >= CLUSTER_FIRST && (uint32_t)(cluster - 2) < fs->data_cluster_count)
    {
        uint16_t next = find_next_cluster(fs, cluster);
        if ((uint32_t)(next - 2) >= fs->data_cluster_count) break;
        cluster = next;
    }

    uint32_t cluster_bytes =
        fs->bs.bpb.sectors_per_cluster *
        fs->bs.bpb.bytes_per_sector;

    uint32_t chain_bytes;
    uint16_t new_cluster = allocate_chain(fs, cluster_bytes, &chain_bytes);
    if (new_cluster == 0) return -1;

    write_fat12_entry(fs->fat, cluster, new_cluster);

    uint32_t lba = data_cluster_to_lba(fs, new_cluster);
    uint8_t* zb = calloc(1, cluster_bytes);
    block_device_write(fs->device, lba,
        fs->bs.bpb.sectors_per_cluster, zb);
    free(zb);

    uint32_t new_slots = cluster_bytes / sizeof(DirectoryEntry);
    DirectoryEntry* new_dir = (DirectoryEntry*)realloc(
        *dir, (*total + new_slots) * sizeof(DirectoryEntry));
    memset(new_dir + *total, 0, new_slots * sizeof(DirectoryEntry));
    *dir = new_dir;
    *total += new_slots;

    return 0;
}

static int find_or_extend_slot(
    FAT12FS* fs,
    DirectoryEntry** entries,
    uint32_t* count,
    uint16_t parent_cluster
)
{
    int slot = find_free_entry(*entries, *count);
    if (slot >= 0) return slot;

    if (parent_cluster == ROOT_DIR_CLUSTER) return -1;

    if (extend_directory_chain(fs, parent_cluster, entries, count) != 0)
        return -1;

    return find_free_entry(*entries, *count);
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
    /* Only absolute paths are supported: every path must start at "/" */
    if (path[0] != '/') return -1;

    /* ---- Step 1: Load Root Directory as Current Directory ---- */
    uint32_t dir_count;
    DirectoryEntry* dir_entries = read_root_directory(fs, &dir_count);
    if (dir_entries == NULL)
    {
        /* Root directory read failed */
        return -1;
    }

    /* Remove leading slash */
    const char* p = path;
    if (*p == '/') p++;

    /* Return the Root Directors if the path contains only "/" */
    if (*p == '\0')
    {
        out->attr = FAT12_ATTR_DIRECTORY;
        out->first_cluster = ROOT_DIR_CLUSTER;
        free(dir_entries);
        return 0;
    }

    while (*p)
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
        DirectoryEntry* found = find_by_name_in_entries(dir_entries, dir_count, name);

        if (found == NULL)
        {
            /* Step 4: Stop: Doesn't exist */
            free(dir_entries);
            return -1;
        }

        /* ---- Step 5: Last component? ---- */
        int is_last = (*p == '\0');
        if (*p == '/') p++;
        if (is_last)
        {
            /* Step 6: Return entry (file or directory) */
            *out = *found;
        }
        else
        {
            /* Step 7: Load subdirectory as Current Directory */
            if (!(found->attr & FAT12_ATTR_DIRECTORY))
            {
                /* Stop: Entry is not a directory */
                free(dir_entries);
                return -1;
            }

            uint32_t sub_count;
            DirectoryEntry* sub_entries = read_directory_entries(fs, found->first_cluster, &sub_count);
            free(dir_entries);
            dir_entries = sub_entries;
            dir_count = sub_count;
        }
    }

    free(dir_entries);
    return 0;
}

static DirectoryEntry* load_parent_directory(
    FAT12FS* fs,
    const char* parent_path,
    uint32_t* out_count,
    uint16_t* out_first_cluster
)
{
    if (parent_path[0] == '/' && parent_path[1] == '\0')
    {
        *out_first_cluster = ROOT_DIR_CLUSTER;
        return read_root_directory(fs, out_count);
    }

    DirectoryEntry resolved;
    if (resolve_path(fs, parent_path, &resolved) != 0)
        return NULL;

    *out_first_cluster = resolved.first_cluster;

    return read_directory_entries(fs, *out_first_cluster, out_count);
}

static int split_parent_name(
    const char* path,
    char* parent,
    int parent_size,
    char* name,
    int name_size
)
{
    /* Only absolute paths are supported: every path must start at "/" */
    if (path[0] != '/') return -1;

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
    if (name_start[0] == '\0') return -1;
    if (strlen(name_start) >= (size_t)name_size) return -1;
    strncpy(name, name_start, name_size - 1);
    name[name_size - 1] = '\0';
    str_upper(name);

    return 0;
}

static int is_dot_entry(const char* name)
{
    return strcmp(name, ".") == 0 ||
           strcmp(name, "..") == 0;
}

static int create_dir_entry(
    FAT12FS* fs,
    const char* path,
    uint16_t first_cluster,
    uint32_t file_size
)
{
    char parent_dir[256];
    char name[13];

    if (split_parent_name(path, parent_dir, sizeof(parent_dir),
                          name, sizeof(name)) != 0)
    {
        return -1;
    }

    uint32_t count;
    uint16_t parent_first_cluster;
    DirectoryEntry* entries = load_parent_directory(
        fs, parent_dir,
        &count, &parent_first_cluster);
    if (entries == NULL) return -1;

    /* the only change from Chapter 6: find_free_entry -> find_or_extend_slot,
       so a full subdirectory grows instead of failing */
    int slot = find_or_extend_slot(
        fs, &entries, &count, parent_first_cluster);
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

    write_directory(fs,
        parent_first_cluster, entries, count);

    free(entries);
    return 0;
}

static int create_directory_contents(
    FAT12FS* fs,
    uint16_t cluster,
    uint16_t parent_cluster
)
{
    uint32_t cluster_bytes =
        fs->bs.bpb.sectors_per_cluster *
        fs->bs.bpb.bytes_per_sector;

    uint8_t* buf = calloc(1, cluster_bytes);

    DirectoryEntry* d = (DirectoryEntry*)buf;

    memcpy(d[0].name, ".          ", 11);
    d[0].attr = FAT12_ATTR_DIRECTORY;
    d[0].first_cluster = cluster;

    memcpy(d[1].name, "..         ", 11);
    d[1].attr = FAT12_ATTR_DIRECTORY;
    d[1].first_cluster = parent_cluster;

    write_cluster_chain(fs, cluster, buf, cluster_bytes);
    free(buf);
    return 0;
}

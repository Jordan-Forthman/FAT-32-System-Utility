#include "fat32.h"
#include <stdlib.h>
#include <ctype.h>

/*
 * Convert a user-facing name ("hello.txt") to the on-disk 8.3 form
 * ("HELLO   TXT"): 8 bytes of name then 3 of extension, space padded, upper
 * case, with no dot stored between them. "." and ".." are stored verbatim.
 * Writing the name with its dot, as this once did, produces entries that any
 * real FAT driver rejects as malformed.
 */
void to_83_name(const char* input, char out[11]) {
    memset(out, ' ', 11);

    if (strcmp(input, ".") == 0)  { out[0] = '.'; return; }
    if (strcmp(input, "..") == 0) { out[0] = '.'; out[1] = '.'; return; }

    const char* dot = strrchr(input, '.');
    size_t name_len = dot ? (size_t)(dot - input) : strlen(input);
    if (name_len > 8) name_len = 8;
    for (size_t i = 0; i < name_len; i++)
        out[i] = (char)toupper((unsigned char)input[i]);

    if (dot) {
        size_t ext_len = strlen(dot + 1);
        if (ext_len > 3) ext_len = 3;
        for (size_t i = 0; i < ext_len; i++)
            out[8 + i] = (char)toupper((unsigned char)dot[1 + i]);
    }
}

// Reverse of to_83_name, for display. out must hold at least 13 bytes.
void from_83_name(const uint8_t raw[11], char* out) {
    int n = 8, e = 3, k = 0;
    while (n > 0 && raw[n - 1] == ' ') n--;
    while (e > 0 && raw[8 + e - 1] == ' ') e--;

    for (int i = 0; i < n; i++) out[k++] = (char)raw[i];
    if (e > 0) {
        out[k++] = '.';
        for (int i = 0; i < e; i++) out[k++] = (char)raw[8 + i];
    }
    out[k] = '\0';
}

// Compare a raw on-disk name against a user-supplied one.
int name_matches(const uint8_t raw[11], const char* name) {
    char want[11];
    to_83_name(name, want);
    return memcmp(raw, want, 11) == 0;
}

// Calculate first data sector
uint32_t get_first_data_sector() {
    return g_bpb.BPB_RsvdSecCnt + (g_bpb.BPB_NumFATs * g_bpb.BPB_FATSz32);
}

// Cluster N byte offset in image
uint32_t get_cluster_byte_offset(uint32_t cluster) {
    uint32_t first_data_sec = get_first_data_sector();
    uint32_t sec_offset = ((cluster - 2) * g_bpb.BPB_SecPerClus) + first_data_sec;
    return sec_offset * g_bpb.BPB_BytsPerSec;
}

// Byte offset of a cluster's entry in FAT copy `fat_index` (0 = first FAT).
uint32_t get_fat_copy_offset(uint32_t cluster, uint32_t fat_index) {
    uint32_t fat_start_sec = g_bpb.BPB_RsvdSecCnt +
                             (fat_index * g_bpb.BPB_FATSz32);
    return (fat_start_sec * g_bpb.BPB_BytsPerSec) + (cluster * 4);
}

// FAT entry offset for cluster, in the first FAT. Reads use this one.
uint32_t get_fat_entry_offset(uint32_t cluster) {
    return get_fat_copy_offset(cluster, 0);
}

// Read next cluster from FAT
uint32_t get_next_cluster(uint32_t current) {
    uint32_t offset = get_fat_entry_offset(current);
    long saved_pos = ftell(g_fp);
    fseek(g_fp, offset, SEEK_SET);
    uint32_t next;
    fread(&next, sizeof(uint32_t), 1, g_fp);
    fseek(g_fp, saved_pos, SEEK_SET);
    if (next >= 0x0FFFFFF8) return EOC_MARKER;
    return next;
}

// Linear search for free cluster in FAT
uint32_t find_free_cluster() {
    uint32_t fat_size = g_bpb.BPB_FATSz32 * g_bpb.BPB_BytsPerSec / 4;
    for (uint32_t cluster = 2; cluster < fat_size; cluster++) {
        uint32_t val = get_next_cluster(cluster);
        if (val == FREE_CLUSTER) return cluster;
    }
    fprintf(stderr, "No free clusters\n");
    return 0;  // Error
}

// Wrapper to preserve fp position
void wrapper_fp(void (*func)(), long* saved_pos) {
    if (saved_pos) *saved_pos = ftell(g_fp);
    func();
    if (saved_pos) fseek(g_fp, *saved_pos, SEEK_SET);
}

void trim_trailing_spaces(char* name, int len) {
    for (int i = len - 1; i >= 0; i--) {
        if (name[i] != ' ') {
            name[i + 1] = '\0';
            return;
        }
    }
    name[0] = '\0';
}

// Find entry by name in cluster chain
DirEntry_t* find_entry(const char* name, uint32_t start_cluster) {
    uint32_t cluster = start_cluster;
    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;

    while (cluster != EOC_MARKER && cluster >= 2) {
        uint32_t offset = get_cluster_byte_offset(cluster);
        if (offset == 0) return NULL;

        long saved_pos = ftell(g_fp);
        fseek(g_fp, offset, SEEK_SET);

        for (uint32_t i = 0; i < clus_size; i += ENTRY_SIZE) {
            DirEntry_t entry;
            if (fread(&entry, sizeof(DirEntry_t), 1, g_fp) != 1) {
                fseek(g_fp, saved_pos, SEEK_SET);
                return NULL;
            }

            // Skip invalid
            if (entry.DIR_Name[0] == 0x00) break;
            if (entry.DIR_Name[0] == 0xE5) continue;
            if (entry.DIR_Attr == ATTR_LONG_NAME) continue;

            if (name_matches(entry.DIR_Name, name)) {
                fseek(g_fp, saved_pos, SEEK_SET);  // Restore
                DirEntry_t* found = malloc(sizeof(DirEntry_t));
                if (found) *found = entry;
                return found;  // Caller frees
            }
        }

        cluster = get_next_cluster(cluster);
    }

    return NULL;
}

// Find byte offset for free entry slot in cluster chain
long find_free_entry_slot(uint32_t start_cluster) {
    uint32_t cluster = start_cluster;
    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;

    while (cluster != EOC_MARKER && cluster >= 2) {
        uint32_t offset = get_cluster_byte_offset(cluster);
        long base_offset = offset;  // For return

        long saved_pos = ftell(g_fp);
        fseek(g_fp, offset, SEEK_SET);

        for (uint32_t i = 0; i < clus_size; i += ENTRY_SIZE) {
            uint8_t first_byte;
            fread(&first_byte, 1, 1, g_fp);
            if (first_byte == 0x00 || first_byte == 0xE5) {
                fseek(g_fp, saved_pos, SEEK_SET);
                return base_offset + i;  // Absolute offset
            }
            fseek(g_fp, ENTRY_SIZE - 1, SEEK_CUR);  // Skip rest
        }

        uint32_t next = get_next_cluster(cluster);
        if (next == cluster) break;  // Loop
        cluster = next;
    }
    return -1;
}

// Write entry at offset
void write_entry(long offset, DirEntry_t* entry) {
    long saved_pos = ftell(g_fp);
    fseek(g_fp, offset, SEEK_SET);
    fwrite(entry, sizeof(DirEntry_t), 1, g_fp);
    fflush(g_fp);  // Ensure persistent
    fseek(g_fp, saved_pos, SEEK_SET);
}

// Allocate cluster, set EOC in FAT (write to first FAT)
uint32_t allocate_cluster() {
    uint32_t new_clus = find_free_cluster();
    if (new_clus == 0) {
        printf("Error: No free clusters\n");
        return 0;
    }

    // Mark end-of-chain in every FAT copy
    long saved_pos = ftell(g_fp);
    set_fat_entry(new_clus, EOC_MARKER);

    // Clear new cluster
    uint32_t clus_offset = get_cluster_byte_offset(new_clus);
    fseek(g_fp, clus_offset, SEEK_SET);
    char zero[ENTRY_SIZE] = {0};
    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;
    for (uint32_t i = 0; i < clus_size; i += ENTRY_SIZE) {
        fwrite(zero, ENTRY_SIZE, 1, g_fp);
    }
    fflush(g_fp);

    fseek(g_fp, saved_pos, SEEK_SET);
    return new_clus;
}

// Init dir entry
void init_dir_entry(DirEntry_t* entry, const char* name, uint8_t attr, uint32_t first_clus, uint32_t size) {
    memset(entry, 0, sizeof(DirEntry_t));
    to_83_name(name, (char*)entry->DIR_Name);
    entry->DIR_Attr = attr;
    entry->DIR_FstClusHI = (first_clus >> 16) & 0xFFFF;
    entry->DIR_FstClusLO = first_clus & 0xFFFF;
    entry->DIR_FileSize = size;
}

// Find free table slot (-1 if full)
int find_free_fd() {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (g_open_table[i].fd == 0) return i;
    }
    return -1;
}

// Parse mode string to bitmask
int parse_mode(const char* flags) {
    if (strcmp(flags, "-r") == 0) return MODE_READ;
    if (strcmp(flags, "-w") == 0) return MODE_WRITE;
    if (strcmp(flags, "-rw") == 0 || strcmp(flags, "-wr") == 0) return MODE_READ | MODE_WRITE;
    return 0;
}

/*
 * Read up to req_size bytes starting at `offset`.
 *
 * The chain is walked to the cluster holding the start offset first, then read
 * forward from the top of each following cluster. The previous version
 * recomputed a within-cluster skip on every iteration, which went negative
 * once past the first cluster and seeked backwards out of the cluster, so
 * reads spanning a cluster boundary returned the wrong bytes.
 */
char* read_file_data(uint32_t first_clus, long offset, uint32_t req_size, uint32_t file_size, uint32_t* out_len) {
    *out_len = 0;
    if (first_clus < 2) return NULL;

    char* buf = malloc(req_size + 1);
    if (!buf) return NULL;
    memset(buf, 0, req_size + 1);

    if (offset < 0) offset = 0;
    if ((uint32_t)offset >= file_size || req_size == 0) return buf;

    // Never read past the end of the file
    uint32_t remaining = file_size - (uint32_t)offset;
    if (req_size > remaining) req_size = remaining;

    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;
    uint32_t cluster = first_clus;
    long saved = ftell(g_fp);

    // Walk the chain to the cluster containing `offset`
    uint32_t skip = (uint32_t)offset;
    while (skip >= clus_size && cluster != EOC_MARKER && cluster >= 2) {
        cluster = get_next_cluster(cluster);
        skip -= clus_size;
    }

    uint32_t read_bytes = 0;
    while (read_bytes < req_size && cluster != EOC_MARKER && cluster >= 2) {
        uint32_t in_clus = (read_bytes == 0) ? skip : 0;
        uint32_t to_read = MIN(clus_size - in_clus, req_size - read_bytes);

        fseek(g_fp, get_cluster_byte_offset(cluster) + in_clus, SEEK_SET);
        if (fread(buf + read_bytes, 1, to_read, g_fp) != to_read) break;

        read_bytes += to_read;
        cluster = get_next_cluster(cluster);
    }

    fseek(g_fp, saved, SEEK_SET);
    *out_len = read_bytes;
    return buf;
}

/*
 * The FSInfo sector caches a free-cluster count and a next-free hint. This
 * tool does not maintain running totals, so rather than leave a stale count
 * behind (which fsck reports as "free cluster summary wrong"), mark both
 * fields 0xFFFFFFFF, the value the spec defines as "unknown". Drivers then
 * recompute them from the FAT.
 */
static void invalidate_fsinfo(void) {
    if (g_bpb.BPB_FSInfo == 0 || g_bpb.BPB_FSInfo == 0xFFFF) return;

    long saved = ftell(g_fp);
    long base = (long)g_bpb.BPB_FSInfo * g_bpb.BPB_BytsPerSec;
    uint32_t unknown = 0xFFFFFFFF;

    fseek(g_fp, base + 488, SEEK_SET);   // FSI_Free_Count
    fwrite(&unknown, sizeof(unknown), 1, g_fp);
    fseek(g_fp, base + 492, SEEK_SET);   // FSI_Nxt_Free
    fwrite(&unknown, sizeof(unknown), 1, g_fp);

    fflush(g_fp);
    fseek(g_fp, saved, SEEK_SET);
}

/*
 * Update a FAT entry in every FAT copy. A FAT32 volume normally carries two
 * FATs that must agree; writing only the first leaves the image inconsistent
 * ("FATs differ" from fsck) even though this tool would still read it back
 * correctly.
 */
void set_fat_entry(uint32_t cluster, uint32_t value) {
    long saved = ftell(g_fp);

    for (uint32_t fat = 0; fat < g_bpb.BPB_NumFATs; fat++) {
        fseek(g_fp, get_fat_copy_offset(cluster, fat), SEEK_SET);
        fwrite(&value, sizeof(uint32_t), 1, g_fp);
    }

    fflush(g_fp);
    fseek(g_fp, saved, SEEK_SET);
    invalidate_fsinfo();
}

// Helper: Clear a FAT chain (used by rm)
void clear_fat_chain(uint32_t first_clus) {
    uint32_t curr = first_clus;
    while (curr != EOC_MARKER && curr >= 2) {
        uint32_t next = get_next_cluster(curr);
        set_fat_entry(curr, FREE_CLUSTER);
        curr = next;
    }
}

// Helper: Find entry offset
void delete_entry_by_name(uint32_t dir_clus, char* name) {
    uint32_t cluster = dir_clus;
    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;

    while (cluster != EOC_MARKER && cluster >= 2) {
        uint32_t offset = get_cluster_byte_offset(cluster);
        long saved_pos = ftell(g_fp);
        fseek(g_fp, offset, SEEK_SET);

        for (uint32_t i = 0; i < clus_size; i += ENTRY_SIZE) {
            DirEntry_t entry;
            fread(&entry, sizeof(DirEntry_t), 1, g_fp);
            
            if (entry.DIR_Name[0] == 0x00) break; 
            if (entry.DIR_Name[0] == 0xE5) continue;
            
            if (name_matches(entry.DIR_Name, name)) {
                fseek(g_fp, -(long)sizeof(DirEntry_t), SEEK_CUR);
                uint8_t tombstone = 0xE5;
                fwrite(&tombstone, 1, 1, g_fp);
                fflush(g_fp);
                fseek(g_fp, saved_pos, SEEK_SET);
                return;
            }
        }
        cluster = get_next_cluster(cluster);
        fseek(g_fp, saved_pos, SEEK_SET);
    }
}

// Helper: Rename/Move
int rename_entry(uint32_t dir_clus, char* src, char* dest) {
    uint32_t cluster = dir_clus;
    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;

    while (cluster != EOC_MARKER && cluster >= 2) {
        uint32_t offset = get_cluster_byte_offset(cluster);
        long saved_pos = ftell(g_fp);
        fseek(g_fp, offset, SEEK_SET);

        for (uint32_t i = 0; i < clus_size; i += ENTRY_SIZE) {
            DirEntry_t entry;
            fread(&entry, sizeof(DirEntry_t), 1, g_fp);
            
            if (entry.DIR_Name[0] == 0x00) break;
            if (entry.DIR_Name[0] == 0xE5) continue;
            
            if (name_matches(entry.DIR_Name, src)) {
                fseek(g_fp, -(long)sizeof(DirEntry_t), SEEK_CUR);

                // Prepare new name in 8.3 form
                char new_name[11];
                to_83_name(dest, new_name);

                // Write Name
                fwrite(new_name, 11, 1, g_fp);
                fflush(g_fp);
                fseek(g_fp, saved_pos, SEEK_SET);
                return 1;
            }
        }
        cluster = get_next_cluster(cluster);
        fseek(g_fp, saved_pos, SEEK_SET);
    }
    return 0;
}

// Helper: Update file size in directory entry
void update_dir_entry_size(uint32_t dir_clus, const char* name, uint32_t new_size) {
    uint32_t cluster = dir_clus;
    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;

    while (cluster != EOC_MARKER && cluster >= 2) {
        uint32_t offset = get_cluster_byte_offset(cluster);
        long saved = ftell(g_fp);
        fseek(g_fp, offset, SEEK_SET);

        for (uint32_t i = 0; i < clus_size; i += ENTRY_SIZE) {
            DirEntry_t entry;
            if (fread(&entry, sizeof(DirEntry_t), 1, g_fp) != 1) break;

            if (entry.DIR_Name[0] == 0x00) break;
            if (entry.DIR_Name[0] == 0xE5) continue;

            if (name_matches(entry.DIR_Name, name)) {
                // Found it. Update size.
                fseek(g_fp, -(long)sizeof(DirEntry_t), SEEK_CUR); // Backtrack
                entry.DIR_FileSize = new_size;
                fwrite(&entry, sizeof(DirEntry_t), 1, g_fp);
                fflush(g_fp);
                fseek(g_fp, saved, SEEK_SET);
                return;
            }
        }
        cluster = get_next_cluster(cluster);
        fseek(g_fp, saved, SEEK_SET);
    }
}

/*
 * Record a file's first cluster in its directory entry. Allocating a cluster
 * for a previously empty file is only half the job: without this the entry
 * still points at cluster 0, so the bytes are written to a cluster nothing
 * references and the file reads back empty on the next open.
 */
void update_dir_entry_cluster(uint32_t dir_clus, const char* name, uint32_t first_clus) {
    uint32_t cluster = dir_clus;
    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;

    while (cluster != EOC_MARKER && cluster >= 2) {
        uint32_t offset = get_cluster_byte_offset(cluster);
        long saved = ftell(g_fp);
        fseek(g_fp, offset, SEEK_SET);

        for (uint32_t i = 0; i < clus_size; i += ENTRY_SIZE) {
            DirEntry_t entry;
            if (fread(&entry, sizeof(DirEntry_t), 1, g_fp) != 1) break;

            if (entry.DIR_Name[0] == 0x00) break;
            if (entry.DIR_Name[0] == 0xE5) continue;

            if (name_matches(entry.DIR_Name, name)) {
                fseek(g_fp, -(long)sizeof(DirEntry_t), SEEK_CUR);
                entry.DIR_FstClusHI = (first_clus >> 16) & 0xFFFF;
                entry.DIR_FstClusLO = first_clus & 0xFFFF;
                fwrite(&entry, sizeof(DirEntry_t), 1, g_fp);
                fflush(g_fp);
                fseek(g_fp, saved, SEEK_SET);
                return;
            }
        }
        cluster = get_next_cluster(cluster);
        fseek(g_fp, saved, SEEK_SET);
    }
}

// Report whether a directory holds anything other than "." and "..".
int dir_is_empty(uint32_t dir_clus) {
    uint32_t cluster = dir_clus;
    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;

    while (cluster != EOC_MARKER && cluster >= 2) {
        long saved = ftell(g_fp);
        fseek(g_fp, get_cluster_byte_offset(cluster), SEEK_SET);

        for (uint32_t i = 0; i < clus_size; i += ENTRY_SIZE) {
            DirEntry_t entry;
            if (fread(&entry, sizeof(DirEntry_t), 1, g_fp) != 1) break;

            if (entry.DIR_Name[0] == 0x00) {      // no entries past here
                fseek(g_fp, saved, SEEK_SET);
                return 1;
            }
            if (entry.DIR_Name[0] == 0xE5) continue;            // deleted
            if (entry.DIR_Attr == ATTR_LONG_NAME) continue;     // LFN fragment
            if (name_matches(entry.DIR_Name, ".") ||
                name_matches(entry.DIR_Name, "..")) continue;

            fseek(g_fp, saved, SEEK_SET);
            return 0;                                            // real entry
        }
        cluster = get_next_cluster(cluster);
        fseek(g_fp, saved, SEEK_SET);
    }
    return 1;
}

// Main Write Logic
int write_file_data(int fd, const char* data, uint32_t len) {
    OpenFile_t* file = &g_open_table[fd];

    // Check if file is empty
    if (file->first_clus == 0) {
        file->first_clus = allocate_cluster();
        if (file->first_clus == 0) return -1;
        // Point the directory entry at the cluster we just allocated.
        update_dir_entry_cluster(file->dir_clus, file->name, file->first_clus);
    }

    uint32_t current_clus = file->first_clus;
    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;
    long total_written = 0;
    
    // Navigate to the cluster containing the current offset
    long temp_offset = file->offset;
    while (temp_offset >= clus_size) {
        uint32_t next = get_next_cluster(current_clus);
        if (next == EOC_MARKER) {
             next = allocate_cluster();
             set_fat_entry(current_clus, next);
        }
        current_clus = next;
        temp_offset -= clus_size;
    }

    // Write loop
    while (total_written < len) {
        uint32_t clus_byte_offset = get_cluster_byte_offset(current_clus);
        uint32_t write_pos_in_clus = file->offset % clus_size;
        uint32_t space_in_clus = clus_size - write_pos_in_clus;
        uint32_t to_write = (len - total_written < space_in_clus) ? (len - total_written) : space_in_clus;

        fseek(g_fp, clus_byte_offset + write_pos_in_clus, SEEK_SET);
        fwrite(data + total_written, 1, to_write, g_fp);
        
        total_written += to_write;
        file->offset += to_write;

        if (file->offset % clus_size == 0 && total_written < len) {
            uint32_t next = get_next_cluster(current_clus);
            if (next == EOC_MARKER) {
                next = allocate_cluster();
                if (next == 0) break; // Disk full
                set_fat_entry(current_clus, next);
            }
            current_clus = next;
        }
    }

    // Update Size if grown
    if (file->offset > file->size) {
        file->size = file->offset;
        update_dir_entry_size(file->dir_clus, file->name, file->size);
    }
    
    return total_written;
}

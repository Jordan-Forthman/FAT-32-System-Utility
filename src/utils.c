#include "fat32.h"
#include <stdlib.h>

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

// FAT entry offset for cluster
uint32_t get_fat_entry_offset(uint32_t cluster) {
    uint32_t first_fat_sec = g_bpb.BPB_RsvdSecCnt;
    return (first_fat_sec * g_bpb.BPB_BytsPerSec) + (cluster * 4);
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

            // Trim and compare name
            char entry_name[12];
            memcpy(entry_name, entry.DIR_Name, 11);
            entry_name[11] = '\0';
            trim_trailing_spaces(entry_name, 11);

            if (strcmp(entry_name, name) == 0) {
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

    // Set EOC in FAT
    uint32_t fat_offset = get_fat_entry_offset(new_clus);
    long saved_pos = ftell(g_fp);
    fseek(g_fp, fat_offset, SEEK_SET);
    uint32_t eoc = EOC_MARKER;
    fwrite(&eoc, sizeof(uint32_t), 1, g_fp);
    fflush(g_fp);

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
    // Pad name with spaces
    strncpy((char*)entry->DIR_Name, name, 11);
    for (int i = strlen(name); i < 11; i++) entry->DIR_Name[i] = ' ';
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

// Read file data from offset
char* read_file_data(uint32_t first_clus, long offset, uint32_t req_size, uint32_t file_size, uint32_t* out_len) {
    if (first_clus < 2) return NULL;

    char* buf = malloc(req_size + 1);
    if (!buf) return NULL;
    memset(buf, 0, req_size + 1);

    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;
    uint32_t cluster = first_clus;
    long cur_offset = 0;
    uint32_t read_bytes = 0;

    while (cluster != EOC_MARKER && read_bytes < req_size && cur_offset < file_size) {
        uint32_t clus_offset = get_cluster_byte_offset(cluster);
        long saved = ftell(g_fp);
        fseek(g_fp, clus_offset, SEEK_SET);

        // Skip to offset within chain
        if (cur_offset + clus_size <= offset) {
            cur_offset += clus_size;
            cluster = get_next_cluster(cluster);
            continue;
        }

        // Read partial/full
        long skip_in_clus = offset - cur_offset;
        fseek(g_fp, skip_in_clus, SEEK_CUR);
        uint32_t to_read = clus_size - skip_in_clus;
        to_read = MIN(to_read, req_size - read_bytes);
        to_read = MIN(to_read, file_size - offset - read_bytes);

        fread(buf + read_bytes, 1, to_read, g_fp);
        read_bytes += to_read;
        cur_offset += clus_size;

        cluster = get_next_cluster(cluster);
        fseek(g_fp, saved, SEEK_SET);
    }

    *out_len = read_bytes;
    return buf;
}

// Helper: Update a specific FAT entry
void set_fat_entry(uint32_t cluster, uint32_t value) {
    uint32_t offset = get_fat_entry_offset(cluster);
    long saved = ftell(g_fp);
    fseek(g_fp, offset, SEEK_SET);
    fwrite(&value, sizeof(uint32_t), 1, g_fp);
    fflush(g_fp);
    fseek(g_fp, saved, SEEK_SET);
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
            
            char entry_name[12];
            memcpy(entry_name, entry.DIR_Name, 11);
            entry_name[11] = '\0';
            trim_trailing_spaces(entry_name, 11);

            if (strcmp(entry_name, name) == 0) {
                fseek(g_fp, -sizeof(DirEntry_t), SEEK_CUR);
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
            
            char entry_name[12];
            memcpy(entry_name, entry.DIR_Name, 11);
            entry_name[11] = '\0';
            trim_trailing_spaces(entry_name, 11);

            if (strcmp(entry_name, src) == 0) {
                fseek(g_fp, -sizeof(DirEntry_t), SEEK_CUR);
                
                // Prepare new name
                char new_name[11];
                memset(new_name, ' ', 11);
                int len = strlen(dest);
                if (len > 11) len = 11;
                memcpy(new_name, dest, len);
                
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

            char entry_name[12];
            memcpy(entry_name, entry.DIR_Name, 11);
            entry_name[11] = '\0';
            trim_trailing_spaces(entry_name, 11);

            if (strcmp(entry_name, name) == 0) {
                // Found it. Update size.
                fseek(g_fp, -sizeof(DirEntry_t), SEEK_CUR); // Backtrack
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

// Main Write Logic
int write_file_data(int fd, const char* data, uint32_t len) {
    OpenFile_t* file = &g_open_table[fd];
    
    // Check if file is empty
    if (file->first_clus == 0) {
        file->first_clus = allocate_cluster();
        if (file->first_clus == 0) return -1;
        
        uint32_t cluster = file->dir_clus;
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

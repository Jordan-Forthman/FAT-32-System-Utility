#include "fat32.h"

void cmd_info() {
    // Calculations for BPB
    uint32_t root_clus = g_bpb.BPB_RootClus;
    uint16_t bytes_per_sec = g_bpb.BPB_BytsPerSec;
    uint8_t sec_per_clus = g_bpb.BPB_SecPerClus;
    uint32_t first_data_sec = get_first_data_sector();
    uint32_t total_data_sec = g_bpb.BPB_TotSec32 - first_data_sec;
    uint32_t total_data_clus = total_data_sec / sec_per_clus;
    uint32_t fat_entries = g_bpb.BPB_FATSz32 * bytes_per_sec / 4;
    uint32_t image_size = g_bpb.BPB_TotSec32 * bytes_per_sec;

    printf("position of root cluster (in cluster #): %u\n", root_clus);
    printf("bytes per sector: %u\n", bytes_per_sec);
    printf("sectors per cluster: %u\n", sec_per_clus);
    printf("total # of clusters in data region: %u\n", total_data_clus);
    printf("# of entries in one FAT: %u\n", fat_entries);
    printf("size of image (in bytes): %u\n", image_size);
}

void cmd_exit() {
    if (g_fp) fclose(g_fp);
    exit(0);
}

void cmd_ls() {
    uint32_t cluster = g_cwd_cluster;
    uint32_t clus_size = g_bpb.BPB_BytsPerSec * g_bpb.BPB_SecPerClus;
    long saved_pos = ftell(g_fp);  // Declare outside loop to restore at end

    while (cluster != EOC_MARKER && cluster >= 2) {  // Valid clusters start at 2
        uint32_t offset = get_cluster_byte_offset(cluster);
        if (offset == 0) {  // Error check (invalid offset)
            printf("Error: Invalid cluster offset\n");
            fseek(g_fp, saved_pos, SEEK_SET);  // Restore on error
            return;
        }

        fseek(g_fp, offset, SEEK_SET);

        // Read entire cluster (but process in 32-byte chunks)
        for (uint32_t i = 0; i < clus_size; i += ENTRY_SIZE) {
            DirEntry_t entry;
            if (fread(&entry, sizeof(DirEntry_t), 1, g_fp) != 1) {
                printf("Error: Failed to read entry\n");
                fseek(g_fp, saved_pos, SEEK_SET);
                return;
            }

            if (entry.DIR_Name[0] == 0x00) break;  // End of dir entries
            if (entry.DIR_Name[0] == 0xE5) continue;  // Deleted
            if (entry.DIR_Attr == ATTR_LONG_NAME) continue;  // Ignore LNDE
            if (entry.DIR_Attr & ATTR_VOLUME_ID) continue;   // Volume label

            // Render the 8.3 bytes back into a readable "NAME.EXT"
            char name[13];
            from_83_name(entry.DIR_Name, name);
            printf("%s\n", name);
        }

        // Next cluster
        uint32_t next_cluster = get_next_cluster(cluster);
        if (next_cluster == cluster) {
            printf("Error: Cluster loop detected\n");
            break;
        }
        cluster = next_cluster;
    }

    // Restore original fp position
    fseek(g_fp, saved_pos, SEEK_SET);

    if (cluster == EOC_MARKER) return;
    printf("Error: Invalid cluster chain\n");
}

void cmd_cd(tokenlist* tokens) {
    if (tokens->size != 2) {
        printf("Error: cd requires exactly one argument (directory name)\n");
        return;
    }

    const char* dirname = tokens->items[1];

    // Special cases
    if (strcmp(dirname, ".") == 0) {
        return;
    }

    // Find the entry in cwd
    DirEntry_t* entry = find_entry(dirname, g_cwd_cluster);
    if (!entry) {
        printf("Error: Directory '%s' not found\n", dirname);
        return;
    }

    // Check if directory
    if (!(entry->DIR_Attr & ATTR_DIRECTORY)) {
        printf("Error: '%s' is not a directory\n", dirname);
        free(entry);
        return;
    }

    // Get new cluster
    uint32_t new_cluster = (entry->DIR_FstClusHI << 16) | entry->DIR_FstClusLO;
    free(entry);

    // Special: .. (parent cluster could be 0 for root)
    if (strcmp(dirname, "..") == 0 && new_cluster == 0) {
        new_cluster = g_bpb.BPB_RootClus;  // Stay at root if parent is 0
    }

    // Update globals
    g_cwd_cluster = new_cluster;

    // Track the path so the prompt stays accurate
    if (strcmp(dirname, "..") == 0) {
        // drop the trailing "NAME/" component
        size_t len = strlen(g_cwd_path);
        if (len > 0) {
            g_cwd_path[len - 1] = '\0';                 // strip trailing '/'
            char* slash = strrchr(g_cwd_path, '/');
            if (slash) slash[1] = '\0';
            else g_cwd_path[0] = '\0';
        }
    } else {
        size_t len = strlen(g_cwd_path);
        snprintf(g_cwd_path + len, sizeof(g_cwd_path) - len, "%s/", dirname);
    }

    if (new_cluster == g_bpb.BPB_RootClus) g_cwd_path[0] = '\0';
    rebuild_prompt();
}

void cmd_mkdir(tokenlist* tokens) {
    if (tokens->size != 2) {
        printf("Error: mkdir requires exactly one argument (directory name)\n");
        return;
    }
    const char* dirname = tokens->items[1];

    // Check exists
    if (find_entry(dirname, g_cwd_cluster)) {
        printf("Error: '%s' already exists\n", dirname);
        return;
    }

    // Find slot in cwd chain
    long slot_offset = find_free_entry_slot(g_cwd_cluster);
    if (slot_offset == -1) {
        printf("Error: No space in directory\n");
        return;
    }

    // Alloc new cluster for dir
    uint32_t new_clus = allocate_cluster();
    if (new_clus == 0) return;

    // Create SNDE
    DirEntry_t entry;
    init_dir_entry(&entry, dirname, ATTR_DIRECTORY, new_clus, 0);
    write_entry(slot_offset, &entry);

    // In new cluster: Add . and ..
    long new_clus_offset = get_cluster_byte_offset(new_clus);
    DirEntry_t dot;
    init_dir_entry(&dot, ".", ATTR_DIRECTORY, new_clus, 0);
    write_entry(new_clus_offset, &dot);

    DirEntry_t dotdot;
    init_dir_entry(&dotdot, "..", ATTR_DIRECTORY, (g_cwd_cluster == g_bpb.BPB_RootClus ? 0 : g_cwd_cluster), 0);
    write_entry(new_clus_offset + ENTRY_SIZE, &dotdot);

    printf("Directory '%s' created\n", dirname);
}

void cmd_creat(tokenlist* tokens) {
    if (tokens->size != 2) {
        printf("Error: creat requires exactly one argument (file name)\n");
        return;
    }
    const char* filename = tokens->items[1];

    // Check exists
    if (find_entry(filename, g_cwd_cluster)) {
        printf("Error: '%s' already exists\n", filename);
        return;
    }

    // Find slot
    long slot_offset = find_free_entry_slot(g_cwd_cluster);
    if (slot_offset == -1) {
        printf("Error: No space in directory\n");
        return;
    }

    // Create SNDE
    DirEntry_t entry;
    init_dir_entry(&entry, filename, ATTR_ARCHIVE, 0, 0);
    write_entry(slot_offset, &entry);

    printf("File '%s' created\n", filename);
}

void cmd_open(tokenlist* tokens) {
    if (tokens->size != 3) {
        printf("Error: open requires filename and flags (-r/-w/-rw)\n");
        return;
    }
    const char* filename = tokens->items[1];
    const char* flags = tokens->items[2];

    DirEntry_t* entry = find_entry(filename, g_cwd_cluster);
    if (!entry) {
        printf("Error: File '%s' not found\n", filename);  // Fixed: , filename
        return;
    }
    if (entry->DIR_Attr & ATTR_DIRECTORY) {
        printf("Error: '%s' is a directory\n", filename);
        free(entry);
        return;
    }

    int mode = parse_mode(flags);
    if (mode == 0) {
        printf("Error: Invalid flags '%s'\n", flags);
        free(entry);
        return;
    }

    int slot = find_free_fd();
    if (slot == -1) {
        printf("Error: Max open files reached\n");
        free(entry);
        return;
    }

    // Fill slot
    memset(&g_open_table[slot], 0, sizeof(OpenFile_t));
    g_open_table[slot].fd = slot + 1;
    strncpy(g_open_table[slot].name, filename, sizeof(g_open_table[slot].name) - 1);
    g_open_table[slot].mode = mode;
    g_open_table[slot].offset = 0;
    strncpy(g_open_table[slot].path, g_prompt, sizeof(g_open_table[slot].path) - 1);
    g_open_table[slot].first_clus = (entry->DIR_FstClusHI << 16) | entry->DIR_FstClusLO;
    g_open_table[slot].size = entry->DIR_FileSize;
    g_open_table[slot].dir_clus = g_cwd_cluster;
    free(entry);
    printf("Opened '%s' as fd %d\n", filename, slot);
}

void cmd_close(tokenlist* tokens) {
    if (tokens->size != 2) {
        printf("Error: close requires fd\n");
        return;
    }
    int fd = atoi(tokens->items[1]);
    if (fd < 0 || fd >= MAX_OPEN_FILES || g_open_table[fd].fd == 0) {
        printf("Error: Invalid or closed fd %d\n", fd);
        return;
    }

    // A file may only be closed from the directory it was opened in. Compare
    // the recorded cluster rather than prompt strings, which mismatched
    // whenever the prompt changed or was truncated.
    if (g_open_table[fd].dir_clus != g_cwd_cluster) {
        printf("Error: fd %d not in cwd\n", fd);
        return;
    }

    memset(&g_open_table[fd], 0, sizeof(OpenFile_t));
    printf("Closed fd %d\n", fd);
}

void cmd_lsof() {
    printf("FD | Name | Mode | Offset | Path\n");
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (g_open_table[i].fd != 0) {
            char mode_str[5];
            strcpy(mode_str, g_open_table[i].mode & MODE_READ ? "-r" : "");
            if (g_open_table[i].mode & MODE_WRITE) strcat(mode_str, "w");
            printf("%d | %s | %s | %ld | %s\n",
                   i, g_open_table[i].name, mode_str, g_open_table[i].offset, g_open_table[i].path);
        }
    }
}

void cmd_size(tokenlist* tokens) {
    if (tokens->size != 2) {
        printf("Error: size requires filename\n");
        return;
    }
    const char* filename = tokens->items[1];

    DirEntry_t* entry = find_entry(filename, g_cwd_cluster);
    if (!entry) {
        printf("Error: File '%s' not found\n", filename);
        return;
    }
    if (entry->DIR_Attr & ATTR_DIRECTORY) {
        printf("Error: '%s' is a directory (size ignored)\n", filename);
        free(entry);
        return;
    }

    printf("%u %s\n", entry->DIR_FileSize, filename);
    free(entry);
}

void cmd_lseek(tokenlist* tokens) {
    if (tokens->size != 3) {
        printf("Error: lseek requires fd and offset\n");
        return;
    }
    int fd = atoi(tokens->items[1]);
    long offset = atol(tokens->items[2]);

    if (fd < 0 || fd >= MAX_OPEN_FILES || g_open_table[fd].fd == 0) {
        printf("Error: Invalid fd %d\n", fd);
        return;
    }

    if (offset < 0) offset = 0;
    if (offset > g_open_table[fd].size) offset = g_open_table[fd].size;

    g_open_table[fd].offset = offset;
    printf("Set offset to %ld for fd %d\n", offset, fd);
}

void cmd_read(tokenlist* tokens) {
    if (tokens->size != 3) {
        printf("Error: read requires fd and size\n");
        return;
    }
    int fd = atoi(tokens->items[1]);
    uint32_t req_size = atoi(tokens->items[2]);

    if (fd < 0 || fd >= MAX_OPEN_FILES || g_open_table[fd].fd == 0) {
        printf("Error: Invalid fd %d\n", fd);
        return;
    }
    if (!(g_open_table[fd].mode & MODE_READ)) {
        printf("Error: fd %d not open for read\n", fd);
        return;
    }

    uint32_t out_len;
    char* data = read_file_data(g_open_table[fd].first_clus, g_open_table[fd].offset, req_size, g_open_table[fd].size, &out_len);
    if (!data) {
        printf("Error: Read failed\n");
        return;
    }

    // Print
    data[out_len] = '\0';
    printf("%s\n", data);
    free(data);

    // Update offset
    g_open_table[fd].offset += out_len;
}

void cmd_write(tokenlist* tokens) {
    if (tokens->size < 3) {
        printf("Error: write requires filename and data\n");
        return;
    }
    const char* filename = tokens->items[1];
    
    //  Reconstruct string data from tokens, bounded: strcat into a fixed
    //  buffer would overflow the stack on a long enough write command.
    char buffer[1024] = "";
    size_t used = 0;
    for (size_t i = 2; i < tokens->size; i++) {
        int n = snprintf(buffer + used, sizeof(buffer) - used, "%s%s",
                         tokens->items[i], (i < tokens->size - 1) ? " " : "");
        if (n < 0 || (size_t)n >= sizeof(buffer) - used) {
            printf("Error: data too long (max %zu bytes)\n", sizeof(buffer) - 1);
            return;
        }
        used += (size_t)n;
    }

    // Strip quotes if present
    char* data_ptr = buffer;
    if (buffer[0] == '"') {
        data_ptr++; // Skip first quote
        size_t len = strlen(data_ptr);
        if (len > 0 && data_ptr[len-1] == '"') data_ptr[len-1] = '\0'; // Remove last quote
    }

    // Find file in open table
    int fd = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (g_open_table[i].fd != 0 && strcmp(g_open_table[i].name, filename) == 0) {
            fd = i;
            break;
        }
    }

    if (fd == -1) {
        printf("Error: File '%s' is not open\n", filename);
        return;
    }
    if (!(g_open_table[fd].mode & MODE_WRITE)) {
        printf("Error: File not open for writing\n");
        return;
    }

    // Write data
    int bytes_written = write_file_data(fd, data_ptr, strlen(data_ptr));
    if (bytes_written >= 0) {
        printf("Wrote %d bytes to '%s'\n", bytes_written, filename);
    } else {
        printf("Error: Failed to write to file\n");
    }
}

void cmd_rm(tokenlist* tokens) {
    if (tokens->size != 2) {
        printf("Error: rm requires filename\n");
        return;
    }
    char* filename = tokens->items[1];

    // Check if file is open (cannot delete open files)
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (g_open_table[i].fd != 0 && strcmp(g_open_table[i].name, filename) == 0) {
            printf("Error: File is currently open\n");
            return;
        }
    }

    // Find entry
    DirEntry_t* entry = find_entry(filename, g_cwd_cluster);
    if (!entry) {
        printf("Error: File '%s' not found\n", filename);
        return;
    }
    if (entry->DIR_Attr & ATTR_DIRECTORY) {
        printf("Error: '%s' is a directory\n", filename);
        free(entry);
        return;
    }

    // Clear FAT Chain
    uint32_t first_clus = (entry->DIR_FstClusHI << 16) | entry->DIR_FstClusLO;
    if (first_clus != 0) {
        clear_fat_chain(first_clus);
    }

    // Mark entry as deleted
    delete_entry_by_name(g_cwd_cluster, filename);

    free(entry);
    printf("File '%s' deleted\n", filename);
}

void cmd_rmdir(tokenlist* tokens) {
    if (tokens->size != 2) {
        printf("Error: rmdir requires directory name\n");
        return;
    }
    char* dirname = tokens->items[1];

    if (strcmp(dirname, ".") == 0 || strcmp(dirname, "..") == 0) {
        printf("Error: Cannot remove . or ..\n");
        return;
    }

    DirEntry_t* entry = find_entry(dirname, g_cwd_cluster);
    if (!entry) {
        printf("Error: Directory '%s' not found\n", dirname);
        return;
    }
    if (!(entry->DIR_Attr & ATTR_DIRECTORY)) {
        printf("Error: '%s' is not a directory\n", dirname);
        free(entry);
        return;
    }

    // Only empty directories may be removed. Without this check the contents
    // are orphaned: their clusters are freed while their entries remain.
    uint32_t first_clus = (entry->DIR_FstClusHI << 16) | entry->DIR_FstClusLO;
    if (first_clus >= 2 && !dir_is_empty(first_clus)) {
        printf("Error: Directory '%s' is not empty\n", dirname);
        free(entry);
        return;
    }

    clear_fat_chain(first_clus);
    delete_entry_by_name(g_cwd_cluster, dirname);

    free(entry);
    printf("Directory '%s' removed\n", dirname);
}

// Copy a file within the current directory.
void cmd_cp(tokenlist* tokens) {
    if (tokens->size != 3) {
        printf("Error: cp requires source and dest\n");
        return;
    }
    const char* src = tokens->items[1];
    const char* dest = tokens->items[2];

    DirEntry_t* s = find_entry(src, g_cwd_cluster);
    if (!s) {
        printf("Error: File '%s' not found\n", src);
        return;
    }
    if (s->DIR_Attr & ATTR_DIRECTORY) {
        printf("Error: '%s' is a directory\n", src);
        free(s);
        return;
    }

    DirEntry_t* existing = find_entry(dest, g_cwd_cluster);
    if (existing) {
        printf("Error: '%s' already exists\n", dest);
        free(existing);
        free(s);
        return;
    }

    long slot = find_free_entry_slot(g_cwd_cluster);
    if (slot == -1) {
        printf("Error: No space in directory\n");
        free(s);
        return;
    }

    uint32_t src_clus = (s->DIR_FstClusHI << 16) | s->DIR_FstClusLO;
    uint32_t size = s->DIR_FileSize;
    free(s);

    // Create the destination entry; clusters are attached as data is written.
    DirEntry_t entry;
    init_dir_entry(&entry, dest, ATTR_ARCHIVE, 0, 0);
    write_entry(slot, &entry);

    if (size == 0 || src_clus < 2) {
        printf("Copied '%s' to '%s' (0 bytes)\n", src, dest);
        return;
    }

    uint32_t out_len = 0;
    char* data = read_file_data(src_clus, 0, size, size, &out_len);
    if (!data) {
        printf("Error: Failed to read '%s'\n", src);
        return;
    }

    // Borrow an open-file slot so the normal write path handles allocation.
    int fd = find_free_fd();
    if (fd == -1) {
        printf("Error: Max open files reached\n");
        free(data);
        return;
    }

    memset(&g_open_table[fd], 0, sizeof(OpenFile_t));
    g_open_table[fd].fd = fd + 1;
    strncpy(g_open_table[fd].name, dest, sizeof(g_open_table[fd].name) - 1);
    g_open_table[fd].mode = MODE_WRITE;
    g_open_table[fd].dir_clus = g_cwd_cluster;

    int written = write_file_data(fd, data, out_len);
    memset(&g_open_table[fd], 0, sizeof(OpenFile_t));   // release the slot
    free(data);

    if (written < 0)
        printf("Error: Failed to write '%s'\n", dest);
    else
        printf("Copied '%s' to '%s' (%d bytes)\n", src, dest, written);
}

void cmd_mv(tokenlist* tokens) {
    if (tokens->size != 3) {
        printf("Error: mv requires source and dest\n");
        return;
    }
    char* src = tokens->items[1];
    char* dest = tokens->items[2];

    // Rename (in same dir)
    if (find_entry(dest, g_cwd_cluster)) {
        printf("Error: Destination exists\n");
        return;
    }

    // Find src entry offset, Update name, Write back
    if (rename_entry(g_cwd_cluster, src, dest)) {
        printf("Moved '%s' to '%s'\n", src, dest);
    } else {
        printf("Error: Move failed\n");
    }
}

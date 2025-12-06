#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"

#define SECTOR_SIZE_DEFAULT 512
#define EOC_MARKER 0x0FFFFFFF
#define FREE_CLUSTER 0x00000000
#define MAX_OPEN_FILES 10
#define ENTRY_SIZE 32
#define MODE_READ 1
#define MODE_WRITE 2
#define MIN(a, b) ((a) < (b) ? (a) : (b))

// BPB Struct
typedef struct __attribute__((packed)) {
uint8_t BS_jmpBoot[3];      // Jump instruction to boot code
    uint8_t BS_OEMName[8];      // OEM Name (e.g., "MSWIN4.1")
    uint16_t BPB_BytsPerSec;    // Bytes per sector (usually 512)
    uint8_t BPB_SecPerClus;     // Sectors per cluster
    uint16_t BPB_RsvdSecCnt;    // Reserved sector count (includes Boot Sector)
    uint8_t BPB_NumFATs;        // Number of FAT tables (usually 2)
    uint16_t BPB_RootEntCnt;    // Number of root entries, 0 for FAT32
    uint16_t BPB_TotSec16;      // Total sectors, 0 for FAT32
    uint8_t BPB_Media;          // Media descriptor
    uint16_t BPB_FATSz16;       // Sectors per FAT, 0 for FAT32
    uint16_t BPB_SecPerTrk;     // Sectors per track
    uint16_t BPB_NumHeads;      // Number of heads
    uint32_t BPB_HiddSec;       // Hidden sectors
    uint32_t BPB_TotSec32;      // Total sectors (32-bit)
    uint32_t BPB_FATSz32;       // Sectors per FAT (32-bit)
    uint16_t BPB_ExtFlags;      // Extended flags
    uint16_t BPB_FSVer;         // File system version
    uint32_t BPB_RootClus;      // Cluster number of the Root Directory
    uint16_t BPB_FSInfo;        // Sector number of FSInfo structure
    uint16_t BPB_BkBootSec;     // Sector number of backup boot sector
    uint8_t BPB_Reserved[12];   // Reserved for future use
} BPB_t;

// Directory Entry. Represents a file or subdirectory on disk.
typedef struct __attribute__((packed)) {
    uint8_t DIR_Name[11];       // Name (8) + Ext (3), space-padded
    uint8_t DIR_Attr;           // Attributes
    uint8_t DIR_NTRes;          // Reserved
    uint8_t DIR_CrtTimeTenth;   // Creation time
    uint16_t DIR_CrtTime;       // Creation time
    uint16_t DIR_CrtDate;       // Creation date
    uint16_t DIR_LstAccDate;    // Last access date
    uint16_t DIR_FstClusHI;     // High 16 bits of first cluster
    uint16_t DIR_WrtTime;       // Last write time
    uint16_t DIR_WrtDate;       // Last write date
    uint16_t DIR_FstClusLO;     // Low 16 bits of first cluster
    uint32_t DIR_FileSize;      // File size
} DirEntry_t;

// Open file entry. Tracks files opened by user.
typedef struct {
    int fd;             // Index (0-9)
    char name[12];      // Name of open file
    int mode;           // 1 = read, 2 = write, 3 = rw
    long offset;        // Byte offset in file
    char path[256];     // Absolute path
    uint32_t first_clus;// Where file starts
    uint32_t size;      // Cached DIR_FileSize
    uint32_t dir_clus;  // Parent directory cluster
} OpenFile_t;

// Global table
extern OpenFile_t g_open_table[MAX_OPEN_FILES];
extern int g_next_fd;  // Next free (linear search)

// Attr bits
#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LONG_NAME 0x0F

// Global state
extern FILE* g_fp;          // Image file pointer
extern BPB_t g_bpb;         // Parsed BPB
extern uint32_t g_cwd_cluster;  // Current working dir cluster
extern char g_prompt[256]; 

// Helpers (utils.c)
uint32_t get_first_data_sector();
uint32_t get_cluster_byte_offset(uint32_t cluster);
uint32_t get_fat_entry_offset(uint32_t cluster);
uint32_t get_next_cluster(uint32_t current);
uint32_t find_free_cluster();
void wrapper_fp(void (*func)(), long* saved_pos);
void trim_trailing_spaces(char* name, int len);
DirEntry_t* find_entry(const char* name, uint32_t start_cluster);
long find_free_entry_slot(uint32_t start_cluster);
void write_entry(long offset, DirEntry_t* entry);
uint32_t allocate_cluster();
void init_dir_entry(DirEntry_t* entry, const char* name, uint8_t attr, uint32_t first_clus, uint32_t size);
int find_free_fd();
int parse_mode(const char* flags);
char* read_file_data(uint32_t first_clus, long offset, uint32_t req_size, uint32_t file_size, uint32_t* out_len);
void set_fat_entry(uint32_t cluster, uint32_t value);
void update_dir_entry_size(uint32_t dir_clus, const char* name, uint32_t new_size);
int write_file_data(int fd, const char* data, uint32_t len);
void clear_fat_chain(uint32_t first_clus);
void delete_entry_by_name(uint32_t dir_clus, char* name);
int rename_entry(uint32_t dir_clus, char* src, char* dest);

// Commands (commands.c)
void cmd_info();
void cmd_exit();
void cmd_ls();
void cmd_cd(tokenlist *tokens);
void cmd_mkdir(tokenlist* tokens);
void cmd_creat(tokenlist* tokens);
void cmd_open(tokenlist* tokens);
void cmd_close(tokenlist* tokens);
void cmd_lsof();
void cmd_size(tokenlist* tokens);
void cmd_lseek(tokenlist* tokens);
void cmd_read(tokenlist* tokens);
void cmd_write(tokenlist* tokens);
void cmd_rm(tokenlist* tokens);
void cmd_rmdir(tokenlist* tokens);
void cmd_cp(tokenlist* tokens);
void cmd_mv(tokenlist* tokens);

// Main dispatch
void dispatch_command(tokenlist* tokens);

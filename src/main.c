#include "fat32.h"

FILE* g_fp = NULL;
BPB_t g_bpb;
uint32_t g_cwd_cluster;
char g_prompt[256];
OpenFile_t g_open_table[MAX_OPEN_FILES] = {0};
int g_next_fd = 0;  // Unused

void dispatch_command(tokenlist* tokens) {
    if (tokens->size == 0) return;
    char* cmd = tokens->items[0];

    if (strcmp(cmd, "info") == 0) {
        cmd_info();
    } else if (strcmp(cmd, "exit") == 0) {
        cmd_exit();
    } else if (strcmp(cmd, "ls") == 0) {
        if (tokens->size > 1) {
            printf("Error: ls takes no arguments\n");
            return;
        }
        cmd_ls();
    } else if (strcmp(cmd, "cd") == 0) {
	cmd_cd(tokens);
    } else if (strcmp(cmd, "mkdir") == 0) {
	cmd_mkdir(tokens);
    } else if (strcmp(cmd, "creat") == 0) {
	cmd_creat(tokens);
    } else if (strcmp(cmd, "open") == 0) {
	cmd_open(tokens);
    } else if (strcmp(cmd, "close") == 0) {
   	cmd_close(tokens);
    } else if (strcmp(cmd, "lsof") == 0) {
    	cmd_lsof();
    } else if (strcmp(cmd, "size") == 0) {
    	cmd_size(tokens);
    } else if (strcmp(cmd, "lseek") == 0) {
    	cmd_lseek(tokens);
    } else if (strcmp(cmd, "read") == 0) {
    	cmd_read(tokens);
    } else if (strcmp(cmd, "write") == 0) {
        cmd_write(tokens);
    } else if (strcmp(cmd, "rm") == 0) {
        cmd_rm(tokens);
    } else if (strcmp(cmd, "rmdir") == 0) {
        cmd_rmdir(tokens);
    } else if (strcmp(cmd, "mv") == 0) {
        cmd_mv(tokens);
    } else if (strcmp(cmd, "cp") == 0) {
        cmd_cp(tokens);
    } else {
        printf("Unknown command: %s\n", cmd);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: ./filesys fat32.img\n");
        return 1;
    }

    g_fp = fopen(argv[1], "rb+");
    if (!g_fp) {
        fprintf(stderr, "%s does not exist\n", argv[1]);
        return 1;
    }

    // Parse BPB (first 512 bytes)
    fread(&g_bpb, sizeof(BPB_t), 1, g_fp);
    if (g_bpb.BPB_BytsPerSec != SECTOR_SIZE_DEFAULT) {
        fprintf(stderr, "Invalid FAT32 image\n");
        fclose(g_fp);
        return 1;
    }

    // Initialize state
    g_cwd_cluster = g_bpb.BPB_RootClus;
    snprintf(g_prompt, sizeof(g_prompt), "%s/>", argv[1]);

    // Shell loop
    while (1) {
        printf("%s", g_prompt);
        char* input = get_input();
        tokenlist* tokens = get_tokens(input);
        dispatch_command(tokens);
        free(input);
        free_tokens(tokens);
    }

    return 0;
}

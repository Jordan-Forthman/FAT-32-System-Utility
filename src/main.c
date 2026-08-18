#include "fat32.h"

FILE* g_fp = NULL;
BPB_t g_bpb;
uint32_t g_cwd_cluster;
char g_prompt[512];
char g_image_name[128];
char g_cwd_path[256];

// Prompt is "<image>/<path>>", e.g. "fat32.img/>" or "fat32.img/DOCS/>".
void rebuild_prompt(void) {
    snprintf(g_prompt, sizeof(g_prompt), "%s/%s>", g_image_name, g_cwd_path);
}
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
    if (fread(&g_bpb, sizeof(BPB_t), 1, g_fp) != 1) {
        fprintf(stderr, "%s is too small to be a FAT32 image\n", argv[1]);
        fclose(g_fp);
        return 1;
    }
    if (g_bpb.BPB_BytsPerSec != SECTOR_SIZE_DEFAULT) {
        fprintf(stderr, "Invalid FAT32 image\n");
        fclose(g_fp);
        return 1;
    }

    // Initialize state. The prompt shows the image's base name plus the path
    // walked so far, so it stays correct for any image and any directory.
    g_cwd_cluster = g_bpb.BPB_RootClus;
    const char* base = strrchr(argv[1], '/');
    snprintf(g_image_name, sizeof(g_image_name), "%s", base ? base + 1 : argv[1]);
    g_cwd_path[0] = '\0';
    rebuild_prompt();

    // Shell loop
    while (1) {
        printf("%s", g_prompt);
        fflush(stdout);

        char* input = get_input();
        if (!input) {   // end of input: Ctrl-D, or a piped script running out
            printf("\n");
            break;
        }

        tokenlist* tokens = get_tokens(input);
        dispatch_command(tokens);
        free(input);
        free_tokens(tokens);
    }

    fclose(g_fp);
    return 0;
}

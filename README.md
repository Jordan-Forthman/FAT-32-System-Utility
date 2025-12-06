# FAT32 File System Utility

[A user-space shell utility that mounts and manipulates FAT32 disk images without corrupting them. Supports navigation, file/directory creation, reading, writing, moving, and deletion while maintaining full FAT32 structure integrity]

## Author
- **Jordan Forthman**: jf24b@fsu.edu
## Implementation Notes

### Part 1: Mounting the Image
- **Responsibilities**: [Parse boot sector, load FAT32 metadata, map FAT tables into memory, initialize root directory, implement 'info' and 'exit' commands, set up shell loop and prompt ]

### Part 2: Navigation
- **Responsibilities**: [Implement directory traversal, cwd tracking, 'cd' and 'ls' commands including "." and ".." directories, error checking]

### Part 3: Create
- **Responsibilities**: [Implement 'mkdir' and 'creat' commands, directory entry creation, file creation, error checking]

### Part 4: Read
- **Responsibilities**: [Implement open file table (max 10), 'open', 'close', 'lsof', 'lseek', and 'read' commands w/ proper offset management & mode checking read permissions]

### Part 5: Update
- **Responsibilities**: [Implement 'write' command with file size extension and cluster allocation, 'mv' command for renaming and cross-directory move while file is closed]

### Part 6: Delete
- **Responsibilities**: [Implement 'rm' for file deletion and 'rmdir' for emptry directory removal only, proper FAT entry seroing and directory entry removal]

### Extra Credit
- **Responsibilities**: [None attempted]

## File Listing
```
filesys/
|
|- src/
|---lexer.c
|---commands.c
|---main.c
|---utils.c
|
|- include/
|---lexer.h
|--- fat32.h
|
|-fat32.img
|-README.md
|-Makefile
```
## How to Compile & Execute

### Requirements
- **Compiler**: gcc
- **Dependencies**: N/A

### Compilation
For a C/C++ example:
```bash
make
```
This will build the executable in:
### Execution
```bash
make run
```
This will run the program

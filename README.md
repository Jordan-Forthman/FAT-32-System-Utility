# FAT32 File System Utility

An interactive shell that mounts a FAT32 disk image and manipulates it directly
in user space, with no kernel driver and no `mount`. It parses the boot sector,
walks the FAT cluster chains itself, and reads and writes real 8.3 directory
entries, so images it produces stay valid to any FAT32 driver.

Written in C99 against nothing but libc.

## Quickstart

```bash
git clone https://github.com/Jordan-Forthman/FAT-32-System-Utility.git
cd FAT-32-System-Utility
make
make run
```

`make run` unpacks the sample volume that ships with the repo and opens it:

```
fat32.img/>ls
WELCOME.TXT
DOCS
EMPTY
fat32.img/>cd DOCS
fat32.img/DOCS/>open NOTES.TXT -r
Opened 'NOTES.TXT' as fd 0
fat32.img/DOCS/>read 0 60
Files and directories here were created by filesys itself.
fat32.img/DOCS/>exit
```

Nothing beyond `gcc` and `make` is required. The sample image is stored
compressed, since a FAT32 volume is mostly zeros and 34 MB shrinks to about
34 KB.

To start from a clean volume instead, with `dosfstools` installed:

```bash
make image-fresh    # mkfs.vfat a new empty 34 MB FAT32 image
```

## Commands

| Command | Description |
| --- | --- |
| `info` | Print boot sector geometry: root cluster, sector and cluster sizes, FAT entries, image size |
| `ls` | List the current directory |
| `cd <dir>` | Change directory, including `.` and `..` |
| `mkdir <dir>` | Create a directory, with its `.` and `..` entries |
| `creat <file>` | Create an empty file |
| `open <file> -r\|-w\|-rw` | Open a file and assign it a descriptor |
| `close <fd>` | Close a descriptor |
| `lsof` | List open files with mode, offset, and path |
| `size <file>` | Print a file's size |
| `lseek <fd> <offset>` | Move a descriptor's read/write position |
| `read <fd> <bytes>` | Read from the current offset |
| `write <file> <data>` | Write at the current offset, extending the file and allocating clusters as needed |
| `cp <src> <dst>` | Copy a file |
| `mv <src> <dst>` | Rename a file or directory |
| `rm <file>` | Delete a file and free its clusters |
| `rmdir <dir>` | Remove an empty directory |
| `exit` | Close the image and quit |

Up to 10 files may be open at once.

## Tests

```bash
make test
```

31 end-to-end checks drive the shell over a pipe against a throwaway image.
Where it matters they validate the resulting image with `fsck.vfat` rather than
trusting what the tool printed, so a write is confirmed by the volume being
consistent afterwards and not merely by a success message.

```
31 passed, 0 failed
```

Covered: mounting and geometry, create and list, write and read back across a
reopen, 8.3 names as stored on disk, reads spanning a cluster boundary, `lseek`
positioning, directory create and traverse, `rmdir` refusing a non-empty
directory, copy, rename, delete, error handling for missing files and bad
descriptors, and an `fsck` pass over an image after a full round of operations.

The `fsck` checks need `dosfstools`; without it those four are skipped and the
rest still run.

## How it works

**Cluster chains.** Files and directories are chains of clusters linked through
the File Allocation Table. Reading a file means walking that chain from the
entry's first cluster; extending one means finding a free cluster, marking it
end-of-chain, and pointing the previous cluster at it.

**Both FATs stay in sync.** A FAT32 volume normally carries two copies of the
table, and they are expected to agree. Every FAT update is written to all
`BPB_NumFATs` copies, so `fsck` does not report the image as inconsistent.

**8.3 names.** On disk a short name is 11 raw bytes: 8 of name then 3 of
extension, space padded, upper case, with no dot stored between them.
`HELLO.TXT` is `HELLO   TXT`. Names are converted on the way in and rendered
back on the way out, so files created here are readable by a real FAT driver.

**Directory entries.** Creating a file writes a 32-byte entry into the first
free slot. Deleting one stamps `0xE5` over the first byte, the same tombstone
FAT itself uses, and frees the cluster chain. `mkdir` additionally seeds the new
cluster with its `.` and `..` entries.

**Open file table.** A fixed table of 10 slots tracks descriptor, mode, byte
offset, cached size, first cluster, and parent directory. Writes go through the
offset in that table, allocating and linking clusters when they cross a cluster
boundary and updating the directory entry's size and first cluster afterwards.

## Layout

```
src/main.c        entry point, shell loop, command dispatch
src/commands.c    one function per shell command
src/utils.c       FAT walking, cluster allocation, directory entry I/O, 8.3 names
src/lexer.c       line reading and tokenizing
include/          headers
tests/            end-to-end test suite
fat32.img.gz      sample volume, unpacked by `make image`
```

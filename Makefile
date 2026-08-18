SRC      := src
OBJ      := obj
BIN      := bin
EXECUTABLE := filesys

SRCS     := $(wildcard $(SRC)/*.c)
OBJS     := $(patsubst $(SRC)/%.c,$(OBJ)/%.o,$(SRCS))
INCS     := -Iinclude/
DIRS     := $(OBJ) $(BIN)
EXEC     := $(BIN)/$(EXECUTABLE)

IMAGE      := fat32.img
IMAGE_SEED := fat32.img.gz
IMAGE_MB   := 34            # FAT32 needs >= 65525 clusters; 34 MB is the floor

CC       := gcc
CFLAGS   := -g -Wall -Wextra -std=c99 $(INCS)
LDFLAGS  :=

all: $(EXEC)

$(EXEC): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OBJ)/%.o: $(SRC)/%.c | $(DIRS)
	$(CC) $(CFLAGS) -c $< -o $@

$(DIRS):
	mkdir -p $@

# Unpack the sample image that ships with the repo. It is stored gzipped
# because a FAT32 volume is mostly zeros: 34 MB compresses to about 34 KB.
$(IMAGE): $(IMAGE_SEED)
	gunzip -c $(IMAGE_SEED) > $@

image: $(IMAGE)

# Build a brand new empty volume instead of unpacking the sample one.
# Needs dosfstools (mkfs.vfat).
image-fresh:
	dd if=/dev/zero of=$(IMAGE) bs=1M count=$(IMAGE_MB) status=none
	mkfs.vfat -F 32 -n FAT32DISK $(IMAGE)

run: $(EXEC) $(IMAGE)
	./$(EXEC) $(IMAGE)

test: $(EXEC)
	./tests/run-tests.sh

clean:
	rm -rf $(OBJ) $(BIN)

# Also drops the unpacked image; the gzipped seed in git is untouched.
distclean: clean
	rm -f $(IMAGE)

.PHONY: all image image-fresh run test clean distclean

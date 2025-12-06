SRC      := src
OBJ      := obj
BIN      := bin
EXECUTABLE := filesys

SRCS     := $(wildcard $(SRC)/*.c)
OBJS     := $(patsubst $(SRC)/%.c,$(OBJ)/%.o,$(SRCS))
INCS     := -Iinclude/
DIRS     := $(OBJ) $(BIN)
EXEC     := $(BIN)/$(EXECUTABLE)

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

run: $(EXEC)
	./$(EXEC) fat32.img

clean:
	rm -rf $(OBJ)/*.o $(EXEC)

.PHONY: all run clean

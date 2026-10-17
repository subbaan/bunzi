CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -D_GNU_SOURCE
CFLAGS  += $(shell pkg-config --cflags sdl3 libpng)
LDLIBS  += $(shell pkg-config --libs sdl3 libpng) -lm -lz

BUILD := build
BIN   := $(BUILD)/bunzi
SRC   := $(wildcard src/*.c)
OBJ   := $(SRC:src/%.c=$(BUILD)/%.o)
HDR   := $(wildcard src/*.h)

.PHONY: all link clean

# the game's art (assets/*/gen) and sounds are committed, so a build needs
# only a C compiler; the local asset tools add `make assets` and `make sounds`
all: $(BIN) link

-include tools/assets.mk

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

link: $(BIN)
	ln -sfn $(BIN) bunzi

clean:
	rm -f $(OBJ) $(BIN)

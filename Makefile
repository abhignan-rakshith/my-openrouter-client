CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c23
CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -pthread \
           $(shell pkg-config --cflags libcurl libcmark)
LDFLAGS += -pthread
LDLIBS   = $(shell pkg-config --libs libcurl libcmark)

# `make install` symlinks the built binary into PREFIX/bin, so a plain
# `make` rebuild is picked up everywhere without reinstalling.
PREFIX  ?= $(HOME)/.local
BINDIR  := $(PREFIX)/bin

SRC := src/main.c src/api.c src/conv.c src/jsonutil.c src/config.c src/buffer.c \
       src/spinner.c src/md.c src/userconfig.c src/lineedit.c src/clipboard.c \
       src/save.c src/screen.c src/models.c src/image.c
OBJ := $(SRC:.c=.o)
HDR := $(wildcard src/*.h)

orc: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f orc $(OBJ)

# Symlink (not copy) so rebuilds are reflected without reinstalling.
install: orc
	@mkdir -p $(BINDIR)
	ln -sf $(abspath orc) $(BINDIR)/orc
	@echo "linked $(BINDIR)/orc -> $(abspath orc)"

uninstall:
	rm -f $(BINDIR)/orc

.PHONY: clean install uninstall

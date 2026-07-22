CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c23
CFLAGS  += -D_POSIX_C_SOURCE=200809L -pthread \
           $(shell pkg-config --cflags libcurl libcmark)
LDFLAGS += -pthread
LDLIBS   = $(shell pkg-config --libs libcurl libcmark)

SRC := src/main.c src/api.c src/conv.c src/jsonutil.c src/config.c src/buffer.c \
       src/spinner.c src/md.c src/userconfig.c
OBJ := $(SRC:.c=.o)
HDR := $(wildcard src/*.h)

orc: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f orc $(OBJ)

.PHONY: clean

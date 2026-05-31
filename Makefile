# epanel-tui C build
# Supports: Linux (x86_64, aarch64), macOS (x86_64, arm64)

NAME    := epanel
VERSION ?= v1.2.0
SRCDIR  := src
BUILDIR := build

SRCS    := $(SRCDIR)/main.c \
           $(SRCDIR)/app.c \
           $(SRCDIR)/ui.c \
           $(SRCDIR)/uuid.c \
           $(SRCDIR)/cJSON.c \
           $(SRCDIR)/file_watch.c \
           $(SRCDIR)/update.c

OBJS    := $(patsubst $(SRCDIR)/%.c,$(BUILDIR)/%.o,$(SRCS))

ifdef APPEND_CFLAGS
CFLAGS += $(APPEND_CFLAGS)
endif

CFLAGS  += -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS  += -Wall -Wextra -Werror -O2
CFLAGS  += -DVERSION=\"$(VERSION)\"

LDFLAGS += -lncurses -lpthread

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Allow overriding the target architecture for cross-compilation
ARCH ?= $(UNAME_M)

ifeq ($(UNAME_S),Linux)
    LDFLAGS += -lrt
    ifeq ($(CC),aarch64-linux-gnu-gcc)
        TARGET := aarch64-unknown-linux-gnu
        CFLAGS += -I/usr/aarch64-linux-gnu/include
        LDFLAGS += -L/usr/lib/aarch64-linux-gnu
    else
        TARGET := x86_64-unknown-linux-gnu
    endif
endif

ifeq ($(UNAME_S),Darwin)
    SRCS += $(SRCDIR)/safari_sync.c
    OBJS += $(BUILDIR)/safari_sync.o
    LDFLAGS += -framework CoreFoundation
    ifeq ($(ARCH),arm64)
        TARGET := aarch64-apple-darwin
    else
        TARGET := x86_64-apple-darwin
    endif
endif

.PHONY: all clean

BIN := $(BUILDIR)/$(NAME)

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILDIR)/%.o: $(SRCDIR)/%.c | $(BUILDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDIR):
	mkdir -p $@

clean:
	rm -rf $(BUILDIR)

# Release packaging (matches Rust workflow output layout)
dist: $(BIN)
	mkdir -p dist
	cp $(BIN) dist/
	tar -czf dist/$(NAME)-$(TARGET).tar.gz -C dist $(NAME)
	rm dist/$(NAME)

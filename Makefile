CC       = gcc
SDL_CFLAGS  := $(shell sdl2-config --cflags)
SDL_LIBS    := $(shell sdl2-config --libs)
FFMPEG_CFLAGS := $(shell pkg-config --cflags libavcodec libavutil libswscale)
FFMPEG_LIBS   := $(shell pkg-config --libs   libavcodec libavutil libswscale)
CFLAGS   = -Wall -Wextra -std=c11 -g -O2 -DPD_USE_FFMPEG -Isrc $(SDL_CFLAGS) $(FFMPEG_CFLAGS)
LDFLAGS  = $(SDL_LIBS) $(FFMPEG_LIBS) -lm
TARGET   = playdia

SRCS = src/main.c \
       src/cpu_tlcs870.c \
       src/cpu_nec78k.c \
       src/cdrom.c \
       src/ak8000.c \
       src/ak8000_pd.c \
       src/zip_stream.c \
       src/vfs_file.c \
       src/miniz/miniz.c \
       src/interconnect.c \
       src/pipeline.c \
       src/bios_hle.c \
       src/playdia_sys.c \
       src/sdl_frontend.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

HDRS = $(wildcard src/*.h)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

test: test_full
	./test_full

test_full: test_full.c src/cpu_tlcs870.c src/cpu_nec78k.c
	$(CC) -Wall -std=c11 -g -Isrc -o test_full test_full.c src/cpu_tlcs870.c src/cpu_nec78k.c

# ── libretro core ────────────────────────────────────────────
# The core has its own makefile, which carries the platform handling the
# buildbot needs. Keeping one source list there beats keeping two in step.
libretro:
	$(MAKE) -f Makefile.libretro

libretro-clean:
	$(MAKE) -f Makefile.libretro clean

.PHONY: libretro libretro-clean

clean:
	rm -f $(OBJS) $(TARGET) test_full

.PHONY: all test clean

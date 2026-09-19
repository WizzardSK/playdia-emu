LOCAL_PATH := $(call my-dir)

CORE_DIR := $(LOCAL_PATH)/../src

include $(CLEAR_VARS)

LOCAL_MODULE    := retro
LOCAL_SRC_FILES := \
	$(CORE_DIR)/playdia_libretro.c \
	$(CORE_DIR)/cpu_tlcs870.c \
	$(CORE_DIR)/cpu_nec78k.c \
	$(CORE_DIR)/cdrom.c \
	$(CORE_DIR)/ak8000.c \
	$(CORE_DIR)/ak8000_pd.c \
	$(CORE_DIR)/zip_stream.c \
	$(CORE_DIR)/vfs_file.c \
	$(CORE_DIR)/miniz/miniz.c \
	$(CORE_DIR)/interconnect.c \
	$(CORE_DIR)/pipeline.c \
	$(CORE_DIR)/bios_hle.c \
	$(CORE_DIR)/playdia_sys.c

LOCAL_CFLAGS  := -O2 -DNDEBUG -std=c11 -I$(CORE_DIR)
LOCAL_LDLIBS  := -lm
LOCAL_LDFLAGS := -Wl,--version-script=$(LOCAL_PATH)/../link.T -Wl,-Bsymbolic

include $(BUILD_SHARED_LIBRARY)

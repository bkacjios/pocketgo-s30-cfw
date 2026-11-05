# n64.mk - the shared N64 platform library for the PocketGo S30 ports
# (display, renderer, audio output and mixer, input). Included by a port's
# Makefile.s30, which sets:
#   N64LIB     this directory (default ../../n64: the port is in ports/<name>)
#   BUILD_DIR  where objects go ($(BUILD_DIR)/n64)
#   LINK       the clang wrapper (s30-build/bin/armv7-clang)
#   SYSROOT    the device sysroot
# and then links $(N64LIB_OBJS) with $(N64LIB_LIBS) (and includes their .d
# files with its own, after its first target). Game code includes the headers
# with $(N64LIB_INCLUDES).

N64LIB ?= ../../n64
# os.c (libultra's OS, s30_os.h) only for ports that set N64LIB_OS := 1
N64LIB_SRCS := $(filter-out $(if $(filter 1,$(N64LIB_OS)),,$(N64LIB)/src/os.c),$(wildcard $(N64LIB)/src/*.c))
N64LIB_OBJS := $(patsubst $(N64LIB)/src/%.c,$(BUILD_DIR)/n64/%.o,$(N64LIB_SRCS))
N64LIB_INCLUDES := -I$(N64LIB)/include/s30
N64LIB_LIBS := -lGLESv2 -lEGL -lgbm -ldrm -lasound -lpthread -lm

$(BUILD_DIR)/n64/%.o: $(N64LIB)/src/%.c
	@mkdir -p $(dir $@)
	$(LINK) -O2 -g -Wall -D_LANGUAGE_C -DF3DEX_GBI_PL -I$(N64LIB)/include/s30 -I$(N64LIB)/include -I$(SYSROOT)/usr/include/libdrm -MMD -MP -c $< -o $@

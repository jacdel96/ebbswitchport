#---------------------------------------------------------------------------------
# ebbswitchport native NRO — libnx frontend linking the snes9x libretro core.
# Based on the stock devkitPro switch application template.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment. e.g. export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET      is the output filename (and NRO/NACP name)
# BUILD       is the intermediate build dir
# SOURCES     dirs of .c/.cpp
# INCLUDES    header dirs
# ROMFS       embedded read-only filesystem (holds game.sfc)
# LIB (native/lib) holds snes9x_libretro_libnx.a
#---------------------------------------------------------------------------------
TARGET   := $(notdir $(CURDIR))
BUILD    := build
SOURCES  := source source/shaders
INCLUDES := include
ROMFS    := romfs

# Output subdir (under ROMFS) for compiled .dksh shaders — see the GLSLFILES/
# ROMFS_TARGETS block below, adapted from devkitPro's deko3d example Makefiles.
OUT_SHADERS := shaders

# App metadata — overridable per game from scripts/build_native.sh
APP_TITLE   ?= EarthBound (native)
APP_AUTHOR  ?= ebbswitchport
APP_VERSION ?= 0.1.0
# APP_ICON  ?= ../assets/icons/earthbound.jpg   # set per-build

#---------------------------------------------------------------------------------
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# Per-game save-file basename (build_native.sh overrides per game).
GAME_ID ?= game

CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) \
           $(INCLUDE) -D__SWITCH__
CFLAGS  += -DSWITCH=1 -DGAME_ID=\"$(GAME_ID)\"
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# snes9x core static lib (C++) + libstdc++ + libnx + deko3d (GPU rendering
# backend, see gpu_video.c). Debug lib (extra internal validation/asserts) by
# default since the GPU path hasn't run on real hardware yet; flip to 0 once
# it's verified working, to switch to the release lib.
DEKO3D_DEBUG ?= 1
ifeq ($(DEKO3D_DEBUG),1)
LIBS := -ldeko3dd -lsnes9x_libretro_libnx -lstdc++ -lnx -lm
else
LIBS := -ldeko3d -lsnes9x_libretro_libnx -lstdc++ -lnx -lm
endif

# lib search: native/lib (core) + libnx.
LIBDIRS := $(PORTLIBS) $(LIBNX) $(TOPDIR)/lib

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH  := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                 $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) -L$(TOPDIR)/lib

# Shaders: .glsl sources -> uam -> .dksh, landing in $(ROMFS)/$(OUT_SHADERS) so
# gpu_video.c can fopen() them from romfs at runtime.
GLSLFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.glsl)))

ifneq ($(strip $(ROMFS)),)
	ROMFS_TARGETS :=
	ROMFS_FOLDERS :=
	ifneq ($(strip $(OUT_SHADERS)),)
		ROMFS_SHADERS := $(ROMFS)/$(OUT_SHADERS)
		ROMFS_TARGETS += $(patsubst %.glsl,$(ROMFS_SHADERS)/%.dksh,$(GLSLFILES))
		ROMFS_FOLDERS += $(ROMFS_SHADERS)
	endif
	export ROMFS_DEPS := $(foreach file,$(ROMFS_TARGETS),$(CURDIR)/$(file))
endif

export BUILD_EXEFS_SRC := $(TOPDIR)/exefs_src

ifeq ($(strip $(APP_ICON)),)
	export APP_ICON := $(LIBNX)/default_icon.jpg
endif

export NROFLAGS += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp

ifeq ($(strip $(ROMFS)),)
else
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all
all: $(ROMFS_TARGETS) | $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@[ -d $@ ] || mkdir -p $@

ifneq ($(strip $(ROMFS_TARGETS)),)
$(ROMFS_TARGETS): | $(ROMFS_FOLDERS)

$(ROMFS_FOLDERS):
	@mkdir -p $@

$(ROMFS_SHADERS)/%_vsh.dksh: %_vsh.glsl
	@echo {vert} $(notdir $<)
	@uam -s vert -o $@ $<

$(ROMFS_SHADERS)/%_fsh.dksh: %_fsh.glsl
	@echo {frag} $(notdir $<)
	@uam -s frag -o $@ $<

$(ROMFS_SHADERS)/%_comp.dksh: %_comp.glsl
	@echo {comp} $(notdir $<)
	@uam -s comp -o $@ $<
endif

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(ROMFS_FOLDERS) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else
.PHONY: all
DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp $(ROMFS_DEPS)
$(OUTPUT).elf : $(OFILES)

-include $(DEPENDS)
endif

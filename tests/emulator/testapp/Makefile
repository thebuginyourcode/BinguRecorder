# Builds BinguTest.cxi: a tiny test "game" for running BinguRecorder in an
# emulator (see ../README.md). Needs devkitARM, libctru and makerom.
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
include $(DEVKITARM)/3ds_rules

TARGET   := BinguTest
SOURCES  := source
ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := -Wall -Wextra -O2 -mword-relocations -ffunction-sections $(ARCH) -I$(CTRULIB)/include -D__3DS__
ASFLAGS  := $(ARCH)
LDFLAGS  := -specs=3dsx.specs $(ARCH) -L$(CTRULIB)/lib

OBJS := $(patsubst $(SOURCES)/%.c,build/%.o,$(wildcard $(SOURCES)/*.c)) \
        $(patsubst $(SOURCES)/%.s,build/%.o,$(wildcard $(SOURCES)/*.s))

all: $(TARGET).cxi

build/%.o: $(SOURCES)/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: $(SOURCES)/%.s
	@mkdir -p build
	$(CC) $(ASFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -lctru -lm -o $@

$(TARGET).smdh:
	smdhtool --create "BinguTest" "BinguRecorder test app" "BinguRecorder" $(CTRULIB)/default_icon.png $@

$(TARGET).cxi: $(TARGET).elf $(TARGET).smdh app.rsf
	makerom -f cxi -o $@ -rsf app.rsf -target t -elf $(TARGET).elf -icon $(TARGET).smdh

clean:
	rm -rf build $(TARGET).elf $(TARGET).smdh $(TARGET).cxi

.PHONY: all clean

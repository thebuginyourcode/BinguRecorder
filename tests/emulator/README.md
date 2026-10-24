# Emulator test

Runs BinguRecorder in the [Azahar](https://github.com/azahar-emu/azahar)
emulator with a small test "game", presses L+R+Down to record 20 seconds,
and checks the video it saves.

- `testapp/`: the test game (a `.cxi` title). It draws a moving pattern with
  its frame number as 16 black/white blocks and beeps once a second. Its
  frame "present" and GPU interrupt setup are copies of Nintendo's library
  code, which CTRPF looks for and hooks in real games.
- `host/retro_host.cpp`: a tiny headless libretro frontend. It runs the
  emulator, holds buttons on chosen frames, and can save screenshots
  (`--shot`) and the emulator's sound (`--audio`).
- `allow_homebrew.py`: makes a copy of the plugin that loads into homebrew
  (the test app is built with libctru).
- `analyze.py`: reads the frame numbers and beeps back out of the recording,
  checks the frame timing, and measures audio/video sync.
- `run.sh`: does all of the above.

## Building the emulator (libretro core, software rendering)

```sh
git clone --depth 1 https://github.com/azahar-emu/azahar.git
cd azahar && git submodule update --init --recursive --depth 1
# Turn on the plugin loader (it isn't a libretro option)
sed -i 's/^static void ParseStorageOptions(void) {/&\n    Settings::values.plugin_loader_enabled = true;/' \
    src/citra_libretro/core_settings.cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_LIBRETRO=ON -DENABLE_QT=OFF \
    -DENABLE_SDL2=OFF -DENABLE_VULKAN=OFF -DENABLE_OPENGL=OFF -DENABLE_TESTS=OFF \
    -DENABLE_ROOM=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_CUBEB=OFF -DENABLE_OPENAL=OFF \
    -DENABLE_LIBUSB=OFF -DENABLE_SCRIPTING=OFF -DENABLE_GDBSTUB=OFF \
    -DCITRA_WARNINGS_AS_ERRORS=OFF
make -C build -j$(nproc) azahar_libretro    # build/bin/Release/azahar_libretro.so
```

The test app also needs `makerom`
([Project_CTR](https://github.com/3DSGuy/Project_CTR)) in `PATH`.

## Running

```sh
AZAHAR_CORE=/path/to/azahar/build/bin/Release/azahar_libretro.so tests/emulator/run.sh
```

The emulator's timing is close to an Old 3DS, but not exact, so treat the
frame rates it reports as a guide for real hardware.

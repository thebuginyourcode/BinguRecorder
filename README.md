# BinguRecorder

A screen recorder for the Nintendo 3DS, made to run on the **Old 3DS**.

While you play, it takes screenshots of the game several times a second,
compresses each one as a JPEG, records the game's sound, and stitches it all
into an `.avi` video on your SD card. An hour of gameplay with sound fits in
well under 1 GB, so it works for long let's play sessions.

## Why it's a plugin and not a `.3dsx` app

A normal homebrew app (`.3dsx`) can only see its own screen. Once you start a
game, the app isn't running anymore. To record a game, BinguRecorder runs
*inside* the game as a **Luma3DS plugin** (`.3gx`). It's built with
[CTRPluginFramework](https://gitlab.com/thepixellizeross/ctrpluginframework).

## Requirements

- A 3DS with custom firmware, running **Luma3DS v13.0 or newer**. Older
  versions don't have the plugin loader built in.
- An SD card with free space. With the default settings, expect roughly
  9–12 MB per minute of video with sound. It depends on how busy the game's
  picture is.

## Installing

1. Copy `dist/BinguRecorder.3gx` to your SD card as
   `sd:/luma/plugins/default.3gx`. The plugin then loads in every game.
   To load it in one game only, use
   `sd:/luma/plugins/<TitleID>/BinguRecorder.3gx` instead.
2. Turn on the plugin loader: press **L + Down + Select** to open the
   Rosalina menu, select **Plugin Loader: [Disabled]** so it changes to
   Enabled, then close the menu.
3. Start a game. The screen flashes blue when the plugin has loaded.

## Recording

| Action | How |
| --- | --- |
| Start or stop recording in game | **L + R + Down** |
| Open the plugin menu | **Select** |
| Start or stop from the menu | Menu → *Start / stop recording* |
| Change the hotkey | Menu → select *Start / stop recording* → controller icon |

Videos are saved in `sd:/BinguRecorder/` with names like
`BinguRec_2026-10-24_15-30-00.avi`. When a file is complete, the top screen
shows a "Saved …" notification.

While you record, a blinking **REC** timer is drawn on the screen that isn't
being recorded, so it doesn't show up in the video. You can turn it off in
Settings.

Time spent in the plugin menu is cut from the video. If the game closes, you
press HOME, or you close the lid, the recording is stopped and saved first.

## Quick setup

Menu → **Quick setup** picks a size, frame rate and priority in one step:

| Preset | Size | Frame rate | Good for |
| --- | --- | --- | --- |
| Sharp | Full (400×240) | 10 fps | RPGs, menus, slow games |
| Balanced | Full | 15 fps | most games |
| **Smooth** | Half (200×120) | 30 fps | action games, let's plays |
| Smoothest | Half | 30 fps, Priority: Recording | heavy games where Smooth skips frames |

10 fps looks choppy on YouTube for anything fast-moving. For let's plays,
**Smooth** is usually the best choice. Upload it scaled up with sharp pixels
(see "The video files" below). Smoothest makes the encoder go ahead of the
game, so the game can slow down while you record. Only use it if the `.txt`
report for a Smooth recording shows skipped frames.

In the emulator test, every preset recorded its full frame rate with no
skipped frames, and the sound was within about 10 ms of the picture.

## Settings (Menu → Settings)

| Setting | Options | Default |
| --- | --- | --- |
| Sound | Off, On (stereo), Small (mono) | On |
| Compression | On (JPEG frames) / Off (raw bitmaps) | On |
| Quality | Low, Medium, High, Max | Medium |
| Priority | Game (full speed) / Recording (smoother video) | Game |
| Screen | Top, Bottom, Both (top above bottom) | Top |
| Frame rate | 5, 10, 15, 20, 24, 30 screenshots per second | 10 |
| Size | Full (400×240) or Half (200×120) | Full |
| REC indicator | On / Off | On |

Settings are saved to `sd:/BinguRecorder/settings.bin`.

### Compression

With compression on, every screenshot is saved as a JPEG inside the AVI
("Motion JPEG"). This makes files about 10× smaller than raw bitmaps.

| Per minute (top screen, full size, 10 fps) | Picture | + Sound (On) | + Sound (Small) |
| --- | --- | --- | --- |
| Compression off | ~115 MB | +2 MB | +0.5 MB |
| Compressed, Medium (default) | ~7–10 MB | +2 MB | +0.5 MB |
| Compressed, High | ~10–15 MB | +2 MB | +0.5 MB |

With the defaults, an hour is roughly 0.55–0.75 GB. Each quality step
changes the picture's size by roughly a third.

Compressing takes CPU time. It runs on two threads. One is on the system
core, where the game's main thread never runs. The other is on the game's
core at the lowest priority, so it only uses time the game leaves free. With
the default **Priority: Game**, recording never slows the game down. If
there isn't enough spare time, it records fewer frames. With
**Priority: Recording**, the thread on the game's core goes first instead.
You get more frames, but the game may slow down while you record.

### Report file

Next to every video, BinguRecorder saves a `.txt` file with the same name.
It shows how many frames the game drew, how many were recorded, how long
each one took to compress, and whether the sound was captured. If a video
looks choppy or has no sound, this file shows why.

### Sound

BinguRecorder records what comes out of the speakers. It reads the final mix
that the 3DS's sound chip (DSP) places in memory every ~5 ms. The sound is
compressed with IMA ADPCM, a fast format most players support:

- **On:** stereo at 32,728 Hz, the 3DS's native rate. About 2 MB per minute.
- **Small:** mono at half that rate. About 0.5 MB per minute. Fine for
  speech and most music, but you lose the stereo.

Sound is lined up with the first video frame and kept in sync for the whole
recording. If it ever has to skip, it inserts silence of the same length
rather than shifting later sound earlier.

A few things to know:

- Your own voice isn't recorded, only the game's sound.
- If the "Saved" message ends with "no sound", that game didn't give access
  to the sound chip's memory. The video is still saved.

### What the Old 3DS can keep up with

The Old 3DS CPU is slow. These settings are a starting point:

- **Top, Full, 10–15 fps.** Compressing a full-size frame took about 50 ms
  in the emulator test, so about 20 fps is the most full size can reach.
- **Top, Half, 30 fps.** Smoother motion, lower resolution. A half-size frame
  took about 11 ms.
- **Both, Full, 5 fps** or **Both, Half, 10–15 fps.**

Games that already use all of the CPU leave less time for compression, and
videos of them will look choppier. If that happens, try Half size or
**Priority: Recording**. The report file shows how many frames per second
were actually recorded. Any frame the 3DS can't keep up with is stored as a
repeat of the previous one, so the video still plays at the correct speed.

## Watching videos on the 3DS

`dist/BinguPlayer.3dsx` is a video player for your recordings.

1. Copy `BinguPlayer.3dsx` to `sd:/3ds/`.
2. Open it from the Homebrew Launcher. The bottom screen lists your
   recordings, newest first.
3. Press **A** to play.

| Button | Action |
| --- | --- |
| A | Play / pause |
| Left / Right | Back / forward 10 seconds |
| B | Back to the list |
| START | Exit (in the list) |

Videos play on the screen they were recorded from: top-screen videos on the
top screen, bottom-screen videos on the bottom screen, and "Both" videos on
both screens. Half-size recordings are scaled back up to fill the screen.

**Sound** needs the 3DS sound firmware dump that most homebrew uses. If the
player says "No sound", run [DSP1](https://github.com/zoogie/DSP1) once. It
saves the firmware to `sd:/3ds/dspfirm.cdc`.

## The video files

The videos are AVI files. With compression on they use Motion JPEG; with
compression off every frame is a 16-bit bitmap. Both play in VLC, mpv
and ffmpeg, and most video editors open them, so you can drop them
straight into a let's play project.

To make an even smaller MP4 for uploading, run this on a PC:

```sh
ffmpeg -i BinguRec_2026-10-24_15-30-00.avi -c:v libx264 -crf 18 -pix_fmt yuv420p -vf "scale=iw*2:ih*2:flags=neighbor" out.mp4
```

The `scale` filter doubles the size with sharp pixels; you can leave it out.

Long recordings are split into files of about 2 GB (`…_part2.avi`, …),
because many players can't open bigger AVI files.

If the 3DS crashes or runs out of battery during a recording, the file is
usually still playable. The plugin updates the file header every few
seconds while it records.

## Building

You need [devkitARM](https://devkitpro.org/wiki/Getting_Started) with
libctru, plus CTRPluginFramework and 3gxtool. Run this script to build both
and install them into `$DEVKITPRO`:

```sh
scripts/install-ctrpf.sh
make -C plugin            # -> plugin/BinguRecorder.3gx
```

The easiest setup is the `devkitpro/devkitarm` Docker image. GitHub Actions
(`.github/workflows/build.yml`) builds the plugin this way on every push. You
can download the result from the run's **Artifacts**.

`tests/run_tests.sh` compiles the AVI writer and JPEG encoder for your PC and
checks their output with ffmpeg (needs `g++` and `ffmpeg`).

`tests/emulator/` runs the actual plugin in the Azahar emulator with a test
game. It records 20 seconds and checks the frame timing and audio/video
sync (see `tests/emulator/README.md`).

## How it works

- `plugin/Sources/Recorder.cpp` uses the CTRPF OSD hook, which runs every time
  the game shows a new frame. When a screenshot is due, the hook converts the
  framebuffer to RGB555 and puts it in one of three buffers. 3DS framebuffers
  are stored rotated, with bottom-up columns, so this conversion is a single
  pass in BMP row order.
- Worker threads take frames from those buffers, compress them, and append
  them to the AVI file in order. If frames were skipped, they write empty
  "repeat" frames to fill the gap.
- A small high-priority thread checks the DSP's frame counters every 2 ms.
  When the DSP finishes a tick, the thread copies the 160 new stereo samples
  into a ring buffer. The workers encode the sound to IMA ADPCM
  (`plugin/Sources/ImaAdpcm.cpp`) and write it between the video frames.
- `plugin/Sources/JpegEncoder.cpp` is a small baseline JPEG encoder made for
  the ARM11 CPU. It uses an integer DCT, 4:2:0 chroma and a lookup table for
  brightness.
- `plugin/Sources/AviWriter.cpp` is a small AVI 1.0 writer (`RIFF`/`hdrl`/`movi`/`idx1`)
  with one video and one audio stream.

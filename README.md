# BinguRecorder

A screen recorder for the Nintendo 3DS, made to run on the **Old 3DS**.

While you play, it takes screenshots of the game several times a second,
compresses each one as a JPEG, and stitches them into an `.avi` video on your
SD card. An hour of gameplay fits in well under 1 GB, so it works for long
let's play sessions.

## Why it's a plugin and not a `.3dsx` app

A normal homebrew app (`.3dsx`) can only see its own screen. Once you start a
game, the app isn't running anymore. To record a game, BinguRecorder runs
*inside* the game as a **Luma3DS plugin** (`.3gx`). It's built with
[CTRPluginFramework](https://gitlab.com/thepixellizeross/ctrpluginframework).

## Requirements

- A 3DS with custom firmware, running **Luma3DS v13.0 or newer**. Older
  versions don't have the plugin loader built in.
- An SD card with free space. With the default settings, expect roughly
  10–15 MB per minute of video. It depends on how busy the game's picture is.

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

## Settings (Menu → Settings)

| Setting | Options | Default |
| --- | --- | --- |
| Compression | On (JPEG frames) / Off (raw bitmaps) | On |
| Quality | Low, Medium, High, Max | High |
| Screen | Top, Bottom, Both (top above bottom) | Top |
| Frame rate | 5, 10, 15, 20, 30 screenshots per second | 10 |
| Size | Full (400×240) or Half (200×120) | Full |
| REC indicator | On / Off | On |

Settings are saved to `sd:/BinguRecorder/settings.bin`.

### Compression

With compression on, every screenshot is saved as a JPEG inside the AVI
("Motion JPEG"). This makes files about 10× smaller than raw bitmaps.

| | Raw (compression off) | Compressed, High quality |
| --- | --- | --- |
| Per minute (top screen, full size, 10 fps) | ~115 MB | ~10–15 MB |
| 1 hour | ~7 GB | ~0.6–0.9 GB |

Lower quality makes files smaller still. Each step down saves roughly a
third of the size.

Compressing takes CPU time. BinguRecorder only uses the time the game leaves
free, so it never slows the game down. It runs on the lowest-priority
threads, one on each CPU core when the game allows it. If there isn't
enough spare time, it records fewer frames.

### What the Old 3DS can keep up with

The Old 3DS CPU is slow. These settings are a starting point:

- **Top, Full, 10 fps.** This is the default and works in most games.
- **Top, Half, 20–30 fps.** Smoother motion, lower resolution.
- **Both, Full, 5 fps** or **Both, Half, 10–15 fps.**

Games that already use all of the CPU leave less time for compression, and
videos of them will look choppier. Turn compression off (if you have the SD
space) or use Half size. Any frame the 3DS can't keep up with is stored as
a repeat of the previous one, so the video still plays at the correct speed.

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

## How it works

- `plugin/Sources/Recorder.cpp` uses the CTRPF OSD hook, which runs every time
  the game shows a new frame. When a screenshot is due, the hook converts the
  framebuffer to RGB555 and puts it in one of three buffers. 3DS framebuffers
  are stored rotated, with bottom-up columns, so this conversion is a single
  pass in BMP row order.
- Worker threads take frames from those buffers, compress them, and append
  them to the AVI file in order. If frames were skipped, they write empty
  "repeat" frames to fill the gap.
- `plugin/Sources/JpegEncoder.cpp` is a small baseline JPEG encoder made for
  the ARM11 CPU. It uses an integer DCT, 4:2:0 chroma and a lookup table for
  brightness.
- `plugin/Sources/AviWriter.cpp` is a small AVI 1.0 writer (`RIFF`/`hdrl`/`movi`/`idx1`).

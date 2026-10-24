#!/usr/bin/env python3
"""Checks a BinguRecorder recording of the emulator test app (testapp/).

The test app draws its frame number as 16 black/white blocks and beeps at
every whole second of its sound clock. From the recording this reads which
game frame each video frame shows, and how far each beep is from the moment
the matching game frame is on screen (audio/video sync).

usage: analyze.py <recording.avi>     (needs ffmpeg)
"""
import struct
import subprocess
import sys
import tempfile

W, H, RATE = 400, 240, 32728


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main(path):
    with tempfile.TemporaryDirectory() as tmp:
        run('ffmpeg', '-i', path, '-f', 'rawvideo', '-pix_fmt', 'rgb24', tmp + '/v.rgb')
        run('ffmpeg', '-i', path, '-map', '0:a', '-ac', '1', '-f', 's16le', tmp + '/a.pcm')
        video = open(tmp + '/v.rgb', 'rb').read()
        audio = open(tmp + '/a.pcm', 'rb').read()

    frames = len(video) // (W * H * 3)

    def counter(f):
        value = 0
        for bit in range(16):
            o = (f * W * H + 14 * W + 4 + bit * 24 + 10) * 3
            if sum(video[o:o + 3]) / 3 > 128:
                value |= 1 << bit
        return value

    c = [counter(f) for f in range(frames)]
    steps = [b - a for a, b in zip(c, c[1:])]
    game_fps = (c[-1] - c[0]) / ((frames - 1) / 10)
    print('video: %d frames (%.1f s), game frame rate %.2f fps' % (frames, frames / 10, game_fps))
    print('game frames between video frames: min %d, max %d, average %.2f' % (min(steps), max(steps), sum(steps) / len(steps)))

    samples = struct.unpack('<%dh' % (len(audio) // 2), audio)
    win, beeps, loud_before = 164, [], False
    for i in range(0, len(samples) - win, win):
        loud = sum(x * x for x in samples[i:i + win]) / win > 4e7
        if loud and not loud_before:
            beeps.append(i / RATE)
        loud_before = loud

    def video_time(app_seconds):
        g = app_seconds * game_fps
        for i in range(frames - 1):
            if c[i] <= g < c[i + 1]:
                return i / 10 + (g - c[i]) / game_fps
        return None

    offsets = []
    for b in beeps:
        candidates = [video_time(k) for k in range(int(c[0] / game_fps) - 1, int(c[-1] / game_fps) + 2)]
        candidates = [t for t in candidates if t is not None]
        if candidates:
            offsets.append(b - min(candidates, key=lambda t: abs(t - b)))

    print('beeps: %d, spacing %s s' % (len(beeps), sorted({round(b - a, 2) for a, b in zip(beeps, beeps[1:])})))
    if offsets:
        print('sound vs picture: average %+.3f s, range %+.3f to %+.3f s'
              % (sum(offsets) / len(offsets), min(offsets), max(offsets)))

    ok = min(steps) > 0 and offsets and max(abs(o) for o in offsets) < 0.1
    print('OK' if ok else 'PROBLEM')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))

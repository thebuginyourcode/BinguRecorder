#!/usr/bin/env python3
"""Checks a BinguRecorder recording of the emulator test app (testapp/).

The test app draws its frame number and its clock (10 ms steps) as rows of
16 black/white blocks, and beeps at every whole second of its sound clock.
From the recording this reads which game frame each video frame shows, and
how far each beep is from the moment the app's clock shows that second on
screen (audio/video sync, independent of the game's frame rate).

usage: analyze.py <recording.avi>     (needs ffmpeg)
"""
import struct
import subprocess
import sys
import tempfile

RATE = 32728


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main(path):
    probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                            'stream=width,height,r_frame_rate', '-of', 'csv=p=0', path],
                           check=True, capture_output=True, text=True).stdout.strip().split(',')
    W, H = int(probe[0]), int(probe[1])
    num, den = probe[2].split('/')
    fps = int(num) / int(den)
    scale = 400 / W     # 2 for half-size recordings

    with tempfile.TemporaryDirectory() as tmp:
        run('ffmpeg', '-i', path, '-f', 'rawvideo', '-pix_fmt', 'rgb24', tmp + '/v.rgb')
        run('ffmpeg', '-i', path, '-map', '0:a', '-ac', '1', '-f', 's16le', tmp + '/a.pcm')
        video = open(tmp + '/v.rgb', 'rb').read()
        audio = open(tmp + '/a.pcm', 'rb').read()

    frames = len(video) // (W * H * 3)

    def bits(f, row_y):
        value = 0
        for bit in range(16):
            x, y = int((4 + bit * 24 + 10) / scale), int(row_y / scale)
            o = (f * W * H + y * W + x) * 3
            if sum(video[o:o + 3]) / 3 > 128:
                value |= 1 << bit
        return value

    c = [bits(f, 14) for f in range(frames)]          # game frame number
    clock = [bits(f, 42) / 100 for f in range(frames)]  # app time, seconds
    steps = [b - a for a, b in zip(c, c[1:]) if b != a]
    game_fps = (c[-1] - c[0]) / ((frames - 1) / fps)
    real = sum(1 for a, b in zip(c, c[1:]) if b != a) + 1
    print('video: %dx%d, %d frames at %g fps (%.1f s), game frame rate %.2f fps'
          % (W, H, frames, fps, frames / fps, game_fps))
    print('different pictures: %d (%.1f per second)' % (real, real / (frames / fps)))
    print('game frames between different pictures: min %d, max %d, average %.2f'
          % (min(steps), max(steps), sum(steps) / len(steps)))

    samples = struct.unpack('<%dh' % (len(audio) // 2), audio)
    win, beeps, loud_before = 164, [], False
    for i in range(0, len(samples) - win, win):
        loud = sum(x * x for x in samples[i:i + win]) / win > 4e7
        if loud and not loud_before:
            beeps.append(i / RATE)
        loud_before = loud

    # Video time at which the app's on-screen clock reaches `app_seconds`.
    # A picture stays up until the next one, so this is accurate to one
    # video frame plus the app's own 10 ms clock steps.
    def video_time(app_seconds):
        for i in range(frames - 1):
            if clock[i] <= app_seconds < clock[i + 1]:
                span = clock[i + 1] - clock[i]
                return (i + (app_seconds - clock[i]) / span) / fps
        return None

    offsets = []
    for b in beeps:
        candidates = [video_time(k) for k in range(int(clock[0]) - 1, int(clock[-1]) + 2)]
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

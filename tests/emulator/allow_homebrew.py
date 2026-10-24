#!/usr/bin/env python3
"""Makes a copy of a .3gx plugin that the plugin loader also loads into
homebrew apps (header flag allow_homebrew_load, bit 9 of the flags word).

Only for testing with the emulator test app: on a real console the plugin is
meant for games, and stays out of homebrew such as BinguPlayer."""
import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, 'rb').read())
assert data[:8] == b'3GX$0002', 'not a 3GX v2 plugin'

FLAGS = 8 + 4 + 4 + 8 * 4   # magic, version, reserved, 8 string fields
flags = struct.unpack_from('<I', data, FLAGS)[0]
struct.pack_into('<I', data, FLAGS, flags | (1 << 9))
open(dst, 'wb').write(data)
print('flags 0x%08x -> 0x%08x' % (flags, flags | (1 << 9)))

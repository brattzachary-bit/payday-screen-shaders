#!/usr/bin/env python3
"""Converts the lens dirt photograph into the blob the plugin loads at runtime.

The plugin cannot decode a JPEG (no WIC, no libjpeg: it is a small MinGW DLL), so the source
image is converted once, here, into a tiny uncompressed file next to the shader:

    "PSSD" | uint32 width | uint32 height | width * height bytes, one grey level each

The dirt texture is a dark field with sparse bright specks and it is only ever read as a single
channel, so one byte per texel is all it needs.

    python3 dev/nativebuild/make_dirt.py [source.jpg] [out.bin]

Defaults: dev/nativebuild/lens_dirt_source.jpg -> Payday Screen Shaders/shaders/lens_dirt.bin
"""
import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

SOURCE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "lens_dirt_source.jpg")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "Payday Screen Shaders", "shaders", "lens_dirt.bin")

# Wide enough to keep the specks crisp, small enough to ship: 1024 x 576 is 576 KB.
WIDTH, HEIGHT = 1024, 576


def main():
    image = Image.open(SOURCE).convert("L").resize((WIDTH, HEIGHT), Image.LANCZOS)
    data = image.tobytes()

    assert len(data) == WIDTH * HEIGHT, len(data)

    with open(OUT, "wb") as f:
        f.write(b"PSSD")
        f.write(struct.pack("<II", WIDTH, HEIGHT))
        f.write(data)

    values = list(data)
    mean = sum(values) / len(values)
    bright = sum(1 for v in values if v > 128) / len(values)

    print("%s -> %s" % (os.path.basename(SOURCE), OUT))
    print("  %d x %d, %.0f KB, mean %.1f/255, %.2f%% bright specks" % (WIDTH, HEIGHT, (8 + len(data)) / 1024.0, mean, bright * 100))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Turns SMAA's two lookup textures into blobs the plugin can load.

SMAA (Jimenez, Echevarria, Masia, Navarro, Gutierrez) ships AreaTex and SearchTex as C headers.
The plugin cannot parse C, and cannot decode PNG, so they are converted here exactly like the lens
dirt: a small header, then raw bytes.

  AreaTex   160 x 560, two channels (R8G8)
  SearchTex  64 x  16, one channel  (R8)

Source: https://github.com/iryoku/smaa (MIT licence; the notice travels with the data).
"""
import re
import struct
import sys

MAGIC = b"PSSS"


def bytes_from_header(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    body = text[text.index("{", text.index("[]")) + 1:]
    body = body[:body.index("}")]
    values = [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]{2}|\b\d{1,3}\b", body)]
    return bytes(values)


def convert(src, dst, width, height, channels):
    data = bytes_from_header(src)
    want = width * height * channels

    if len(data) != want:
        sys.exit("%s: got %d bytes, expected %d" % (src, len(data), want))

    with open(dst, "wb") as f:
        f.write(MAGIC + struct.pack("<III", width, height, channels) + data)

    print("%s -> %s (%dx%d, %d channel(s), %d bytes)" % (src, dst, width, height, channels, len(data)))


if __name__ == "__main__":
    area, search, out = sys.argv[1], sys.argv[2], sys.argv[3]
    convert(area, out + "/smaa_area.bin", 160, 560, 2)
    convert(search, out + "/smaa_search.bin", 64, 16, 1)

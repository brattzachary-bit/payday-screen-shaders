#!/usr/bin/env python3
"""Renders a screenshot through Payday Screen Shaders' real shader chain, for tuning defaults by eye.

    python3 preview.py <image> <out.png> [key=value ...] [--vanilla]

Settings are the defaults in lua/core.lua, turned into what the plugin receives exactly as
PSS:native_value does (toggles 1/0, choices index - 1, a strength whose switch is off sent as 0),
then any key=value overrides. --vanilla renders with the plugin's neutral values instead.

A screenshot has no depth, so only effects that work from colour change the picture: grading,
bloom, adaptation, local tone mapping, sharpening, lens effects. Needs the native test built
(build_pss.sh test) and wine with Proton's vkd3d, as for the native test.
"""
import os
import re
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MOD = os.path.join(ROOT, "Payday Screen Shaders")
OUT = os.path.join(HERE, "out")


def lua_defaults():
    text = open(os.path.join(MOD, "lua", "core.lua")).read()
    block = text[text.index("PSS.PARAMS = {"):]
    block = block[:block.index("\n}\n")]
    params = []

    for line in block.splitlines():
        m = re.search(r'\{key = "(\w+)", kind = "(\w+)", default = ([^,}]+)(.*)\}', line)

        if m:
            key, kind, default, rest = m.groups()
            gate = re.search(r'gate = "(\w+)"', rest)
            params.append({"key": key, "kind": kind, "default": default.strip(), "gate": gate.group(1) if gate else None,
                           "lua_only": "lua_only = true" in rest})

    return params


def native_lines(overrides):
    params = lua_defaults()
    values = {p["key"]: p["default"] for p in params}
    values.update(overrides)
    lines = []

    for p in params:
        if p["lua_only"] or p["kind"] == "lut":
            continue

        v = values[p["key"]]

        if p["gate"] and values[p["gate"]] != "true":
            n = 0.0
        elif p["kind"] == "toggle":
            n = 1.0 if v == "true" else 0.0
        elif p["kind"] == "choice":
            n = float(v) - 1.0
        else:
            n = float(v)

        lines.append("%s %s" % (p["key"], n))

    luts = sorted(f for f in os.listdir(os.path.join(MOD, "luts")) if f.lower().endswith(".cube"))
    lut = luts[int(values.get("lut", "1")) - 1]
    return lines, lut


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    vanilla = "--vanilla" in sys.argv
    image, target = args[0], args[1]
    overrides = dict(a.split("=", 1) for a in args[2:])

    im = Image.open(image).convert("RGBA")
    w, h = im.size
    open(os.path.join(OUT, "preview_in.rgba"), "wb").write(im.tobytes())

    for name in ("pss.hlsl", "smaa.hlsl", "smaa_area.bin", "smaa_search.bin", "lens_dirt.bin"):
        with open(os.path.join(MOD, "shaders", name), "rb") as src, open(os.path.join(OUT, name), "wb") as dst:
            dst.write(src.read())

    lines, lut = native_lines(overrides)

    with open(os.path.join(MOD, "luts", lut), "rb") as src, open(os.path.join(OUT, "preview.cube"), "wb") as dst:
        dst.write(src.read())

    settings = ["enabled 1"] if vanilla else lines + ["lut_path preview.cube", "dirt_path lens_dirt.bin"]
    open(os.path.join(OUT, "preview_settings.txt"), "w").write("\n".join(settings) + "\n")

    env = dict(os.environ, WINEPREFIX=os.path.expanduser("~/.wine"), WINEDEBUG="-all")
    result = subprocess.run(["wine", "pss_test.exe", "--render", "pss.hlsl", "preview_settings.txt", "preview_in.rgba",
                             str(w), str(h), "preview_out.rgba", "12"], cwd=OUT, env=env, capture_output=True, text=True)

    if result.returncode != 0:
        sys.exit("render failed: " + result.stdout + result.stderr)

    Image.frombytes("RGBA", (w, h), open(os.path.join(OUT, "preview_out.rgba"), "rb").read()).convert("RGB").save(target)
    print("wrote", target)


if __name__ == "__main__":
    main()

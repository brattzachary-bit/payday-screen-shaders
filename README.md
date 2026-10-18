# Payday Screen Shaders

Screen shaders for PAYDAY 2 that change the game world only. The HUD and menus are never touched.

The camera, your settings and the plugin's status pass between the mod and its plugin through memory, so nothing is written to your drive while you play; only your preferences are saved to a file. This needs SuperBLT 4.0.3-beta or newer; on an older SuperBLT the mod falls back to small files and says so in mods/logs/pss_native.log.

A SuperBLT native plugin runs the mod's own shaders at the moment the game has finished drawing the
world and has not drawn the HUD yet. It is not ReShade and not a tweak of the game's own settings.

## Install

1. Install [SuperBLT](https://superblt.znix.xyz/).
2. Open [Tags](https://codeberg.org/pd2-mods/ScreenShaders/tags) and download the newest version as
   a ZIP.
3. Unzip it into `PAYDAY 2/mods/`, so that `mod.txt` sits directly inside the mod's folder:
   `PAYDAY 2/mods/<folder>/mod.txt`. Keep `native/pss.dll` inside it. The `dev` folder is not used by
   the game and can be deleted.
4. Start the game. The first launch pauses for a few seconds while the shaders compile; later
   launches use the cache.

After an update, restart the game: the plugin cannot be reloaded while the game runs.

## The look

It ships with the author's own settings: an enhanced PAYDAY 2, aimed at how PAYDAY 3 looks.
Four presets at the top of the options switch the whole look in one click:

| Preset | What you get |
|---|---|
| **Enhanced** (default) | The full look: the Enhanced column below |
| **Performance** | Enhanced without indirect light, ray traced lighting, reflections and light shafts; lighter ambient occlusion; SMAA |
| **Just the cleanup** | SMAA T2x, light sharpening, moire reduction, debanding and dithering. Vanilla with the rough edges taken off |
| **Enhanced (origin)** | The original author's Enhanced look from 0.5.3: stronger bloom and colour, Night Ops LUT, vignette, chromatic aberration, depth of field, motion blur, FSR 2x and SMAA T2x (0.5.3 had SMAA). Its Panini and spherical projection are left out: they were removed in 0.5.11 and 0.5.16 |

## Effects

Effects marked *depth* read the game's depth buffer. Everything else works from the picture alone.

| Effect | What it does | Enhanced (default) | Performance | Just the cleanup | Enhanced (origin) |
|---|---|---|---|---|---|
| Antialiasing | Off, FXAA, SMAA, SMAA T2x or TAA (see below) | SMAA T2x | SMAA | SMAA T2x | SMAA T2x |
| Sharpening | Contrast adaptive sharpening (AMD CAS); eases off at depth edges | on | on | light | on |
| FSR 1.0 | Redraws the frame larger along its edges and fits it back (AMD EASU and RCAS). Below 1x it shrinks and rebuilds the frame instead, which washes out shimmer | off | off | off | on, 2x |
| Moire reduction | Calms the shimmer of fine textures in the distance | on | on | on | on |
| Texture detail | Strengthens the fine contrast textures already have | on | on | off | on |
| Debanding, dithering | Hide colour steps in dark gradients | on | on | on | on |
| Eye adaptation | Centre-weighted exposure, eased over time | on | on | off | on |
| Bloom | Soft glow around bright light | on | on | off | strong |
| Local tone mapping | Balances dark and bright areas, keeps their detail | on | on | off | on |
| Colour grade | Temperature, tint, shadows, highlights, lift, gamma, gain, contrast, saturation, vibrance, and a 3D LUT | on, Film Stock LUT 25% | on, Film Stock LUT 25% | off | on, Night Ops LUT 50% |
| Clarity | Midtone local contrast | on | on | off | on |
| Obscurance | Deepens creases from brightness alone; steps aside for real ambient occlusion | on | on | off | on |
| Monocular cues | Edge separation, a little distance fade, softer screen edges | on | on | off | on |
| Colour bloom | Strong colours spread softly into their surroundings | off | off | off | off |
| Ambient occlusion (depth) | Contact shadows from real geometry (GTAO) | on | half strength | off | on |
| Aerial perspective (depth) | Distant surfaces fade into the colour of the air | on | on | off | on |
| Indirect light (depth) | One bounce of light between nearby surfaces | on | off | off | on |
| Ray traced lighting (depth) | Screen space rays gather light, built up over frames | on | off | off | on |
| Reflections (depth) | Screen space reflections on smooth floors | on | off | off | on |
| Light shafts | Beams from bright windows and lamps | on | off | off | on |
| Height fog (depth) | Fog that pools low around you | off | off | off | off |
| Depth of field (depth) | Focuses on what is under the crosshair, with its own blur size while aiming; your weapon stays sharp | off | off | off | on |
| Vignette, chromatic aberration, film grain | Lens effects | off | off | off | vignette, chromatic aberration |
| Lens dirt | Dust on the lens, lit by the bright light in the frame | off | off | off | off |
| Halation | Warm bleed around the brightest highlights | off | off | off | off |
| Surface grain | Fine grain that moves with the surfaces | off | off | off | off |
| Motion blur | Blur along the motion found in the picture | off | off | off | on |

LUTs are the `.cube` files in `luts/`. Add your own there and they appear in the menu.

### Antialiasing

| Choice | |
|---|---|
| FXAA | Cheapest, softest |
| SMAA | Sharp |
| SMAA T2x | Smoother than SMAA, no trails. The default |
| TAA | Smoothest, but things that move by themselves can trail |

SMAA T2x and TAA turn the camera by a fraction of a pixel each frame (*Camera jitter*). Your aim is
not affected. The jitter starts a few seconds after loading in, once the plugin has checked that the
frames really show it; until then SMAA does the work.

## Options

**Options → Mod Options → Screen Shaders**

* **Enabled** and the three presets at the top.
* One page per group: Antialiasing and sharpening, Light and exposure, Colour, Shape and depth,
  Depth and distance, Lighting, Atmosphere, Lens and film. Changes show live; every page ends with
  *Reset this page*.
* **Controls**: keys for *Toggle shaders*, *Toggle split view* and *Draw capture*.
* **Tools**: split view (the untouched game on the left half), reload shaders, plugin status, debug
  views, and the game's own settings.
* **Info**: what it does, what other players see, what's new.

Settings are saved in `mods/saves/pss_settings.json`. *Reset all to defaults* brings back the
shipped look.

## Fair play

It changes only the picture on your own screen. Nothing is sent to anyone, other players need nothing,
and nothing hidden is revealed. It steps aside while you look through a security camera.

## The game's own settings

While the mod runs, it turns off PAYDAY 2's own antialiasing, ambient occlusion and depth of field,
because it does all three itself. Your old values are saved, come back when you turn off *Replace
vanilla effects* (Tools) or with *Restore game settings*, and since 0.8.0 are handed back every time
you quit the game - so removing the mod cannot leave them switched off. They are only taken off while
the plugin is really drawing. The game's light adaptation and colour grading are left alone.

## Troubleshooting

* **Tools → Plugin status** says whether the plugin loaded, whether the shaders compiled, whether the
  game's depth was found, what the antialiasing is doing, and the GPU time of each stage.
* **Depth effects do nothing:** turn on *Show depth buffer* (Depth and distance). A magenta and black
  checkerboard means the game's depth was not found.
* **One effect alone:** *Debug view* (Tools) shows ambient occlusion, indirect light, ray traced
  lighting or reflections on their own.
* **Shaders on the HUD, or nowhere:** press *Draw capture* where it happens, then read
  `mods/logs/pss_capture.txt`.
* **Logs:** `mods/logs/pss_native.log` (plugin), `mods/logs/pss_status.txt`, and the BLT log in
  `mods/logs`.
* `shaders/pss.hlsl.cache` can be deleted at any time; it is rebuilt on the next launch.

## How it works

* The plugin hooks Direct3D 11 (the swap chain's `Present` and the game's own draw calls), learns
  which fullscreen pass draws the finished world, and runs the shader chain right after it.
* It copies the game's depth target for the depth effects. Lua measures the camera every frame for
  the effects that need it.
* Settings reach the plugin through `mods/saves/pss_live.txt`, because SuperBLT 4.0.2-beta gives
  native plugins no Lua functions.
* `shaders/pss.hlsl` is compiled at runtime by the game's own `d3dcompiler_47`, so it can be edited
  and reloaded from the Tools page without restarting.

## Development

* Build the plugin with MinGW-w64: `sh native/build.sh <version>` writes `native/pss.dll`. The source
  is in `native/src`.
* Tests and the test host: [`dev/README.md`](dev/README.md).
* How each effect works, what was measured and why: [`dev/NOTES.md`](dev/NOTES.md).
* What changed in each version: *Info → What's new* in the game, and the commit log.

## Moods and the workshop

Two things the grade answers to, both optional (Lens): *Hurt closes in* draws the edges in and drains
the colour while you are badly hurt, further still on the floor, lifting as you heal; *Stealth and loud*
reads a shade cooler in stealth and warmer once it is loud. Both lend the grade an offset for as long
as the mood lasts; your saved settings never move. *Off in the workshop* (Tools) steps the chain aside
while you modify a weapon.

## Credits

* AndreikaRPM – the antialiasing as it stands: the tuned SMAA, SMAA T2x and the temporal
  antialiasing, with the camera measurement and frame sync they need, developed in the
  [codeberg.org/pd2-mods/ScreenShaders](https://codeberg.org/pd2-mods/ScreenShaders) fork (0.6.0 to
  0.7.5) and taken back here with thanks, under the GPL-2.0-or-later this mod has always carried.

* Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez – SMAA (MIT
  licence). `shaders/smaa.hlsl` is their reference implementation with its notice intact, and the two
  lookup textures are converted from their published headers by `dev/make_smaa.py`.
* Timothy Lottes (NVIDIA) – FXAA 3.11, which the FXAA option follows.
* Dennis Gustafsson – the single pass bokeh technique the depth of field blur follows.
* Jorge Jimenez, Xian-Chun Wu, Angelo Pesce and Adrian Jarabo – *Practical Realtime Strategies for
  Accurate Indirect Occlusion* (2016), the GTAO the ambient occlusion follows.
* Damien Hembert (AlucardDH) – the idea of estimating motion vectors by block matching colour and depth,
  from his GPL-2 ReShade shaders. No code from them is included.
* AMD – FidelityFX Contrast Adaptive Sharpening (MIT licence), which the sharpening pass follows,
  and FidelityFX Super Resolution 1.0 (MIT licence), whose EASU and RCAS the optional
  upscaling stage follows; FidelityFX Super Resolution 2.2 (MIT licence), whose depth clip and
  accumulation weight the temporal antialiasing follows.
* Marius Bjørge (ARM) – the dual filter blur used by the bloom and blur chains.
* Thomas Luft, Carsten Colditz and Oliver Deussen – *Image Enhancement by Unsharp Masking the Depth
  Buffer* (2006), whose idea obscurance and edge separation follow, applied to brightness because the
  plugin has no depth.
* SuperBLT team – the native plugin loader.

The lens dirt photograph was supplied by Copper. Shaders, plugin, colour grades and menus by Copper.

## Licence

The plugin is licensed GPL-2.0-or-later, which is what SuperBLT requires of native plugins. Its full
source is in `native/src`. See `LICENSE.txt`.

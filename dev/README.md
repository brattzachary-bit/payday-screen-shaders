# Developing Payday Screen Shaders

Everything the game needs is in the repository root (`mod.txt`, `lua/`, `shaders/`, `luts/`,
`native/`). This `dev/` folder holds the build and test tooling and is left out of release archives.

## Native plugin

`native/src/` is the complete source of `native/pss.dll` (GPL-2.0-or-later, see `LICENSE.txt`).

* `sh dev/nativebuild/build_pss.sh` builds `native/pss.dll` with MinGW inside a podman container
  (see `dev/nativebuild/Containerfile`); nothing is installed on the host.
* `sh dev/nativebuild/build_pss.sh test` also builds the offline native test
  (`dev/nativebuild/test/pss_test.cpp`) into `dev/nativebuild/out/pss_test.exe`. Run it with Wine
  or Proton (it needs a prefix with d3d11 and `libvkd3d*-1.dll`).
* Known issue (2026-10-18): `dev/nativebuild/test/pss_test.cpp` predates the 0.7.5 fork merge and
  no longer compiles (it still references the removed Panini/spherical projections and
  `Effects::MotionTexture`). The DLL build is unaffected; the test needs updating to the current
  effect chain.
* Shaders must compile with vkd3d-shader as well as d3dcompiler: under Proton the game's
  `d3dcompiler_47` is Wine's.
* The DLL cannot be reloaded in a running game: a new build needs a game restart.

* `sh dev/nativebuild/build_pss.sh shaders` builds `dev/nativebuild/out/shadercheck.exe`; run it under Wine
  with the path to `shaders/pss.hlsl` and it compiles every entry point with Wine's d3dcompiler_47
  (vkd3d-shader), which is what the game uses under Proton. Do this after every shader change.

## Lua side

`luajit dev/tests/run_pss_tests.lua` runs the headless Lua test suite (no game needed).
`luajit dev/tests/test_memory_bridge.lua` exercises the memory bridge's Lua side against a mocked
plugin API (from AndreikaRPM's fork). Both must pass before a release.

## Assets

* `dev/nativebuild/make_luts.py` generates the `.cube` colour grades in `luts/`.
* `dev/nativebuild/make_dirt.py` converts `dev/nativebuild/lens_dirt_source.jpg` into
  `shaders/lens_dirt.bin`.
* `dev/nativebuild/make_smaa.py` converts the SMAA lookup headers into `shaders/smaa_*.bin`.

## Releases

A release is a tag. `git archive --format=zip --prefix="Payday Screen Shaders/" vX.Y.Z` produces the
zip that goes on Nexus Mods; `dev/` is excluded by `.gitattributes`.

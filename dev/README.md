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
* The offline render test (`build_pss.sh test`, 153 checks) needs a Direct3D 11 device. Run it with
  Proton's Wine and DXVK, the game's own stack under Proton:

      P="$HOME/.steam/steam/steamapps/common/Proton 10.0/files"
      export WINEPREFIX=/tmp/pss-pfx WINEDEBUG=-all
      "$P/bin/wine" wineboot -u
      cp "$P"/lib/vkd3d/x86_64-windows/libvkd3d*-1.dll "$WINEPREFIX/drive_c/windows/system32/"
      cd <a folder holding pss_test.exe, pss.hlsl, smaa.hlsl, smaa_*.bin, lens_dirt.bin, a .cube>
      "$P/bin/wine" pss_test.exe pss.hlsl lut.cube lens_dirt.bin

  Host Wine with wined3d also runs it (151 checks); only the hook test is DXVK-specific. The run
  prints the GPU cost tables for FSR, antialiasing, depth of field and the depth effects.
* `pss_test.exe --render` pushes one RGBA frame through the whole chain with a settings file;
  `dev/nativebuild/preview.py` wraps it for a screenshot with the shipped defaults plus overrides.
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

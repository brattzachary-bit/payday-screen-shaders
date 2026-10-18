# In-process bridge (AndreikaRPM, fork 0.7.6 and 0.7.7)

Design note written by AndreikaRPM for the memory transport, kept as merged into 0.9.0. The MSVC build and PowerShell test scripts it mentions are in the fork at codeberg.org/pd2-mods/ScreenShaders, not here; this tree builds with `dev/nativebuild/build_pss.sh` (MinGW) and tests the Lua side with `dev/tests/test_memory_bridge.lua`.


Version 0.7.7 ships in-process camera/settings transport with automatic recovery when a native load succeeds without returning its API table. Shaders, LUTs, effects, hooks, parameters and State sources retain the 0.7.5 implementation.

The returned API exposes `begin_memory()`, `publish_camera(text)`, `publish_settings(text)`, `status_text()` and `bridge_status()`. Owned mailboxes hold camera payloads up to 4095 bytes and settings up to 65535 bytes. Producers do not retain Lua pointers or call render/State code. The original camera parser/history is consumed before rendering; settings use the existing parser on the 100 ms worker.

The handshake fences both consumers. Supported startup waits for negotiation; legacy fallback is allowed only before memory starts. Once selected, memory remains selected through publication failures. Handshake failures retry. If the loader temporarily withholds the API table, Lua retries acquisition from the main state at most once per second, including after a coroutine-first load. A later healthy native PushLua promotes an initial file fallback back to pending negotiation. Captured APIs remain in memory mode through publication failures. Fresh generations preserve camera expiry semantics: identical new packets renew freshness, re-reading one generation does not, and fully invalid packets cannot renew the 500 ms lifetime. Status uses the original formatter on a resident snapshot.

`bridge_status()` exposes publication/consumption/application/rejection counts and legacy read attempts. Live checks found advancing counters, camera arriving, compiled shaders and settled temporal antialiasing. The 60 s Lua file-open diagnostic recorded zero monitored IPC opens, corroborated by zero native read attempts. Persistent preferences and diagnostic logging still use files.

The 0.7.7 release smoke test withheld both mods' API tables once in a running heist. Both recovered in 1.08 seconds; camera/settings and wall-filter consumption continued, with zero legacy IPC reads or Lua IPC file opens during the following focused 60-second combat sample. The preservation check found only the expected newly generated shader cache; original installations and preferences were restored.

## Reproduction

Build with Visual Studio C++ x64 Build Tools using `powershell -NoProfile -File native/build-memory.ps1`. The shipped DLL uses MSVC; original 0.7.5 used MinGW. `native/build.sh` remains available for cross-building, but its output is not the measured MSVC binary.

Run `powershell -NoProfile -File dev/test-memory.ps1` for normal and AddressSanitizer parser/API/concurrency checks. Each flavor publishes 80,000 concurrent packets; assertion totals vary with scheduling. Differential baselines are `dev/bridge.original.cpp` and `dev/core.original.lua`.

With Lupa's LuaJIT 2.1 runtime in a development Python environment, run `python dev/run-lua-memory-tests.py`. The new memory test and eight prior suites pass; two existing preset assertions fail identically on baseline and candidate, giving zero new regressions. `python dev/check_params.py .` checks parameter consistency. Generated test files are ignored and development files excluded from runtime archives.

## Benchmark limits

Four 120 s combat captures with 30 s warmup each, 180 FPS cap, order file→memory→memory→file, with PSS and Acoustics changed together:

| Transport | Mean average FPS | Mean 1% low FPS | Mean 0.1% low FPS |
|---|---:|---:|---:|
| File |169.45|112.23|80.70|
| Memory |171.62|142.05|110.13|

Low FPS is 1000 divided by the mean frame time of the slowest ceil(N×fraction) frames. Both memory runs exceeded both file runs in these tail metrics. One PC/scene, two runs per condition, AI variation and the compiler change limit the conclusion; it does not isolate each mod's contribution or pure IPC cost. Constant 180 FPS and long-session stability are not established.

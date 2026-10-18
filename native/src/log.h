#pragma once

// Thread-safe append-only log. The render thread writes here too, so it never touches Lua.
#ifdef __GNUC__
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void Log(const char* fmt, ...);
#endif

// Where the log goes (the offline test writes beside itself). The next Log() starts the file afresh.
void LogSetPath(const char* path);

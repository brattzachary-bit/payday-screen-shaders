#pragma once

// Thread-safe append-only log. The render thread writes here too, so it never touches Lua.
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

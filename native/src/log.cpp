#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

static std::mutex g_mutex;
static std::string g_path = "mods/logs/pss_native.log";
static bool g_truncate = true;

void Log(const char* fmt, ...)
{
	char buffer[2048];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	std::lock_guard<std::mutex> lock(g_mutex);

	FILE* file = fopen(g_path.c_str(), g_truncate ? "w" : "a");

	g_truncate = false;

	if (!file)
		return;

	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(file, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buffer);
	fclose(file);
}

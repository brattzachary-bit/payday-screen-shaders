#include "bridge.h"

#include <mutex>
#include <atomic>
#include <cstdarg>
#include <algorithm>
#include "mailbox.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "log.h"
#include "state.h"
#include "memory-version.h"

namespace
{
	FILETIME g_lastWrite = {};
	DWORD g_lastSize = 0;

	// -1 until the first read. The first value seen is only a baseline, so a file left over from
	// the last session neither fakes a heartbeat nor replays an old reload or capture request.
	long g_tick = -1;
	long g_reload = -1;
	long g_capture = -1;

	bool g_started = false;

	FILETIME g_cameraWrite = {};
	double g_cameraFrame = -1.0;
	std::string g_cameraPath;
	std::mutex g_cameraMutex;
	std::mutex g_settingsMutex;
	enum class Transport { File, Pending, Memory };
	std::atomic<Transport> g_transport{Transport::File};
	BoundedMailbox<4096> g_cameraMailbox;
	BoundedMailbox<65536> g_settingsMailbox;
	uint64_t g_cameraConsumed = 0, g_settingsConsumed = 0;
	struct Counts {
		std::atomic<uint64_t> publishedCamera{0}, consumedCamera{0}, appliedCamera{0};
		std::atomic<uint64_t> publishedSettings{0}, consumedSettings{0};
		std::atomic<uint64_t> rejectedCamera{0}, rejectedSettings{0}, diskCamera{0}, diskSettings{0};
	} g_counts;
}


namespace {
bool ApplyCameraText(char* text, bool sameTime)
{
	// Newest last in the file, newest first here. A file caught halfway through being written ends in a
	// short line, which is skipped; the lines before it are whole.
	CameraPose recent[CAMERA_LINES];
	double newest[9] = {};
	int lines = 0;
	char* line = text;

	while (line && *line)
	{
		char* next = strchr(line, '\n');

		if (next)
			*next++ = 0;
		else
			break; // no newline yet: still being written

		double v[31] = {};
		int read = 0;
		char* at = line;

		for (char* end; read < 31; read++, at = end)
		{
			v[read] = strtod(at, &end);

			if (end == at)
				break;
		}

		// A camera that is not a number (seen in the game while loading) would blacken the frame.
		bool finite = true;

		for (int i = 0; i < read; i++)
			finite = finite && std::isfinite(v[i]);

		// 9 numbers is an older Lua with no pose, 21 one with no jitter axes, 30 one with no jitter state.
		if (finite && (read == 9 || read == 21 || read == 30 || read == 31))
		{
			for (int i = CAMERA_LINES - 1; i > 0; i--)
				recent[i] = recent[i - 1];

			CameraPose& pose = recent[0];
			pose = CameraPose();
			pose.valid = read >= 21;
			pose.frame = (float)v[8];
			pose.tan[0] = (float)v[0];
			pose.tan[1] = (float)v[1];
			pose.pitch = (float)v[2];
			pose.roll = (float)v[3];
			pose.aim = v[5] > 0.5;
			int out = read >= 30 ? 21 : 12;
			pose.luaJitter = read == 31 ? (int)v[30] : -1;
			pose.jitter[0] = (float)v[6];
			pose.jitter[1] = (float)v[7];

			// Since 0.6.6 the game state follows the numbers, as a word.
			if (read == 31)
				sscanf(at, " %31[A-Za-z0-9_]", pose.state);

			for (int i = 0; i < 3 && pose.valid; i++)
			{
				pose.pos[i] = v[9 + i];
				pose.right[i] = (float)v[12 + i];
				pose.up[i] = (float)v[15 + i];
				pose.fwd[i] = (float)v[18 + i];
				pose.outRight[i] = (float)v[out + i];
				pose.outUp[i] = (float)v[out + 3 + i];
				pose.outFwd[i] = (float)v[out + 6 + i];
			}

			memcpy(newest, v, sizeof(newest));
			lines++;
		}

		line = next;
	}

	if (!lines || (sameTime && newest[8] == g_cameraFrame)) return false;
	g_cameraFrame = newest[8];
	State::SetCamera((float)newest[0], (float)newest[1], (float)newest[2], (float)newest[3], (float)newest[4], (float)newest[5], (float)newest[6], (float)newest[7], (float)newest[8], recent);
	return true;
}
bool ApplySettingsText(const std::string& text)
{
	if (text.find("\nend") == std::string::npos) return false;
	size_t start = 0;

	while (start < text.size())
	{
		size_t end = text.find('\n', start);

		if (end == std::string::npos)
			end = text.size();

		std::string line = text.substr(start, end - start);
		start = end + 1;

		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		if (line.empty() || line == "end")
			continue;

		size_t space = line.find(' ');
		std::string key = line.substr(0, space);
		std::string value = space == std::string::npos ? "" : line.substr(space + 1);

		if (key == "shader_path")
		{
			State::SetShaderPath(value);
		}
		else if (key == "lut_path")
		{
			State::SetLutPath(value);
		}
		else if (key == "dirt_path")
		{
			State::SetDirtPath(value);
		}
		else if (key == "tick" || key == "reload" || key == "capture")
		{
			long number = strtol(value.c_str(), nullptr, 10);
			long& last = key == "tick" ? g_tick : (key == "reload" ? g_reload : g_capture);
			bool changed = last >= 0 && number != last;

			last = number;

			if (!changed)
				continue;

			if (key == "tick")
				State::SetActive(true);
			else if (key == "reload")
				State::Reload();
			else
				State::RequestCapture(2);
		}
		else
		{
			State::SetParam(key.c_str(), strtof(value.c_str(), nullptr));
		}
	}

	return true;
}
void AppendStatus(std::string& text, const char* format, ...)
{
	char part[32768];
	va_list args; va_start(args, format);
	int n = vsnprintf(part, sizeof(part), format, args);
	va_end(args);
	if (n < 0) return;
	const size_t used = std::min<size_t>(static_cast<size_t>(n), sizeof(part) - 1);
	if (text.size() < 32767) text.append(part, std::min<size_t>(used, 32767 - text.size()));
}
std::string StatusText()
{
	std::string text;
	Status s = State::GetStatus();
	bool active = State::Snapshot().active;

	AppendStatus(text, "Plugin %s  hooks %s  present hook %s\n", MODULE_VERSION, s.hooksInstalled ? "yes" : "NO", s.presentHooked ? "yes" : "NO");
	AppendStatus(text, "Settings from Lua: %s\n", active ? "arriving" : "NOT arriving");
	AppendStatus(text, "Shaders compiled: %s\n", s.compiled ? "yes" : "no");
	AppendStatus(text, "Back buffer %dx%d  format %d%s\n", s.backbufferW, s.backbufferH, s.rtvFormat, s.srgb ? " (sRGB)" : "");
	AppendStatus(text, "Depth target: %s", s.depthFound ? "found" : "NOT found");

	if (s.depthFound)
		AppendStatus(text, " %dx%d format %d, %s", s.depthW, s.depthH, s.depthFormat, s.depthReadable ? "readable" : "NOT readable");

	if (!s.depthError.empty())
		AppendStatus(text, " (%s)", s.depthError.c_str());

	AppendStatus(text, "\n");
	// The debug view is named, not numbered: "debug view 2" does not tell anyone that the screen is
	// red because the EXPOSURE overlay is on rather than because depth is missing.
	static const char* kViews[] = {"Off", "Bloom only", "Exposure - this one paints the screen RED", "Ambient occlusion only", "Indirect light only", "Ray traced lighting only", "Reflections only"};
	int view = s.depthDebugView < 0 ? 0 : (s.depthDebugView > 6 ? 6 : s.depthDebugView);

	if (s.cameraValid)
		AppendStatus(text, "Camera: arriving (half field of view tangents %.3f x %.3f, pitch %.1f, height %.0f, frame %.0f)\n", s.cameraTanX, s.cameraTanY, s.cameraPitch, s.cameraHeight, s.cameraFrame);
	else
		AppendStatus(text, "Camera: not arriving (only needed by occlusion, indirect light, ray traced lighting, reflections and height fog)\n");

	// Lua watches these lines: it only jitters the camera while the second says allowed.
	AppendStatus(text, "Temporal antialiasing: %s (%s)\n", s.taaRunning ? "running" : "not running", s.taa.c_str());
	AppendStatus(text, "Camera jitter: %s\n", s.jitter.c_str());
	AppendStatus(text, "Depth mode given to the shader: %.1f%s\n", s.depthMode,
		s.depthMode < -0.5f ? "  <- the shader is being told there is NO depth" : "  (depth is available to the shaders)");
	AppendStatus(text, "Debug view: %d, %s\n", s.depthDebugView, kViews[view]);
	AppendStatus(text, "Frames %llu  processed %llu\n", (unsigned long long)s.frames, (unsigned long long)s.injectedFrames);
	AppendStatus(text, "Last frame: %d draws, %d into the back buffer, %d fullscreen passes\n", s.lastTotalDraws, s.lastBackbufferDraws, s.lastFullscreenDraws);
	AppendStatus(text, "Shaders run after fullscreen pass: %d%s\n", s.targetFullscreenDraw, s.targetFullscreenDraw > 0 ? "" : " (none last frame)");
	AppendStatus(text, "Final pass: %s\n", s.finalPassLearned ? "learned" : "still learning");
	AppendStatus(text, "Frames without shaders in the last second: %d\n", s.unprocessedLastSecond);
	AppendStatus(text, "GPU time per frame: %s\n", s.gpuTime.c_str());

	if (!s.error.empty())
		AppendStatus(text, "Error: %s\n", s.error.c_str());

	return text;
}
}

void Bridge::ConfigureMemoryCapability(bool available)
{
	std::scoped_lock lock(g_cameraMutex, g_settingsMutex);
	// A supported native build waits for the explicit Lua handshake. It never
	// races a leftover camera/settings file into the new session before it.
	if (g_transport.load() != Transport::Memory)
		g_transport.store(available ? Transport::Pending : Transport::File);
}
bool Bridge::BeginMemory()
{
	// Order against a complete legacy read/parse/commit, not only the file read.
	std::scoped_lock lock(g_cameraMutex, g_settingsMutex);
	if (g_transport.load() == Transport::File) return false;
	g_transport.store(Transport::Memory);
	return true;
}
bool Bridge::PublishCamera(const char* text, size_t size)
{
	if (g_transport.load() != Transport::Memory || !g_cameraMailbox.Publish(text, size)) {
		g_counts.rejectedCamera.fetch_add(1); return false;
	}
	g_counts.publishedCamera.fetch_add(1); return true;
}
bool Bridge::PublishSettings(const char* text, size_t size)
{
	if (g_transport.load() != Transport::Memory || !g_settingsMailbox.Publish(text, size)) {
		g_counts.rejectedSettings.fetch_add(1); return false;
	}
	g_counts.publishedSettings.fetch_add(1); return true;
}
void Bridge::PollCamera(const char* path)
{
	std::lock_guard<std::mutex> lock(g_cameraMutex);
	const auto mode = g_transport.load();
	if (mode == Transport::Pending) return;
	if (mode == Transport::Memory) {
		char text[4096]; size_t size = 0;
		if (!g_cameraMailbox.Take(g_cameraConsumed, text, size)) return;
		g_counts.consumedCamera.fetch_add(1);
		// A distinct publication is equivalent to a newly written file, even if
		// its text/frame repeats. Polling without a publication never renews it.
		if (!ApplyCameraText(text, false)) { g_counts.rejectedCamera.fetch_add(1); return; }
		g_counts.appliedCamera.fetch_add(1);
		return;
	}
	g_counts.diskCamera.fetch_add(1);
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return;
	bool sameTime = CompareFileTime(&data.ftLastWriteTime, &g_cameraWrite) == 0;
	FILE* file = fopen(path, "rb");
	if (!file) return;
	char text[4096];
	size_t size = fread(text, 1, sizeof(text) - 1, file);
	fclose(file); text[size] = 0;
	if (ApplyCameraText(text, sameTime)) g_cameraWrite = data.ftLastWriteTime;
}
void Bridge::PollCameraNow()
{
	if (!g_cameraPath.empty()) PollCamera(g_cameraPath.c_str());
}
bool Bridge::Poll(const char* path)
{
	std::lock_guard<std::mutex> lock(g_settingsMutex);
	const auto mode = g_transport.load();
	if (mode == Transport::Pending) return false;
	if (mode == Transport::Memory) {
		char text[65536]; size_t size = 0;
		if (!g_settingsMailbox.Take(g_settingsConsumed, text, size)) return false;
		g_counts.consumedSettings.fetch_add(1);
		if (!ApplySettingsText(std::string(text, size))) { g_counts.rejectedSettings.fetch_add(1); return false; }
		return true;
	}
	g_counts.diskSettings.fetch_add(1);
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return false;
	if (CompareFileTime(&data.ftLastWriteTime, &g_lastWrite) == 0 && data.nFileSizeLow == g_lastSize) return false;
	FILE* file = fopen(path, "rb");
	if (!file) return false;
	std::string text; char buffer[4096]; size_t size;
	while ((size = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, size);
	fclose(file);
	if (text.find("\nend") == std::string::npos) return false;
	g_lastWrite = data.ftLastWriteTime; g_lastSize = data.nFileSizeLow;
	return ApplySettingsText(text);
}
size_t Bridge::CopyStatusText(char* output, size_t capacity)
{
	if (!output || !capacity) return 0;
	const std::string text = StatusText();
	const size_t size = std::min(text.size(), capacity - 1);
	memcpy(output, text.data(), size); output[size] = 0; return size;
}
Bridge::BridgeStatus Bridge::GetBridgeStatus()
{
	BridgeStatus s{}; s.memory = g_transport.load() == Transport::Memory;
	s.pending = g_transport.load() == Transport::Pending;
	s.publishedCamera = g_counts.publishedCamera.load(); s.consumedCamera = g_counts.consumedCamera.load();
	s.appliedCamera = g_counts.appliedCamera.load(); s.publishedSettings = g_counts.publishedSettings.load();
	s.consumedSettings = g_counts.consumedSettings.load(); s.rejectedCamera = g_counts.rejectedCamera.load();
	s.rejectedSettings = g_counts.rejectedSettings.load(); s.diskCamera = g_counts.diskCamera.load();
	s.diskSettings = g_counts.diskSettings.load(); return s;
}
void Bridge::WriteStatus(const char* path)
{
	const std::string text = StatusText();
	FILE* file = fopen(path, "w");

	if (!file)
		return;

	fwrite(text.data(), 1, text.size(), file);
	fclose(file);
}
void Bridge::Start(const char* livePath, const char* statusPath)
{
	if (g_started)
		return;

	g_started = true;

	std::string live = livePath;
	std::string status = statusPath;
	std::string camera = live.substr(0, live.find_last_of("/\\") + 1) + "pss_camera.txt";
	g_cameraPath = camera;

	// The settings file every 100 ms and the status file once a second. The camera is not read here:
	// the render thread reads it right before every frame (PollCameraNow).
	std::thread([live, status]() {
		for (int ticks = 1;; ticks++)
		{
			Poll(live.c_str());

			if (ticks % 10 == 0)
				WriteStatus(status.c_str());

			Sleep(100);
		}
	}).detach();

	Log("reading settings from %s", livePath);
}

#include "bridge.h"

#include <mutex>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "log.h"
#include "state.h"

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
}

// One line per frame, "tanX tanY pitch roll height aiming jitterX jitterY frame", then (since 0.5.21)
// the camera position, its right, up and forward axes as drawn, and the same axes without the jitter.
// Since 0.5.22 Lua writes its last few lines, oldest first, so a frame drawn on another thread can
// still find its own. Only a changed file counts, so a file left behind never keeps the camera valid.
// Read every frame by the render thread (PollCameraNow), even if the file time has not moved: two
// writes inside one tick of the file clock look like one, and the frame number tells them apart.
void Bridge::PollCamera(const char* path)
{
	std::lock_guard<std::mutex> lock(g_cameraMutex);
	WIN32_FILE_ATTRIBUTE_DATA data;

	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data))
		return;

	bool sameTime = CompareFileTime(&data.ftLastWriteTime, &g_cameraWrite) == 0;
	FILE* file = fopen(path, "rb");

	if (!file)
		return;

	char text[4096];
	size_t size = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	text[size] = 0;

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

	if (!lines || (sameTime && newest[8] == g_cameraFrame))
		return;

	g_cameraWrite = data.ftLastWriteTime;
	g_cameraFrame = newest[8];
	State::SetCamera((float)newest[0], (float)newest[1], (float)newest[2], (float)newest[3], (float)newest[4], (float)newest[5], (float)newest[6], (float)newest[7], (float)newest[8], recent);
}

void Bridge::PollCameraNow()
{
	if (!g_cameraPath.empty())
		PollCamera(g_cameraPath.c_str());
}

bool Bridge::Poll(const char* path)
{
	WIN32_FILE_ATTRIBUTE_DATA data;

	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data))
		return false;

	if (CompareFileTime(&data.ftLastWriteTime, &g_lastWrite) == 0 && data.nFileSizeLow == g_lastSize)
		return false;

	FILE* file = fopen(path, "rb");

	if (!file)
		return false;

	std::string text;
	char buffer[4096];
	size_t read;

	while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, read);

	fclose(file);

	// A file caught halfway through being written has no end marker yet; take it next poll.
	if (text.find("\nend") == std::string::npos)
		return false;

	g_lastWrite = data.ftLastWriteTime;
	g_lastSize = data.nFileSizeLow;

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

void Bridge::WriteStatus(const char* path)
{
	Status s = State::GetStatus();
	bool active = State::Snapshot().active;
	FILE* file = fopen(path, "w");

	if (!file)
		return;

	fprintf(file, "Plugin %s  hooks %s  present hook %s\n", MODULE_VERSION, s.hooksInstalled ? "yes" : "NO", s.presentHooked ? "yes" : "NO");
	fprintf(file, "Settings from Lua: %s\n", active ? "arriving" : "NOT arriving");
	fprintf(file, "Shaders compiled: %s\n", s.compiled ? "yes" : "no");
	fprintf(file, "Back buffer %dx%d  format %d%s\n", s.backbufferW, s.backbufferH, s.rtvFormat, s.srgb ? " (sRGB)" : "");
	fprintf(file, "Depth target: %s", s.depthFound ? "found" : "NOT found");

	if (s.depthFound)
		fprintf(file, " %dx%d format %d, %s", s.depthW, s.depthH, s.depthFormat, s.depthReadable ? "readable" : "NOT readable");

	if (!s.depthError.empty())
		fprintf(file, " (%s)", s.depthError.c_str());

	fprintf(file, "\n");
	// The debug view is named, not numbered: "debug view 2" does not tell anyone that the screen is
	// red because the EXPOSURE overlay is on rather than because depth is missing.
	static const char* kViews[] = {"Off", "Bloom only", "Exposure - this one paints the screen RED", "Ambient occlusion only", "Indirect light only", "Ray traced lighting only", "Reflections only"};
	int view = s.depthDebugView < 0 ? 0 : (s.depthDebugView > 6 ? 6 : s.depthDebugView);

	if (s.cameraValid)
		fprintf(file, "Camera: arriving (half field of view tangents %.3f x %.3f, pitch %.1f, height %.0f, frame %.0f)\n", s.cameraTanX, s.cameraTanY, s.cameraPitch, s.cameraHeight, s.cameraFrame);
	else
		fprintf(file, "Camera: not arriving (only needed by occlusion, indirect light, ray traced lighting, reflections and height fog)\n");

	// Lua watches these lines: it only jitters the camera while the second says allowed.
	fprintf(file, "Temporal antialiasing: %s (%s)\n", s.taaRunning ? "running" : "not running", s.taa.c_str());
	fprintf(file, "Camera jitter: %s\n", s.jitter.c_str());
	fprintf(file, "Depth mode given to the shader: %.1f%s\n", s.depthMode,
		s.depthMode < -0.5f ? "  <- the shader is being told there is NO depth" : "  (depth is available to the shaders)");
	fprintf(file, "Debug view: %d, %s\n", s.depthDebugView, kViews[view]);
	fprintf(file, "Frames %llu  processed %llu\n", (unsigned long long)s.frames, (unsigned long long)s.injectedFrames);
	fprintf(file, "Last frame: %d draws, %d into the back buffer, %d fullscreen passes\n", s.lastTotalDraws, s.lastBackbufferDraws, s.lastFullscreenDraws);
	fprintf(file, "Shaders run after fullscreen pass: %d%s\n", s.targetFullscreenDraw, s.targetFullscreenDraw > 0 ? "" : " (none last frame)");
	fprintf(file, "Final pass: %s\n", s.finalPassLearned ? "learned" : "still learning");
	fprintf(file, "Frames without shaders in the last second: %d\n", s.unprocessedLastSecond);
	fprintf(file, "GPU time per frame: %s\n", s.gpuTime.c_str());

	if (!s.error.empty())
		fprintf(file, "Error: %s\n", s.error.c_str());

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

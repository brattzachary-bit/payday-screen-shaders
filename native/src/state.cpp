#include "state.h"

#include <windows.h>

#include <mutex>

namespace
{
	const ULONGLONG HEARTBEAT_TIMEOUT_MS = 1500;
	const ULONGLONG CAMERA_TIMEOUT_MS = 500;

	std::mutex g_mutex;
	Settings g_settings;
	Status g_status;
	int g_captureRequest = 0;
	ULONGLONG g_heartbeat = 0;
	ULONGLONG g_camera = 0;

	float Clamp(int index, float value)
	{
		if (value != value)
			return kParams[index].def;

		return value < kParams[index].min ? kParams[index].min : (value > kParams[index].max ? kParams[index].max : value);
	}
}

void State::Init()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	for (int i = 0; i < PARAM_SLOTS; i++)
		g_settings.p[i] = i < P_COUNT ? kParams[i].def : 0.0f;

	g_settings.shaderGen = 1;
	g_settings.lutGen = 1;
	g_settings.dirtGen = 1;
	g_settings.active = false;
}

bool State::SetParam(const char* name, float value)
{
	int index = FindParam(name);

	if (index < 0 || value != value)
		return false;

	if (value < kParams[index].min)
		value = kParams[index].min;

	if (value > kParams[index].max)
		value = kParams[index].max;

	std::lock_guard<std::mutex> lock(g_mutex);
	g_settings.p[index] = value;

	return true;
}

void State::SetShaderPath(const std::string& path)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (g_settings.shaderPath != path)
	{
		g_settings.shaderPath = path;
		g_settings.shaderGen++;
	}
}

void State::SetLutPath(const std::string& path)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (g_settings.lutPath != path)
	{
		g_settings.lutPath = path;
		g_settings.lutGen++;
	}
}

void State::SetDirtPath(const std::string& path)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (g_settings.dirtPath != path)
	{
		g_settings.dirtPath = path;
		g_settings.dirtGen++;
	}
}

void State::Reload()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	g_settings.shaderGen++;
	g_settings.lutGen++;
	g_settings.dirtGen++;
}

// Lua calls this as a heartbeat from its update hooks. Loading screens stop Lua updates, so a
// stale heartbeat switches the effects off by itself; nothing has to hook the load start.
void State::SetActive(bool active)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	g_settings.active = active;
	g_heartbeat = GetTickCount64();
}

// The camera arrives on its own, faster channel (bridge.cpp). A line that stops arriving means Lua
// stopped measuring - a menu, a loading screen, or every effect that needed it switched off - and
// nothing may keep using the last one it sent.
void State::SetCamera(float tanX, float tanY, float pitch, float roll, float height, float aim, float jitterX, float jitterY, float frame, const CameraPose* recent)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	g_settings.p[P_CAMERA_TAN_X] = Clamp(P_CAMERA_TAN_X, tanX);
	g_settings.p[P_CAMERA_TAN_Y] = Clamp(P_CAMERA_TAN_Y, tanY);
	g_settings.p[P_CAMERA_PITCH] = Clamp(P_CAMERA_PITCH, pitch);
	g_settings.p[P_CAMERA_ROLL] = Clamp(P_CAMERA_ROLL, roll);
	g_settings.p[P_CAMERA_HEIGHT] = Clamp(P_CAMERA_HEIGHT, height);
	g_settings.p[P_CAMERA_AIM] = Clamp(P_CAMERA_AIM, aim);
	g_settings.p[P_JITTER_X] = Clamp(P_JITTER_X, jitterX);
	g_settings.p[P_JITTER_Y] = Clamp(P_JITTER_Y, jitterY);
	g_settings.p[P_CAMERA_FRAME] = Clamp(P_CAMERA_FRAME, frame);
	for (int i = 0; i < CAMERA_LINES; i++)
		g_settings.recent[i] = recent[i];

	g_camera = GetTickCount64();
}

Settings State::Snapshot()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	Settings copy = g_settings;
	ULONGLONG now = GetTickCount64();

	if (now - g_heartbeat > HEARTBEAT_TIMEOUT_MS)
		copy.active = false;

	copy.p[P_CAMERA_VALID] = g_camera != 0 && now - g_camera <= CAMERA_TIMEOUT_MS ? 1.0f : 0.0f;

	// Without a fresh camera nobody is known to be aiming, and there is no pose to reproject with.
	if (copy.p[P_CAMERA_VALID] < 0.5f)
	{
		copy.p[P_CAMERA_AIM] = 0.0f;
		for (CameraPose& pose : copy.recent)
			pose.valid = false;
	}

	return copy;
}

void State::RequestCapture(int frames)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	g_captureRequest = frames;
}

int State::TakeCaptureRequest()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	int frames = g_captureRequest;
	g_captureRequest = 0;

	return frames;
}

Status State::GetStatus()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_status;
}

void State::UpdateStatus(void (*fn)(Status&, void*), void* user)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	fn(g_status, user);
}

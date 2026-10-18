#pragma once

#include <cstdint>
#include <string>

#include "params.h"

// Everything Lua writes and the render thread reads (and back), behind one mutex.
// The render thread takes a Snapshot once per frame, never per draw call.
// Where the camera is and which way it faces, from Lua's render-time camera line: position in world
// units, then its right, up and forward axes, oriented to the screen. right/up/fwd are the camera the
// frame was drawn with, jitter included; out* the same camera without the jitter. valid is false with
// an older Lua that sends no pose.
struct CameraPose
{
	bool valid = false;
	double pos[3] = {};
	float right[3] = {};
	float up[3] = {};
	float fwd[3] = {};
	float outRight[3] = {};
	float outUp[3] = {};
	float outFwd[3] = {};
	float tan[2] = {};
	float frame = 0.0f;
	float pitch = 0.0f, roll = 0.0f; // degrees, and whether aiming down sights: for the log only
	bool aim = false;
	int luaJitter = -1; // why Lua is or is not jittering (lua/core.lua, PSS._jitter_state); -1 not sent
	float jitter[2] = {}; // the jitter in this line, in pixels (Lua's x right, y up)
	char state[32] = {}; // the game state Lua was in, for the log; empty if not sent
};

// Lua sends its last few camera lines, newest first here: when the game draws on a thread of its own,
// the frame being drawn may be one of the older ones (effects.cpp, TaaSync).
static const int CAMERA_LINES = 4;

struct Settings
{
	float p[PARAM_SLOTS];
	CameraPose recent[CAMERA_LINES];
	bool renderOnLuaThread = false; // set by the hook each frame, not by Lua
	std::string shaderPath;
	std::string lutPath;
	std::string dirtPath;
	unsigned shaderGen;
	unsigned lutGen;
	unsigned dirtGen;
	bool active;
};

struct Status
{
	bool hooksInstalled = false;
	bool presentHooked = false;
	bool compiled = false;
	std::string error;
	uint64_t frames = 0;
	uint64_t injectedFrames = 0;
	int lastBackbufferDraws = 0;
	int lastTotalDraws = 0;
	int lastFullscreenDraws = 0;
	int targetFullscreenDraw = 0;
	bool finalPassLearned = false;
	int unprocessedLastSecond = 0;
	int backbufferW = 0;
	int backbufferH = 0;
	int rtvFormat = 0;
	bool srgb = false;

	// The camera as last measured by Lua, and whether it is fresh enough to use.
	bool cameraValid = false;
	float cameraTanX = 0.0f;
	float cameraTanY = 0.0f;
	float cameraPitch = 0.0f;
	float cameraHeight = 0.0f;
	float cameraFrame = 0.0f;
	bool taaRunning = false;

	// The game's depth target, as found by the hook. depthError explains a failure in words, so
	// the options menu can tell the player why the depth effects are doing nothing.
	bool depthFound = false;
	bool depthReadable = false;
	int depthW = 0;
	int depthH = 0;
	int depthFormat = 0;
	float depthMode = -1.0f; // what the shader was really told last frame
	int depthDebugView = 0;  // and which debug view it was asked to draw
	std::string depthError;
	std::string gpuTime;     // GPU time per frame, by stage
	std::string taa;         // temporal antialiasing, and whether the camera line keeps step
	std::string jitter;      // whether Lua may jitter the camera, and if not why
};

namespace State
{
	void Init();

	bool SetParam(const char* name, float value);
	void SetShaderPath(const std::string& path);
	void SetLutPath(const std::string& path);
	void SetDirtPath(const std::string& path);
	void Reload();
	void SetActive(bool active);
	// A camera line from Lua. P_CAMERA_VALID is 1 in snapshots taken within half a second of it.
	void SetCamera(float tanX, float tanY, float pitch, float roll, float height, float aim, float jitterX, float jitterY, float frame, const CameraPose* recent);
	Settings Snapshot();

	void RequestCapture(int frames);
	int TakeCaptureRequest();

	Status GetStatus();
	void UpdateStatus(void (*fn)(Status&, void*), void* user);
}

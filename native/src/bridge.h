#pragma once
#include <cstddef>
#include <cstdint>

// Supported loader APIs negotiate bounded in-memory live settings, camera and
// status. Unsupported native/loader combinations use the original file bridge
// from startup. Persistent settings and optional diagnostic status logs remain.
namespace Bridge
{
	struct BridgeStatus {
		bool memory = false, pending = false;
		uint64_t publishedCamera = 0, consumedCamera = 0, appliedCamera = 0;
		uint64_t publishedSettings = 0, consumedSettings = 0;
		uint64_t rejectedCamera = 0, rejectedSettings = 0, diskCamera = 0, diskSettings = 0;
	};
	void ConfigureMemoryCapability(bool available);
	bool BeginMemory();
	bool PublishCamera(const char* text, size_t size);
	bool PublishSettings(const char* text, size_t size);
	size_t CopyStatusText(char* output, size_t capacity);
	BridgeStatus GetBridgeStatus();
	// Consumes a new settings packet, or a changed legacy file. True when applied.
	bool Poll(const char* path);

	void WriteStatus(const char* path);

	// Consumes a new camera packet, or reads the legacy camera file.
	void PollCamera(const char* path);
	// The same read, from the render thread, right before the shaders run: Lua writes the camera as
	// the frame starts rendering, so this is the camera that frame was drawn with.
	void PollCameraNow();

	void Start(const char* livePath, const char* statusPath);
}

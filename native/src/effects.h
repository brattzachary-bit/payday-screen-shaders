#pragma once

#include <d3d11_1.h>

#include <string>

#include "state.h"

struct RenderTexture
{
	ID3D11Texture2D* tex = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	UINT w = 0;
	UINT h = 0;

	void Release();
};

// A pair of buffers an effect accumulates into over frames, and which one holds the latest.
struct History
{
	RenderTexture t[2];
	int index = 0;
	bool reset = true;

	void Release();
};

// The shader chain. Knows nothing about hooks: it is handed a finished world image (the
// render target the game is about to draw its HUD into) and writes the processed image
// back into it. The test harness drives this class directly.
class Effects
{
  public:
	static const int BLOOM_LEVELS = 6;
	static const int WIDE_LEVELS = 3; // 1/8, 1/16, 1/32 of the screen
	static const int MAX_INPUTS = 24;

	// Runs every enabled pass. The game's pipeline state is swapped out for the duration
	// and swapped back in afterwards, so the caller's bindings are untouched.
	//
	// depth is the game's own depth target, found by the hook, or null. It is COPIED rather than
	// sampled in place: a copy does not need the game's texture to have been created as a shader
	// resource, which is not something a mod gets to choose.
	bool Run(ID3D11Device* device, ID3D11DeviceContext1* ctx, ID3D11Texture2D* target, ID3D11RenderTargetView* targetRtv, DXGI_FORMAT rtvFormat, const Settings& settings, ID3D11Texture2D* depth = nullptr);
	// The half-resolution motion found this frame (dev/nativebuild/test reads it back).
	ID3D11Texture2D* MotionTexture() const { return m_motion[0].tex; }

	// Why the depth effects are not running, for the status file. Empty means no complaint.
	const std::string& DepthError() const { return m_depthError; }
	bool DepthReady() const { return m_depthCopy.srv != nullptr; }
	// What was actually written into the shader's DEPTH_MODE last frame. -1 means the shader was
	// told there is no depth, which is the only thing that makes the Depth view show red, so this
	// is what settles an argument between the status file and the screen.
	float LastDepthMode() const { return m_lastDepthMode; }
	bool SmaaReady() const { return m_smaaAreaSrv != nullptr && m_smaaSearchSrv != nullptr; }
	// Whether temporal antialiasing ran last frame. Lua will not jitter the camera unless this is
	// true: a jittered frame nothing puts back is just a shaking picture.
	bool TaaRunning() const { return m_taaRunning; }
	// Temporal antialiasing: whether it is reprojecting with the camera, and how well the camera line
	// keeps step with the frames, for the status file.
	std::string TaaSummary() const;
	// Whether a jittered frame can be put back exactly: the camera line it was drawn with is known
	// (TaaSync) and the jitter has not been caught missing from the frame.
	bool SyncLocked() const { return m_syncLocked; }
	bool JitterFailed() const { return m_jitterFailed; }
	// GPU time per stage, measured with timestamp queries, for the status file.
	std::string GpuSummary() const;

	void Release();

	bool Compiled() const { return m_compiled; }
	const std::string& Error() const { return m_error; }

  private:
	// The stages GPU time is measured between, in the order Run reaches them.
	enum GpuStage { GPU_DEPTH, GPU_AA, GPU_MOTION, GPU_DOF, GPU_BLUR, GPU_AO, GPU_IL, GPU_RTGI, GPU_SSR, GPU_SHAFTS, GPU_GRADE, GPU_FINAL, GPU_STAGES };
	static const int GPU_FRAMES = 4; // results are read this many frames later, so nothing waits

	struct GpuFrame
	{
		ID3D11Query* disjoint = nullptr;
		ID3D11Query* stamp[GPU_STAGES + 1] = {};
		bool pending = false;
	};

	void GpuBegin(ID3D11DeviceContext* ctx);
	void GpuMark(ID3D11DeviceContext* ctx, GpuStage stage);
	void GpuEnd(ID3D11DeviceContext* ctx);

	bool EnsureDevice(ID3D11Device* device);
	bool Compile(const std::string& path);
	void LoadLut(const std::string& path);
	void LoadDirt(const std::string& path);
	bool EnsureTargets(const D3D11_TEXTURE2D_DESC& desc, DXGI_FORMAT rtvFormat);
	// The FSR buffers are made only while that stage is on, and given back when it is switched off:
	// at 2x on a 1080p screen they are the largest allocation in the mod.
	bool EnsureUpscale(UINT w, UINT h, float scale);
	// SMAA's two lookup textures, from the blobs dev/nativebuild/make_smaa.py writes beside the
	// shader. Missing or damaged leaves them null, and antialiasing then falls back to FXAA.
	void LoadSmaa(const std::string& dir);
	// Everything the lighting, motion and shaft effects need, made the first time one of them is
	// switched on and given back when the last is switched off, like the FSR buffers.
	bool EnsureAdvanced(UINT w, UINT h);
	bool EnsureScaled(UINT w, UINT h, float scale);
	void ReleaseScaled();
	// Which camera line frames are drawn with, measured from depth (see Effects::TaaSync).
	void TaaSync(ID3D11DeviceContext* ctx, const Settings& s, int back);
	void ReleaseSync();
	void ReleaseAdvanced();
	// One temporal accumulation step into history; returns the accumulated result.
	ID3D11ShaderResourceView* Accumulate(ID3D11DeviceContext* ctx, History& history, ID3D11ShaderResourceView* now, float keep, bool motionOk, ID3D11ShaderResourceView* previousPyramid);
	// Copies the game's depth target into m_depthCopy, making it if the size or format changed.
	// Returns false (with m_depthError set) when that cannot be done, which switches the depth
	// effects off rather than failing the frame.
	bool EnsureDepth(ID3D11DeviceContext1* ctx, ID3D11Texture2D* depth);
	void ReleaseUpscale();
	void ReleaseTargets();
	void ReleaseShaders();
	void WriteBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer, const void* data, size_t size);
	void Pass(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT w, UINT h, float srcW, float srcH, ID3D11ShaderResourceView* const* srvs, UINT count, float extra0 = 0.0f, float extra1 = 0.0f);

	ID3D11Device* m_device = nullptr;
	ID3DDeviceContextState* m_state = nullptr;

	ID3D11VertexShader* m_vs = nullptr;
	ID3D11PixelShader* m_psFxaa = nullptr;
	ID3D11PixelShader* m_psLuma = nullptr;
	ID3D11PixelShader* m_psAdapt = nullptr;
	ID3D11PixelShader* m_psBloomPrefilter = nullptr;
	ID3D11PixelShader* m_psBloomDown = nullptr;
	ID3D11PixelShader* m_psBloomUp = nullptr;
	ID3D11PixelShader* m_psGrade = nullptr;
	ID3D11PixelShader* m_psFinal = nullptr;
	ID3D11PixelShader* m_psWideDown = nullptr;
	ID3D11PixelShader* m_psTintPrefilter = nullptr;
	ID3D11PixelShader* m_psHaloPrefilter = nullptr;
	ID3D11PixelShader* m_psLightInfo = nullptr;
	ID3D11PixelShader* m_psEasu = nullptr;
	ID3D11PixelShader* m_psLinearDepth = nullptr;
	ID3D11PixelShader* m_psTaa = nullptr;
	ID3D11PixelShader* m_psSmaaT2x = nullptr;
	ID3D11PixelShader* m_psAo = nullptr;
	ID3D11PixelShader* m_psAoBlur = nullptr;
	ID3D11PixelShader* m_psSmaaEdges = nullptr;
	ID3D11PixelShader* m_psSmaaWeights = nullptr;
	ID3D11PixelShader* m_psSmaaBlend = nullptr;
	ID3D11PixelShader* m_psFocus = nullptr;
	ID3D11PixelShader* m_psDofPrepare = nullptr;
	ID3D11PixelShader* m_psDofBlur = nullptr;
	ID3D11PixelShader* m_psDofFill = nullptr;
	ID3D11PixelShader* m_psDofComposite = nullptr;
	ID3D11PixelShader* m_psIl = nullptr;
	ID3D11PixelShader* m_psRtgi = nullptr;
	ID3D11PixelShader* m_psSsr = nullptr;
	ID3D11PixelShader* m_psDepthBlur = nullptr;
	ID3D11PixelShader* m_psMotionInput = nullptr;
	ID3D11PixelShader* m_psMotionSearch = nullptr;
	ID3D11PixelShader* m_psTemporal = nullptr;
	ID3D11PixelShader* m_psMotionBlur = nullptr;
	ID3D11PixelShader* m_psShaftMask = nullptr;
	ID3D11PixelShader* m_psShafts = nullptr;
	ID3D11PixelShader* m_psGrain = nullptr;
	ID3D11PixelShader* m_psDownsample = nullptr;

	ID3D11Buffer* m_cbFrame = nullptr;
	ID3D11Buffer* m_cbPass = nullptr;
	ID3D11Buffer* m_cbReproject = nullptr; // b2: this frame's and last frame's camera, for PS_Taa
	ID3D11SamplerState* m_linear = nullptr;
	ID3D11SamplerState* m_point = nullptr;
	ID3D11RasterizerState* m_raster = nullptr;
	ID3D11DepthStencilState* m_depth = nullptr;

	RenderTexture m_src;
	RenderTexture m_aa;
	// sRGB views of the two, for SMAA's blend pass (null when the format has no sRGB twin).
	ID3D11ShaderResourceView* m_srcSrgbSrv = nullptr;
	ID3D11RenderTargetView* m_aaSrgbRtv = nullptr;
	RenderTexture m_lum;
	RenderTexture m_adapt[2];
	RenderTexture m_down[BLOOM_LEVELS];
	RenderTexture m_up[BLOOM_LEVELS];
	RenderTexture m_grade;
	RenderTexture m_t2xPrev; // SMAA T2x: last frame's graded picture, as drawn (jittered the other way)
	bool m_t2xReset = true;
	bool m_t2xRunning = false;
	RenderTexture m_blur[2]; // half and quarter resolution copies (clarity, monocular cues, and the start of the wide chains)
	RenderTexture m_wide[WIDE_LEVELS]; // wide blur of the image: obscurance and monocular cues
	RenderTexture m_tint[WIDE_LEVELS]; // wide blur of strong colour only: colour bloom
	RenderTexture m_halo[WIDE_LEVELS]; // wide blur of the brightest highlights only: halation
	RenderTexture m_light;             // 1x1: where the bright light in the frame is, and how strong
	RenderTexture m_upscale;           // FSR 1.0: the EASU result (RCAS runs in the final pass)
	RenderTexture m_ao;                // half resolution ambient occlusion from depth
	RenderTexture m_aoBlur;            // ...blurred along surfaces but not across edges
	RenderTexture m_depthCopy;         // our own copy of the game's depth target
	RenderTexture m_depthLinear;       // ...as distance from the camera, worked out once a frame
	RenderTexture m_smaaEdges;         // SMAA: edges found this frame
	RenderTexture m_smaaWeights;       // SMAA: how much each edge covers each pixel
	RenderTexture m_dofHalf[2];        // depth of field: prepared half resolution buffer, then the blur
	RenderTexture m_dof;               // depth of field: the composited frame
	RenderTexture m_focus[2];          // 1x1: the focus distance, eased from frame to frame
	int m_focusIndex = 0;
	bool m_focusReset = true;
	double m_dofIdle = 0.0; // seconds since you stopped aiming, in the aiming mode

	// Advanced effects (EnsureAdvanced).
	RenderTexture m_motionPyr[2];      // half resolution brightness and depth, with mips: this frame and last
	RenderTexture m_motion[3];         // motion found at 1/2, 1/4 and 1/8 of the screen
	int m_motionIndex = 0;
	bool m_motionReset = true;
	History m_aoHist;
	History m_ilHist;
	History m_rtgiHist;
	History m_ssrHist;
	History m_grainHist; // full resolution: the surface grain, carried from frame to frame
	History m_taaHist;   // full resolution: the temporally accumulated frame
	RenderTexture m_scaled;   // render scale cleanup: the frame shrunk
	RenderTexture m_rescaled; // ...and rebuilt at screen size
	UINT m_scaledW = 0;
	UINT m_scaledH = 0;
	RenderTexture m_il;
	RenderTexture m_ilBlur;
	RenderTexture m_rtgi;
	RenderTexture m_rtgiBlur;
	RenderTexture m_ssr;
	RenderTexture m_shaftMask;
	RenderTexture m_shafts;
	RenderTexture m_mblur;
	bool m_advanced = false;
	bool m_loggedAdvancedFailure = false;
	UINT m_advancedW = 0;
	UINT m_advancedH = 0;
	unsigned m_frameCount = 0;
	bool m_taaRunning = false;
	// Last frame's camera, for reprojection, and how often the camera line arrived one frame apart.
	CameraPose m_prevPose;
	float m_lastNewest = -1.0f;
	unsigned m_poseFrames = 0, m_poseInStep = 0, m_poseStale = 0;
	unsigned m_lastPoseFrames = 0, m_lastPoseInStep = 0, m_lastPoseStale = 0;
	// TaaSync: four candidate cameras scored on how well they turn last frame's depth into this one's.
	static const int SYNC_SLOTS = 4;   // readbacks in flight, read a few frames later without waiting
	static const int SYNC_VOTES = 64;  // how many clear answers the decision looks back over
	struct SyncRead
	{
		ID3D11Texture2D* tex = nullptr;
		bool pending = false;
		bool valid[4] = {};
		bool jittered = false;
		int back = 0;
		float frame = 0.0f, height = 0.0f, pitch = 0.0f, roll = 0.0f; // the camera line checked, for the log
		bool aim = false;
		bool zoom = false; // the field of view changed between the two lines
		char state[32] = {}; // the game state, for the log
	};
	void SyncResult(const float r[4], const SyncRead& read);
	void PauseJitter(const char* why);
	RenderTexture m_syncOut;    // 128x64 residuals, mipped down to their average
	RenderTexture m_depthPrev;  // last frame's distance
	ID3D11Buffer* m_cbSync = nullptr;
	ID3D11PixelShader* m_psTaaSync = nullptr;
	SyncRead m_syncRead[SYNC_SLOTS];
	int m_syncSlot = 0;
	float m_depthPrevFrame = -1.0f;    // the newest camera line when m_depthPrev was taken
	unsigned char m_votes[SYNC_VOTES] = {};
	int m_voteCount = 0, m_voteAt = 0;
	unsigned char m_landed[SYNC_VOTES] = {};
	int m_landCount = 0, m_landAt = 0;
	int m_jitterChecks = 0;            // jittered frames checked, clear answer or not
	int m_syncBack = 0;                // how many lines back the frames are drawn with
	bool m_syncLocked = false;         // ...and that answer is settled
	bool m_jitterFailed = false;       // the jitter was found not to reach the frame: paused
	double m_pauseLeft = 0.0;          // seconds until it is tried again
	double m_pauseNext = 10.0;         // the next pause, doubled each time on a level (up to 320 s)
	int m_missLogged = 0;              // frames without the jitter logged since the level started or it was tried again
	unsigned m_situation[2][2][2] = {}; // jitter checks by situation, [aiming][zooming][seen], clear answers only
	char m_gameState[32] = {};         // the last game state logged
	float m_lineFrameMax = -1.0f;      // the highest camera line number seen; a lower one is a new level
	ID3D11Texture2D* m_smaaAreaTex = nullptr;
	ID3D11ShaderResourceView* m_smaaAreaSrv = nullptr;
	ID3D11Texture2D* m_smaaSearchTex = nullptr;
	ID3D11ShaderResourceView* m_smaaSearchSrv = nullptr;
	DXGI_FORMAT m_depthFormat = DXGI_FORMAT_UNKNOWN;
	std::string m_depthError;
	float m_lastDepthMode = -1.0f;
	GpuFrame m_gpu[GPU_FRAMES];
	int m_gpuSlot = 0;
	bool m_gpuTiming = false; // this frame's queries were issued
	float m_gpuMs[GPU_STAGES] = {};
	bool m_gpuMeasured = false;
	UINT m_upW = 0;
	UINT m_upH = 0;
	DXGI_FORMAT m_upFormat = DXGI_FORMAT_UNKNOWN;
	UINT m_lumMips = 1;
	int m_adaptIndex = 0;
	bool m_adaptReset = true;

	ID3D11Texture3D* m_lutTex = nullptr;
	ID3D11ShaderResourceView* m_lutSrv = nullptr;
	int m_lutSize = 0;

	// The lens dirt photograph, converted by dev/nativebuild/make_dirt.py. Missing or unreadable
	// leaves these null and the effect simply never runs.
	ID3D11Texture2D* m_dirtTex = nullptr;
	ID3D11ShaderResourceView* m_dirtSrv = nullptr;

	UINT m_targetW = 0;
	UINT m_targetH = 0;
	DXGI_FORMAT m_targetFormat = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT m_targetRtvFormat = DXGI_FORMAT_UNKNOWN;

	unsigned m_shaderGen = 0;
	unsigned m_lutGen = 0;
	unsigned m_dirtGen = 0;
	bool m_compiled = false;
	std::string m_error;

	long long m_lastCounter = 0;
	double m_time = 0.0;
};

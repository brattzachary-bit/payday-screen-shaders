#include "hooks.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "bridge.h"
#include "effects.h"
#include "log.h"
#include "state.h"
#include "vtbl.h"

/*
	How "under the HUD" is found - read off a real draw capture, not assumed.

	Diesel does NOT draw the world to the back buffer once and put the HUD on top. It uses the
	back buffer as a scratch scene target first (meshes and a fullscreen pass go into it, it is
	copied out and post processed at 1440x810 / 960x540 / 480x270), and only then writes the
	final composite into it with one more fullscreen triangle. The HUD and menus follow as quads
	(6 vertices) and text.

	So the world's finished image is the LAST fullscreen triangle (3 vertices, viewport covering
	the back buffer) drawn into the back buffer in a frame. That is only known once the frame is
	over, so the count from previous frames decides: once a frame has had that many fullscreen
	triangles, the shaders run right after the last one. The count has to be the same two frames
	in a row, so a scene change cannot put them in the wrong place, and a frame without any
	(loading screens are GUI only) is never touched.

	Position alone flickers in a heist: some frames add a fullscreen pass, and "after pass 4"
	then lands before the real final pass, which paints over the result. So the final pass is
	also learned by IDENTITY: its pixel shader (the same object in menu and heist). Once the last
	fullscreen triangle has used the same pixel shader for LEARN_FRAMES frames, the shaders run
	after any fullscreen triangle drawn with it, however many passes came before. If it is not
	seen for FORGET_FRAMES frames that do have fullscreen passes, it is learned again. Until it
	is learned, the position rule above is used.

	"inject_after" = N overrides both: run after the Nth fullscreen triangle instead.
	"require_fullscreen" off also counts 3-vertex draws whose viewport is smaller.

	The capture function records the real call order so this can be checked against what the
	game does in any scene.
*/

extern DWORD g_luaThread;
// Whether the game draws on the thread Lua runs on: -1 not known yet, 0 no, 1 yes.
static int g_renderOnLuaThread = -1;

namespace
{
	using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
	using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
	using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
	using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
	using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
	using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
	using OMSetRenderTargetsAndUAVsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
	using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
	using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
	using CopySubresourceRegionFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
	using ResolveSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
	using ClearRenderTargetViewFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
	using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

	DrawFn oDraw;
	DrawIndexedFn oDrawIndexed;
	DrawInstancedFn oDrawInstanced;
	DrawIndexedInstancedFn oDrawIndexedInstanced;
	PSSetShaderFn oPSSetShader;
	OMSetRenderTargetsFn oOMSetRenderTargets;
	OMSetRenderTargetsAndUAVsFn oOMSetRenderTargetsAndUAVs;
	RSSetViewportsFn oRSSetViewports;
	CopyResourceFn oCopyResource;
	CopySubresourceRegionFn oCopySubresourceRegion;
	ResolveSubresourceFn oResolveSubresource;
	ClearRenderTargetViewFn oClearRenderTargetView;
	ExecuteCommandListFn oExecuteCommandList;
	PresentFn oPresent;
	Present1Fn oPresent1;

	struct FrameState
	{
		bool backbufferBound = false;
		UINT backbufferDraws = 0;
		UINT totalDraws = 0;
		UINT fullscreenDraws = 0;
		bool injected = false;
		bool ran = false; // the effects really ran (injected is only "tried")
		float viewportW = 0.0f;
		float viewportH = 0.0f;
		ID3D11PixelShader* ps = nullptr;
		ID3D11PixelShader* lastFullscreenPs = nullptr;
		bool sawFinalPs = false;
		UINT injectedAfterPass = 0;

		// The game's depth target for this frame. A raw pointer, never referenced: it is only
		// used later in the same frame, while the game itself is holding the texture.
		ID3D11Texture2D* depthTex = nullptr;
		UINT depthW = 0;
		UINT depthH = 0;
		int depthFormat = 0;
	};

	const int LEARN_FRAMES = 30;
	const int FORGET_FRAMES = 120;

	FrameState g_frame;
	Settings g_settings;
	bool g_haveSettings = false;

	UINT g_lastFullscreenDraws = 0;
	UINT g_stableFullscreenDraws = 0;
	UINT g_targetFullscreenDraw = 0;

	// Compared as pointers only, never dereferenced.
	ID3D11PixelShader* g_finalPs = nullptr;
	ID3D11PixelShader* g_candidatePs = nullptr;
	int g_candidateFrames = 0;
	int g_finalMissingFrames = 0;

	// Frames that should have had the shaders but did not, per second, for the log and status.
	ULONGLONG g_windowStart = 0;
	UINT g_windowFrames = 0;
	UINT g_windowSkipped = 0;
	UINT g_windowMinPasses = 0;
	UINT g_windowMaxPasses = 0;
	UINT g_lastSecondSkipped = 0;
	int g_skipLogLines = 0;

	// Raw pointers only: holding a reference to the back buffer would make ResizeBuffers fail.
	ID3D11Resource* g_backbuffer = nullptr;
	UINT g_backbufferW = 0;
	UINT g_backbufferH = 0;
	ID3D11DeviceContext* g_immediate = nullptr;

	thread_local bool t_inside = false;

	Effects g_effects;

	FILE* g_capture = nullptr;
	int g_captureFramesLeft = 0;
	int g_captureLines = 0;
	uint64_t g_frameNumber = 0;
	uint64_t g_injectedFrames = 0;
	int g_lastRtvFormat = 0;

	// Last frame's depth findings, for the status file. Written on the render thread once a frame.
	bool g_depthFound = false;
	UINT g_depthW = 0;
	UINT g_depthH = 0;
	int g_depthFormat = 0;
	bool g_loggedDepth = false;
	bool g_presentHooked = false;
	bool g_hooksInstalled = false;
	bool g_contextTried = false;

	// The DLL that holds an address. Empty when it is in none (heap, not code).
	std::string ModuleOf(const void* addr)
	{
		HMODULE module = nullptr;
		char path[MAX_PATH];

		if (!addr || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &module) || !GetModuleFileNameA(module, path, sizeof(path)))
			return "";

		return path;
	}

	bool g_loggedRunFailure = false;

	void Capture(const char* fmt, ...)
	{
		if (!g_capture || g_captureLines > 60000)
			return;

		va_list args;
		va_start(args, fmt);
		vfprintf(g_capture, fmt, args);
		va_end(args);
		fputc('\n', g_capture);
		g_captureLines++;
	}

	void* Patch(void** vtbl, int slot, void* hook)
	{
		if (vtbl[slot] == hook)
			return nullptr;

		DWORD old;
		VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
		void* original = vtbl[slot];
		vtbl[slot] = hook;
		VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);

		return original;
	}

	// Only the game's own immediate context (taken from its swap chain) counts as the frame.
	bool IsImmediate(ID3D11DeviceContext* ctx)
	{
		return ctx == g_immediate;
	}

	bool IsBackbuffer(ID3D11RenderTargetView* rtv)
	{
		if (!rtv || !g_backbuffer)
			return false;

		ID3D11Resource* resource = nullptr;
		rtv->GetResource(&resource);

		bool result = resource == g_backbuffer;

		if (resource)
			resource->Release();

		return result;
	}

	void DescribeRtv(char* out, size_t size, ID3D11RenderTargetView* rtv)
	{
		if (!rtv)
		{
			snprintf(out, size, "null");
			return;
		}

		ID3D11Resource* resource = nullptr;
		rtv->GetResource(&resource);

		D3D11_RENDER_TARGET_VIEW_DESC rd;
		rtv->GetDesc(&rd);

		UINT w = 0, h = 0;
		ID3D11Texture2D* tex = nullptr;

		if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)))
		{
			D3D11_TEXTURE2D_DESC td;
			tex->GetDesc(&td);
			w = td.Width;
			h = td.Height;
			tex->Release();
		}

		snprintf(out, size, "res=%p %ux%u fmt=%d%s", (void*)resource, w, h, (int)rd.Format, resource == g_backbuffer ? " BACKBUFFER" : "");

		if (resource)
			resource->Release();
	}

	bool WantInject()
	{
		return g_haveSettings && g_settings.active && g_settings.p[P_ENABLED] >= 0.5f;
	}

	void RunEffects(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv)
	{
		ID3D11Resource* resource = nullptr;
		rtv->GetResource(&resource);

		ID3D11Texture2D* tex = nullptr;

		if (!resource || FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)))
		{
			if (resource)
				resource->Release();

			return;
		}

		resource->Release();

		ID3D11DeviceContext1* ctx1 = nullptr;
		ID3D11Device* device = nullptr;

		if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1)))
		{
			ctx->GetDevice(&device);

			D3D11_RENDER_TARGET_VIEW_DESC rd;
			rtv->GetDesc(&rd);
			g_lastRtvFormat = (int)rd.Format;

			g_depthFound = g_frame.depthTex != nullptr;
			g_depthW = g_frame.depthW;
			g_depthH = g_frame.depthH;
			g_depthFormat = g_frame.depthFormat;

			if (g_depthFound && !g_loggedDepth)
			{
				g_loggedDepth = true;
				Log("depth target found: %ux%u format %d", g_depthW, g_depthH, g_depthFormat);
			}

			// Temporal antialiasing needs the camera this very frame was drawn with. Lua writes it from
			// Setup:pre_render, just before the game renders; on the same thread that is always this frame's.
			if (g_renderOnLuaThread < 0)
			{
				// PSS_TEST_RENDER_THREAD: the test host draws on Lua's thread; this makes it act as if not.
				g_renderOnLuaThread = g_luaThread == GetCurrentThreadId() && !GetEnvironmentVariableA("PSS_TEST_RENDER_THREAD", nullptr, 0) ? 1 : 0;
				Log("Lua runs on thread %lu, the renderer on %lu: %s", (unsigned long)g_luaThread, (unsigned long)GetCurrentThreadId(),
					g_renderOnLuaThread ? "the same, so the camera line is this frame's" : "DIFFERENT, so which camera line each frame was drawn with is measured (TaaSync)");
			}

			Bridge::PollCameraNow();
			Settings fresh = State::Snapshot();
			fresh.renderOnLuaThread = g_renderOnLuaThread == 1;

			t_inside = true;
			bool ok = g_effects.Run(device, ctx1, tex, rtv, rd.Format, fresh, g_frame.depthTex);
			t_inside = false;

			if (ok)
			{
				g_injectedFrames++;
				g_frame.ran = true;
				g_loggedRunFailure = false;
			}
			else if (!g_loggedRunFailure)
			{
				g_loggedRunFailure = true;
				Log("effects did not run: %s", g_effects.Error().c_str());
			}

			device->Release();
			ctx1->Release();
		}

		tex->Release();
	}

	// Called before the game's draw goes through. Returns true when the shaders should run
	// straight AFTER it: it is the fullscreen pass that holds the world's final image.
	bool BeforeDraw(ID3D11DeviceContext* ctx, const char* kind, UINT count, UINT instances, bool indexed)
	{
		if (t_inside || !IsImmediate(ctx))
			return false;

		g_frame.totalDraws++;

		if (g_capture)
			Capture("  %s count=%u inst=%u ps=%p vp=%.0fx%.0f%s", kind, count, instances, (void*)g_frame.ps, g_frame.viewportW, g_frame.viewportH, g_frame.backbufferBound ? " ->BACKBUFFER" : "");

		if (!g_frame.backbufferBound)
			return false;

		g_frame.backbufferDraws++;

		bool coversBackbuffer = fabs(g_frame.viewportW - (float)g_backbufferW) < 1.0f && fabs(g_frame.viewportH - (float)g_backbufferH) < 1.0f;
		bool fullscreenTriangle = !indexed && count == 3 && instances <= 1 && (coversBackbuffer || g_settings.p[P_REQUIRE_FULLSCREEN] < 0.5f);

		if (!fullscreenTriangle)
			return false;

		g_frame.fullscreenDraws++;
		g_frame.lastFullscreenPs = g_frame.ps;

		bool isFinal = g_finalPs && g_frame.ps == g_finalPs;

		if (isFinal)
			g_frame.sawFinalPs = true;

		if (g_frame.injected || !WantInject())
			return false;

		UINT chosen = (UINT)g_settings.p[P_INJECT_AFTER];

		if (chosen > 0)
			return g_frame.fullscreenDraws == chosen;

		if (g_finalPs)
			return isFinal;

		return g_targetFullscreenDraw > 0 && g_frame.fullscreenDraws == g_targetFullscreenDraw;
	}

	void AfterDraw(ID3D11DeviceContext* ctx, bool inject)
	{
		if (!inject)
			return;

		g_frame.injected = true;
		g_frame.injectedAfterPass = g_frame.fullscreenDraws;

		ID3D11RenderTargetView* rtv = nullptr;
		ctx->OMGetRenderTargets(1, &rtv, nullptr);

		if (!rtv)
			return;

		if (g_capture)
			Capture("  >>> SHADERS RUN HERE (after fullscreen pass %u)", g_frame.fullscreenDraws);

		RunEffects(ctx, rtv);
		rtv->Release();
	}

	void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* ctx, UINT count, UINT start)
	{
		bool inject = BeforeDraw(ctx, "Draw", count, 1, false);
		oDraw(ctx, count, start);
		AfterDraw(ctx, inject);
	}

	void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base)
	{
		bool inject = BeforeDraw(ctx, "DrawIndexed", count, 1, true);
		oDrawIndexed(ctx, count, start, base);
		AfterDraw(ctx, inject);
	}

	void STDMETHODCALLTYPE hkDrawInstanced(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, UINT startInstance)
	{
		bool inject = BeforeDraw(ctx, "DrawInstanced", count, instances, false);
		oDrawInstanced(ctx, count, instances, start, startInstance);
		AfterDraw(ctx, inject);
	}

	void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, INT base, UINT startInstance)
	{
		bool inject = BeforeDraw(ctx, "DrawIndexedInstanced", count, instances, true);
		oDrawIndexedInstanced(ctx, count, instances, start, base, startInstance);
		AfterDraw(ctx, inject);
	}

	void STDMETHODCALLTYPE hkPSSetShader(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* instances, UINT count)
	{
		if (!t_inside && IsImmediate(ctx))
			g_frame.ps = ps;

		oPSSetShader(ctx, ps, instances, count);
	}

	/*
		Finding depth. Diesel is a deferred renderer: it binds several render targets at once and
		draws the world's distance into one of them, a single channel floating point texture the
		size of the back buffer. That is a COLOUR target, not a depth stencil buffer, which is why
		this can be read at all without touching how the game creates its textures.

		A real capture (mods/logs/pss_capture.txt) shows it written early in the frame and not
		cleared again until the next one, so by the time the shaders run it is finished and
		untouched. The last matching target bound in a frame is the one used.
	*/
	void NoteDepthCandidate(ID3D11RenderTargetView* rtv)
	{
		if (!rtv || !g_backbufferW || !g_backbufferH)
			return;

		D3D11_RENDER_TARGET_VIEW_DESC rd;
		rtv->GetDesc(&rd);

		if (rd.Format != DXGI_FORMAT_R32_FLOAT && rd.Format != DXGI_FORMAT_R16_FLOAT)
			return;

		ID3D11Resource* resource = nullptr;
		rtv->GetResource(&resource);

		if (!resource)
			return;

		ID3D11Texture2D* tex = nullptr;

		if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)))
		{
			D3D11_TEXTURE2D_DESC td;
			tex->GetDesc(&td);

			// Full screen only: the game keeps half resolution depth copies too, and those would
			// be the wrong thing to hand a full resolution pass.
			if (td.Width == g_backbufferW && td.Height == g_backbufferH && td.SampleDesc.Count == 1)
			{
				g_frame.depthTex = tex;
				g_frame.depthW = td.Width;
				g_frame.depthH = td.Height;
				g_frame.depthFormat = (int)td.Format;

				if (g_capture)
					Capture("  depth candidate res=%p %ux%u fmt=%d", (void*)resource, td.Width, td.Height, (int)td.Format);
			}

			tex->Release();
		}

		resource->Release();
	}

	void TrackTargets(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs)
	{
		if (t_inside || !IsImmediate(ctx))
			return;

		if (count == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
			return;

		ID3D11RenderTargetView* first = count > 0 && rtvs ? rtvs[0] : nullptr;
		g_frame.backbufferBound = IsBackbuffer(first);

		if (rtvs)
		{
			UINT scan = count > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ? D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT : count;

			for (UINT i = 0; i < scan; i++)
				NoteDepthCandidate(rtvs[i]);
		}

		if (g_capture)
		{
			char desc[160];
			DescribeRtv(desc, sizeof(desc), first);
			Capture("OMSetRenderTargets n=%u rt0: %s", count, desc);

			// Every other bound target too: Diesel writes several at once, and one of them may hold the
			// surface normals the lighting effects currently rebuild from depth.
			UINT listed = count > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ? D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT : count;

			for (UINT i = 1; rtvs && i < listed; i++)
			{
				if (!rtvs[i])
					continue;

				DescribeRtv(desc, sizeof(desc), rtvs[i]);
				Capture("    rt%u: %s", i, desc);
			}
		}
	}

	void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
	{
		TrackTargets(ctx, count, rtvs);
		oOMSetRenderTargets(ctx, count, rtvs, dsv);
	}

	void STDMETHODCALLTYPE hkOMSetRenderTargetsAndUAVs(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv, UINT uavSlot, UINT uavCount, ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts)
	{
		TrackTargets(ctx, count, rtvs);
		oOMSetRenderTargetsAndUAVs(ctx, count, rtvs, dsv, uavSlot, uavCount, uavs, initialCounts);
	}

	void STDMETHODCALLTYPE hkRSSetViewports(ID3D11DeviceContext* ctx, UINT count, const D3D11_VIEWPORT* viewports)
	{
		if (!t_inside && IsImmediate(ctx) && count > 0 && viewports)
		{
			g_frame.viewportW = viewports[0].Width;
			g_frame.viewportH = viewports[0].Height;
		}

		oRSSetViewports(ctx, count, viewports);
	}

	void STDMETHODCALLTYPE hkCopyResource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src)
	{
		if (g_capture && !t_inside && IsImmediate(ctx))
			Capture("  CopyResource dst=%p%s src=%p%s", (void*)dst, dst == g_backbuffer ? " BACKBUFFER" : "", (void*)src, src == g_backbuffer ? " BACKBUFFER" : "");

		oCopyResource(ctx, dst, src);
	}

	void STDMETHODCALLTYPE hkCopySubresourceRegion(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box)
	{
		if (g_capture && !t_inside && IsImmediate(ctx))
			Capture("  CopySubresourceRegion dst=%p%s src=%p", (void*)dst, dst == g_backbuffer ? " BACKBUFFER" : "", (void*)src);

		oCopySubresourceRegion(ctx, dst, dstSub, x, y, z, src, srcSub, box);
	}

	void STDMETHODCALLTYPE hkResolveSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub, DXGI_FORMAT format)
	{
		if (g_capture && !t_inside && IsImmediate(ctx))
			Capture("  ResolveSubresource dst=%p%s src=%p", (void*)dst, dst == g_backbuffer ? " BACKBUFFER" : "", (void*)src);

		oResolveSubresource(ctx, dst, dstSub, src, srcSub, format);
	}

	void STDMETHODCALLTYPE hkClearRenderTargetView(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const FLOAT* color)
	{
		if (g_capture && !t_inside && IsImmediate(ctx))
		{
			char desc[160];
			DescribeRtv(desc, sizeof(desc), rtv);
			Capture("  ClearRenderTargetView %s", desc);
		}

		oClearRenderTargetView(ctx, rtv, color);
	}

	void STDMETHODCALLTYPE hkExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore)
	{
		if (g_capture && !t_inside && IsImmediate(ctx))
			Capture("  ExecuteCommandList %p (deferred rendering in use)", (void*)list);

		oExecuteCommandList(ctx, list, restore);
	}

	void UpdateStatus(Status& s, void*)
	{
		s.hooksInstalled = g_hooksInstalled;
		s.presentHooked = g_presentHooked;
		s.compiled = g_effects.Compiled();
		s.error = g_effects.Error();
		s.frames = g_frameNumber;
		s.injectedFrames = g_injectedFrames;
		s.lastBackbufferDraws = (int)g_frame.backbufferDraws;
		s.lastTotalDraws = (int)g_frame.totalDraws;
		s.lastFullscreenDraws = (int)g_frame.fullscreenDraws;
		s.targetFullscreenDraw = (int)g_frame.injectedAfterPass;
		s.finalPassLearned = g_finalPs != nullptr;
		s.unprocessedLastSecond = (int)g_lastSecondSkipped;
		s.backbufferW = (int)g_backbufferW;
		s.backbufferH = (int)g_backbufferH;
		s.rtvFormat = g_lastRtvFormat;
		s.srgb = g_lastRtvFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || g_lastRtvFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		s.depthFound = g_depthFound;
		s.depthReadable = g_effects.DepthReady();
		s.depthW = (int)g_depthW;
		s.depthH = (int)g_depthH;
		s.depthFormat = g_depthFormat;
		s.depthMode = g_effects.LastDepthMode();
		s.cameraValid = g_settings.p[P_CAMERA_VALID] > 0.5f;
		s.cameraTanX = g_settings.p[P_CAMERA_TAN_X];
		s.cameraTanY = g_settings.p[P_CAMERA_TAN_Y];
		s.cameraPitch = g_settings.p[P_CAMERA_PITCH];
		s.cameraHeight = g_settings.p[P_CAMERA_HEIGHT];
		s.cameraFrame = g_settings.p[P_CAMERA_FRAME];
		s.taaRunning = g_effects.TaaRunning();
		s.depthDebugView = (int)g_settings.p[P_DEBUG_VIEW];
		s.depthError = g_effects.DepthError();
		s.gpuTime = g_effects.GpuSummary();
		s.taa = g_effects.TaaSummary();
		// A jittered frame can only be put back with the camera line it was drawn with. On Lua's own
		// thread that is always the newest; otherwise it must have been measured (Effects::TaaSync).
		if (!s.taaRunning)
			s.jitter = "not allowed (temporal antialiasing is not running)";
		else if (g_effects.JitterFailed())
			s.jitter = "paused (the frames did not show it; it is tried again shortly; see pss_native.log)";
		else if (g_renderOnLuaThread != 1 && !g_effects.SyncLocked())
			s.jitter = "not allowed yet (the game draws on its own thread; waiting to measure which camera line its frames use)";
		else
			s.jitter = "allowed";

		// And what Lua says it is doing about it.
		static const char* kLua[] = {"jittering", "off in the options", "waiting for the plugin to allow it", "stopped: its code failed (see the BLT log)", "the game moved the camera after the turn, so the frame has none"};
		int lua = g_settings.recent[0].luaJitter;
		s.jitter += "; Lua: ";
		s.jitter += lua >= 0 && lua <= 4 ? kLua[lua] : "not saying (no camera line, or an older Lua)";
	}

	// Every context method this plugin hooks: slot, hook, where the original goes.
#define PSS_CONTEXT_HOOKS(X) \
	X(VT_CTX_DRAW, hkDraw, oDraw) \
	X(VT_CTX_DRAW_INDEXED, hkDrawIndexed, oDrawIndexed) \
	X(VT_CTX_DRAW_INSTANCED, hkDrawInstanced, oDrawInstanced) \
	X(VT_CTX_DRAW_INDEXED_INSTANCED, hkDrawIndexedInstanced, oDrawIndexedInstanced) \
	X(VT_CTX_PS_SET_SHADER, hkPSSetShader, oPSSetShader) \
	X(VT_CTX_OM_SET_RENDER_TARGETS, hkOMSetRenderTargets, oOMSetRenderTargets) \
	X(VT_CTX_OM_SET_RENDER_TARGETS_AND_UAVS, hkOMSetRenderTargetsAndUAVs, oOMSetRenderTargetsAndUAVs) \
	X(VT_CTX_RS_SET_VIEWPORTS, hkRSSetViewports, oRSSetViewports) \
	X(VT_CTX_COPY_RESOURCE, hkCopyResource, oCopyResource) \
	X(VT_CTX_COPY_SUBRESOURCE_REGION, hkCopySubresourceRegion, oCopySubresourceRegion) \
	X(VT_CTX_RESOLVE_SUBRESOURCE, hkResolveSubresource, oResolveSubresource) \
	X(VT_CTX_CLEAR_RENDER_TARGET_VIEW, hkClearRenderTargetView, oClearRenderTargetView) \
	X(VT_CTX_EXECUTE_COMMAND_LIST, hkExecuteCommandList, oExecuteCommandList)

	/*
		Why the game's context gets a vtable of OUR own - read off a real log (0.5.6, Windows, AMD):
		d3d11.dll builds each context's vtable inside the context object and keeps REWRITING its Draw
		slots as pipeline state changes. A hook written into those slots is gone before the next draw,
		while hooks in the other slots stay.

		So the context's vtable pointer is pointed at a table of ours. Each slot of it is a stub that
		jumps to whatever d3d11 has in ITS table for that slot at the moment of the call, so d3d11's
		rewrites keep working; the slots hooked here hold the hooks, and their "original" is that slot's
		stub.
	*/
	void** g_liveTable = nullptr; // d3d11's own table for the game context, which it keeps rewriting
	void** g_ourTable = nullptr;  // where the game context's vtable pointer points instead
	int g_tableChecks = 0;

	// Per slot: mov rax, live; jmp [rax + slot*8]. A tail jump, so any signature passes through untouched.
	void** BuildForwardingTable(void** live)
	{
		const int STUB = 16;
		BYTE* code = (BYTE*)VirtualAlloc(nullptr, VT_CTX_COUNT * STUB, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

		if (!code)
			return nullptr;

		void** table = new void*[VT_CTX_COUNT];

		for (int i = 0; i < VT_CTX_COUNT; i++)
		{
			BYTE* stub = code + i * STUB;
			int32_t offset = i * (int32_t)sizeof(void*);

			stub[0] = 0x48; // mov rax, imm64
			stub[1] = 0xB8;
			memcpy(stub + 2, &live, sizeof(live));
			stub[10] = 0xFF; // jmp qword ptr [rax + disp32]
			stub[11] = 0xA0;
			memcpy(stub + 12, &offset, sizeof(offset));
			table[i] = stub;
		}

		DWORD old;
		VirtualProtect(code, VT_CTX_COUNT * STUB, PAGE_EXECUTE_READ, &old);
		FlushInstructionCache(GetCurrentProcess(), code, VT_CTX_COUNT * STUB);

		return table;
	}

	// Once a frame: something could set the vtable pointer back (undone here), or write d3d11's
	// entries into our table instead of its own (logged; then the draws are not seen).
	void CheckTable()
	{
		void** vt = *(void***)g_immediate;

		if (vt == g_ourTable && g_ourTable[VT_CTX_DRAW] == (void*)hkDraw)
			return;

		if (g_tableChecks++ < 10)
			Log("frame %llu: game context vtable %p (ours %p, d3d11's %p), Draw in ours %p [%s]", (unsigned long long)g_frameNumber,
				(void*)vt, (void*)g_ourTable, (void*)g_liveTable, g_ourTable[VT_CTX_DRAW], ModuleOf(g_ourTable[VT_CTX_DRAW]).c_str());

		if (vt == g_liveTable)
			*(void***)g_immediate = g_ourTable;
	}

	// Hooks the GAME's immediate context, found through its swap chain. A throwaway device's context
	// is not enough: d3d11.dll gives every context a vtable of its own.
	void HookGameContext(IDXGISwapChain* swap)
	{
		ID3D11Device* device = nullptr;

		if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), (void**)&device)) || !device)
			return;

		// One attempt only; the log says why it failed.
		g_contextTried = true;

		ID3D11DeviceContext* ctx = nullptr;
		device->GetImmediateContext(&ctx);
		device->Release();

		if (!ctx)
			return;

		void** vt = *(void***)ctx;
		std::string drawModule = ModuleOf(vt[VT_CTX_DRAW]);
		std::string omsetModule = ModuleOf(vt[VT_CTX_OM_SET_RENDER_TARGETS]);

		// Never write into something that is not a vtable of code: that would corrupt the game's memory.
		if (drawModule.empty() || omsetModule.empty())
		{
			Log("the game context's Draw/OMSetRenderTargets are not code in any DLL; NOT patching it");
			ctx->Release();

			return;
		}

		void** table = BuildForwardingTable(vt);

		if (!table)
		{
			Log("could not allocate the forwarding table; NOT hooking the context");
			ctx->Release();

			return;
		}

#define X(slot, hook, original) original = (decltype(original))table[slot]; table[slot] = (void*)hook;
		PSS_CONTEXT_HOOKS(X)
#undef X

		g_liveTable = vt;
		g_ourTable = table;

		// The swap itself: one aligned pointer write, on the thread that renders.
		*(void***)ctx = table;

		// Raw pointer, compared only: the game owns the context.
		g_immediate = ctx;
		ctx->Release();

		g_hooksInstalled = oDraw && oOMSetRenderTargets;

		Log("game context hooked: %d", g_hooksInstalled ? 1 : 0);
	}

	void OnPresent(IDXGISwapChain* swap)
	{
		if (!g_contextTried)
			HookGameContext(swap);

		if (g_hooksInstalled)
			CheckTable();

		ID3D11Texture2D* backbuffer = nullptr;

		if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer)) && backbuffer)
		{
			D3D11_TEXTURE2D_DESC desc;
			backbuffer->GetDesc(&desc);
			g_backbuffer = backbuffer;
			g_backbufferW = desc.Width;
			g_backbufferH = desc.Height;
			backbuffer->Release();
		}

		// Where the next frame's shaders go: after the last fullscreen pass, once the count has
		// held for two frames, or after the pass the user chose.
		UINT count = g_frame.fullscreenDraws;
		g_stableFullscreenDraws = count == g_lastFullscreenDraws ? count : 0;
		g_lastFullscreenDraws = count;

		// Learn the final pass by its pixel shader, and let go of it if it stops appearing.
		ID3D11PixelShader* last = g_frame.lastFullscreenPs;

		if (last && last == g_candidatePs)
		{
			g_candidateFrames++;
		}
		else
		{
			g_candidatePs = last;
			g_candidateFrames = last ? 1 : 0;
		}

		if (!g_finalPs && g_candidateFrames >= LEARN_FRAMES)
		{
			g_finalPs = g_candidatePs;
			g_finalMissingFrames = 0;
			Log("final pass learned: pixel shader %p (fullscreen pass %u of %u)", (void*)g_finalPs, count, count);
		}
		else if (g_finalPs && count > 0)
		{
			g_finalMissingFrames = g_frame.sawFinalPs ? 0 : g_finalMissingFrames + 1;

			if (g_finalMissingFrames >= FORGET_FRAMES)
			{
				Log("final pass shader %p not seen for %d frames; learning it again", (void*)g_finalPs, FORGET_FRAMES);
				g_finalPs = nullptr;
				g_candidateFrames = 0;
			}
		}

		if (count > 0)
		{
			g_windowMinPasses = g_windowFrames == 0 || count < g_windowMinPasses ? count : g_windowMinPasses;
			g_windowMaxPasses = count > g_windowMaxPasses ? count : g_windowMaxPasses;

			if (WantInject() && !g_frame.ran)
				g_windowSkipped++;
		}

		g_windowFrames++;

		ULONGLONG now = GetTickCount64();

		if (now - g_windowStart >= 1000)
		{
			if (g_windowSkipped > 0 && g_skipLogLines < 300)
			{
				g_skipLogLines++;
				Log("%u of %u frames went without shaders in the last second (fullscreen passes per frame %u..%u, final pass %s)", g_windowSkipped, g_windowFrames, g_windowMinPasses, g_windowMaxPasses, g_finalPs ? "learned" : "not learned");
			}

			g_lastSecondSkipped = g_windowSkipped;
			g_windowStart = now;
			g_windowFrames = 0;
			g_windowSkipped = 0;
			g_windowMinPasses = 0;
			g_windowMaxPasses = 0;
		}

		if (g_capture)
		{
			Capture("PRESENT frame=%llu backbuffer_draws=%u total_draws=%u fullscreen_passes=%u shaders_after_pass=%u injected=%d", (unsigned long long)g_frameNumber, g_frame.backbufferDraws, g_frame.totalDraws, g_frame.fullscreenDraws, g_targetFullscreenDraw, g_frame.injected ? 1 : 0);

			if (--g_captureFramesLeft <= 0)
			{
				fclose(g_capture);
				g_capture = nullptr;
				Log("capture finished");
			}
		}

		State::UpdateStatus(UpdateStatus, nullptr);

		int request = State::TakeCaptureRequest();

		if (request > 0 && !g_capture)
		{
			g_capture = fopen("mods/logs/pss_capture.txt", "w");
			g_captureFramesLeft = request;
			g_captureLines = 0;

			if (g_capture)
			{
				Log("capturing %d frames to mods/logs/pss_capture.txt", request);
				Capture("# Payday Screen Shaders draw capture. backbuffer=%p %ux%u inject_after=%.0f require_fullscreen=%.0f active=%d enabled=%.0f", (void*)g_backbuffer, g_backbufferW, g_backbufferH, g_settings.p[P_INJECT_AFTER], g_settings.p[P_REQUIRE_FULLSCREEN], g_settings.active ? 1 : 0, g_settings.p[P_ENABLED]);
			}
		}

		g_frameNumber++;
		g_frame = FrameState();
		g_settings = State::Snapshot();
		g_haveSettings = true;

		UINT chosen = (UINT)g_settings.p[P_INJECT_AFTER];
		g_targetFullscreenDraw = chosen > 0 ? chosen : g_stableFullscreenDraws;
	}

	HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swap, UINT sync, UINT flags)
	{
		if (t_inside)
			return oPresent(swap, sync, flags);

		OnPresent(swap);

		// DXVK may forward Present1 into Present; do not count that frame twice.
		t_inside = true;
		HRESULT hr = oPresent(swap, sync, flags);
		t_inside = false;

		return hr;
	}

	HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
	{
		if (t_inside)
			return oPresent1(swap, sync, flags, params);

		OnPresent(swap);

		t_inside = true;
		HRESULT hr = oPresent1(swap, sync, flags, params);
		t_inside = false;

		return hr;
	}

	LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		return DefWindowProcA(hwnd, msg, wp, lp);
	}
}

bool InstallHooks()
{
	if (g_presentHooked)
		return true;

	WNDCLASSEXA wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = DummyWndProc;
	wc.hInstance = GetModuleHandleA(nullptr);
	wc.lpszClassName = "PSS_DummyWindow";
	RegisterClassExA(&wc);

	HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferDesc.Width = 64;
	sd.BufferDesc.Height = 64;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 1;
	sd.OutputWindow = hwnd;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	IDXGISwapChain* swap = nullptr;
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* ctx = nullptr;

	HRESULT hr = hwnd ? D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &device, nullptr, &ctx) : E_FAIL;

	if (FAILED(hr))
	{
		Log("dummy device and swap chain failed (0x%08lx); trying without a swap chain", (unsigned long)hr);
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx);
	}

	if (FAILED(hr) || !ctx)
	{
		Log("could not create a D3D11 device to hook (0x%08lx)", (unsigned long)hr);

		if (hwnd)
			DestroyWindow(hwnd);

		UnregisterClassA(wc.lpszClassName, wc.hInstance);

		return false;
	}

	// Only the swap chain is hooked here; the game's own context is hooked on its first present
	// (HookGameContext), since contexts do not share one vtable.
	if (swap)
	{
		void** svt = *(void***)swap;
		oPresent = (PresentFn)Patch(svt, VT_SWAP_PRESENT, (void*)hkPresent);

		IDXGISwapChain1* swap1 = nullptr;

		if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&swap1)))
		{
			void** svt1 = *(void***)swap1;
			void* original = Patch(svt1, VT_SWAP1_PRESENT1, (void*)hkPresent1);

			if (original)
				oPresent1 = (Present1Fn)original;

			swap1->Release();
		}

		g_presentHooked = oPresent != nullptr;
		swap->Release();
	}

	ctx->Release();
	device->Release();

	if (hwnd)
		DestroyWindow(hwnd);

	UnregisterClassA(wc.lpszClassName, wc.hInstance);

	Log("present hooked: %d (slots draw=%d omset=%d present=%d present1=%d); the game's context is hooked on its first present", g_presentHooked ? 1 : 0, VT_CTX_DRAW, VT_CTX_OM_SET_RENDER_TARGETS, VT_SWAP_PRESENT, VT_SWAP1_PRESENT1);

	State::UpdateStatus(UpdateStatus, nullptr);

	return g_presentHooked;
}

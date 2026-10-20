// Offline test for Payday Screen Shaders' native side. Runs under Proton's wine with DXVK, i.e.
// the same d3d11/dxgi/d3dcompiler the game gets, so shader compatibility is tested for real.
//
//   pss_test.exe <path to pss.hlsl> <path to a .cube LUT>
//
// Exit code 0 = all passed.

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bridge.h"
#include "effects.h"
#include "hooks.h"
#include "log.h"
#include "state.h"

// plugin.cpp owns this in the DLL (the thread SuperBLT loaded it on); the test has no plugin.cpp.
DWORD g_luaThread = 0;

static int g_passed = 0;
static int g_failed = 0;
static const char* g_dirt = ""; // the lens dirt blob, when the runner passes one

static void Check(const char* name, bool ok, const std::string& detail = "")
{
	if (ok)
		g_passed++;
	else
		g_failed++;

	printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : " -- ", detail.c_str());
	fflush(stdout);
}

static Settings NeutralSettings(const char* shader, const char* lut)
{
	Settings s;

	for (int i = 0; i < PARAM_SLOTS; i++)
		s.p[i] = i < P_COUNT ? kParams[i].def : 0.0f;

	s.p[P_ENABLED] = 1.0f;
	s.shaderPath = shader;
	s.lutPath = lut;
	s.dirtPath = g_dirt;
	s.shaderGen = 1;
	s.lutGen = 1;
	s.dirtGen = 1;
	s.active = true;

	return s;
}

static ID3D11Texture2D* MakeTarget(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, std::vector<uint8_t>& pixels)
{
	pixels.resize((size_t)w * h * 4);

	for (UINT y = 0; y < h; y++)
	{
		for (UINT x = 0; x < w; x++)
		{
			uint8_t* p = &pixels[((size_t)y * w + x) * 4];
			// gradient, a hard diagonal edge, and a few bright highlights
			p[0] = (uint8_t)(x * 255 / (w - 1));
			p[1] = (uint8_t)(y * 255 / (h - 1));
			p[2] = (x + y) % 64 < 32 ? 40 : 200;
			p[3] = 255;

			if ((x / 40 + y / 40) % 7 == 0)
				p[0] = p[1] = p[2] = 250;
		}
	}

	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA data = {pixels.data(), w * 4, 0};
	ID3D11Texture2D* tex = nullptr;
	dev->CreateTexture2D(&d, &data, &tex);

	return tex;
}

// A target filled by a pixel function (x, y, rgba out).
static ID3D11Texture2D* MakePattern(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, std::vector<uint8_t>& pixels, const std::function<void(UINT, UINT, uint8_t*)>& fill)
{
	pixels.resize((size_t)w * h * 4);

	for (UINT y = 0; y < h; y++)
	{
		for (UINT x = 0; x < w; x++)
		{
			uint8_t* p = &pixels[((size_t)y * w + x) * 4];
			p[3] = 255;
			fill(x, y, p);
		}
	}

	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA data = {pixels.data(), w * 4, 0};
	ID3D11Texture2D* tex = nullptr;
	dev->CreateTexture2D(&d, &data, &tex);

	return tex;
}

static double Luma8(const uint8_t* p)
{
	return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
}

static std::vector<uint8_t> ReadBack(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex)
{
	D3D11_TEXTURE2D_DESC d;
	tex->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	d.MiscFlags = 0;

	ID3D11Texture2D* staging = nullptr;
	dev->CreateTexture2D(&d, nullptr, &staging);
	ctx->CopyResource(staging, tex);

	std::vector<uint8_t> out((size_t)d.Width * d.Height * 4);
	D3D11_MAPPED_SUBRESOURCE m;

	if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
	{
		for (UINT y = 0; y < d.Height; y++)
			memcpy(&out[(size_t)y * d.Width * 4], (uint8_t*)m.pData + y * m.RowPitch, d.Width * 4);

		ctx->Unmap(staging, 0);
	}

	staging->Release();

	return out;
}

static int MaxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, double* mean = nullptr)
{
	int worst = 0;
	double total = 0;

	for (size_t i = 0; i < a.size() && i < b.size(); i++)
	{
		if (i % 4 == 3)
			continue;

		int d = abs((int)a[i] - (int)b[i]);
		worst = d > worst ? d : worst;
		total += d;
	}

	if (mean)
		*mean = total / (a.size() * 0.75);

	return worst;
}

static void TestEffects(ID3D11Device* dev, ID3D11DeviceContext1* ctx, const char* shader, const char* lut, DXGI_FORMAT fmt, const char* label)
{
	const UINT W = 640, H = 360;
	std::vector<uint8_t> input;
	ID3D11Texture2D* target = MakeTarget(dev, W, H, fmt, input);
	ID3D11RenderTargetView* rtv = nullptr;
	dev->CreateRenderTargetView(target, nullptr, &rtv);

	Effects fx;
	Settings s = NeutralSettings(shader, lut);

	// A viewport and render target the "game" had bound, which must survive the run.
	std::vector<uint8_t> scratchPixels;
	ID3D11Texture2D* gameTex = MakeTarget(dev, 100, 50, DXGI_FORMAT_R8G8B8A8_UNORM, scratchPixels);
	ID3D11RenderTargetView* gameRtv = nullptr;
	dev->CreateRenderTargetView(gameTex, nullptr, &gameRtv);
	ctx->OMSetRenderTargets(1, &gameRtv, nullptr);
	D3D11_VIEWPORT gameVp = {3.0f, 4.0f, 97.0f, 46.0f, 0.0f, 1.0f};
	ctx->RSSetViewports(1, &gameVp);

	bool ran = fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, s);
	Check((std::string(label) + ": shaders compile with the game's d3dcompiler").c_str(), ran && fx.Compiled(), fx.Error());

	if (!ran)
	{
		rtv->Release();
		target->Release();
		gameRtv->Release();
		gameTex->Release();
		return;
	}

	ID3D11RenderTargetView* boundRtv = nullptr;
	ctx->OMGetRenderTargets(1, &boundRtv, nullptr);
	UINT vpCount = 1;
	D3D11_VIEWPORT boundVp = {};
	ctx->RSGetViewports(&vpCount, &boundVp);
	Check((std::string(label) + ": the game's render target is still bound afterwards").c_str(), boundRtv == gameRtv);
	Check((std::string(label) + ": the game's viewport is still set afterwards").c_str(), vpCount == 1 && boundVp.TopLeftX == 3.0f && boundVp.Width == 97.0f);

	if (boundRtv)
		boundRtv->Release();

	std::vector<uint8_t> neutral = ReadBack(dev, ctx, target);
	int neutralDiff = MaxDiff(input, neutral);
	Check((std::string(label) + ": neutral settings leave the image unchanged").c_str(), neutralDiff <= 1, "max channel diff " + std::to_string(neutralDiff));

	// Every effect on, several frames so adaptation and bloom settle.
	s.p[P_AA] = 1.0f;
	s.p[P_SHARPEN] = 0.6f;
	s.p[P_BLOOM] = 0.6f;
	s.p[P_ADAPT] = 0.6f;
	s.p[P_SHADOWS] = 0.3f;
	s.p[P_CONTRAST] = 0.2f;
	s.p[P_VIBRANCE] = 0.2f;
	s.p[P_LUT_MIX] = 0.8f;
	s.p[P_VIGNETTE] = 0.3f;
	s.p[P_GRAIN] = 0.2f;
	s.p[P_EXPOSURE] = 0.3f;
	s.p[P_TEMPERATURE] = 0.3f;
	s.p[P_TINT] = -0.2f;
	s.p[P_CLARITY] = 0.4f;
	s.p[P_DEBAND] = 0.5f;
	s.p[P_DITHER] = 1.0f;
	s.p[P_CHROMATIC] = 0.4f;
	s.p[P_FLARE] = 0.5f;
	s.p[P_COLOR_BLOOM] = 0.5f;
	s.p[P_CUES] = 0.5f;

	for (int frame = 0; frame < 5; frame++)
	{
		std::vector<uint8_t> fresh;
		ID3D11Texture2D* again = MakeTarget(dev, W, H, fmt, fresh);
		ctx->CopyResource(target, again);
		again->Release();
		fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, s);
	}

	std::vector<uint8_t> graded = ReadBack(dev, ctx, target);
	double mean = 0;
	int gradedDiff = MaxDiff(input, graded, &mean);
	Check((std::string(label) + ": all effects on changes the image").c_str(), gradedDiff > 8, "max diff " + std::to_string(gradedDiff));
	Check((std::string(label) + ": but does not destroy it").c_str(), mean < 60.0, "mean diff " + std::to_string(mean));

	uint64_t sum = 0;

	for (size_t i = 0; i < graded.size(); i += 4)
		sum += graded[i] + graded[i + 1] + graded[i + 2];

	double average = (double)sum / (graded.size() / 4 * 3);
	Check((std::string(label) + ": result is not black or blown out").c_str(), average > 30 && average < 225, "average " + std::to_string(average));

	// Split view: left half must be the untouched input.
	s.p[P_SPLIT] = 1.0f;
	{
		std::vector<uint8_t> fresh;
		ID3D11Texture2D* again = MakeTarget(dev, W, H, fmt, fresh);
		ctx->CopyResource(target, again);
		again->Release();
	}
	fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, s);
	std::vector<uint8_t> split = ReadBack(dev, ctx, target);
	int leftDiff = 0;

	for (UINT y = 0; y < H; y++)
	{
		for (UINT x = 0; x < W / 2 - 2; x++)
		{
			for (int c = 0; c < 3; c++)
			{
				size_t i = ((size_t)y * W + x) * 4 + c;
				int d = abs((int)split[i] - (int)input[i]);
				leftDiff = d > leftDiff ? d : leftDiff;
			}
		}
	}

	Check((std::string(label) + ": split view shows the original on the left").c_str(), leftDiff <= 1, "max diff " + std::to_string(leftDiff));

	// ---- the three shape and depth effects, each on its own ----
	auto runPattern = [&](const Settings& st, const std::function<void(UINT, UINT, uint8_t*)>& fill, std::vector<uint8_t>& in) {
		ID3D11Texture2D* pattern = MakePattern(dev, W, H, fmt, in, fill);
		ctx->CopyResource(target, pattern);
		pattern->Release();
		fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, st);
		return ReadBack(dev, ctx, target);
	};
	auto at = [&](std::vector<uint8_t>& img, UINT x, UINT y) { return &img[((size_t)y * W + x) * 4]; };
	std::string L(label);

	// Bloom warmth (0.11, halation folded in): a white highlight on a mid grey wall glows warm with
	// the warmth up, neutral with it down, and the far corner is untouched either way.
	{
		auto lamp = [](UINT x, UINT y, uint8_t* p) {
			p[0] = p[1] = p[2] = 60;
			if (x >= 280 && x < 360 && y >= 140 && y < 220) p[0] = p[1] = p[2] = 255;
		};
		Settings warm = NeutralSettings(shader, lut);
		warm.p[P_BLOOM] = 1.0f;
		warm.p[P_BLOOM_THRESHOLD] = 0.6f;
		warm.p[P_BLOOM_TINT] = 1.0f;
		Settings neutral = warm;
		neutral.p[P_BLOOM_TINT] = 0.0f;
		std::vector<uint8_t> in, in2;
		std::vector<uint8_t> warmOut = runPattern(warm, lamp, in);
		std::vector<uint8_t> neutralOut = runPattern(neutral, lamp, in2);
		const uint8_t* nearWarm = at(warmOut, 375, 180);
		const uint8_t* nearNeutral = at(neutralOut, 375, 180);
		double warmth = (double)nearWarm[0] - (double)nearWarm[2];
		double neutrality = fabs((double)nearNeutral[0] - (double)nearNeutral[2]);
		double farShift = Luma8(at(warmOut, 40, 330)) - Luma8(at(in, 40, 330));

		Check((L + ": bloom warmth bleeds warm around a bright highlight").c_str(), warmth > 2.0, "red minus blue " + std::to_string(warmth));
		Check((L + ": without warmth the bloom is neutral").c_str(), neutrality <= 1.5, "red minus blue " + std::to_string(neutrality));
		Check((L + ": the bloom stays near the highlight").c_str(), fabs(farShift) < 3.0, "far corner shift " + std::to_string(farShift));
	}

	// Colour bloom: grey wall with a saturated red panel and a white light.
	{
		Settings c = NeutralSettings(shader, lut);
		c.p[P_COLOR_BLOOM] = 1.0f;
		c.p[P_COLOR_BLOOM_RADIUS] = 1.0f;
		std::vector<uint8_t> in;
		auto wall = [](UINT x, UINT y, uint8_t* p) {
			p[0] = p[1] = p[2] = 128;
			if (x >= 100 && x < 200 && y >= 130 && y < 230) { p[0] = 220; p[1] = 25; p[2] = 25; }
			if (x >= 440 && x < 540 && y >= 130 && y < 230) { p[0] = p[1] = p[2] = 250; }
		};
		std::vector<uint8_t> out = runPattern(c, wall, in);
		const uint8_t* nearRed = at(out, 215, 180);
		const uint8_t* nearWhite = at(out, 555, 180);
		double redShift = (double)nearRed[0] - (double)nearRed[2];
		int whiteTint = std::max({abs((int)nearWhite[0] - (int)nearWhite[1]), abs((int)nearWhite[1] - (int)nearWhite[2]), abs((int)nearWhite[0] - (int)nearWhite[2])});
		double sumIn = 0, sumOut = 0;

		for (UINT y = 0; y < H; y++)
			for (UINT x = 0; x < W; x++)
			{
				sumIn += Luma8(at(in, x, y));
				sumOut += Luma8(at(out, x, y));
			}

		double meanShift = fabs(sumOut - sumIn) / (W * H);

		Check((L + ": colour bloom spreads a strong colour onto its surroundings").c_str(), redShift > 2.0, "red minus blue next to the panel " + std::to_string(redShift));
		Check((L + ": colour bloom does not spread white light").c_str(), whiteTint <= 1, "tint next to the light " + std::to_string(whiteTint));
		Check((L + ": colour bloom keeps overall brightness").c_str(), meanShift < 1.5, "mean luma shift " + std::to_string(meanShift));
	}

	// Monocular cues on the standard test image.
	{
		Settings m = NeutralSettings(shader, lut);
		m.p[P_CUES] = 1.0f;
		std::vector<uint8_t> in;
		std::vector<uint8_t> out = runPattern(m, [](UINT x, UINT y, uint8_t* p) {
			p[0] = (uint8_t)(x * 255 / 639);
			p[1] = (uint8_t)(y * 255 / 359);
			p[2] = (x + y) % 64 < 32 ? 40 : 200;
			if ((x / 40 + y / 40) % 7 == 0) p[0] = p[1] = p[2] = 250;
		}, in);
		double meanDiff = 0;
		int maxDiff = MaxDiff(in, out, &meanDiff);

		Check((L + ": monocular cues change the image").c_str(), maxDiff > 2, "max diff " + std::to_string(maxDiff));
		Check((L + ": monocular cues stay subtle").c_str(), meanDiff < 12.0, "mean diff " + std::to_string(meanDiff));
	}

	// Lens dirt: a dark room with one bright lamp on the left. The dirt must light up on the side
	// away from the lamp, never darken anything, and do nothing at all with no light in frame.
	if (g_dirt && *g_dirt)
	{
		Settings d = NeutralSettings(shader, lut);
		d.p[P_DIRT] = 1.0f;
		d.p[P_DIRT_OFFSET] = 0.5f;
		std::vector<uint8_t> in;
		auto lampLeft = [](UINT x, UINT y, uint8_t* p) {
			uint8_t v = 30;
			if (x >= 40 && x < 140 && y >= 130 && y < 230) v = 255;
			p[0] = p[1] = p[2] = v;
		};
		std::vector<uint8_t> out = runPattern(d, lampLeft, in);
		double leftGain = 0, rightGain = 0, darkest = 0;

		for (UINT y = 0; y < H; y++)
			for (UINT x = 0; x < W; x++)
			{
				double gain = Luma8(at(out, x, y)) - Luma8(at(in, x, y));
				darkest = std::min(darkest, gain);

				if (x > 160 && x < W / 2) leftGain = std::max(leftGain, gain);
				if (x > W / 2 + 40) rightGain = std::max(rightGain, gain);
			}

		Check((L + ": lens dirt lights up opposite the light").c_str(), rightGain > leftGain + 0.5, "left " + std::to_string(leftGain) + " right " + std::to_string(rightGain));
		Check((L + ": lens dirt only ever adds light").c_str(), darkest >= -1.5, "darkest " + std::to_string(darkest));

		// The same settings on a frame with nothing bright in it: no light, so no dirt.
		std::vector<uint8_t> darkIn;
		std::vector<uint8_t> darkOut = runPattern(d, [](UINT x, UINT y, uint8_t* p) { p[0] = p[1] = p[2] = (uint8_t)(18 + (x / 80) * 3); }, darkIn);
		int darkDiff = MaxDiff(darkIn, darkOut);
		Check((L + ": lens dirt stays invisible with no bright light in frame").c_str(), darkDiff <= 1, "max diff " + std::to_string(darkDiff));
	}

	// Lens flare (0.11): a bright lamp off centre throws a ghost on the far side of the centre
	// (the bloom read mirrored through the middle at 1.15x lands it at 0.5 + (lamp - 0.5) / -1.15),
	// and with nothing bright in frame there is no flare at all.
	{
		Settings fl = NeutralSettings(shader, lut);
		fl.p[P_BLOOM] = 1.0f;
		fl.p[P_BLOOM_THRESHOLD] = 0.6f;
		fl.p[P_FLARE] = 1.0f;
		fl.p[P_FLARE_LENGTH] = 1.0f;
		Settings noFlare = fl;
		noFlare.p[P_FLARE] = 0.0f;
		auto lamp = [](UINT x, UINT y, uint8_t* p) {
			p[0] = p[1] = p[2] = 60;
			if (x >= 432 && x < 592 && y >= 70 && y < 170) p[0] = p[1] = p[2] = 255; // centre (0.8, 0.33)
		};
		std::vector<uint8_t> in, in2, in3;
		std::vector<uint8_t> out = runPattern(fl, lamp, in);
		std::vector<uint8_t> plain = runPattern(noFlare, lamp, in2);
		double ghost = Luma8(at(out, 153, 234)) - Luma8(at(plain, 153, 234));
		// The bottom right corner is outside every ghost of this lamp (the nearest ghost covers the
		// bottom LEFT quarter) and off the streak's rows.
		double farCorner = Luma8(at(out, 600, 340)) - Luma8(at(plain, 600, 340));
		std::vector<uint8_t> darkOut = runPattern(fl, [](UINT x, UINT y, uint8_t* p) { p[0] = p[1] = p[2] = (uint8_t)(18 + (x / 80) * 3); }, in3);
		int darkDiff = MaxDiff(in3, darkOut);

		Check((L + ": the lens flare throws a ghost across the centre").c_str(), ghost > 2.0, "ghost gain " + std::to_string(ghost));
		Check((L + ": the flare leaves a corner clear of its ghosts alone").c_str(), fabs(farCorner) < 2.0, "corner " + std::to_string(farCorner));
		Check((L + ": no bright light, no flare").c_str(), darkDiff <= 1, "max diff " + std::to_string(darkDiff));
	}

	// Panini and spherical: the frame is reshaped, but no empty or smeared border appears and the
	// centre keeps its content.
	for (int mode = 0; mode < 2; mode++)
	{
		Settings pr = NeutralSettings(shader, lut);
		pr.p[mode == 0 ? P_PANINI : P_SPHERICAL] = 1.0f;
		std::vector<uint8_t> in;
		auto grid = [](UINT x, UINT y, uint8_t* p) {
			uint8_t v = (x % 40 < 3 || y % 40 < 3) ? 230 : 45;
			p[0] = p[1] = p[2] = v;
		};
		std::vector<uint8_t> out = runPattern(pr, grid, in);
		int changed = MaxDiff(in, out);
		double darkestEdge = 255;

		for (UINT x = 0; x < W; x++)
		{
			darkestEdge = std::min(darkestEdge, Luma8(at(out, x, 0)));
			darkestEdge = std::min(darkestEdge, Luma8(at(out, x, H - 1)));
		}

		double centre = Luma8(at(out, W / 2, H / 2));
		std::string name = mode == 0 ? ": panini projection" : ": spherical projection";

		Check((L + name + " reshapes the frame").c_str(), changed > 8, "max diff " + std::to_string(changed));
		Check((L + name + " leaves no empty border").c_str(), darkestEdge > 20.0, "darkest edge " + std::to_string(darkestEdge));
		Check((L + name + " keeps the middle of the picture").c_str(), centre > 20.0, "centre " + std::to_string(centre));
	}

	// FSR 1.0. A hard diagonal edge and a field of fine detail: the upscale must change the picture,
	// must not ring outside the values that were actually drawn, must sharpen only through RCAS (the
	// CAS pass is bypassed while it is on), and must still work under a projection.
	{
		auto edges = [](UINT x, UINT y, uint8_t* p) {
			uint8_t v = ((x + y * 2) % 96) < 48 ? 40 : 210;

			if (y > 240 && ((x / 3 + y / 3) % 2))
				v = 120;

			p[0] = p[1] = p[2] = v;
		};

		// Sum of neighbour differences: how much local contrast the image carries.
		auto energy = [&](std::vector<uint8_t>& img) {
			double total = 0;

			for (UINT y = 1; y < H - 1; y++)
				for (UINT x = 1; x < W - 1; x++)
					total += fabs(Luma8(at(img, x, y)) - Luma8(at(img, x + 1, y))) + fabs(Luma8(at(img, x, y)) - Luma8(at(img, x, y + 1)));

			return total / ((W - 2) * (H - 2));
		};

		Settings u = NeutralSettings(shader, lut);
		std::vector<uint8_t> in;
		std::vector<uint8_t> plain = runPattern(u, edges, in);

		u.p[P_UPSCALE] = 1.0f;
		u.p[P_UPSCALE_SCALE] = 2.0f;
		u.p[P_UPSCALE_SHARPNESS] = 0.0f;
		std::vector<uint8_t> resolved = runPattern(u, edges, in);

		double lowest = 255, highest = 0, meanDiff = 0;
		MaxDiff(plain, resolved, &meanDiff);

		for (UINT y = 0; y < H; y++)
			for (UINT x = 0; x < W; x++)
			{
				double l = Luma8(at(resolved, x, y));
				lowest = std::min(lowest, l);
				highest = std::max(highest, l);
			}

		Check((L + ": FSR upscaling changes the picture").c_str(), meanDiff > 0.5, "mean diff " + std::to_string(meanDiff));
		Check((L + ": FSR upscaling never rings past what was drawn").c_str(), lowest > 38.0 && highest < 212.0, "range " + std::to_string(lowest) + ".." + std::to_string(highest));

		// RCAS at the upscaled size raises local contrast; CAS on top of it must not.
		u.p[P_UPSCALE_SHARPNESS] = 1.0f;
		std::vector<uint8_t> sharpened = runPattern(u, edges, in);
		Check((L + ": upscale sharpness sharpens the upscaled frame").c_str(), energy(sharpened) > energy(resolved) + 0.05, "energy " + std::to_string(energy(resolved)) + " -> " + std::to_string(energy(sharpened)));

		u.p[P_SHARPEN] = 1.0f;
		std::vector<uint8_t> withCas = runPattern(u, edges, in);
		int casDiff = MaxDiff(sharpened, withCas);
		Check((L + ": the CAS pass is bypassed while FSR upscaling is on").c_str(), casDiff <= 1, "max diff " + std::to_string(casDiff));

		// Every scale factor allocates, runs and keeps a sane picture.
		for (float scale = 1.25f; scale <= 2.01f; scale += 0.25f)
		{
			Settings sc = NeutralSettings(shader, lut);
			sc.p[P_UPSCALE] = 1.0f;
			sc.p[P_UPSCALE_SCALE] = scale;
			sc.p[P_UPSCALE_SHARPNESS] = 0.5f;

			std::vector<uint8_t> scaled = runPattern(sc, edges, in);
			uint64_t sum = 0;

			for (size_t k = 0; k < scaled.size(); k += 4)
				sum += scaled[k] + scaled[k + 1] + scaled[k + 2];

			double avg = (double)sum / (scaled.size() / 4 * 3);
			char label[64];
			snprintf(label, sizeof(label), ": FSR at %.2fx keeps the picture", (double)scale);
			Check((L + label).c_str(), avg > 30 && avg < 225, "average " + std::to_string(avg));
		}

		// Under a projection the warp resamples the upscaled buffer: still no empty border.
		Settings pr = NeutralSettings(shader, lut);
		pr.p[P_UPSCALE] = 1.0f;
		pr.p[P_UPSCALE_SCALE] = 2.0f;
		pr.p[P_UPSCALE_SHARPNESS] = 0.5f;
		pr.p[P_PANINI] = 1.0f;

		std::vector<uint8_t> projected = runPattern(pr, edges, in);
		double darkestEdge = 255;

		for (UINT x = 0; x < W; x++)
		{
			darkestEdge = std::min(darkestEdge, Luma8(at(projected, x, 0)));
			darkestEdge = std::min(darkestEdge, Luma8(at(projected, x, H - 1)));
		}

		Check((L + ": a projection over the upscaled buffer leaves no empty border").c_str(), darkestEdge > 20.0, "darkest edge " + std::to_string(darkestEdge));

		// And switching it off is an exact passthrough again, buffers released.
		Settings off = NeutralSettings(shader, lut);
		std::vector<uint8_t> back = runPattern(off, edges, in);
		int offDiff = MaxDiff(in, back);
		Check((L + ": switching FSR upscaling off restores the passthrough").c_str(), offDiff <= 1, "max diff " + std::to_string(offDiff));
	}

	// Each effect off again: back to an exact passthrough.
	{
		Settings n = NeutralSettings(shader, lut);
		std::vector<uint8_t> in;
		std::vector<uint8_t> out = runPattern(n, [](UINT x, UINT y, uint8_t* p) { p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = 90; }, in);
		int d = MaxDiff(in, out);
		Check((L + ": switching the new effects off restores the passthrough").c_str(), d <= 1, "max diff " + std::to_string(d));
	}

	fx.Release();
	rtv->Release();
	target->Release();
	gameRtv->Release();
	gameTex->Release();
}

// ------------------------------------------------------------------------------------------
// What FSR upscaling costs, at a real screen size. Printed, never asserted: it is this machine's
// GPU under DXVK, not the player's, so it is only ever the shape of the curve.

static void ReportUpscaleCost(ID3D11Device* dev, ID3D11DeviceContext1* ctx, const char* shader, const char* lut)
{
	const UINT W = 1920, H = 1080;
	std::vector<uint8_t> pixels;
	ID3D11Texture2D* target = MakeTarget(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, pixels);
	ID3D11RenderTargetView* rtv = nullptr;
	dev->CreateRenderTargetView(target, nullptr, &rtv);

	Effects fx;
	const float scales[4] = {0.0f, 1.25f, 1.5f, 2.0f};
	double baseline = 0;

	printf("  FSR cost at %ux%u (this machine, DXVK; whole chain, 40 frames):\n", W, H);

	for (int i = 0; i < 4; i++)
	{
		Settings s = NeutralSettings(shader, lut);
		s.p[P_AA] = 1.0f;
		s.p[P_BLOOM] = 0.4f;
		s.p[P_UPSCALE] = scales[i] > 0.0f ? 1.0f : 0.0f;
		s.p[P_UPSCALE_SCALE] = scales[i] > 0.0f ? scales[i] : 1.5f;
		s.p[P_UPSCALE_SHARPNESS] = 0.5f;

		for (int warm = 0; warm < 5; warm++)
			fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, s);

		ReadBack(dev, ctx, target);

		LARGE_INTEGER a, b, freq;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&a);

		for (int frame = 0; frame < 40; frame++)
			fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, s);

		ReadBack(dev, ctx, target); // forces the queued work to finish before we stop the clock
		QueryPerformanceCounter(&b);

		double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart / 40.0;

		if (i == 0)
		{
			baseline = ms;
			printf("    off    %.3f ms/frame\n", ms);
		}
		else
		{
			printf("    %.2fx  %.3f ms/frame  (+%.3f ms, %.0f MB of targets)\n", (double)scales[i], ms, ms - baseline,
				(double)W * H * scales[i] * scales[i] * 4.0 * 2.0 / (1024.0 * 1024.0));
		}
	}

	fx.Release();
	rtv->Release();
	target->Release();
}

// What SMAA and depth of field cost, next to FXAA and nothing, at 1080p on this machine. Printed,
// not asserted: timings belong to the machine, but the numbers go in the README.
static void ReportAaAndDofCost(ID3D11Device* dev, ID3D11DeviceContext1* ctx, const char* shader, const char* lut)
{
	const UINT W = 1920, H = 1080;
	std::vector<uint8_t> pixels;
	ID3D11Texture2D* target = MakeTarget(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, pixels);
	ID3D11RenderTargetView* rtv = nullptr;
	dev->CreateRenderTargetView(target, nullptr, &rtv);

	std::vector<float> ramp((size_t)W * H);

	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
			ramp[(size_t)y * W + x] = 200.0f + (float)x / (float)(W - 1) * 12000.0f;

	D3D11_TEXTURE2D_DESC dd = {};
	dd.Width = W;
	dd.Height = H;
	dd.MipLevels = 1;
	dd.ArraySize = 1;
	dd.Format = DXGI_FORMAT_R32_FLOAT;
	dd.SampleDesc.Count = 1;
	dd.Usage = D3D11_USAGE_DEFAULT;
	dd.BindFlags = D3D11_BIND_RENDER_TARGET;
	D3D11_SUBRESOURCE_DATA data = {ramp.data(), W * 4, 0};
	ID3D11Texture2D* depth = nullptr;
	dev->CreateTexture2D(&dd, &data, &depth);

	struct Case
	{
		const char* name;
		float aa;
		float aaMode;
		float dof;
	};

	const Case cases[] = {
		{"nothing           ", 0.0f, 0.0f, 0.0f},
		{"FXAA              ", 1.0f, 0.0f, 0.0f},
		{"SMAA              ", 1.0f, 1.0f, 0.0f},
		{"DoF, default size ", 0.0f, 0.0f, 0.4f},
		{"DoF, largest size ", 0.0f, 0.0f, 1.0f},
	};

	Effects fx;
	double baseline = 0.0;

	printf("  SMAA and depth of field cost at %ux%u (this machine, DXVK; 40 frames):\n", W, H);

	for (const Case& c : cases)
	{
		Settings s = NeutralSettings(shader, lut);
		s.p[P_AA] = c.aa;
		s.p[P_AA_MODE] = c.aaMode;
		s.p[P_DOF] = c.dof;
		s.p[P_DEPTH_MODE] = 1.0f;

		for (int warm = 0; warm < 5; warm++)
			fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, s, depth);

		ReadBack(dev, ctx, target);

		LARGE_INTEGER a, b, freq;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&a);

		for (int frame = 0; frame < 40; frame++)
			fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, s, depth);

		ReadBack(dev, ctx, target);
		QueryPerformanceCounter(&b);

		double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart / 40.0;

		if (baseline == 0.0)
			baseline = ms;

		printf("    %s %.3f ms/frame  (+%.3f ms)\n", c.name, ms, ms - baseline);
	}

	fx.Release();
	depth->Release();
	rtv->Release();
	target->Release();
}

// ------------------------------------------------------------------------------------------
// Lighting, motion and atmosphere: one scene each, built so the property that matters can be
// measured, and the effect switched off in the baseline.

static ID3D11Texture2D* MakeDepth(ID3D11Device* dev, UINT w, UINT h, const std::function<float(UINT, UINT)>& depthAt)
{
	std::vector<float> d((size_t)w * h);

	for (UINT y = 0; y < h; y++)
		for (UINT x = 0; x < w; x++)
			d[(size_t)y * w + x] = depthAt(x, y);

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	D3D11_SUBRESOURCE_DATA data = {d.data(), w * 4, 0};
	ID3D11Texture2D* tex = nullptr;
	dev->CreateTexture2D(&desc, &data, &tex);

	return tex;
}

static void WithCamera(Settings& s, UINT w, UINT h, float pitch = 0.0f)
{
	s.p[P_CAMERA_TAN_X] = 0.7673f;
	s.p[P_CAMERA_TAN_Y] = 0.7673f * (float)h / (float)w;
	s.p[P_CAMERA_PITCH] = pitch;
	s.p[P_CAMERA_ROLL] = 0.0f;
	s.p[P_CAMERA_HEIGHT] = 0.0f;
	s.p[P_CAMERA_VALID] = 1.0f;
	s.p[P_DEPTH_MODE] = 1.0f;
	s.p[P_DEPTH_FAR] = 20000.0f;
}

static float HalfToFloat(uint16_t v)
{
	int sign = (v >> 15) & 1;
	int exponent = (v >> 10) & 0x1f;
	int mantissa = v & 0x3ff;
	float value = exponent == 0 ? ldexp((float)mantissa, -24) : (exponent == 31 ? 1e30f : ldexp((float)(mantissa | 0x400), exponent - 25));

	return sign ? -value : value;
}

// Reads an RGBA16F texture back as floats, four per texel.
static std::vector<float> ReadBackHalf(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT& w, UINT& h)
{
	D3D11_TEXTURE2D_DESC d;
	tex->GetDesc(&d);
	w = d.Width;
	h = d.Height;
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	d.MiscFlags = 0;
	d.MipLevels = 1;

	ID3D11Texture2D* staging = nullptr;
	dev->CreateTexture2D(&d, nullptr, &staging);
	ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);

	std::vector<float> out((size_t)w * h * 4);
	D3D11_MAPPED_SUBRESOURCE m;

	if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
	{
		for (UINT y = 0; y < h; y++)
		{
			const uint16_t* row = (const uint16_t*)((uint8_t*)m.pData + y * m.RowPitch);

			for (UINT i = 0; i < w * 4; i++)
				out[(size_t)y * w * 4 + i] = HalfToFloat(row[i]);
		}

		ctx->Unmap(staging, 0);
	}

	staging->Release();

	return out;
}

static double RegionMean(const std::vector<uint8_t>& px, UINT w, UINT x0, UINT x1, UINT y0, UINT y1, int channel)
{
	double total = 0.0;

	for (UINT y = y0; y < y1; y++)
		for (UINT x = x0; x < x1; x++)
			total += channel < 0 ? Luma8(&px[((size_t)y * w + x) * 4]) : px[((size_t)y * w + x) * 4 + channel];

	return total / ((double)(x1 - x0) * (double)(y1 - y0));
}

static double MeanDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
	double mean = 0.0;
	MaxDiff(a, b, &mean);
	return mean;
}

static void TestAdvanced(ID3D11Device* dev, ID3D11DeviceContext1* ctx, const char* shader, const char* lut)
{
	const UINT W = 640, H = 360;
	using Fill = std::function<void(UINT, UINT, uint8_t*)>;

	// Renders one frame (or several) of a scene with the given settings on its own Effects.
	auto render = [&](Effects& fx, const Fill& fill, ID3D11Texture2D* depth, const Settings& s, std::vector<uint8_t>& input) {
		ID3D11Texture2D* t = MakePattern(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, input, fill);
		ID3D11RenderTargetView* r = nullptr;
		dev->CreateRenderTargetView(t, nullptr, &r);
		fx.Run(dev, ctx, t, r, DXGI_FORMAT_UNKNOWN, s, depth);
		std::vector<uint8_t> out = ReadBack(dev, ctx, t);
		r->Release();
		t->Release();
		return out;
	};

	std::vector<uint8_t> input;
	Fill grey = [](UINT, UINT, uint8_t* p) { p[0] = p[1] = p[2] = 128; };

	// ---- GTAO: a flat wall and a tilted wall get no occlusion at all; a step gets it beside the step ----
	{
		ID3D11Texture2D* flat = MakeDepth(dev, W, H, [](UINT, UINT) { return 1000.0f; });
		ID3D11Texture2D* tilted = MakeDepth(dev, W, H, [&](UINT x, UINT) { return 1.0f / (1.0f / 700.0f + (float)x / (float)W * (1.0f / 1400.0f - 1.0f / 700.0f)); });
		ID3D11Texture2D* step = MakeDepth(dev, W, H, [&](UINT x, UINT) { return x < W / 2 ? 900.0f : 1000.0f; });

		Settings s = NeutralSettings(shader, lut);
		WithCamera(s, W, H);
		s.p[P_AO] = 1.0f;
		s.p[P_AO_RADIUS] = 300.0f;
		s.p[P_TEMPORAL] = 0.0f;

		Effects a, b, c;
		std::vector<uint8_t> onFlat = render(a, grey, flat, s, input);
		double flatChange = MeanDiff(input, onFlat);
		Check("gtao: a flat wall gets no occlusion", flatChange < 0.5, "mean change " + std::to_string(flatChange));

		std::vector<uint8_t> onTilted = render(b, grey, tilted, s, input);
		double tiltedChange = MeanDiff(input, onTilted);
		Check("gtao: a tilted wall gets none either", tiltedChange < 1.5, "mean change " + std::to_string(tiltedChange));

		std::vector<uint8_t> onStep = render(c, grey, step, s, input);
		double beside = RegionMean(input, W, W / 2 + 2, W / 2 + 14, 100, 260, -1) - RegionMean(onStep, W, W / 2 + 2, W / 2 + 14, 100, 260, -1);
		double away = RegionMean(input, W, W - 60, W - 20, 100, 260, -1) - RegionMean(onStep, W, W - 60, W - 20, 100, 260, -1);
		Check("gtao: the surface beside a step is occluded", beside > 3.0, "darkened by " + std::to_string(beside));
		Check("gtao: the same surface far from the step is not", away < 1.5 && away > -1.5, "changed by " + std::to_string(away));

		flat->Release();
		tilted->Release();
		step->Release();
	}

	// ---- indirect light and ray traced lighting: a red wall in front of a grey one tints it ----
	{
		ID3D11Texture2D* step = MakeDepth(dev, W, H, [&](UINT x, UINT) { return x < W / 2 ? 900.0f : 1000.0f; });
		Fill redGrey = [&](UINT x, UINT, uint8_t* p) {
			p[0] = x < W / 2 ? 255 : 150;
			p[1] = x < W / 2 ? 20 : 150;
			p[2] = x < W / 2 ? 20 : 150;
		};

		auto redness = [&](const std::vector<uint8_t>& px, UINT x0, UINT x1) {
			return RegionMean(px, W, x0, x1, 100, 260, 0) - RegionMean(px, W, x0, x1, 100, 260, 1);
		};

		Settings il = NeutralSettings(shader, lut);
		WithCamera(il, W, H);
		il.p[P_IL] = 1.0f;
		il.p[P_IL_RADIUS] = 400.0f;
		il.p[P_TEMPORAL] = 0.0f;
		Effects fx;
		std::vector<uint8_t> lit = render(fx, redGrey, step, il, input);
		double besideWall = redness(lit, W / 2 + 2, W / 2 + 20);
		double awayFromWall = redness(lit, W - 60, W - 20);
		Check("indirect light: red bounces onto the grey wall beside it", besideWall > awayFromWall + 2.0, "beside " + std::to_string(besideWall) + " away " + std::to_string(awayFromWall));

		Settings rt = NeutralSettings(shader, lut);
		WithCamera(rt, W, H);
		rt.p[P_RTGI] = 1.0f;
		rt.p[P_RTGI_DISTANCE] = 600.0f;
		rt.p[P_TEMPORAL] = 1.0f;

		Effects lens;
		std::vector<std::vector<uint8_t>> frames;

		for (int frame = 0; frame < 24; frame++)
			frames.push_back(render(lens, redGrey, step, rt, input));

		double early = MeanDiff(frames[1], frames[2]);
		double late = MeanDiff(frames[22], frames[23]);
		Check("ray traced lighting: temporal accumulation settles the noise", late < early * 0.6, "frame to frame change early " + std::to_string(early) + " late " + std::to_string(late));

		double rtNear = redness(frames[23], W / 2 + 2, W / 2 + 20);
		double rtFar = redness(frames[23], W - 60, W - 20);
		Check("ray traced lighting: rays find the red wall", rtNear > rtFar + 1.0, "near " + std::to_string(rtNear) + " far " + std::to_string(rtFar));

		step->Release();
	}

	// ---- reflections: a dark floor reflects the bright wall at the end of the room ----
	{
		const float tanY = 0.7673f * (float)H / (float)W;
		const float wall = 2000.0f;
		auto floorDepth = [&](UINT y) {
			float ndc = 1.0f - 2.0f * ((float)y + 0.5f) / (float)H;
			return ndc < 0.0f ? std::min(150.0f / (-ndc * tanY), wall) : wall;
		};

		UINT floorStart = H;

		for (UINT y = 0; y < H; y++)
		{
			if (floorDepth(y) < wall)
			{
				floorStart = y;
				break;
			}
		}

		ID3D11Texture2D* room = MakeDepth(dev, W, H, [&](UINT, UINT y) { return floorDepth(y); });
		Fill roomColour = [&](UINT, UINT y, uint8_t* p) {
			bool isWall = floorDepth(y) >= wall;
			p[0] = isWall ? 255 : 30;
			p[1] = isWall ? 140 : 30;
			p[2] = isWall ? 0 : 30;
		};

		Settings off = NeutralSettings(shader, lut);
		WithCamera(off, W, H);
		Settings on = off;
		on.p[P_SSR] = 1.0f;
		on.p[P_SSR_BLUR] = 0.0f;
		on.p[P_TEMPORAL] = 0.0f;

		Effects a, b;
		std::vector<uint8_t> plain = render(a, roomColour, room, off, input);
		std::vector<uint8_t> shiny = render(b, roomColour, room, on, input);
		double before = RegionMean(plain, W, 20, W - 20, floorStart + 4, std::min(floorStart + 40, H - 1), 0);
		double after = RegionMean(shiny, W, 20, W - 20, floorStart + 4, std::min(floorStart + 40, H - 1), 0);
		Check("reflections: the floor picks up the wall", after > before + 5.0, "floor red before " + std::to_string(before) + " after " + std::to_string(after));

		// The same room with a busy, carpet-like floor of the same average colour: it should reflect
		// far less, which is what keeps every floor from looking wet.
		Fill busyRoom = [&](UINT x, UINT y, uint8_t* p) {
			bool isWall = floorDepth(y) >= wall;
			uint8_t floorValue = ((x / 2 + y / 2) % 2) ? 10 : 50;
			p[0] = isWall ? 255 : floorValue;
			p[1] = isWall ? 140 : floorValue;
			p[2] = isWall ? 0 : floorValue;
		};
		Effects plainBusy, shinyBusy;
		std::vector<uint8_t> busyPlain = render(plainBusy, busyRoom, room, off, input);
		std::vector<uint8_t> busyShiny = render(shinyBusy, busyRoom, room, on, input);
		double busyGain = RegionMean(busyShiny, W, 20, W - 20, floorStart + 4, std::min(floorStart + 40, H - 1), 0) - RegionMean(busyPlain, W, 20, W - 20, floorStart + 4, std::min(floorStart + 40, H - 1), 0);
		Check("reflections: a busy, carpet-like floor reflects far less than a smooth one", busyGain < (after - before) * 0.5,
			"busy +" + std::to_string(busyGain) + " smooth +" + std::to_string(after - before));

		// The same floor with ambient occlusion: a floor seen at a shallow angle has a normal almost
		// exactly across the view axis, which once flipped the normal and would have darkened (and
		// reflected into) every floor. Near the camera, well away from the wall, it must be untouched.
		Settings occluded = off;
		occluded.p[P_AO] = 1.0f;
		occluded.p[P_AO_RADIUS] = 100.0f;
		occluded.p[P_TEMPORAL] = 0.0f;
		Effects c;
		std::vector<uint8_t> floorAo = render(c, roomColour, room, occluded, input);
		double floorChange = RegionMean(plain, W, 20, W - 20, H - 60, H - 20, -1) - RegionMean(floorAo, W, 20, W - 20, H - 60, H - 20, -1);
		Check("gtao: a floor seen at a shallow angle gets no occlusion", floorChange < 1.0 && floorChange > -1.0, "changed by " + std::to_string(floorChange));

		room->Release();
	}

	// ---- motion: a known shift is found; motion blur leaves a still scene alone ----
	{
		ID3D11Texture2D* depth = MakeDepth(dev, W, H, [](UINT, UINT) { return 1000.0f; });
		auto pattern = [&](int dx, int dy) {
			return Fill([=](UINT xi, UINT yi, uint8_t* p) {
				int x = (int)xi - dx;
				int y = (int)yi - dy;
				x = x < 0 ? 0 : x;
				y = y < 0 ? 0 : y;
				p[0] = (uint8_t)(x * 255 / (W - 1));
				p[1] = (uint8_t)(y * 255 / (H - 1));
				p[2] = (x + y) % 64 < 32 ? 40 : 200;

				if ((x / 40 + y / 40) % 7 == 0)
					p[0] = p[1] = p[2] = 250;
			});
		};

		Settings view = NeutralSettings(shader, lut);
		view.p[P_MOTION_VIEW] = 1.0f;
		Effects tracker;
		render(tracker, pattern(0, 0), depth, view, input);
		render(tracker, pattern(8, 4), depth, view, input);

		UINT mw = 0, mh = 0;
		std::vector<float> motion = ReadBackHalf(dev, ctx, tracker.MotionTexture(), mw, mh);
		std::vector<float> xs, ys;

		for (UINT y = mh / 4; y < mh * 3 / 4; y++)
		{
			for (UINT x = mw / 4; x < mw * 3 / 4; x++)
			{
				xs.push_back(motion[((size_t)y * mw + x) * 4] * W);
				ys.push_back(motion[((size_t)y * mw + x) * 4 + 1] * H);
			}
		}

		std::sort(xs.begin(), xs.end());
		std::sort(ys.begin(), ys.end());
		float mx = xs.empty() ? 99.0f : xs[xs.size() / 2];
		float my = ys.empty() ? 99.0f : ys[ys.size() / 2];
		Check("motion: an 8 by 4 pixel shift is found", fabs(mx + 8.0f) < 1.5f && fabs(my + 4.0f) < 1.5f,
			"median " + std::to_string(mx) + ", " + std::to_string(my) + " px (expected -8, -4)");

		Settings blur = NeutralSettings(shader, lut);
		blur.p[P_MBLUR] = 1.0f;
		Effects still;
		render(still, pattern(0, 0), depth, blur, input);
		std::vector<uint8_t> stillOut = render(still, pattern(0, 0), depth, blur, input);
		int stillDiff = MaxDiff(input, stillOut);
		Check("motion blur: a still scene is left alone", stillDiff <= 1, "max channel diff " + std::to_string(stillDiff));

		Effects moving;
		render(moving, pattern(0, 0), depth, blur, input);
		std::vector<uint8_t> movedOut = render(moving, pattern(8, 4), depth, blur, input);
		int movedDiff = MaxDiff(input, movedOut);
		Check("motion blur: a moving scene is blurred", movedDiff > 20, "max channel diff " + std::to_string(movedDiff));

		depth->Release();
	}

	// ---- light shafts: beams below a bright light, nothing in a far corner ----
	{
		ID3D11Texture2D* depth = MakeDepth(dev, W, H, [](UINT, UINT) { return 15000.0f; });
		Fill lamp = [&](UINT x, UINT y, uint8_t* p) {
			float dx = (float)x - W / 2.0f;
			float dy = (float)y - 60.0f;
			uint8_t v = dx * dx + dy * dy < 30.0f * 30.0f ? 255 : 20;
			p[0] = p[1] = p[2] = v;
		};

		Settings off = NeutralSettings(shader, lut);
		WithCamera(off, W, H);
		Settings on = off;
		on.p[P_SHAFTS] = 1.0f;
		on.p[P_SHAFTS_LENGTH] = 0.5f;

		Effects a, b;
		std::vector<uint8_t> dark = render(a, lamp, depth, off, input);
		std::vector<uint8_t> beams = render(b, lamp, depth, on, input);
		double below = RegionMean(beams, W, W / 2 - 10, W / 2 + 10, 100, 150, -1) - RegionMean(dark, W, W / 2 - 10, W / 2 + 10, 100, 150, -1);
		double corner = RegionMean(beams, W, 10, 60, H - 60, H - 10, -1) - RegionMean(dark, W, 10, 60, H - 60, H - 10, -1);
		Check("light shafts: beams reach below the light", below > 3.0, "brightened by " + std::to_string(below));
		Check("light shafts: a far corner stays dark", corner < 2.0, "brightened by " + std::to_string(corner));

		depth->Release();
	}

	// ---- height fog: thicker looking down than up, and nothing without a camera ----
	{
		ID3D11Texture2D* depth = MakeDepth(dev, W, H, [](UINT, UINT) { return 3000.0f; });
		Settings fog = NeutralSettings(shader, lut);
		WithCamera(fog, W, H);
		fog.p[P_HFOG] = 1.0f;
		fog.p[P_HFOG_HEIGHT] = 200.0f;

		Effects a;
		std::vector<uint8_t> out = render(a, grey, depth, fog, input);
		double up = fabs(RegionMean(out, W, 20, W - 20, 20, 60, -1) - 128.0);
		double down = fabs(RegionMean(out, W, 20, W - 20, H - 60, H - 20, -1) - 128.0);
		Check("height fog: thicker looking down than looking up", down > up + 3.0, "change up " + std::to_string(up) + " down " + std::to_string(down));

		fog.p[P_CAMERA_VALID] = 0.0f;
		Effects b;
		std::vector<uint8_t> blind = render(b, grey, depth, fog, input);
		int blindDiff = MaxDiff(input, blind);
		Check("height fog: without a camera it does nothing", blindDiff <= 1, "max channel diff " + std::to_string(blindDiff));

		depth->Release();
	}

	// ---- moire reduction, texture detail, surface grain ----
	{
		// Roughness: how much neighbouring pixels differ, which is what all three of these change.
		auto roughness = [&](const std::vector<uint8_t>& px, UINT x0, UINT x1, UINT y0, UINT y1) {
			double total = 0.0;
			for (UINT y = y0; y < y1; y++)
				for (UINT x = x0; x < x1 - 1; x++)
				{
					size_t a = ((size_t)y * W + x) * 4;
					double d = Luma8(&px[a]) - Luma8(&px[a + 4]);
					total += d < 0.0 ? -d : d;
				}
			return total / ((double)(x1 - x0 - 1) * (double)(y1 - y0));
		};

		ID3D11Texture2D* distant = MakeDepth(dev, W, H, [](UINT, UINT) { return 4000.0f; });
		// Left: a one pixel checkerboard, which is what a fine texture turns into at distance.
		// Right: one hard edge, which must survive.
		Fill checkerAndEdge = [&](UINT x, UINT y, uint8_t* p) {
			uint8_t v = x < W / 2 ? (((x + y) % 2) ? 90 : 165) : (x < W * 3 / 4 ? 40 : 200);
			p[0] = p[1] = p[2] = v;
		};

		Settings off = NeutralSettings(shader, lut);
		WithCamera(off, W, H);
		Settings on = off;
		on.p[P_DEMOIRE] = 1.0f;

		Effects a, b;
		std::vector<uint8_t> plain = render(a, checkerAndEdge, distant, off, input);
		std::vector<uint8_t> calmed = render(b, checkerAndEdge, distant, on, input);
		double shimmerBefore = roughness(plain, 20, W / 2 - 20, 40, H - 40);
		double shimmerAfter = roughness(calmed, 20, W / 2 - 20, 40, H - 40);
		double edgeBefore = RegionMean(plain, W, W * 3 / 4 + 4, W * 3 / 4 + 20, 40, H - 40, -1) - RegionMean(plain, W, W * 3 / 4 - 20, W * 3 / 4 - 4, 40, H - 40, -1);
		double edgeAfter = RegionMean(calmed, W, W * 3 / 4 + 4, W * 3 / 4 + 20, 40, H - 40, -1) - RegionMean(calmed, W, W * 3 / 4 - 20, W * 3 / 4 - 4, 40, H - 40, -1);

		Check("moire: a pixel-scale checkerboard is calmed", shimmerAfter < shimmerBefore * 0.5,
			"roughness " + std::to_string(shimmerBefore) + " -> " + std::to_string(shimmerAfter));
		Check("moire: a real edge keeps its contrast", edgeAfter > edgeBefore * 0.9,
			"edge " + std::to_string(edgeBefore) + " -> " + std::to_string(edgeAfter));

		// Texture detail on a gentle fine pattern: the fine contrast should come up.
		Fill faintTexture = [&](UINT x, UINT y, uint8_t* p) {
			uint8_t v = (uint8_t)(120 + ((x * 7 + y * 13) % 5) * 3);
			p[0] = p[1] = p[2] = v;
		};

		Settings detail = off;
		detail.p[P_DETAIL] = 1.0f;
		Effects c, d;
		std::vector<uint8_t> flat = render(c, faintTexture, distant, off, input);
		std::vector<uint8_t> detailed = render(d, faintTexture, distant, detail, input);
		double flatRough = roughness(flat, 40, W - 40, 40, H - 40);
		double detailRough = roughness(detailed, 40, W - 40, 40, H - 40);
		Check("texture detail: fine surface contrast comes up", detailRough > flatRough * 1.3,
			"roughness " + std::to_string(flatRough) + " -> " + std::to_string(detailRough));

		// Surface grain: it must show, and then hold still on a still scene rather than crawl.
		ID3D11Texture2D* closeUp = MakeDepth(dev, W, H, [](UINT, UINT) { return 1000.0f; });
		Settings grain = off;
		grain.p[P_SURFACE_GRAIN] = 1.0f;
		grain.p[P_TEMPORAL] = 1.0f;

		Effects e, f;
		std::vector<uint8_t> clean = render(e, grey, closeUp, off, input);
		render(f, grey, closeUp, grain, input);
		std::vector<uint8_t> grainy = render(f, grey, closeUp, grain, input);
		std::vector<uint8_t> grainyAgain = render(f, grey, closeUp, grain, input);
		double cleanRough = roughness(clean, 40, W - 40, 40, H - 40);
		double grainRough = roughness(grainy, 40, W - 40, 40, H - 40);
		int crawl = MaxDiff(grainy, grainyAgain);

		Check("surface grain: it shows on a flat surface", grainRough > cleanRough + 0.5,
			"roughness " + std::to_string(cleanRough) + " -> " + std::to_string(grainRough));
		Check("surface grain: it holds still on a still scene instead of crawling", crawl <= 2,
			"max channel diff between frames " + std::to_string(crawl));

		distant->Release();
		closeUp->Release();
	}

	// ---- render scale cleanup, and temporal supersampling ----
	{
		auto roughness = [&](const std::vector<uint8_t>& px, UINT x0, UINT x1, UINT y0, UINT y1) {
			double total = 0.0;
			for (UINT y = y0; y < y1; y++)
				for (UINT x = x0; x < x1 - 1; x++)
				{
					size_t a = ((size_t)y * W + x) * 4;
					double d = Luma8(&px[a]) - Luma8(&px[a + 4]);
					total += d < 0.0 ? -d : d;
				}
			return total / ((double)(x1 - x0 - 1) * (double)(y1 - y0));
		};

		ID3D11Texture2D* flatDepth = MakeDepth(dev, W, H, [](UINT, UINT) { return 2000.0f; });
		Fill checkerAndEdge = [&](UINT x, UINT y, uint8_t* p) {
			uint8_t v = x < W / 2 ? (((x + y) % 2) ? 90 : 165) : (x < W * 3 / 4 ? 40 : 200);
			p[0] = p[1] = p[2] = v;
		};

		Settings plain = NeutralSettings(shader, lut);
		WithCamera(plain, W, H);
		Settings shrunk = plain;
		shrunk.p[P_RENDER_SCALE] = 0.6f;

		Effects a, b;
		std::vector<uint8_t> native = render(a, checkerAndEdge, flatDepth, plain, input);
		std::vector<uint8_t> rebuilt = render(b, checkerAndEdge, flatDepth, shrunk, input);
		double aliasBefore = roughness(native, 20, W / 2 - 20, 40, H - 40);
		double aliasAfter = roughness(rebuilt, 20, W / 2 - 20, 40, H - 40);
		double edgeBefore = RegionMean(native, W, W * 3 / 4 + 8, W * 3 / 4 + 24, 40, H - 40, -1) - RegionMean(native, W, W * 3 / 4 - 24, W * 3 / 4 - 8, 40, H - 40, -1);
		double edgeAfter = RegionMean(rebuilt, W, W * 3 / 4 + 8, W * 3 / 4 + 24, 40, H - 40, -1) - RegionMean(rebuilt, W, W * 3 / 4 - 24, W * 3 / 4 - 8, 40, H - 40, -1);

		Check("render scale: shrinking and rebuilding washes out the engine's aliasing", aliasAfter < aliasBefore * 0.6,
			"roughness " + std::to_string(aliasBefore) + " -> " + std::to_string(aliasAfter));
		Check("render scale: a real edge survives it", edgeAfter > edgeBefore * 0.85,
			"edge " + std::to_string(edgeBefore) + " -> " + std::to_string(edgeAfter));

		// Temporal supersampling. The jitter is simulated the way the camera really does it: the same
		// anti-aliased edge drawn half a pixel across, alternating frame to frame. Accumulating those
		// must settle to something steadier than either frame - that IS the supersampling.
		auto slope = [&](float offset) {
			return Fill([=](UINT x, UINT y, uint8_t* p) {
				float coverage = ((float)x + offset) * 0.37f + 40.0f - (float)y + 0.5f;
				coverage = coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
				uint8_t v = (uint8_t)(25.0f + coverage * 205.0f);
				p[0] = p[1] = p[2] = v;
			});
		};

		Settings taa = plain;
		taa.p[P_TAA] = 1.0f;

		// Since 0.6 temporal antialiasing reprojects with the camera (PS_Taa) and only runs with two
		// consecutive valid poses one frame apart. The half-pixel shift of the slope image is declared
		// as the camera jitter: fwd turned by half a pixel, outFwd (the un-jittered camera) not.
		auto pose = [&](int frame, float jitterPx) {
			CameraPose c;
			c.valid = true;
			float a = jitterPx * 2.0f * taa.p[P_CAMERA_TAN_X] / (float)W; // radians for small angles
			float right[3] = {cosf(a), -sinf(a), 0.0f}, up[3] = {0.0f, 0.0f, 1.0f}, fwd[3] = {sinf(a), cosf(a), 0.0f};
			float outRight[3] = {1.0f, 0.0f, 0.0f}, outFwd[3] = {0.0f, 1.0f, 0.0f};
			memcpy(c.right, right, sizeof(right)); memcpy(c.up, up, sizeof(up)); memcpy(c.fwd, fwd, sizeof(fwd));
			memcpy(c.outRight, outRight, sizeof(outRight)); memcpy(c.outUp, up, sizeof(up)); memcpy(c.outFwd, outFwd, sizeof(outFwd));
			c.tan[0] = taa.p[P_CAMERA_TAN_X]; c.tan[1] = taa.p[P_CAMERA_TAN_Y];
			c.frame = (float)(frame + 1);
			c.jitter[0] = jitterPx; c.jitter[1] = 0.0f;
			c.luaJitter = 1;
			return c;
		};

		Effects still, accumulating;
		std::vector<uint8_t> jitterA, jitterB, taaA, taaB;

		for (int frame = 0; frame < 12; frame++)
		{
			taa.recent[0] = pose(frame, frame % 2 ? 0.5f : 0.0f);
			taa.p[P_JITTER_X] = frame % 2 ? 0.5f : 0.0f;
			taa.p[P_CAMERA_FRAME] = (float)(frame + 1);
			jitterA = render(still, slope(frame % 2 ? 0.5f : 0.0f), flatDepth, plain, input);
			taaA = render(accumulating, slope(frame % 2 ? 0.5f : 0.0f), flatDepth, taa, input);

			if (frame == 10)
			{
				jitterB = jitterA;
				taaB = taaA;
			}
		}

		double withoutTaa = MeanDiff(jitterB, jitterA);
		double withTaa = MeanDiff(taaB, taaA);
		Check("temporal supersampling: jittered frames settle instead of flickering", withTaa < withoutTaa * 0.5,
			"frame to frame change " + std::to_string(withoutTaa) + " -> " + std::to_string(withTaa));

		flatDepth->Release();
	}

	// ---- local tone mapping: dark areas come up, bright areas come down ----
	{
		ID3D11Texture2D* depth = MakeDepth(dev, W, H, [](UINT, UINT) { return 1000.0f; });
		Fill halves = [&](UINT x, UINT y, uint8_t* p) {
			bool stripe = (x + y) % 8 < 4;
			uint8_t v = x < W / 2 ? (stripe ? 12 : 30) : (stripe ? 220 : 245);
			p[0] = p[1] = p[2] = v;
		};

		Settings ltm = NeutralSettings(shader, lut);
		ltm.p[P_LTM] = 1.0f;
		ltm.p[P_LTM_DETAIL] = 0.2f;
		Effects a;
		std::vector<uint8_t> out = render(a, halves, depth, ltm, input);
		double darkUp = RegionMean(out, W, 40, W / 2 - 40, 60, H - 60, -1) - RegionMean(input, W, 40, W / 2 - 40, 60, H - 60, -1);
		double brightDown = RegionMean(input, W, W / 2 + 40, W - 40, 60, H - 60, -1) - RegionMean(out, W, W / 2 + 40, W - 40, 60, H - 60, -1);
		Check("local tone mapping: the dark half comes up", darkUp > 3.0, "brightened by " + std::to_string(darkUp));
		Check("local tone mapping: the bright half comes down", brightDown > 3.0, "darkened by " + std::to_string(brightDown));

		depth->Release();
	}
}

// What each lighting, motion and atmosphere effect costs at 1080p, on its own and all together.
static void ReportAdvancedCost(ID3D11Device* dev, ID3D11DeviceContext1* ctx, const char* shader, const char* lut)
{
	const UINT W = 1920, H = 1080;
	std::vector<uint8_t> pixels;
	ID3D11Texture2D* target = MakeTarget(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, pixels);
	ID3D11RenderTargetView* rtv = nullptr;
	dev->CreateRenderTargetView(target, nullptr, &rtv);
	ID3D11Texture2D* depth = MakeDepth(dev, W, H, [&](UINT x, UINT y) { return 300.0f + (float)x / (float)W * 3000.0f + (y > H / 2 ? 0.0f : 800.0f); });

	struct Case
	{
		const char* name;
		int params[4];
		float values[4];
	};

	const Case cases[] = {
		{"nothing                         ", {-1, -1, -1, -1}, {}},
		{"GTAO (temporal off)             ", {P_AO, P_TEMPORAL, -1, -1}, {1.0f, 0.0f}},
		{"GTAO (temporal on)              ", {P_AO, P_TEMPORAL, -1, -1}, {1.0f, 1.0f}},
		{"indirect light (temporal on)    ", {P_IL, P_TEMPORAL, -1, -1}, {1.0f, 1.0f}},
		{"ray traced lighting (temporal)  ", {P_RTGI, P_TEMPORAL, -1, -1}, {1.0f, 1.0f}},
		{"reflections (temporal on)       ", {P_SSR, P_TEMPORAL, -1, -1}, {1.0f, 1.0f}},
		{"motion blur                     ", {P_MBLUR, -1, -1, -1}, {1.0f}},
		{"light shafts                    ", {P_SHAFTS, -1, -1, -1}, {1.0f}},
		{"height fog                      ", {P_HFOG, -1, -1, -1}, {1.0f}},
		{"local tone mapping              ", {P_LTM, -1, -1, -1}, {1.0f}},
		{"temporal supersampling          ", {P_TAA, -1, -1, -1}, {1.0f}},
		{"render scale cleanup (0.7)      ", {P_RENDER_SCALE, -1, -1, -1}, {0.7f}},
	};

	Effects fx;
	double baseline = 0.0;

	printf("  Lighting, motion and atmosphere cost at %ux%u (this machine, DXVK; 30 frames):\n", W, H);

	auto measure = [&](Settings& s) {
		for (int warm = 0; warm < 5; warm++)
			fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, s, depth);

		ReadBack(dev, ctx, target);
		LARGE_INTEGER a, b, freq;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&a);

		for (int frame = 0; frame < 30; frame++)
			fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, s, depth);

		ReadBack(dev, ctx, target);
		QueryPerformanceCounter(&b);

		return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart / 30.0;
	};

	for (const Case& c : cases)
	{
		Settings s = NeutralSettings(shader, lut);
		WithCamera(s, W, H);

		for (int k = 0; k < 4; k++)
			if (c.params[k] >= 0)
				s.p[c.params[k]] = c.values[k];

		double ms = measure(s);

		if (baseline == 0.0)
			baseline = ms;

		printf("    %s %.3f ms/frame  (+%.3f ms)\n", c.name, ms, ms - baseline);
	}

	// What the mod actually ships as (mirrors the defaults in lua/core.lua), so the README can quote a
	// number for the look people get on first launch rather than for every effect at once.
	Settings shipped = NeutralSettings(shader, lut);
	WithCamera(shipped, W, H);
	// Mirrors lua/core.lua's shipped look (0.11): keep this in step when the defaults change.
	shipped.p[P_AA] = 1.0f;
	shipped.p[P_AA_MODE] = 2.0f;   // SMAA T2x (runs as SMAA here: no camera poses)
	shipped.p[P_SHARPEN] = 0.35f;
	shipped.p[P_DEMOIRE] = 0.4f;
	shipped.p[P_DETAIL] = 0.25f;
	shipped.p[P_DEBAND] = 0.5f;
	shipped.p[P_DITHER] = 1.0f;
	shipped.p[P_ADAPT] = 0.35f;
	shipped.p[P_BLOOM] = 0.7f;
	shipped.p[P_BLOOM_THRESHOLD] = 0.75f;
	shipped.p[P_BLOOM_RADIUS] = 1.2f;
	shipped.p[P_BLOOM_TINT] = 0.25f;
	shipped.p[P_FLARE] = 0.35f;
	shipped.p[P_LTM] = 0.35f;
	shipped.p[P_LTM_DETAIL] = 0.3f;
	shipped.p[P_CLARITY] = 0.15f;
	shipped.p[P_CUES] = 0.3f;
	shipped.p[P_CONTRAST] = 0.5f;
	shipped.p[P_VIBRANCE] = 1.0f;
	shipped.p[P_LUT_MIX] = 0.5f;
	shipped.p[P_VIGNETTE] = 0.32f;
	shipped.p[P_CHROMATIC] = 0.4f;
	shipped.p[P_AO] = 1.0f;
	shipped.p[P_AO_RADIUS] = 200.0f;
	shipped.p[P_IL] = 0.75f;
	shipped.p[P_IL_RADIUS] = 200.0f;
	shipped.p[P_RTGI] = 0.75f;
	shipped.p[P_RTGI_DISTANCE] = 500.0f;
	shipped.p[P_SSR] = 0.2f;
	shipped.p[P_SSR_BLUR] = 1.0f;
	shipped.p[P_FOG] = 0.2f;
	shipped.p[P_FOG_START] = 0.25f;
	shipped.p[P_SHAFTS] = 0.4f;
	shipped.p[P_DOF] = 0.1f;   // from the hip
	shipped.p[P_DOF_AIM] = 0.5f; // and while aiming
	shipped.p[P_MBLUR] = 0.5f;
	shipped.p[P_TEMPORAL] = 1.0f;
	double shippedCost = measure(shipped);
	printf("    the shipped defaults            %.3f ms/frame  (+%.3f ms)\n", shippedCost, shippedCost - baseline);

	Settings all = NeutralSettings(shader, lut);
	WithCamera(all, W, H);
	all.p[P_AO] = 1.0f;
	all.p[P_IL] = 1.0f;
	all.p[P_RTGI] = 1.0f;
	all.p[P_SSR] = 1.0f;
	all.p[P_MBLUR] = 1.0f;
	all.p[P_SHAFTS] = 1.0f;
	all.p[P_HFOG] = 1.0f;
	all.p[P_LTM] = 1.0f;
	all.p[P_TEMPORAL] = 1.0f;
	double everything = measure(all);
	printf("    everything above, together      %.3f ms/frame  (+%.3f ms)\n", everything, everything - baseline);

	fx.Release();
	depth->Release();
	rtv->Release();
	target->Release();
}

// ------------------------------------------------------------------------------------------
// The file bridge: what lua/core.lua writes must land in the render thread's settings.

/*
	Depth. The plugin COPIES the game's depth target instead of making a shader resource view on
	it, because the bind flags a game asked for are not a mod's choice. The fake depth target here
	is therefore created deliberately WITHOUT D3D11_BIND_SHADER_RESOURCE: it is what one of
	Diesel's private render targets looks like, and a view on it would be impossible. If anyone
	ever "simplifies" the copy away, these tests stop passing.
*/
static void TestDepth(ID3D11Device* dev, ID3D11DeviceContext1* ctx, const char* shader, const char* lut)
{
	const UINT W = 640, H = 360;
	std::vector<uint8_t> input;
	ID3D11Texture2D* target = MakeTarget(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, input);
	ID3D11RenderTargetView* rtv = nullptr;
	dev->CreateRenderTargetView(target, nullptr, &rtv);

	// Distance ramps from near on the left to far on the right, with a nearer block in the middle
	// for the occlusion taps to find. The left and right quarters stay pure ramp, so they can be
	// compared against each other.
	std::vector<float> depthPixels((size_t)W * H);

	for (UINT y = 0; y < H; y++)
	{
		for (UINT x = 0; x < W; x++)
		{
			float d = 200.0f + (float)x / (float)(W - 1) * 12000.0f;

			// Kept well below the centre of the screen: the autofocus reads a small cluster around the
			// crosshair, and a block right at its edge would (correctly) pull the focus nearer.
			if (x > W * 3 / 8 && x < W * 5 / 8 && y > H * 3 / 4)
				d *= 0.5f;

			depthPixels[(size_t)y * W + x] = d;
		}
	}

	D3D11_TEXTURE2D_DESC dd = {};
	dd.Width = W;
	dd.Height = H;
	dd.MipLevels = 1;
	dd.ArraySize = 1;
	dd.Format = DXGI_FORMAT_R32_FLOAT;
	dd.SampleDesc.Count = 1;
	dd.Usage = D3D11_USAGE_DEFAULT;
	dd.BindFlags = D3D11_BIND_RENDER_TARGET; // no SHADER_RESOURCE: only a copy can read this

	D3D11_SUBRESOURCE_DATA data = {depthPixels.data(), W * 4, 0};
	ID3D11Texture2D* depth = nullptr;
	HRESULT hr = dev->CreateTexture2D(&dd, &data, &depth);
	Check("depth: a target the game never shared can be made", SUCCEEDED(hr) && depth != nullptr);

	if (!depth)
	{
		rtv->Release();
		target->Release();

		return;
	}

	Effects fx;

	// Every depth effect turned up full, but no depth handed over. The promise made in the menu
	// and in the shader is that they then do nothing whatsoever.
	Settings without = NeutralSettings(shader, lut);
	without.p[P_AO] = 1.0f;
	without.p[P_FOG] = 1.0f;
	without.p[P_DOF] = 1.0f;

	bool ranWithout = fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, without);
	std::vector<uint8_t> missing = ReadBack(dev, ctx, target);
	int missingDiff = MaxDiff(input, missing);
	Check("depth: with no depth target the depth effects change nothing", ranWithout && missingDiff <= 1, "max channel diff " + std::to_string(missingDiff));
	Check("depth: and the plugin can say why", !fx.DepthReady() && !fx.DepthError().empty(), fx.DepthError());

	// Occlusion only, so any change can only have come from the depth buffer.
	ID3D11Texture2D* aoTarget = MakeTarget(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, input);
	ID3D11RenderTargetView* aoRtv = nullptr;
	dev->CreateRenderTargetView(aoTarget, nullptr, &aoRtv);

	// A real crease, well inside the occlusion radius: a wall 100 units in front of another. GTAO
	// finds no occlusion on the gently curved ramp above (correctly - it is not a crease), and the
	// ramp's own step is thousands of units deep, far outside any occlusion radius. This depth
	// target is also made without D3D11_BIND_SHADER_RESOURCE, so only the copy can read it.
	std::vector<float> creasePixels((size_t)W * H);

	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
			creasePixels[(size_t)y * W + x] = x < W / 2 ? 900.0f : 1000.0f;

	D3D11_SUBRESOURCE_DATA creaseData = {creasePixels.data(), W * 4, 0};
	ID3D11Texture2D* crease = nullptr;
	dev->CreateTexture2D(&dd, &creaseData, &crease);

	Settings ao = NeutralSettings(shader, lut);
	ao.p[P_AO] = 1.0f;
	ao.p[P_AO_RADIUS] = 200.0f;
	ao.p[P_DEPTH_MODE] = 1.0f;
	ao.p[P_TEMPORAL] = 0.0f;

	bool ranAo = fx.Run(dev, ctx, aoTarget, aoRtv, DXGI_FORMAT_UNKNOWN, ao, crease);
	crease->Release();
	std::vector<uint8_t> occluded = ReadBack(dev, ctx, aoTarget);
	int aoDiff = MaxDiff(input, occluded);
	Check("depth: occlusion reads the copied buffer and changes the image", ranAo && aoDiff > 1, "max channel diff " + std::to_string(aoDiff));
	Check("depth: the copy worked without a shader view on the game's texture", fx.DepthReady() && fx.DepthError().empty(), fx.DepthError());

	// Aerial perspective must land on the FAR side of the ramp. This is the check that proves the
	// buffer is read the right way round, not merely read.
	ID3D11Texture2D* fogTarget = MakeTarget(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, input);
	ID3D11RenderTargetView* fogRtv = nullptr;
	dev->CreateRenderTargetView(fogTarget, nullptr, &fogRtv);

	Settings fog = NeutralSettings(shader, lut);
	fog.p[P_FOG] = 1.0f;
	fog.p[P_FOG_START] = 0.0f;
	fog.p[P_DEPTH_MODE] = 1.0f; // the ramp holds world units
	fog.p[P_DEPTH_FAR] = 12000.0f;

	fx.Run(dev, ctx, fogTarget, fogRtv, DXGI_FORMAT_UNKNOWN, fog, depth);
	std::vector<uint8_t> hazed = ReadBack(dev, ctx, fogTarget);

	double nearChange = 0.0;
	double farChange = 0.0;

	for (UINT y = 0; y < H; y++)
	{
		for (UINT x = 0; x < W / 4; x++)
		{
			size_t left = ((size_t)y * W + x) * 4;
			size_t right = ((size_t)y * W + (W - 1 - x)) * 4;
			double dl = Luma8(&input[left]) - Luma8(&hazed[left]);
			double dr = Luma8(&input[right]) - Luma8(&hazed[right]);

			nearChange += dl < 0.0 ? -dl : dl;
			farChange += dr < 0.0 ? -dr : dr;
		}
	}

	Check("depth: haze lands on the far side, not the near side", farChange > nearChange * 2.0,
		"near " + std::to_string((int)nearChange) + " far " + std::to_string((int)farChange));

	// Automatic mode has to reach the same conclusion as being told: the ramp holds world units,
	// which cannot be mistaken for a projected buffer because they leave 0..1 entirely.
	Settings autoFog = fog;
	autoFog.p[P_DEPTH_MODE] = 0.0f;

	ID3D11Texture2D* autoTarget = MakeTarget(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, input);
	ID3D11RenderTargetView* autoRtv = nullptr;
	dev->CreateRenderTargetView(autoTarget, nullptr, &autoRtv);

	fx.Run(dev, ctx, autoTarget, autoRtv, DXGI_FORMAT_UNKNOWN, autoFog, depth);
	std::vector<uint8_t> autoHazed = ReadBack(dev, ctx, autoTarget);
	int autoDiff = MaxDiff(hazed, autoHazed);
	Check("depth: automatic reads a world unit buffer the same way as being told", autoDiff <= 1, "max channel diff " + std::to_string(autoDiff));

	// The Depth view must actually show depth when a depth target IS present. Red is the "no depth
	// target" warning, so red here means something between the hook and the constant buffer broke.
	// FSR is on at 2x because the report that prompted this test had it on.
	Settings view = NeutralSettings(shader, lut);
	view.p[P_DEPTH_VIEW] = 1.0f;
	view.p[P_UPSCALE] = 1.0f;
	view.p[P_UPSCALE_SCALE] = 2.0f;

	ID3D11Texture2D* viewTarget = MakeTarget(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, input);
	ID3D11RenderTargetView* viewRtv = nullptr;
	dev->CreateRenderTargetView(viewTarget, nullptr, &viewRtv);

	fx.Run(dev, ctx, viewTarget, viewRtv, DXGI_FORMAT_UNKNOWN, view, depth);
	std::vector<uint8_t> shown = ReadBack(dev, ctx, viewTarget);

	int reddest = 0;
	double leftLuma = 0.0;
	double rightLuma = 0.0;

	for (UINT y = 0; y < H; y++)
	{
		for (UINT x = 0; x < W; x++)
		{
			const uint8_t* px = &shown[((size_t)y * W + x) * 4];
			int rg = (int)px[0] - (int)px[1];
			int rb = (int)px[0] - (int)px[2];
			int red = rg < rb ? rb : rg;

			reddest = red > reddest ? red : reddest;

			if (x < W / 4)
				leftLuma += Luma8(px);

			if (x >= W * 3 / 4)
				rightLuma += Luma8(px);
		}
	}

	Check("depth: the depth view shows depth, not the no-depth warning", reddest < 24, "reddest channel lead " + std::to_string(reddest));
	Check("depth: the depth view is bright near and dark far", leftLuma > rightLuma * 1.5,
		"left " + std::to_string((int)leftLuma) + " right " + std::to_string((int)rightLuma));

	viewRtv->Release();
	viewTarget->Release();

	// Depth of field. The ramp puts the centre of the screen about 6300 units away. With focus on
	// the crosshair, that column must stay sharp while both the near left edge and the far right
	// edge blur. Then fix the focus at the far edge instead, and the sharp and blurred sides swap.
	auto bandChange = [&](const std::vector<uint8_t>& out, UINT x0, UINT x1, UINT y0, UINT y1) {
		double total = 0.0;
		for (UINT y = y0; y < y1; y++)
			for (UINT x = x0; x < x1; x++)
			{
				size_t at = ((size_t)y * W + x) * 4;
				double d = Luma8(&input[at]) - Luma8(&out[at]);
				total += d < 0.0 ? -d : d;
			}
		return total / ((double)(x1 - x0) * (double)(y1 - y0));
	};

	auto runDof = [&](Effects& lens, float focus) {
		Settings dof = NeutralSettings(shader, lut);
		dof.p[P_DOF] = 1.0f;
		dof.p[P_DOF_FOCUS] = focus;
		dof.p[P_DOF_RANGE] = 0.1f;
		dof.p[P_DOF_NEAR] = 1.0f;
		dof.p[P_DEPTH_MODE] = 1.0f;
		dof.p[P_DEPTH_FAR] = 20000.0f;

		ID3D11Texture2D* t = MakeTarget(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, input);
		ID3D11RenderTargetView* r = nullptr;
		dev->CreateRenderTargetView(t, nullptr, &r);
		lens.Run(dev, ctx, t, r, DXGI_FORMAT_UNKNOWN, dof, depth);
		std::vector<uint8_t> out = ReadBack(dev, ctx, t);
		r->Release();
		t->Release();
		return out;
	};

	// A fresh lens for each scenario: focus eases from frame to frame, so a lens that just looked
	// somewhere else would still be refocusing.
	Effects lookingLens;
	std::vector<uint8_t> looked = runDof(lookingLens, 0.0f);
	double centre = bandChange(looked, W / 2 - 6, W / 2 + 6, 0, H / 2 - 30);
	double farSide = bandChange(looked, W - 40, W - 10, 0, H / 2 - 30);
	double nearSide = bandChange(looked, 10, 40, 0, H / 2 - 30);

	Check("dof: what is under the crosshair stays sharp", centre < 2.0, "mean change " + std::to_string(centre));
	Check("dof: the far side blurs", farSide > centre * 4.0 && farSide > 3.0, "far " + std::to_string(farSide) + " centre " + std::to_string(centre));
	Check("dof: the near side blurs too", nearSide > centre * 4.0 && nearSide > 3.0, "near " + std::to_string(nearSide) + " centre " + std::to_string(centre));

	Effects fixedLens;
	std::vector<uint8_t> fixedFar = runDof(fixedLens, 12300.0f / 20000.0f);
	double farWhenFocused = bandChange(fixedFar, W - 40, W - 10, 0, H / 2 - 30);
	double centreWhenFar = bandChange(fixedFar, W / 2 - 6, W / 2 + 6, 0, H / 2 - 30);
	Check("dof: a fixed focus distance moves the sharp zone there", farWhenFocused < farSide * 0.5 && centreWhenFar > centre + 1.0,
		"far " + std::to_string(farWhenFocused) + " centre " + std::to_string(centreWhenFar));

	// Straight back to looking at the centre, one frame later: the focus must still be on its way,
	// so the far side is still sharper than it is for a lens that has settled on the centre.
	std::vector<uint8_t> refocusing = runDof(fixedLens, 0.0f);
	double farRefocusing = bandChange(refocusing, W - 40, W - 10, 0, H / 2 - 30);
	Check("dof: refocusing eases instead of snapping", farRefocusing < farSide,
		"far " + std::to_string(farRefocusing) + " settled " + std::to_string(farSide));

	// Only while aiming: nothing blurs with sights down, the background does with sights up, and
	// lowering them closes the lens smoothly rather than snapping sharp.
	auto runAim = [&](Effects& lens, float aim) {
		Settings dof = NeutralSettings(shader, lut);
		dof.p[P_DOF] = 0.0f;     // nothing from the hip
		dof.p[P_DOF_AIM] = 1.0f; // everything while aiming
		dof.p[P_DOF_RANGE] = 0.1f;
		dof.p[P_DOF_NEAR] = 1.0f;
		dof.p[P_CAMERA_VALID] = 1.0f;
		dof.p[P_CAMERA_AIM] = aim;
		dof.p[P_DEPTH_MODE] = 1.0f;
		dof.p[P_DEPTH_FAR] = 20000.0f;

		ID3D11Texture2D* t = MakeTarget(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, input);
		ID3D11RenderTargetView* r = nullptr;
		dev->CreateRenderTargetView(t, nullptr, &r);
		lens.Run(dev, ctx, t, r, DXGI_FORMAT_UNKNOWN, dof, depth);
		std::vector<uint8_t> out = ReadBack(dev, ctx, t);
		r->Release();
		t->Release();
		return out;
	};

	Effects hipLens;
	double hipFar = bandChange(runAim(hipLens, 0.0f), W - 40, W - 10, 0, H / 2 - 30);
	Check("dof: with the hip size at zero, nothing blurs with sights down", hipFar < 0.5, "far " + std::to_string(hipFar));

	Effects adsLens;
	double adsFar = bandChange(runAim(adsLens, 1.0f), W - 40, W - 10, 0, H / 2 - 30);
	Check("dof: the aiming size blurs the background with sights up", adsFar > 3.0, "far " + std::to_string(adsFar));

	double loweringFar = bandChange(runAim(adsLens, 0.0f), W - 40, W - 10, 0, H / 2 - 30);
	Check("dof: lowering your sights closes the lens smoothly", loweringFar > adsFar * 0.5, "far " + std::to_string(loweringFar) + " while aiming " + std::to_string(adsFar));

	autoRtv->Release();
	autoTarget->Release();
	fogRtv->Release();
	fogTarget->Release();
	aoRtv->Release();
	aoTarget->Release();
	depth->Release();
	rtv->Release();
	target->Release();
}

// SMAA on one long, shallow, high contrast edge: exactly the case FXAA handles worst. Pixels on the
// edge must take in-between values; pixels away from it must not change at all.
static void TestSmaa(ID3D11Device* dev, ID3D11DeviceContext1* ctx, const char* shader, const char* lut)
{
	const UINT W = 640, H = 360;
	std::vector<uint8_t> input;
	auto line = [](UINT x) { return (float)x * 0.37f + 40.0f; };
	ID3D11Texture2D* target = MakePattern(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, input, [&](UINT x, UINT y, uint8_t* p) {
		p[0] = p[1] = p[2] = (float)y > line(x) ? 230 : 25;
	});
	ID3D11RenderTargetView* rtv = nullptr;
	dev->CreateRenderTargetView(target, nullptr, &rtv);

	Effects fx;
	Settings s = NeutralSettings(shader, lut);
	s.p[P_AA] = 1.0f;
	s.p[P_AA_MODE] = 1.0f;

	bool ran = fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, s);
	Check("smaa: the passes compile and run", ran, fx.Error());
	Check("smaa: the lookup textures load", fx.SmaaReady());

	std::vector<uint8_t> out = ReadBack(dev, ctx, target);
	int blended = 0;
	int strayed = 0;

	for (UINT y = 0; y < H; y++)
	{
		for (UINT x = 0; x < W; x++)
		{
			size_t at = ((size_t)y * W + x) * 4;
			int before = input[at];
			int after = out[at];
			float distance = (float)y - line(x);
			distance = distance < 0.0f ? -distance : distance;

			if (distance <= 1.5f && after > 40 && after < 215)
				blended++;

			if (distance > 4.0f && (after - before > 2 || before - after > 2))
				strayed++;
		}
	}

	Check("smaa: the edge gets in-between values", blended > 300, std::to_string(blended) + " blended edge pixels");
	Check("smaa: nothing away from the edge changes", strayed == 0, std::to_string(strayed) + " pixels changed away from the edge");

	rtv->Release();
	target->Release();
}

static void WriteFile(const char* path, const char* text)
{
	FILE* f = fopen(path, "wb");
	fputs(text, f);
	fclose(f);
}

static void TestBridge()
{
	const char* path = "pss_live_test.txt";

	WriteFile(path, "shader_path mods/Payday Screen Shaders/shaders/pss.hlsl\nlut_path \nsharpen 0.35\nenabled 1\ntick 4\nreload 0\ncapture 0\nend\n");
	Check("bridge: a complete file is read", Bridge::Poll(path));

	Settings s = State::Snapshot();
	Check("bridge: numbers arrive", fabs(s.p[P_SHARPEN] - 0.35f) < 0.0001f && s.p[P_ENABLED] == 1.0f);
	Check("bridge: paths with spaces arrive whole", s.shaderPath == "mods/Payday Screen Shaders/shaders/pss.hlsl", s.shaderPath);
	Check("bridge: the first tick is only a baseline, not a heartbeat", !s.active);

	Check("bridge: an unchanged file is not reread", !Bridge::Poll(path));

	Sleep(20);
	WriteFile(path, "sharpen 0.9\ntick 5\n");
	Check("bridge: a half-written file is ignored", !Bridge::Poll(path));
	Check("bridge: and changes nothing", fabs(State::Snapshot().p[P_SHARPEN] - 0.35f) < 0.0001f);

	unsigned shaderGen = State::Snapshot().shaderGen;
	Sleep(20);
	WriteFile(path, "shader_path mods/Payday Screen Shaders/shaders/pss.hlsl\nsharpen 0.9\ntick 5\nreload 1\ncapture 1\nend\n");
	Check("bridge: the next complete file is read", Bridge::Poll(path));

	s = State::Snapshot();
	Check("bridge: a new tick is a heartbeat", s.active);
	Check("bridge: a new reload count recompiles", s.shaderGen == shaderGen + 1);
	Check("bridge: a new capture count requests a capture", State::TakeCaptureRequest() == 2);
	Check("bridge: out of range values are clamped", fabs(s.p[P_SHARPEN] - 0.9f) < 0.0001f);

	Bridge::WriteStatus("pss_status_test.txt");
	FILE* f = fopen("pss_status_test.txt", "rb");
	char status[512] = {};

	if (f)
	{
		fread(status, 1, sizeof(status) - 1, f);
		fclose(f);
	}

	Check("bridge: status is written for the menu", strstr(status, "Settings from Lua: arriving") != nullptr, status);
}

// ------------------------------------------------------------------------------------------
// The hook test: a fake "game" frame of world blit + HUD draw, then Present.

static const char* kGameShader = R"(
cbuffer C : register(b0) { float4 color; float4 rect; };
struct O { float4 pos : SV_Position; };
O VS(uint id : SV_VertexID) { O o; float x = (id == 1) ? 2.0 : ((id == 4) ? 2.0 : 0.0); float y = (id == 2) ? 2.0 : ((id == 5) ? 2.0 : 0.0); o.pos = float4((x * 2.0 - 1.0) * rect.x + rect.z, (1.0 - y * 2.0) * rect.y + rect.w, 0.0, 1.0); return o; }
float4 PS(O i) : SV_Target { return color; }
float4 PS2(O i) : SV_Target { return float4(color.rgb, color.a); }
)";

typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static void TestHooks(const char* shader)
{
	if (!InstallHooks())
	{
		Check("hooks install on a DXVK device", false);
		return;
	}

	Status st = State::GetStatus();
	Check("the Present hook installs", st.presentHooked);
	// Since 0.5.x the game's own context is hooked on its first Present, not here: checked after the frames.

	WNDCLASSEXA wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(nullptr);
	wc.lpszClassName = "PSS_TestGame";
	RegisterClassExA(&wc);
	HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "pss test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, nullptr, nullptr, wc.hInstance, nullptr);

	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferDesc.Width = 320;
	sd.BufferDesc.Height = 180;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.OutputWindow = hwnd;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	IDXGISwapChain* swap = nullptr;
	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;

	if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx)))
	{
		Check("game swap chain for the hook test", false);
		return;
	}

	HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
	D3DCompileFn compile = compiler ? (D3DCompileFn)GetProcAddress(compiler, "D3DCompile") : nullptr;

	if (!compile)
	{
		Check("d3dcompiler_47 for the fake game shaders", false);
		return;
	}

	ID3DBlob* vsb = nullptr;
	ID3DBlob* psb = nullptr;
	ID3DBlob* errors = nullptr;
	compile(kGameShader, strlen(kGameShader), "game", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &errors);
	compile(kGameShader, strlen(kGameShader), "game", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, nullptr);

	if (!vsb || !psb)
	{
		Check("the fake game shaders compile", false, errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "");
		return;
	}

	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* ps = nullptr;
	dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
	dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);

	// Scratch passes use a different pixel shader than the final composite, as in the game.
	ID3DBlob* ps2b = nullptr;
	compile(kGameShader, strlen(kGameShader), "game", nullptr, nullptr, "PS2", "ps_5_0", 0, 0, &ps2b, nullptr);
	ID3D11PixelShader* psScratch = nullptr;
	dev->CreatePixelShader(ps2b->GetBufferPointer(), ps2b->GetBufferSize(), nullptr, &psScratch);

	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = 32;
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Buffer* cb = nullptr;
	dev->CreateBuffer(&cbd, nullptr, &cb);

	// panel = a small shape in the top left corner, drawn with the full viewport like GUI quads.
	auto setColor = [&](float r, float g, float b, bool panel = false) {
		D3D11_MAPPED_SUBRESOURCE m;
		ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
		float c[8] = {r, g, b, 1.0f, panel ? 0.1f : 1.0f, panel ? 0.1f : 1.0f, panel ? -0.9f : 0.0f, panel ? 0.9f : 0.0f};
		memcpy(m.pData, c, 32);
		ctx->Unmap(cb, 0);
	};

	// Darken the world by half with gain; nothing else. Easy to measure.
	State::SetShaderPath(shader);
	State::SetParam("enabled", 1.0f);
	State::SetParam("gain", 0.5f);
	State::SetParam("inject_after", 0.0f);
	State::SetParam("require_fullscreen", 1.0f);

	std::vector<uint8_t> pixels;
	D3D11_TEXTURE2D_DESC bbDesc = {};

	std::vector<uint8_t> pixelsExtraPass;

	// Frames 0-44: scratch pass + final composite. The position rule starts the shaders on frame
	// 3; the final pass's pixel shader is learned after 30 frames. From frame 45 an extra scratch
	// pass comes first, as when a heist adds one: position would now be wrong, identity is not.
	uint64_t injectedBeforeExtraPass = 0;

	for (int frame = 0; frame < 50; frame++)
	{
		if (frame == 45)
			injectedBeforeExtraPass = State::GetStatus().injectedFrames;

		State::SetActive(true);

		ID3D11Texture2D* bb = nullptr;
		swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
		bb->GetDesc(&bbDesc);
		ID3D11RenderTargetView* rtv = nullptr;
		dev->CreateRenderTargetView(bb, nullptr, &rtv);

		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(vs, nullptr, 0);
		ctx->PSSetShader(ps, nullptr, 0);
		ctx->PSSetConstantBuffers(0, 1, &cb);
		ctx->VSSetConstantBuffers(0, 1, &cb);
		ctx->OMSetRenderTargets(1, &rtv, nullptr);

		D3D11_VIEWPORT full = {0, 0, 320, 180, 0, 1};
		ctx->RSSetViewports(1, &full);

		// As in the real capture: Diesel first uses the back buffer as a scratch scene target
		// (a fullscreen pass that gets overwritten), then writes its final composite.
		ctx->PSSetShader(psScratch, nullptr, 0);
		setColor(0.2f, 0.2f, 0.2f);
		ctx->Draw(3, 0);

		if (frame >= 45)
			ctx->Draw(3, 0);

		ctx->PSSetShader(ps, nullptr, 0);
		setColor(1.0f, 1.0f, 1.0f);
		ctx->Draw(3, 0);

		auto probe = [&](const char* when) {
			if (frame != 49)
				return;

			std::vector<uint8_t> px = ReadBack(dev, ctx, bb);
			uint8_t* w = &px[((size_t)120 * bbDesc.Width + 200) * 4];
			uint8_t* h = &px[((size_t)5 * bbDesc.Width + 10) * 4];
			printf("  frame %d %s: world %u,%u,%u  corner %u,%u,%u\n", frame, when, w[0], w[1], w[2], h[0], h[1], h[2]);
		};

		probe("after composite");

		// HUD: 6-vertex quads over the full viewport, here a red panel in the corner.
		setColor(1.0f, 0.0f, 0.0f, true);
		ctx->Draw(6, 0);
		probe("after hud");

		if (frame == 44)
			pixels = ReadBack(dev, ctx, bb);

		if (frame == 49)
			pixelsExtraPass = ReadBack(dev, ctx, bb);

		rtv->Release();
		bb->Release();
		swap->Present(0, 0);
	}

	st = State::GetStatus();
	Check("the effects ran on the hooked frames", st.injectedFrames >= 2, "injected " + std::to_string(st.injectedFrames) + " error: " + st.error);

	if (!pixels.empty())
	{
		auto at = [&](UINT x, UINT y) { return &pixels[((size_t)y * bbDesc.Width + x) * 4]; };
		uint8_t* world = at(200, 120);
		uint8_t* panel = at(10, 5);

		char detail[96];
		snprintf(detail, sizeof(detail), "world %u,%u,%u", world[0], world[1], world[2]);
		// 255 = ran too early (the composite overwrote it), 63 = ran twice.
		Check("hooks install on a DXVK device", State::GetStatus().hooksInstalled);
		Check("the world is processed once, after its final composite", world[0] > 110 && world[0] < 145, detail);

		snprintf(detail, sizeof(detail), "hud %u,%u,%u", panel[0], panel[1], panel[2]);
		Check("the HUD drawn after it is untouched", panel[0] == 255 && panel[1] == 0 && panel[2] == 0, detail);
	}

	Check("the final pass is learned by its pixel shader", st.finalPassLearned);

	if (!pixelsExtraPass.empty())
	{
		uint8_t* world = &pixelsExtraPass[((size_t)120 * bbDesc.Width + 200) * 4];
		char detail[64];
		snprintf(detail, sizeof(detail), "world %u,%u,%u", world[0], world[1], world[2]);
		Check("an extra fullscreen pass before the final one does not move the shaders", world[0] > 110 && world[0] < 145, detail);
		// Every one of the five frames with the extra pass (45..49) must still be processed. This used
		// to read the plugin's one-second "frames without shaders" window, which also counts the two
		// start-up frames that wait by design for the pass count to settle; once shader compilation
		// grew past half a second, that window closed with them still in it.
		uint64_t processedWithExtraPass = st.injectedFrames - injectedBeforeExtraPass;
		Check("and no frame with the extra pass went without shaders", processedWithExtraPass == 5, "processed " + std::to_string(processedWithExtraPass) + " of 5");
	}

	cb->Release();
	vs->Release();
	ps->Release();
	psScratch->Release();
	ps2b->Release();
	vsb->Release();
	psb->Release();
	swap->Release();
	ctx->Release();
	dev->Release();
	DestroyWindow(hwnd);
}

// Preview mode: runs the real shader chain on a screenshot, for tuning defaults by eye.
//   pss_test.exe --render <pss.hlsl> <settings.txt> <in.rgba> <width> <height> <out.rgba> [frames]
// settings.txt holds "name value" lines as lua/core.lua writes them, plus lut_path and dirt_path.
// No depth is supplied, so only the effects that work from colour change the picture.
static int Render(int argc, char** argv)
{
	if (argc < 8)
	{
		printf("usage: pss_test.exe --render <pss.hlsl> <settings.txt> <in.rgba> <width> <height> <out.rgba> [frames]\n");
		return 2;
	}

	const char* shader = argv[2];
	UINT w = (UINT)atoi(argv[5]);
	UINT h = (UINT)atoi(argv[6]);
	int frames = argc > 8 ? atoi(argv[8]) : 8;

	LogSetPath("pss_render.log");
	State::Init();

	std::string lut;
	std::string dirt;
	FILE* settings = fopen(argv[3], "rb");

	if (!settings)
	{
		printf("cannot open %s\n", argv[3]);
		return 2;
	}

	char line[512];
	std::vector<std::pair<std::string, float>> values;

	while (fgets(line, sizeof(line), settings))
	{
		char key[128] = {};
		char value[384] = {};

		if (sscanf(line, "%127s %383[^\r\n]", key, value) != 2)
			continue;

		if (strcmp(key, "lut_path") == 0)
			lut = value;
		else if (strcmp(key, "dirt_path") == 0)
			dirt = value;
		else
			values.push_back({key, (float)atof(value)});
	}

	fclose(settings);
	g_dirt = dirt.c_str();

	FILE* in = fopen(argv[4], "rb");
	std::vector<uint8_t> pixels((size_t)w * h * 4);

	if (!in || fread(pixels.data(), 1, pixels.size(), in) != pixels.size())
	{
		printf("cannot read %ux%u RGBA from %s\n", w, h, argv[4]);
		return 2;
	}

	fclose(in);

	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx0 = nullptr;

	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx0)))
		return 1;

	ID3D11DeviceContext1* ctx = nullptr;
	ctx0->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx);

	Settings s = NeutralSettings(shader, lut.c_str());

	for (const auto& kv : values)
	{
		int index = FindParam(kv.first.c_str());

		if (index >= 0)
			s.p[index] = kv.second < kParams[index].min ? kParams[index].min : (kv.second > kParams[index].max ? kParams[index].max : kv.second);
	}

	std::vector<uint8_t> scratch;
	ID3D11Texture2D* target = MakePattern(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, scratch, [&](UINT x, UINT y, uint8_t* p) {
		memcpy(p, &pixels[((size_t)y * w + x) * 4], 4);
	});
	ID3D11Texture2D* original = MakePattern(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, scratch, [&](UINT x, UINT y, uint8_t* p) {
		memcpy(p, &pixels[((size_t)y * w + x) * 4], 4);
	});
	ID3D11RenderTargetView* rtv = nullptr;
	dev->CreateRenderTargetView(target, nullptr, &rtv);

	Effects fx;

	for (int frame = 0; frame < frames; frame++)
	{
		ctx->CopyResource(target, original);

		if (!fx.Run(dev, ctx, target, rtv, DXGI_FORMAT_UNKNOWN, s))
		{
			printf("run failed: %s\n", fx.Error().c_str());
			return 1;
		}
	}

	std::vector<uint8_t> out = ReadBack(dev, ctx, target);
	FILE* result = fopen(argv[7], "wb");
	fwrite(out.data(), 1, out.size(), result);
	fclose(result);
	printf("rendered %ux%u, %d frames\n", w, h, frames);

	return 0;
}

int main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--render") == 0)
		return Render(argc, argv);

	if (argc < 3)
	{
		printf("usage: pss_test.exe <pss.hlsl> <lut.cube> [lens_dirt.bin]\n");
		return 2;
	}

	if (argc > 3)
		g_dirt = argv[3];

	LogSetPath("pss_test_native.log");
	State::Init();

	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
	Check("D3D11 device (DXVK)", SUCCEEDED(hr));

	if (FAILED(hr))
		return 1;

	ID3D11DeviceContext1* ctx1 = nullptr;
	ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1);

	TestEffects(dev, ctx1, argv[1], argv[2], DXGI_FORMAT_R8G8B8A8_UNORM, "unorm");
	TestEffects(dev, ctx1, argv[1], argv[2], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "srgb");
	ReportUpscaleCost(dev, ctx1, argv[1], argv[2]);
	TestDepth(dev, ctx1, argv[1], argv[2]);
	TestSmaa(dev, ctx1, argv[1], argv[2]);
	TestAdvanced(dev, ctx1, argv[1], argv[2]);
	ReportAaAndDofCost(dev, ctx1, argv[1], argv[2]);
	ReportAdvancedCost(dev, ctx1, argv[1], argv[2]);

	ctx1->Release();
	ctx->Release();
	dev->Release();

	TestBridge();
	TestHooks(argv[1]);

	printf("\n%d passed, %d failed\n", g_passed, g_failed);

	return g_failed == 0 ? 0 : 1;
}

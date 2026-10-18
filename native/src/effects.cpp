#include "effects.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "log.h"

namespace
{
	typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

	// Diesel only reflects precompiled bytecode, but the game still loads d3dcompiler_47,
	// so it is always there to compile our source at runtime.
	D3DCompileFn Compiler()
	{
		static D3DCompileFn fn = nullptr;

		if (!fn)
		{
			HMODULE module = LoadLibraryA("d3dcompiler_47.dll");

			if (!module)
				module = LoadLibraryA("d3dcompiler_43.dll");

			if (module)
				fn = (D3DCompileFn)GetProcAddress(module, "D3DCompile");
		}

		return fn;
	}

	// The bytecode cache file: length-prefixed strings, written and read in the same order.
	void Put(std::string& out, const std::string& text)
	{
		uint32_t n = (uint32_t)text.size();
		out.append((const char*)&n, 4);
		out += text;
	}

	bool Take(const std::string& in, size_t& at, std::string& text)
	{
		uint32_t n = 0;

		if (in.size() - at < 4)
			return false;

		memcpy(&n, in.data() + at, 4);
		at += 4;

		if (in.size() - at < n)
			return false;

		text.assign(in, at, n);
		at += n;

		return true;
	}

	std::string ReadAll(const std::string& path)
	{
		std::ifstream file(path, std::ios::binary);
		std::stringstream ss;
		ss << file.rdbuf();

		return ss.str();
	}

	template <typename T> void SafeRelease(T*& p)
	{
		if (p)
		{
			p->Release();
			p = nullptr;
		}
	}

	bool IsTypeless(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			return true;
		default:
			return false;
		}
	}

	bool IsSrgb(DXGI_FORMAT f)
	{
		return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
	}

	// The typeless and sRGB members of an 8-bit format's family. Other formats have no sRGB view.
	bool SrgbTwin(DXGI_FORMAT f, DXGI_FORMAT* typeless, DXGI_FORMAT* srgb)
	{
		switch (f)
		{
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			*typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;
			*srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			return true;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			*typeless = DXGI_FORMAT_B8G8R8A8_TYPELESS;
			*srgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
			return true;
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			*typeless = DXGI_FORMAT_B8G8R8X8_TYPELESS;
			*srgb = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
			return true;
		default:
			return false;
		}
	}

	uint16_t ToHalf(float value)
	{
		uint32_t bits;
		memcpy(&bits, &value, sizeof(bits));

		uint32_t sign = (bits >> 16) & 0x8000;
		int32_t exponent = (int32_t)((bits >> 23) & 0xff) - 127 + 15;
		uint32_t mantissa = bits & 0x7fffff;

		if (exponent <= 0)
			return (uint16_t)sign;

		if (exponent >= 31)
			return (uint16_t)(sign | 0x7c00);

		return (uint16_t)(sign | (exponent << 10) | (mantissa >> 13));
	}

	// viewFormat: the format both views use when the texture itself is typeless.
	bool MakeTexture(ID3D11Device* dev, RenderTexture& rt, UINT w, UINT h, DXGI_FORMAT format, bool renderTarget, bool mips, DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = w;
		desc.Height = h;
		desc.MipLevels = mips ? 0 : 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
		desc.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

		if (FAILED(dev->CreateTexture2D(&desc, nullptr, &rt.tex)))
			return false;

		D3D11_RENDER_TARGET_VIEW_DESC rd = {viewFormat, D3D11_RTV_DIMENSION_TEXTURE2D};

		if (renderTarget && FAILED(dev->CreateRenderTargetView(rt.tex, viewFormat != DXGI_FORMAT_UNKNOWN ? &rd : nullptr, &rt.rtv)))
			return false;

		if (viewFormat != DXGI_FORMAT_UNKNOWN)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = viewFormat;
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = (UINT)-1;

			if (FAILED(dev->CreateShaderResourceView(rt.tex, &sd, &rt.srv)))
				return false;
		}
		else if (FAILED(dev->CreateShaderResourceView(rt.tex, nullptr, &rt.srv)))
		{
			return false;
		}

		rt.w = w;
		rt.h = h;

		return true;
	}
}

void RenderTexture::Release()
{
	SafeRelease(srv);
	SafeRelease(rtv);
	SafeRelease(tex);
	w = h = 0;
}

void History::Release()
{
	t[0].Release();
	t[1].Release();
	index = 0;
	reset = true;
}

void Effects::ReleaseTargets()
{
	m_src.Release();
	m_aa.Release();
	SafeRelease(m_srcSrgbSrv);
	SafeRelease(m_aaSrgbRtv);
	m_lum.Release();
	m_adapt[0].Release();
	m_adapt[1].Release();

	for (int i = 0; i < BLOOM_LEVELS; i++)
	{
		m_down[i].Release();
		m_up[i].Release();
	}

	m_grade.Release();
	m_t2xPrev.Release();
	m_blur[0].Release();
	m_blur[1].Release();

	for (int i = 0; i < WIDE_LEVELS; i++)
	{
		m_wide[i].Release();
		m_tint[i].Release();
		m_halo[i].Release();
	}

	m_light.Release();
	ReleaseScaled();
	ReleaseSync();
	m_ao.Release();
	m_aoBlur.Release();
	m_depthCopy.Release();
	m_depthLinear.Release();
	m_depthFormat = DXGI_FORMAT_UNKNOWN;
	m_smaaEdges.Release();
	m_smaaWeights.Release();
	m_dofHalf[0].Release();
	m_dofHalf[1].Release();
	m_dof.Release();
	m_focus[0].Release();
	m_focus[1].Release();
	m_focusReset = true;
	ReleaseAdvanced();
	ReleaseUpscale();
	m_targetW = m_targetH = 0;
	m_targetFormat = m_targetRtvFormat = DXGI_FORMAT_UNKNOWN;
	m_adaptReset = true;
}

void Effects::ReleaseShaders()
{
	SafeRelease(m_vs);
	SafeRelease(m_psFxaa);
	SafeRelease(m_psLuma);
	SafeRelease(m_psAdapt);
	SafeRelease(m_psBloomPrefilter);
	SafeRelease(m_psBloomDown);
	SafeRelease(m_psBloomUp);
	SafeRelease(m_psGrade);
	SafeRelease(m_psFinal);
	SafeRelease(m_psWideDown);
	SafeRelease(m_psTintPrefilter);
	SafeRelease(m_psHaloPrefilter);
	SafeRelease(m_psLightInfo);
	SafeRelease(m_psEasu);
	SafeRelease(m_psLinearDepth);
	SafeRelease(m_psTaa);
	SafeRelease(m_psSmaaT2x);
	SafeRelease(m_psTaaSync);
	SafeRelease(m_psAo);
	SafeRelease(m_psAoBlur);
	SafeRelease(m_psSmaaEdges);
	SafeRelease(m_psSmaaWeights);
	SafeRelease(m_psSmaaBlend);
	SafeRelease(m_psFocus);
	SafeRelease(m_psDofPrepare);
	SafeRelease(m_psDofBlur);
	SafeRelease(m_psDofFill);
	SafeRelease(m_psDofComposite);
	SafeRelease(m_psIl);
	SafeRelease(m_psRtgi);
	SafeRelease(m_psSsr);
	SafeRelease(m_psDepthBlur);
	SafeRelease(m_psMotionInput);
	SafeRelease(m_psMotionSearch);
	SafeRelease(m_psTemporal);
	SafeRelease(m_psMotionBlur);
	SafeRelease(m_psShaftMask);
	SafeRelease(m_psShafts);
	SafeRelease(m_psGrain);
	SafeRelease(m_psDownsample);
	m_compiled = false;
}

void Effects::Release()
{
	ReleaseTargets();
	ReleaseShaders();
	SafeRelease(m_lutSrv);
	SafeRelease(m_lutTex);
	SafeRelease(m_dirtSrv);
	SafeRelease(m_dirtTex);
	SafeRelease(m_smaaAreaSrv);
	SafeRelease(m_smaaAreaTex);
	SafeRelease(m_smaaSearchSrv);
	SafeRelease(m_smaaSearchTex);
	SafeRelease(m_cbFrame);
	SafeRelease(m_cbPass);
	SafeRelease(m_cbReproject);
	SafeRelease(m_cbSync);

	for (GpuFrame& f : m_gpu)
	{
		SafeRelease(f.disjoint);

		for (ID3D11Query*& q : f.stamp)
			SafeRelease(q);

		f.pending = false;
	}

	m_gpuSlot = 0;
	m_gpuMeasured = false;
	SafeRelease(m_linear);
	SafeRelease(m_point);
	SafeRelease(m_raster);
	SafeRelease(m_depth);
	SafeRelease(m_state);
	SafeRelease(m_device);
	m_lutSize = 0;
	m_shaderGen = 0;
	m_lutGen = 0;
	m_dirtGen = 0;
}

bool Effects::EnsureDevice(ID3D11Device* device)
{
	if (m_device == device && m_state)
		return true;

	Release();

	m_device = device;
	m_device->AddRef();

	ID3D11Device1* device1 = nullptr;

	if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), (void**)&device1)))
	{
		m_error = "device has no ID3D11Device1 (needed to swap pipeline state)";
		return false;
	}

	// Microsoft's docs: a device created SINGLETHREADED needs every context state object created with
	// D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED too. PAYDAY 2's is.
	UINT deviceFlags = device->GetCreationFlags();
	UINT stateFlags = deviceFlags & D3D11_CREATE_DEVICE_SINGLETHREADED ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
	D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
	HRESULT hr = device1->CreateDeviceContextState(stateFlags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &m_state);
	device1->Release();

	if (FAILED(hr))
	{
		char text[160];
		snprintf(text, sizeof(text), "CreateDeviceContextState failed (0x%08lx, device flags 0x%x, feature level 0x%x)", (unsigned long)hr, deviceFlags, (unsigned)level);
		m_error = text;
		return false;
	}

	D3D11_BUFFER_DESC cb = {};
	cb.Usage = D3D11_USAGE_DYNAMIC;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cb.ByteWidth = (8 + PARAM_SLOTS) * 4;
	device->CreateBuffer(&cb, nullptr, &m_cbFrame);
	cb.ByteWidth = 32;
	device->CreateBuffer(&cb, nullptr, &m_cbPass);
	cb.ByteWidth = 14 * 16;
	device->CreateBuffer(&cb, nullptr, &m_cbReproject);
	cb.ByteWidth = 32 * 16;
	device->CreateBuffer(&cb, nullptr, &m_cbSync);

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	device->CreateSamplerState(&sd, &m_linear);
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	device->CreateSamplerState(&sd, &m_point);

	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	device->CreateRasterizerState(&rd, &m_raster);

	D3D11_DEPTH_STENCIL_DESC dd = {};
	dd.DepthEnable = FALSE;
	dd.StencilEnable = FALSE;
	device->CreateDepthStencilState(&dd, &m_depth);

	if (!m_cbFrame || !m_cbPass || !m_cbReproject || !m_cbSync || !m_linear || !m_point || !m_raster || !m_depth)
	{
		m_error = "failed to create fixed pipeline objects";
		return false;
	}

	// Timing only. A query that cannot be made leaves that frame's set empty and untimed.
	for (GpuFrame& f : m_gpu)
	{
		D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
		bool ok = SUCCEEDED(device->CreateQuery(&qd, &f.disjoint));
		qd.Query = D3D11_QUERY_TIMESTAMP;

		for (ID3D11Query*& q : f.stamp)
			ok = ok && SUCCEEDED(device->CreateQuery(&qd, &q));

		if (!ok)
		{
			SafeRelease(f.disjoint);

			for (ID3D11Query*& q : f.stamp)
				SafeRelease(q);
		}
	}

	return true;
}

bool Effects::Compile(const std::string& path)
{
	ReleaseShaders();

	D3DCompileFn compile = Compiler();

	if (!compile)
	{
		m_error = "d3dcompiler_47.dll not available";
		return false;
	}

	std::ifstream file(path, std::ios::binary);

	if (!file)
	{
		m_error = "cannot open shader file: " + path;
		return false;
	}

	std::stringstream ss;
	ss << file.rdbuf();
	std::string source = ss.str();

	// #include "name" lines are pasted in here, from beside the shader, rather than left to the
	// compiler: Wine's d3dcompiler resolves includes against the game's working directory, not the
	// shader's. One level only, which is all this mod uses.
	std::string dir = path.substr(0, path.find_last_of("/\\") + 1);

	for (size_t at = source.find("#include \""); at != std::string::npos; at = source.find("#include \"", at))
	{
		size_t nameStart = at + 10;
		size_t nameEnd = source.find('"', nameStart);
		size_t lineEnd = nameEnd == std::string::npos ? std::string::npos : source.find('\n', nameEnd);

		if (nameEnd == std::string::npos)
		{
			m_error = "unterminated #include in " + path;
			return false;
		}

		std::string name = source.substr(nameStart, nameEnd - nameStart);
		std::ifstream included(dir + name, std::ios::binary);

		if (!included)
		{
			m_error = "cannot open included shader file: " + dir + name;
			return false;
		}

		std::stringstream text;
		text << included.rdbuf();
		std::string body = text.str() + "\n";

		source.replace(at, (lineEnd == std::string::npos ? source.size() : lineEnd) - at, body);
		at += body.size();
	}

	struct Entry
	{
		const char* name;
		bool vertex;
		void* out;
	};

	Entry entries[] = {
		{"VS_Fullscreen", true, &m_vs},
		{"PS_FXAA", false, &m_psFxaa},
		{"PS_Luma", false, &m_psLuma},
		{"PS_Adapt", false, &m_psAdapt},
		{"PS_BloomPrefilter", false, &m_psBloomPrefilter},
		{"PS_BloomDown", false, &m_psBloomDown},
		{"PS_BloomUp", false, &m_psBloomUp},
		{"PS_Grade", false, &m_psGrade},
		{"PS_Final", false, &m_psFinal},
		{"PS_WideDown", false, &m_psWideDown},
		{"PS_TintPrefilter", false, &m_psTintPrefilter},
		{"PS_HaloPrefilter", false, &m_psHaloPrefilter},
		{"PS_LightInfo", false, &m_psLightInfo},
		{"PS_Easu", false, &m_psEasu},
		{"PS_LinearDepth", false, &m_psLinearDepth},
		{"PS_Taa", false, &m_psTaa},
		{"PS_SmaaT2x", false, &m_psSmaaT2x},
		{"PS_TaaSync", false, &m_psTaaSync},
		{"PS_Ao", false, &m_psAo},
		{"PS_AoBlur", false, &m_psAoBlur},
		{"PS_SmaaEdges", false, &m_psSmaaEdges},
		{"PS_SmaaWeights", false, &m_psSmaaWeights},
		{"PS_SmaaBlend", false, &m_psSmaaBlend},
		{"PS_Focus", false, &m_psFocus},
		{"PS_DofPrepare", false, &m_psDofPrepare},
		{"PS_DofBlur", false, &m_psDofBlur},
		{"PS_DofFill", false, &m_psDofFill},
		{"PS_DofComposite", false, &m_psDofComposite},
		{"PS_Il", false, &m_psIl},
		{"PS_Rtgi", false, &m_psRtgi},
		{"PS_Ssr", false, &m_psSsr},
		{"PS_DepthBlur", false, &m_psDepthBlur},
		{"PS_MotionInput", false, &m_psMotionInput},
		{"PS_MotionSearch", false, &m_psMotionSearch},
		{"PS_Temporal", false, &m_psTemporal},
		{"PS_MotionBlur", false, &m_psMotionBlur},
		{"PS_ShaftMask", false, &m_psShaftMask},
		{"PS_Shafts", false, &m_psShafts},
		{"PS_Grain", false, &m_psGrain},
		{"PS_Downsample", false, &m_psDownsample},
	};

	const size_t count = sizeof(entries) / sizeof(entries[0]);
	const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	auto profile = [](const Entry& e) { return e.vertex ? "vs_5_0" : "ps_5_0"; };

	// Compiling is what freezes the game for seconds, so the bytecode is kept beside the shader
	// (pss.hlsl.cache) together with the exact source it came from. The same source next time
	// skips the compiler; any difference, a damaged file or bytecode the driver refuses compiles
	// as before.
	std::string cachePath = path + ".cache";
	std::string header = "pss shader cache 1, flags " + std::to_string(flags);
	std::vector<std::string> code(count);

	for (int attempt = 0; attempt < 2; attempt++)
	{
		DWORD started = GetTickCount();
		bool cached = false;

		if (attempt == 0)
		{
			std::string file = ReadAll(cachePath), text;
			size_t at = 0;
			cached = Take(file, at, text) && text == header && Take(file, at, text) && text == source;

			for (size_t i = 0; cached && i < count; i++)
				cached = Take(file, at, text) && text == std::string(entries[i].name) + " " + profile(entries[i]) && Take(file, at, code[i]);

			cached = cached && at == file.size();
		}

		if (!cached)
		{
			std::string file;
			Put(file, header);
			Put(file, source);

			for (size_t i = 0; i < count; i++)
			{
				const Entry& e = entries[i];
				ID3DBlob* blob = nullptr;
				ID3DBlob* errors = nullptr;
				HRESULT hr = compile(source.data(), source.size(), "pss.hlsl", nullptr, nullptr, e.name, profile(e), flags, 0, &blob, &errors);

				if (FAILED(hr) || !blob)
				{
					m_error = std::string("compile failed in ") + e.name;

					if (errors)
						m_error += ": " + std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize());

					SafeRelease(errors);
					SafeRelease(blob);

					return false;
				}

				SafeRelease(errors);
				code[i].assign((const char*)blob->GetBufferPointer(), blob->GetBufferSize());
				blob->Release();
				Put(file, std::string(e.name) + " " + profile(e));
				Put(file, code[i]);
			}

			// Written beside and then moved over, so a crash mid-write never leaves half a cache.
			std::string temp = cachePath + ".tmp";
			std::ofstream out(temp, std::ios::binary);
			bool written = out.write(file.data(), file.size()).good();
			out.close();

			if (!written || !MoveFileExA(temp.c_str(), cachePath.c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				DeleteFileA(temp.c_str());
				Log("could not write the shader cache %s; the next launch compiles again", cachePath.c_str());
			}
		}

		DWORD loaded = GetTickCount();
		HRESULT hr = S_OK;

		for (size_t i = 0; i < count && SUCCEEDED(hr); i++)
		{
			const Entry& e = entries[i];

			if (e.vertex)
				hr = m_device->CreateVertexShader(code[i].data(), code[i].size(), nullptr, (ID3D11VertexShader**)e.out);
			else
				hr = m_device->CreatePixelShader(code[i].data(), code[i].size(), nullptr, (ID3D11PixelShader**)e.out);

			if (FAILED(hr))
				m_error = std::string("create shader failed for ") + e.name;
		}

		if (SUCCEEDED(hr))
		{
			m_compiled = true;
			m_error.clear();
			LoadSmaa(dir);
			Log("%s: %s in %lu ms, shader objects created in %lu ms", path.c_str(), cached ? "read from the cache" : "compiled", (unsigned long)(loaded - started), (unsigned long)(GetTickCount() - loaded));

			return true;
		}

		ReleaseShaders();

		if (!cached)
			return false;

		Log("the driver refused the cached shaders (%s); compiling", m_error.c_str());
	}

	return false;
}

// .cube (Adobe/Resolve) 3D LUT. Red changes fastest in the file, which is exactly the texel
// order of a D3D Texture3D (x, then y, then z), so the data goes up as read.
void Effects::LoadLut(const std::string& path)
{
	SafeRelease(m_lutSrv);
	SafeRelease(m_lutTex);
	m_lutSize = 0;

	if (path.empty())
		return;

	std::ifstream file(path);

	if (!file)
	{
		Log("cannot open LUT %s", path.c_str());
		return;
	}

	int size = 0;
	std::vector<float> values;
	std::string line;

	while (std::getline(file, line))
	{
		size_t start = line.find_first_not_of(" \t\r");

		if (start == std::string::npos || line[start] == '#')
			continue;

		line = line.substr(start);

		if (line.compare(0, 11, "LUT_3D_SIZE") == 0)
		{
			size = atoi(line.c_str() + 11);
			continue;
		}

		if (line.compare(0, 11, "LUT_1D_SIZE") == 0)
		{
			Log("LUT %s is 1D, only 3D LUTs are supported", path.c_str());
			return;
		}

		if (isalpha((unsigned char)line[0]))
			continue; // TITLE, DOMAIN_MIN, DOMAIN_MAX

		float r, g, b;

		if (sscanf(line.c_str(), "%f %f %f", &r, &g, &b) == 3)
		{
			values.push_back(r);
			values.push_back(g);
			values.push_back(b);
		}
	}

	if (size < 2 || size > 128 || values.size() != (size_t)size * size * size * 3)
	{
		Log("LUT %s is malformed (size %d, %lu values)", path.c_str(), size, (unsigned long)(values.size() / 3));
		return;
	}

	std::vector<uint16_t> texels((size_t)size * size * size * 4);

	for (size_t i = 0, n = values.size() / 3; i < n; i++)
	{
		texels[i * 4] = ToHalf(values[i * 3]);
		texels[i * 4 + 1] = ToHalf(values[i * 3 + 1]);
		texels[i * 4 + 2] = ToHalf(values[i * 3 + 2]);
		texels[i * 4 + 3] = ToHalf(1.0f);
	}

	D3D11_TEXTURE3D_DESC desc = {};
	desc.Width = desc.Height = desc.Depth = size;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA data = {};
	data.pSysMem = texels.data();
	data.SysMemPitch = size * 8;
	data.SysMemSlicePitch = size * size * 8;

	if (FAILED(m_device->CreateTexture3D(&desc, &data, &m_lutTex)) || FAILED(m_device->CreateShaderResourceView(m_lutTex, nullptr, &m_lutSrv)))
	{
		SafeRelease(m_lutTex);
		Log("failed to create LUT texture for %s", path.c_str());
		return;
	}

	m_lutSize = size;
	Log("loaded LUT %s (%d^3)", path.c_str(), size);
}

// The lens dirt photograph, converted to a flat blob by dev/nativebuild/make_dirt.py:
// "PSSD", width, height, then one grey byte per texel. Anything wrong with the file leaves the
// texture null, and Run() then skips the effect entirely rather than guessing at a pattern.
namespace
{
	// A PSSS blob: "PSSS", width, height, channels (uint32 each), then the texels.
	bool LoadLookup(ID3D11Device* device, const std::string& path, UINT w, UINT h, UINT channels, DXGI_FORMAT format, ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv)
	{
		std::ifstream file(path, std::ios::binary);

		if (!file)
		{
			Log("cannot open %s", path.c_str());
			return false;
		}

		char magic[4] = {};
		uint32_t header[3] = {};
		file.read(magic, 4);
		file.read((char*)header, sizeof(header));

		if (!file || memcmp(magic, "PSSS", 4) != 0 || header[0] != w || header[1] != h || header[2] != channels)
		{
			Log("%s is not the expected %ux%u lookup texture", path.c_str(), w, h);
			return false;
		}

		std::vector<uint8_t> texels((size_t)w * h * channels);
		file.read((char*)texels.data(), (std::streamsize)texels.size());

		if ((size_t)file.gcount() != texels.size())
		{
			Log("%s is truncated", path.c_str());
			return false;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = w;
		desc.Height = h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA data = {texels.data(), w * channels, 0};

		if (FAILED(device->CreateTexture2D(&desc, &data, tex)) || FAILED(device->CreateShaderResourceView(*tex, nullptr, srv)))
		{
			SafeRelease(*srv);
			SafeRelease(*tex);
			return false;
		}

		return true;
	}
}

void Effects::LoadSmaa(const std::string& dir)
{
	SafeRelease(m_smaaAreaSrv);
	SafeRelease(m_smaaAreaTex);
	SafeRelease(m_smaaSearchSrv);
	SafeRelease(m_smaaSearchTex);

	bool area = LoadLookup(m_device, dir + "smaa_area.bin", 160, 560, 2, DXGI_FORMAT_R8G8_UNORM, &m_smaaAreaTex, &m_smaaAreaSrv);
	bool search = LoadLookup(m_device, dir + "smaa_search.bin", 64, 16, 1, DXGI_FORMAT_R8_UNORM, &m_smaaSearchTex, &m_smaaSearchSrv);

	if (area && search)
	{
		Log("loaded SMAA lookup textures");
	}
	else
	{
		SafeRelease(m_smaaAreaSrv);
		SafeRelease(m_smaaAreaTex);
		SafeRelease(m_smaaSearchSrv);
		SafeRelease(m_smaaSearchTex);
		Log("SMAA lookup textures missing; antialiasing will use FXAA");
	}
}

void Effects::LoadDirt(const std::string& path)
{
	SafeRelease(m_dirtSrv);
	SafeRelease(m_dirtTex);

	if (path.empty())
		return;

	std::ifstream file(path, std::ios::binary);

	if (!file)
	{
		Log("cannot open lens dirt %s", path.c_str());
		return;
	}

	char magic[4] = {};
	uint32_t w = 0;
	uint32_t h = 0;

	file.read(magic, 4);
	file.read((char*)&w, 4);
	file.read((char*)&h, 4);

	if (!file || memcmp(magic, "PSSD", 4) != 0 || w < 8 || h < 8 || w > 8192 || h > 8192)
	{
		Log("lens dirt %s is not a PSSD blob", path.c_str());
		return;
	}

	std::vector<uint8_t> texels((size_t)w * h);
	file.read((char*)texels.data(), (std::streamsize)texels.size());

	if ((size_t)file.gcount() != texels.size())
	{
		Log("lens dirt %s is truncated", path.c_str());
		return;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA data = {};
	data.pSysMem = texels.data();
	data.SysMemPitch = w;

	if (FAILED(m_device->CreateTexture2D(&desc, &data, &m_dirtTex)) || FAILED(m_device->CreateShaderResourceView(m_dirtTex, nullptr, &m_dirtSrv)))
	{
		SafeRelease(m_dirtTex);
		Log("failed to create the lens dirt texture from %s", path.c_str());
		return;
	}

	Log("loaded lens dirt %s (%ux%u)", path.c_str(), w, h);
}

bool Effects::EnsureTargets(const D3D11_TEXTURE2D_DESC& desc, DXGI_FORMAT rtvFormat)
{
	if (rtvFormat == DXGI_FORMAT_UNKNOWN)
		rtvFormat = desc.Format;

	if (m_src.tex && m_targetW == desc.Width && m_targetH == desc.Height && m_targetFormat == desc.Format && m_targetRtvFormat == rtvFormat)
		return true;

	ReleaseTargets();

	if (IsTypeless(rtvFormat))
	{
		m_error = "render target view has a typeless format";
		return false;
	}

	UINT w = desc.Width;
	UINT h = desc.Height;
	// SMAA's notes: the blend pass should read and write through sRGB views, so it blends in linear
	// light; in gamma space a blended edge comes out too dark. So the source copy and the AA target
	// are made typeless when the format has an sRGB twin, and get one extra sRGB view each.
	DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT srgb = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT unusedTypeless, unusedSrgb;
	bool twin = SrgbTwin(rtvFormat, &typeless, &srgb) && SrgbTwin(desc.Format, &unusedTypeless, &unusedSrgb) && unusedTypeless == typeless;
	bool ok = MakeTexture(m_device, m_src, w, h, twin ? typeless : desc.Format, false, false, twin || IsTypeless(desc.Format) ? rtvFormat : DXGI_FORMAT_UNKNOWN);

	ok = ok && MakeTexture(m_device, m_aa, w, h, twin ? typeless : rtvFormat, true, false, twin ? rtvFormat : DXGI_FORMAT_UNKNOWN);

	if (ok && twin)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = srgb;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		D3D11_RENDER_TARGET_VIEW_DESC rd = {srgb, D3D11_RTV_DIMENSION_TEXTURE2D};

		// Without them SMAA just blends in gamma space, as before.
		if (FAILED(m_device->CreateShaderResourceView(m_src.tex, &sd, &m_srcSrgbSrv)) || FAILED(m_device->CreateRenderTargetView(m_aa.tex, &rd, &m_aaSrgbRtv)))
		{
			SafeRelease(m_srcSrgbSrv);
			SafeRelease(m_aaSrgbRtv);
		}
	}
	ok = ok && MakeTexture(m_device, m_grade, w, h, rtvFormat, true, false);
	ok = ok && MakeTexture(m_device, m_t2xPrev, w, h, rtvFormat, true, false);
	ok = ok && MakeTexture(m_device, m_lum, 256, 256, DXGI_FORMAT_R16G16B16A16_FLOAT, true, true);
	ok = ok && MakeTexture(m_device, m_adapt[0], 1, 1, DXGI_FORMAT_R32_FLOAT, true, false);
	ok = ok && MakeTexture(m_device, m_adapt[1], 1, 1, DXGI_FORMAT_R32_FLOAT, true, false);

	for (int i = 0; ok && i < BLOOM_LEVELS; i++)
	{
		UINT lw = w >> (i + 1);
		UINT lh = h >> (i + 1);

		lw = lw ? lw : 1;
		lh = lh ? lh : 1;
		ok = MakeTexture(m_device, m_down[i], lw, lh, DXGI_FORMAT_R11G11B10_FLOAT, true, false);
		ok = ok && MakeTexture(m_device, m_up[i], lw, lh, DXGI_FORMAT_R11G11B10_FLOAT, true, false);
	}

	ok = ok && MakeTexture(m_device, m_blur[0], w > 1 ? w / 2 : 1, h > 1 ? h / 2 : 1, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);
	ok = ok && MakeTexture(m_device, m_blur[1], w > 3 ? w / 4 : 1, h > 3 ? h / 4 : 1, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);

	for (int i = 0; ok && i < WIDE_LEVELS; i++)
	{
		UINT lw = w >> (i + 3);
		UINT lh = h >> (i + 3);

		lw = lw ? lw : 1;
		lh = lh ? lh : 1;
		ok = MakeTexture(m_device, m_wide[i], lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);
		ok = ok && MakeTexture(m_device, m_tint[i], lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);
		ok = ok && MakeTexture(m_device, m_halo[i], lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);
	}

	// One pixel: where the bright light in the frame is, and how much of it there is (lens dirt).
	ok = ok && MakeTexture(m_device, m_light, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);

	// Ambient occlusion, half resolution: the taps are what costs, and the blur that follows only
	// averages neighbours at a similar distance, so the edges stay sharp regardless.
	ok = ok && MakeTexture(m_device, m_ao, w > 1 ? w / 2 : 1, h > 1 ? h / 2 : 1, DXGI_FORMAT_R16_FLOAT, true, false);
	ok = ok && MakeTexture(m_device, m_aoBlur, w > 1 ? w / 2 : 1, h > 1 ? h / 2 : 1, DXGI_FORMAT_R16_FLOAT, true, false);

	ok = ok && MakeTexture(m_device, m_smaaEdges, w, h, DXGI_FORMAT_R8G8_UNORM, true, false);
	ok = ok && MakeTexture(m_device, m_smaaWeights, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, false);

	// Depth of field: half resolution float, because the blur radius is stored signed in alpha.
	ok = ok && MakeTexture(m_device, m_dofHalf[0], w > 1 ? w / 2 : 1, h > 1 ? h / 2 : 1, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);
	ok = ok && MakeTexture(m_device, m_dofHalf[1], w > 1 ? w / 2 : 1, h > 1 ? h / 2 : 1, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);
	ok = ok && MakeTexture(m_device, m_dof, w, h, rtvFormat, true, false);
	// r the focus distance, g how open the lens is (the aiming mode eases it).
	ok = ok && MakeTexture(m_device, m_focus[0], 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, true, false);
	ok = ok && MakeTexture(m_device, m_focus[1], 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, true, false);

	if (!ok)
	{
		ReleaseTargets();
		m_error = "failed to create intermediate textures";
		return false;
	}

	D3D11_TEXTURE2D_DESC lumDesc;
	m_lum.tex->GetDesc(&lumDesc);
	m_lumMips = lumDesc.MipLevels;

	m_targetW = w;
	m_targetH = h;
	m_targetFormat = desc.Format;
	m_targetRtvFormat = rtvFormat;
	Log("targets %ux%u format %d rtv %d", w, h, (int)desc.Format, (int)rtvFormat);

	return true;
}

void Effects::ReleaseAdvanced()
{
	for (int i = 0; i < 2; i++)
		m_motionPyr[i].Release();

	for (int i = 0; i < 3; i++)
		m_motion[i].Release();

	m_aoHist.Release();
	m_ilHist.Release();
	m_rtgiHist.Release();
	m_ssrHist.Release();
	m_grainHist.Release();
	m_taaHist.Release();
	m_il.Release();
	m_ilBlur.Release();
	m_rtgi.Release();
	m_rtgiBlur.Release();
	m_ssr.Release();
	m_shaftMask.Release();
	m_shafts.Release();
	m_mblur.Release();
	m_motionIndex = 0;
	m_motionReset = true;
	m_advanced = false;
	m_advancedW = m_advancedH = 0;
}

bool Effects::EnsureAdvanced(UINT w, UINT h)
{
	if (m_advanced && m_advancedW == w && m_advancedH == h)
		return true;

	ReleaseAdvanced();

	UINT hw = w > 1 ? w / 2 : 1;
	UINT hh = h > 1 ? h / 2 : 1;
	const DXGI_FORMAT half = DXGI_FORMAT_R16G16B16A16_FLOAT;
	bool ok = true;

	for (int i = 0; ok && i < 2; i++)
		ok = MakeTexture(m_device, m_motionPyr[i], hw, hh, DXGI_FORMAT_R16G16_FLOAT, true, true);

	for (int level = 0; ok && level < 3; level++)
	{
		UINT lw = hw >> level;
		UINT lh = hh >> level;
		ok = MakeTexture(m_device, m_motion[level], lw ? lw : 1, lh ? lh : 1, half, true, false);
	}

	for (int i = 0; ok && i < 2; i++)
	{
		ok = MakeTexture(m_device, m_aoHist.t[i], hw, hh, DXGI_FORMAT_R16_FLOAT, true, false);
		ok = ok && MakeTexture(m_device, m_ilHist.t[i], hw, hh, half, true, false);
		ok = ok && MakeTexture(m_device, m_rtgiHist.t[i], hw, hh, half, true, false);
		ok = ok && MakeTexture(m_device, m_ssrHist.t[i], hw, hh, half, true, false);
		// Grain is full resolution (it is pixel-fine by nature) but only one 8-bit channel.
		ok = ok && MakeTexture(m_device, m_grainHist.t[i], w, h, DXGI_FORMAT_R8_UNORM, true, false);
		// Float: blending a tenth of each frame in drifts by less than an 8-bit step, and would stall.
		ok = ok && MakeTexture(m_device, m_taaHist.t[i], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);
	}

	ok = ok && MakeTexture(m_device, m_il, hw, hh, half, true, false);
	ok = ok && MakeTexture(m_device, m_ilBlur, hw, hh, half, true, false);
	ok = ok && MakeTexture(m_device, m_rtgi, hw, hh, half, true, false);
	ok = ok && MakeTexture(m_device, m_rtgiBlur, hw, hh, half, true, false);
	ok = ok && MakeTexture(m_device, m_ssr, hw, hh, half, true, false);
	ok = ok && MakeTexture(m_device, m_shaftMask, hw, hh, half, true, false);
	ok = ok && MakeTexture(m_device, m_shafts, hw, hh, half, true, false);
	ok = ok && MakeTexture(m_device, m_mblur, w, h, m_targetRtvFormat, true, false);

	if (!ok)
	{
		ReleaseAdvanced();

		if (!m_loggedAdvancedFailure)
		{
			m_loggedAdvancedFailure = true;
			Log("could not make the buffers for lighting, motion and shafts; those effects stay off");
		}

		return false;
	}

	m_advanced = true;
	m_advancedW = w;
	m_advancedH = h;

	return true;
}

ID3D11ShaderResourceView* Effects::Accumulate(ID3D11DeviceContext* ctx, History& history, ID3D11ShaderResourceView* now, float keep, bool motionOk, ID3D11ShaderResourceView* previousPyramid)
{
	int next = 1 - history.index;
	ID3D11ShaderResourceView* inputs[MAX_INPUTS] = {};
	inputs[0] = now;
	inputs[1] = history.t[history.index].srv;
	inputs[2] = m_motion[0].srv;
	inputs[3] = previousPyramid;
	inputs[14] = m_depthLinear.srv;

	const RenderTexture& out = history.t[next];
	Pass(ctx, m_psTemporal, out.rtv, out.w, out.h, (float)out.w, (float)out.h, inputs, MAX_INPUTS, keep, history.reset || !motionOk ? 1.0f : 0.0f);

	history.index = next;
	history.reset = false;

	return out.srv;
}

void Effects::ReleaseScaled()
{
	m_scaled.Release();
	m_rescaled.Release();
	m_scaledW = m_scaledH = 0;
}

// The smaller buffer the frame is shrunk into, and the screen-size one it is rebuilt in. Made only
// while the render scale is below 1, and given back when it returns.
bool Effects::EnsureScaled(UINT w, UINT h, float scale)
{
	UINT sw = (UINT)(w * scale);
	UINT sh = (UINT)(h * scale);

	sw = sw > 16 ? sw : 16;
	sh = sh > 16 ? sh : 16;

	if (m_scaled.tex && m_scaledW == sw && m_scaledH == sh && m_rescaled.w == w)
		return true;

	ReleaseScaled();

	if (!MakeTexture(m_device, m_scaled, sw, sh, m_targetRtvFormat, true, false) ||
		!MakeTexture(m_device, m_rescaled, w, h, m_targetRtvFormat, true, false))
	{
		ReleaseScaled();
		return false;
	}

	m_scaledW = sw;
	m_scaledH = sh;

	return true;
}

void Effects::ReleaseUpscale()
{
	m_upscale.Release();
	m_upW = m_upH = 0;
	m_upFormat = DXGI_FORMAT_UNKNOWN;
}

// One buffer at scale x the screen, for EASU. It is the only large allocation that depends on a
// setting, so a failure here is not fatal - Run just leaves the stage off for this frame and the
// frame goes out at screen resolution as before.
bool Effects::EnsureUpscale(UINT w, UINT h, float scale)
{
	UINT uw = (UINT)((float)w * scale + 0.5f);
	UINT uh = (UINT)((float)h * scale + 0.5f);

	uw = uw ? uw : 1;
	uh = uh ? uh : 1;

	if (m_upscale.tex && m_upW == uw && m_upH == uh && m_upFormat == m_targetRtvFormat)
		return true;

	ReleaseUpscale();

	if (!MakeTexture(m_device, m_upscale, uw, uh, m_targetRtvFormat, true, false))
	{
		ReleaseUpscale();
		Log("could not create the %ux%u upscaling buffer; FSR stays off", uw, uh);

		return false;
	}

	m_upW = uw;
	m_upH = uh;
	m_upFormat = m_targetRtvFormat;
	Log("upscale targets %ux%u (%.2fx)", uw, uh, (double)scale);

	return true;
}

void Effects::WriteBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer, const void* data, size_t size)
{
	D3D11_MAPPED_SUBRESOURCE mapped;

	if (SUCCEEDED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		memcpy(mapped.pData, data, size);
		ctx->Unmap(buffer, 0);
	}
}

void Effects::Pass(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT w, UINT h, float srcW, float srcH, ID3D11ShaderResourceView* const* srvs, UINT count, float extra0, float extra1)
{
	ID3D11ShaderResourceView* nulls[MAX_INPUTS] = {};

	// Unbind inputs before binding the output: a texture cannot be both at once.
	ctx->PSSetShaderResources(0, MAX_INPUTS, nulls);
	ctx->OMSetRenderTargets(1, &rtv, nullptr);

	D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
	ctx->RSSetViewports(1, &vp);

	float pass[8] = {1.0f / srcW, 1.0f / srcH, srcW, srcH, extra0, extra1, 1.0f / (float)w, 1.0f / (float)h};
	WriteBuffer(ctx, m_cbPass, pass, sizeof(pass));

	ctx->PSSetShaderResources(0, count, srvs);
	ctx->PSSetShader(ps, nullptr, 0);
	ctx->Draw(3, 0);
}

// The game's depth target, copied into a texture of our own. A shader resource view can only be
// made on a texture the game created with that bind flag, which is not a mod's choice; a copy has
// no such condition, so this works whatever Diesel asked for.
bool Effects::EnsureDepth(ID3D11DeviceContext1* ctx, ID3D11Texture2D* depth)
{
	D3D11_TEXTURE2D_DESC desc;
	depth->GetDesc(&desc);

	// Any failure gives the copy back. Keeping it would leave the status file saying "readable"
	// while the shader is being told there is no depth at all, which is exactly the kind of
	// disagreement that wastes an evening.
	if (desc.SampleDesc.Count != 1)
	{
		m_depthCopy.Release();
		m_depthLinear.Release();
		m_depthFormat = DXGI_FORMAT_UNKNOWN;
		m_depthError = "the game's depth target is multisampled";

		return false;
	}

	if (IsTypeless(desc.Format))
	{
		m_depthCopy.Release();
		m_depthLinear.Release();
		m_depthFormat = DXGI_FORMAT_UNKNOWN;
		m_depthError = "the game's depth target has a typeless format";

		return false;
	}

	if (!m_depthCopy.tex || m_depthCopy.w != desc.Width || m_depthCopy.h != desc.Height || m_depthFormat != desc.Format)
	{
		m_depthCopy.Release();
		m_depthLinear.Release();
		m_depthFormat = DXGI_FORMAT_UNKNOWN;

		if (!MakeTexture(m_device, m_depthCopy, desc.Width, desc.Height, desc.Format, false, false) || !MakeTexture(m_device, m_depthLinear, desc.Width, desc.Height, DXGI_FORMAT_R32_FLOAT, true, false))
		{
			m_depthCopy.Release();
			m_depthLinear.Release();
			m_depthError = "could not make a texture to copy the depth into";

			return false;
		}

		m_depthFormat = desc.Format;
	}

	ctx->CopyResource(m_depthCopy.tex, depth);
	m_depthError.clear();

	return true;
}

// GPU time. Each frame's timestamps are read GPU_FRAMES frames later without waiting; a set that
// is still not ready is simply polled again next frame, and that frame goes untimed.
void Effects::GpuBegin(ID3D11DeviceContext* ctx)
{
	GpuFrame& f = m_gpu[m_gpuSlot];
	m_gpuTiming = false;

	if (!f.disjoint)
		return;

	if (f.pending)
	{
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
		UINT64 t[GPU_STAGES + 1];

		if (ctx->GetData(f.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			return;

		for (int i = 0; i <= GPU_STAGES; i++)
		{
			if (ctx->GetData(f.stamp[i], &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				return;
		}

		f.pending = false;

		// Disjoint means the clock changed mid-frame (power state, for example): that frame is dropped.
		if (!dj.Disjoint && dj.Frequency)
		{
			for (int i = 0; i < GPU_STAGES; i++)
			{
				float ms = t[i + 1] > t[i] ? (float)((double)(t[i + 1] - t[i]) * 1000.0 / (double)dj.Frequency) : 0.0f;
				m_gpuMs[i] = m_gpuMeasured ? m_gpuMs[i] + (ms - m_gpuMs[i]) * 0.05f : ms; // eased over ~20 frames
			}

			m_gpuMeasured = true;
		}
	}

	ctx->Begin(f.disjoint);
	ctx->End(f.stamp[0]);
	m_gpuTiming = true;
}

void Effects::GpuMark(ID3D11DeviceContext* ctx, GpuStage stage)
{
	if (m_gpuTiming)
		ctx->End(m_gpu[m_gpuSlot].stamp[stage + 1]);
}

void Effects::GpuEnd(ID3D11DeviceContext* ctx)
{
	if (!m_gpuTiming)
		return;

	ctx->End(m_gpu[m_gpuSlot].disjoint);
	m_gpu[m_gpuSlot].pending = true;
	m_gpuSlot = (m_gpuSlot + 1) % GPU_FRAMES;
}

void Effects::ReleaseSync()
{
	m_syncOut.Release();
	m_depthPrev.Release();
	m_depthPrevFrame = -1.0f;

	for (SyncRead& r : m_syncRead)
	{
		SafeRelease(r.tex);
		r.pending = false;
	}
}

namespace
{
	// PSS_TEST_SYNC_LOG: log every TaaSync answer, for the test host.
	const bool g_syncLog = GetEnvironmentVariableA("PSS_TEST_SYNC_LOG", nullptr, 0) != 0;

	float HalfToFloat(uint16_t h)
	{
		int e = (h >> 10) & 31;
		int m = h & 1023;
		float v = e == 0 ? ldexpf((float)m, -24) : (e == 31 ? 1.0e9f : ldexpf((float)(m | 1024), e - 25));

		return h & 0x8000 ? -v : v;
	}
}

// SMAA T2x: which of the area textures' two subsample sets (smaa.hlsl, @SUBSAMPLE_INDICES) belongs to the
// frame jittered to the right; the other frame takes the other. Set 1, as the reference's table pairs it
// with (0.25, -0.25); it also scored best in the harness (dev/NOTES.md).
static const float kT2xSubsampleRight = 1.0f;

// Which camera line was this frame drawn with? When the game draws on a thread of its own, the line
// Lua wrote as the frame started may already have been followed by the next one when the shaders run.
// Each candidate pair of cameras (lines 0, 1 and 2 back, and the settled one without its jitter) moves
// this frame's depth back into last frame's view; the right pair predicts last frame's depth best
// (PS_TaaSync). Only changes of motion tell candidates apart (a steady turn looks the same from any
// pair), so only clear answers are counted. The result is read back a few frames later, never waited on.
void Effects::TaaSync(ID3D11DeviceContext* ctx, const Settings& s, int back)
{
	// Answers that have arrived.
	for (int i = 0; i < SYNC_SLOTS; i++)
	{
		SyncRead& r = m_syncRead[(m_syncSlot + i) % SYNC_SLOTS];
		D3D11_MAPPED_SUBRESOURCE m;

		if (!r.pending || ctx->Map(r.tex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK)
			continue;

		const uint16_t* h = (const uint16_t*)m.pData;
		float result[4] = {HalfToFloat(h[0]), HalfToFloat(h[1]), HalfToFloat(h[2]), HalfToFloat(h[3])};
		ctx->Unmap(r.tex, 0);
		r.pending = false;

		if (g_syncLog)
			Log("sync back %d jittered %d: %.5f %.5f %.5f %.5f", r.back, r.jittered ? 1 : 0, result[0], result[1], result[2], result[3]);

		SyncResult(result, r);
	}

	if (m_depthPrev.tex && (m_depthPrev.w != m_depthLinear.w || m_depthPrev.h != m_depthLinear.h))
		ReleaseSync();

	if (!m_syncOut.tex)
	{
		bool ok = MakeTexture(m_device, m_syncOut, 128, 64, DXGI_FORMAT_R16G16B16A16_FLOAT, true, true) &&
			MakeTexture(m_device, m_depthPrev, m_depthLinear.w, m_depthLinear.h, DXGI_FORMAT_R32_FLOAT, false, false);
		D3D11_TEXTURE2D_DESC sd = {1, 1, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};

		for (SyncRead& r : m_syncRead)
			ok = ok && SUCCEEDED(m_device->CreateTexture2D(&sd, nullptr, &r.tex));

		if (!ok)
		{
			ReleaseSync();

			return;
		}
	}

	// Candidate c: this frame drawn with line c back, last frame with line c + 1. Each needs both lines,
	// one frame apart, and last frame's depth must be the frame just before this one.
	const CameraPose* p = s.recent;
	bool valid[4] = {};
	float c[32 * 4] = {};

	for (int k = 0; k < 4; k++)
	{
		int at = k < 3 ? k : back;

		if (at + 1 >= CAMERA_LINES || !p[at].valid || !p[at + 1].valid || p[at].frame != p[at + 1].frame + 1.0f)
			continue;

		const CameraPose& a = p[at];
		const CameraPose& b = p[at + 1];
		bool drawn = k < 3; // the fourth is the settled line without its jitter
		const float* axes[6] = {drawn ? a.right : a.outRight, drawn ? a.up : a.outUp, drawn ? a.fwd : a.outFwd, drawn ? b.right : b.outRight, drawn ? b.up : b.outUp, drawn ? b.fwd : b.outFwd};
		float* o = c + k * 32;

		for (int i = 0; i < 6; i++)
			memcpy(o + i * 4, axes[i], 3 * sizeof(float));

		o[24] = (float)(a.pos[0] - b.pos[0]);
		o[25] = (float)(a.pos[1] - b.pos[1]);
		o[26] = (float)(a.pos[2] - b.pos[2]);
		o[28] = a.tan[0];
		o[29] = a.tan[1];
		o[30] = b.tan[0];
		o[31] = b.tan[1];
		valid[k] = true;
	}

	SyncRead& slot = m_syncRead[m_syncSlot];
	bool measure = valid[back] && m_depthPrevFrame >= 0.0f && p[0].frame == m_depthPrevFrame + 1.0f && !slot.pending;

	if (measure)
	{
		// An invalid candidate gets a camera that sees nothing (zero tangents): the worst score.
		WriteBuffer(ctx, m_cbSync, c, sizeof(c));
		ctx->PSSetConstantBuffers(3, 1, &m_cbSync);
		ID3D11ShaderResourceView* inputs[MAX_INPUTS] = {nullptr, m_depthPrev.srv};
		inputs[14] = m_depthLinear.srv;
		Pass(ctx, m_psTaaSync, m_syncOut.rtv, m_syncOut.w, m_syncOut.h, (float)m_depthLinear.w, (float)m_depthLinear.h, inputs, MAX_INPUTS);
		ctx->OMSetRenderTargets(0, nullptr, nullptr);
		ctx->GenerateMips(m_syncOut.srv);
		ctx->CopySubresourceRegion(slot.tex, 0, 0, 0, 0, m_syncOut.tex, 7, nullptr);

		const CameraPose& a = p[back];
		const CameraPose& b = p[back + 1];
		slot.jittered = memcmp(a.fwd, a.outFwd, sizeof(a.fwd)) != 0 || memcmp(b.fwd, b.outFwd, sizeof(b.fwd)) != 0;
		slot.back = back;
		slot.frame = a.frame;
		slot.height = (float)a.pos[2];
		slot.pitch = a.pitch;
		slot.roll = a.roll;
		slot.aim = a.aim;
		slot.zoom = fabsf(a.tan[0] / b.tan[0] - 1.0f) > 1.0e-4f;
		memcpy(slot.state, a.state, sizeof(slot.state));
		memcpy(slot.valid, valid, sizeof(valid));
		slot.pending = true;
		m_syncSlot = (m_syncSlot + 1) % SYNC_SLOTS;
	}

}

void Effects::SyncResult(const float r[4], const SyncRead& read)
{
	// Which line back predicts this frame best, when it clearly does. Only candidates that had both
	// their lines take part.
	int best = -1, second = -1;

	for (int k = 0; k < 3; k++)
	{
		if (!read.valid[k])
			continue;

		if (best < 0 || r[k] < r[best])
			second = best, best = k;
		else if (second < 0 || r[k] < r[second])
			second = k;
	}

	if (second >= 0 && r[best] < 0.8f * r[second] && r[second] - r[best] > 2.0e-4f)
	{
		m_votes[m_voteAt] = (unsigned char)best;
		m_voteAt = (m_voteAt + 1) % SYNC_VOTES;
		m_voteCount = m_voteCount < SYNC_VOTES ? m_voteCount + 1 : SYNC_VOTES;

		int count[3] = {};

		for (int i = 0; i < m_voteCount; i++)
			count[m_votes[i]]++;

		int lead = count[1] > count[0] ? 1 : 0;
		lead = count[2] > count[lead] ? 2 : lead;

		// Settled at 9 in 10 of at least 32 clear answers; unsettled below 3 in 4, or if the lead changes.
		if (m_syncLocked && (lead != m_syncBack || count[lead] * 4 < m_voteCount * 3))
			m_syncLocked = false;
		else if (!m_syncLocked && m_voteCount >= 32 && count[lead] * 10 >= m_voteCount * 9)
			m_syncLocked = true;

		m_syncBack = lead;
	}

	// Did the jitter reach the frame? Then the cameras as drawn predict it better than the same cameras
	// without the jitter. If they mostly do not, the jitter is paused (PauseJitter).
	int back = read.back;

	if (!read.jittered || m_jitterFailed || !read.valid[back] || !read.valid[3])
		return;

	// A jitter that is never confirmed is refused too: the picture must not depend on an unchecked guess.
	if (++m_jitterChecks >= 300 && m_landCount < 32)
	{
		char why[120];
		snprintf(why, sizeof(why), "only %d clear answers in %d jittered frames checked", m_landCount, m_jitterChecks);
		PauseJitter(why);

		return;
	}

	int landed = r[back] < 0.8f * r[3] ? 1 : (r[3] < 0.8f * r[back] ? 0 : -1);

	if (landed < 0)
		return;

	m_situation[read.aim ? 1 : 0][read.zoom ? 1 : 0][landed]++;

	if (!landed && m_missLogged < 10)
	{
		m_missLogged++;
		// The errors tell a dropped jitter (the line without it fits best) from a frame drawn with
		// another line than the settled one (that line fits best).
		Log("camera jitter not seen in a frame (camera line %.0f, height %.0f, pitch %.1f, roll %.1f%s%s, game state %s; depth error with lines 0, 1, 2 back %.5f %.5f %.5f, line %d back without its jitter %.5f)",
			read.frame, read.height, read.pitch, read.roll, read.aim ? ", aiming" : "", read.zoom ? ", field of view changing" : "", read.state[0] ? read.state : "unknown",
			read.valid[0] ? r[0] : -1.0f, read.valid[1] ? r[1] : -1.0f, read.valid[2] ? r[2] : -1.0f, back, r[3]);
	}

	m_landed[m_landAt] = (unsigned char)landed;
	m_landAt = (m_landAt + 1) % SYNC_VOTES;
	m_landCount = m_landCount < SYNC_VOTES ? m_landCount + 1 : SYNC_VOTES;

	int yes = 0;

	for (int i = 0; i < m_landCount; i++)
		yes += m_landed[i];

	// Only clear answers count. Refused when fewer than 4 in 5 show the jitter: a jitter that reaches
	// only some frames would shake the picture on the others.
	if (m_landCount >= 32 && yes * 5 < m_landCount * 4)
	{
		char why[120];
		snprintf(why, sizeof(why), "in %d of %d checks the frame matched the camera without its jitter", m_landCount - yes, m_landCount);
		PauseJitter(why);
	}
}

// In the game the jitter once missed the frames for about a second mid-heist (13 checks in a row),
// and refusing it for the rest of the level cost minutes of play. So it is paused and tried again:
// after 10 s, then 20, 40 ... up to 320 s while it keeps missing on that level. A jitter that never
// reaches the frames shakes them for about a second per try, ever more rarely.
void Effects::PauseJitter(const char* why)
{
	m_jitterFailed = true;
	m_pauseLeft = m_pauseNext;
	m_pauseNext = m_pauseNext * 2.0 < 320.0 ? m_pauseNext * 2.0 : 320.0;
	Log("camera jitter paused for %.0f s: %s", m_pauseLeft, why);
}

std::string Effects::TaaSummary() const
{
	std::string text = m_taaRunning ? (m_t2xRunning ? "SMAA T2x, reprojecting with the camera" : "reprojecting with the camera") : "not running";
	char part[200];

	if (m_lastPoseFrames)
	{
		snprintf(part, sizeof(part), "; camera line one frame apart %u%% of the time, repeated %u%%", m_lastPoseInStep * 100 / m_lastPoseFrames, m_lastPoseStale * 100 / m_lastPoseFrames);
		text += part;
	}

	if (m_voteCount)
	{
		int count = 0;

		for (int i = 0; i < m_voteCount; i++)
			count += m_votes[i] == m_syncBack ? 1 : 0;

		snprintf(part, sizeof(part), "; frames match the camera line %d back in %d of the last %d clear checks%s", m_syncBack, count, m_voteCount, m_syncLocked ? " (settled)" : " (not settled yet: turn around a bit)");
		text += part;
	}

	if (m_jitterChecks)
	{
		snprintf(part, sizeof(part), "; %d jittered frames checked", m_jitterChecks);
		text += part;
	}

	if (m_landCount)
	{
		int yes = 0;

		for (int i = 0; i < m_landCount; i++)
			yes += m_landed[i];

		snprintf(part, sizeof(part), "; jitter seen in the frame in %d of %d checks", yes, m_landCount);
		text += part;
	}

	const unsigned (*n)[2][2] = m_situation;
	snprintf(part, sizeof(part), "; jitter checks seen/clear: still view %u/%u, zooming %u/%u, aiming %u/%u, aiming and zooming %u/%u",
		n[0][0][1], n[0][0][0] + n[0][0][1], n[0][1][1], n[0][1][0] + n[0][1][1], n[1][0][1], n[1][0][0] + n[1][0][1], n[1][1][1], n[1][1][0] + n[1][1][1]);
	text += part;

	return text;
}

std::string Effects::GpuSummary() const
{
	if (!m_gpuMeasured)
		return "not measured yet";

	static const char* kNames[GPU_STAGES] = {"frame copy and depth preparation", "antialiasing", "motion and temporal", "depth of field", "bloom, blurs and adaptation", "ambient occlusion", "indirect light", "ray traced lighting", "reflections", "light shafts", "grade and temporal antialiasing", "motion blur, FSR and final pass"};
	float total = 0.0f;
	std::string parts;

	for (int i = 0; i < GPU_STAGES; i++)
	{
		total += m_gpuMs[i];

		// Under 0.01 ms is a stage that is off.
		if (m_gpuMs[i] >= 0.01f)
		{
			char part[80];
			snprintf(part, sizeof(part), "%s%s %.2f", parts.empty() ? "" : ", ", kNames[i], (double)m_gpuMs[i]);
			parts += part;
		}
	}

	char head[48];
	snprintf(head, sizeof(head), "%.2f ms (", (double)total);

	return head + parts + ")";
}

bool Effects::Run(ID3D11Device* device, ID3D11DeviceContext1* ctx, ID3D11Texture2D* target, ID3D11RenderTargetView* targetRtv, DXGI_FORMAT rtvFormat, const Settings& s, ID3D11Texture2D* depthTex)
{
	if (!EnsureDevice(device))
		return false;

	if (s.shaderGen != m_shaderGen)
	{
		m_shaderGen = s.shaderGen;

		if (!Compile(s.shaderPath))
			Log("%s", m_error.c_str());
	}

	if (!m_compiled)
		return false;

	if (s.lutGen != m_lutGen)
	{
		m_lutGen = s.lutGen;
		LoadLut(s.lutPath);
	}

	if (s.dirtGen != m_dirtGen)
	{
		m_dirtGen = s.dirtGen;
		LoadDirt(s.dirtPath);
	}

	D3D11_TEXTURE2D_DESC desc;
	target->GetDesc(&desc);

	if (desc.SampleDesc.Count != 1)
	{
		m_error = "multisampled back buffer is not supported";
		return false;
	}

	if (!EnsureTargets(desc, rtvFormat))
		return false;

	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);

	double dt = m_lastCounter ? (double)(counter.QuadPart - m_lastCounter) / (double)frequency.QuadPart : 0.0;
	m_lastCounter = counter.QuadPart;

	if (dt > 0.25)
		dt = 0.25; // a hitch or a pause menu should not snap the adaptation

	m_time = fmod(m_time + dt, 1000.0);

	float w = (float)desc.Width;
	float h = (float)desc.Height;
	DXGI_FORMAT effectiveRtv = rtvFormat == DXGI_FORMAT_UNKNOWN ? desc.Format : rtvFormat;

	const float* p = s.p;

	// FSR 1.0. The buffer is made (and given back) here, before anything is written to the
	// constant buffer, so the shader is told what really exists rather than what was asked for.
	// 1x and below is not an upscale (Lua sends the factor below 1x as render scale instead).
	float upscaleScale = p[P_UPSCALE_SCALE] < 1.0f ? 1.0f : (p[P_UPSCALE_SCALE] > 2.0f ? 2.0f : p[P_UPSCALE_SCALE]);
	bool upscale = p[P_UPSCALE] > 0.5f && upscaleScale > 1.001f && EnsureUpscale(desc.Width, desc.Height, upscaleScale);

	if (!upscale && m_upscale.tex)
		ReleaseUpscale();

	// Depth. Everything that reads it is switched off by writing DEPTH_MODE = -1, the same way the
	// FSR stage is told what was really allocated: the shader is never told about a buffer that is
	// not there.
	bool haveDepth = depthTex != nullptr && EnsureDepth(ctx, depthTex);

	if (!depthTex)
	{
		m_depthCopy.Release();
		m_depthLinear.Release();
		m_depthFormat = DXGI_FORMAT_UNKNOWN;
		m_depthError = "no depth target found among the game's render targets";
	}

	bool clarity = fabs(p[P_CLARITY]) > 0.001f;
	// Obscurance steps aside for real ambient occlusion (see PS_Grade), so its chain is not built then.
	bool obscurance = p[P_OBSCURANCE] > 0.001f && !(haveDepth && p[P_AO] > 0.001f);
	bool colorBloom = p[P_COLOR_BLOOM] > 0.001f;
	bool cues = p[P_CUES] > 0.001f;
	bool halation = p[P_HALATION] > 0.001f;
	bool dirt = p[P_DIRT] > 0.001f && m_dirtSrv != nullptr;
	bool ao = haveDepth && p[P_AO] > 0.001f;
	bool il = haveDepth && p[P_IL] > 0.001f;
	bool rtgi = haveDepth && p[P_RTGI] > 0.001f;
	bool ssr = haveDepth && p[P_SSR] > 0.001f;
	bool shafts = p[P_SHAFTS] > 0.001f;
	bool mblur = p[P_MBLUR] > 0.001f;
	bool ltm = p[P_LTM] > 0.001f;
	bool fogGlow = haveDepth && p[P_FOG_GLOW] > 0.001f && (p[P_FOG] > 0.001f || p[P_HFOG] > 0.001f);
	bool temporal = p[P_TEMPORAL] > 0.5f;
	bool motionView = p[P_MOTION_VIEW] > 0.5f;
	bool surfaceGrain = p[P_SURFACE_GRAIN] > 0.001f;
	// SMAA T2x (Antialiasing method 3) takes temporal antialiasing's place while it is chosen: both jitter
	// the camera and both need it reprojected.
	bool t2x = p[P_AA] > 0.5f && p[P_AA_MODE] > 1.5f && haveDepth;
	bool taa = p[P_TAA] > 0.001f && !t2x;
	// Temporal antialiasing reprojects with the camera instead of the estimated motion (PS_Taa).
	bool needMotion = mblur || motionView || (temporal && (ao || il || rtgi || ssr || surfaceGrain));
	bool advanced = (il || rtgi || ssr || shafts || surfaceGrain || taa || t2x || needMotion) && EnsureAdvanced(desc.Width, desc.Height);

	if (!advanced)
	{
		if (m_advanced)
			ReleaseAdvanced();

		// SMAA T2x writes into TAA's buffers, so it goes too; plain SMAA then does the antialiasing.
		il = rtgi = ssr = shafts = mblur = needMotion = motionView = surfaceGrain = taa = t2x = false;
	}

	bool lightInfo = dirt || shafts || fogGlow;
	bool wide = obscurance || cues || ltm;
	bool quarter = clarity || cues || ltm;

	float frame[8 + PARAM_SLOTS] = {};
	frame[0] = w;
	frame[1] = h;
	frame[2] = 1.0f / w;
	frame[3] = 1.0f / h;
	frame[4] = (float)m_time;
	frame[5] = (float)dt;
	frame[6] = IsSrgb(effectiveRtv) ? 1.0f : 0.0f;
	frame[7] = (float)m_lutSize;
	memcpy(frame + 8, s.p, sizeof(s.p));
	frame[8 + P_UPSCALE] = upscale ? 1.0f : 0.0f;
	frame[8 + P_UPSCALE_SCALE] = upscaleScale;
	frame[8 + P_DEPTH_MODE] = haveDepth ? p[P_DEPTH_MODE] : -1.0f;
	m_lastDepthMode = frame[8 + P_DEPTH_MODE];
	// Only tell the shader about grain once there is a buffer holding it.
	frame[8 + P_SURFACE_GRAIN] = surfaceGrain ? p[P_SURFACE_GRAIN] : 0.0f;
	frame[8 + P_FRAME] = (float)(m_frameCount++ % 100000u);

	ID3DDeviceContextState* previous = nullptr;
	ctx->SwapDeviceContextState(m_state, &previous);
	GpuBegin(ctx);

	WriteBuffer(ctx, m_cbFrame, frame, sizeof(frame));
	ctx->CopyResource(m_src.tex, target);

	// Which camera line this frame was drawn with: the newest when the game draws on Lua's own thread,
	// otherwise the one TaaSync measured. A newest line Lua did not renew means it did not run before
	// this frame: nothing moved and nothing was jittered as far as anyone knows.
	int back = s.renderOnLuaThread ? 0 : m_syncBack;
	bool stale = s.recent[0].valid && s.recent[0].frame == m_lastNewest;
	CameraPose pose = stale ? m_prevPose : s.recent[back];

	if (stale)
	{
		memcpy(pose.right, pose.outRight, sizeof(pose.right));
		memcpy(pose.up, pose.outUp, sizeof(pose.up));
		memcpy(pose.fwd, pose.outFwd, sizeof(pose.fwd));
	}

	// Last frame's distance, for PS_Taa to tell surfaces that were there from ones that were not, when it
	// really is the frame just before this one.
	bool prevDepth = m_depthPrev.tex && s.recent[0].valid && s.recent[0].frame == m_depthPrevFrame + 1.0f;

	// Reprojection: that camera (jitter included), the same without the jitter, last frame's without it,
	// and how far it moved. The position difference is taken in double here, so world coordinates in the
	// tens of thousands do not cost the shader its precision. A jump of more than 10 m is a cut.
	double delta[3] = {pose.pos[0] - m_prevPose.pos[0], pose.pos[1] - m_prevPose.pos[1], pose.pos[2] - m_prevPose.pos[2]};
	bool reproject = pose.valid && m_prevPose.valid && delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2] < 1000.0 * 1000.0;
	// The depth effects' histories follow the camera too, once it is known which line the frames are
	// drawn with (TaaSync has settled, or the game draws on Lua's thread); until then, the motion
	// guessed from the picture.
	bool cameraMotion = reproject && (s.renderOnLuaThread || m_syncLocked);
	{
		float r[56] = {
			pose.right[0], pose.right[1], pose.right[2], 0.0f,
			pose.up[0], pose.up[1], pose.up[2], 0.0f,
			pose.fwd[0], pose.fwd[1], pose.fwd[2], 0.0f,
			pose.outRight[0], pose.outRight[1], pose.outRight[2], pose.valid ? 1.0f : 0.0f,
			pose.outUp[0], pose.outUp[1], pose.outUp[2], 0.0f,
			pose.outFwd[0], pose.outFwd[1], pose.outFwd[2], 0.0f,
			m_prevPose.outRight[0], m_prevPose.outRight[1], m_prevPose.outRight[2], prevDepth ? 1.0f : 0.0f,
			m_prevPose.outUp[0], m_prevPose.outUp[1], m_prevPose.outUp[2], 0.0f,
			m_prevPose.outFwd[0], m_prevPose.outFwd[1], m_prevPose.outFwd[2], 0.0f,
			(float)delta[0], (float)delta[1], (float)delta[2], reproject ? 1.0f : 0.0f,
			pose.tan[0], pose.tan[1], m_prevPose.tan[0], m_prevPose.tan[1],
			m_prevPose.right[0], m_prevPose.right[1], m_prevPose.right[2], cameraMotion ? 1.0f : 0.0f,
			m_prevPose.up[0], m_prevPose.up[1], m_prevPose.up[2], 0.0f,
			m_prevPose.fwd[0], m_prevPose.fwd[1], m_prevPose.fwd[2], 0.0f,
		};
		WriteBuffer(ctx, m_cbReproject, r, sizeof(r));
	}

	// With the camera jittered, temporal antialiasing replaces FXAA or SMAA: an edge already smoothed in
	// one frame only blurs further when accumulated (measured: SMAA before it raised the error by half).
	// Without jitter a still view gets no new samples, so SMAA stays.
	bool taaNow = taa && reproject && haveDepth;
	bool t2xNow = t2x && reproject;
	bool jittered = pose.valid && memcmp(pose.fwd, pose.outFwd, sizeof(pose.fwd)) != 0;

	// How the camera line keeps step: one frame apart is right; the same frame twice means Lua did
	// not write before this frame rendered. Counted in windows of 300 frames for the status file.
	if (s.recent[0].valid && m_lastNewest >= 0.0f)
	{
		m_poseFrames++;
		m_poseInStep += s.recent[0].frame == m_lastNewest + 1.0f ? 1 : 0;
		m_poseStale += stale ? 1 : 0;

		if (m_poseFrames >= 300)
		{
			m_lastPoseFrames = m_poseFrames;
			m_lastPoseInStep = m_poseInStep;
			m_lastPoseStale = m_poseStale;
			m_poseFrames = m_poseInStep = m_poseStale = 0;
		}
	}

	m_prevPose = pose;
	m_lastNewest = s.recent[0].valid ? s.recent[0].frame : -1.0f;

	// Lua numbers its camera lines from 1 each time it starts, which it does for every level. A lower
	// number than before is a new level: the jitter is checked again there, so one scene where it did
	// not reach the frames (seen near a heist's end) does not cost the next heist.
	if (s.recent[0].valid)
	{
		if (s.recent[0].frame < m_lineFrameMax)
		{
			if (m_jitterFailed)
				Log("new level: the camera jitter is checked again");

			m_jitterFailed = false;
			m_pauseNext = 10.0;
			m_landCount = m_landAt = m_jitterChecks = m_missLogged = 0;
		}

		m_lineFrameMax = s.recent[0].frame;

		// So pauses and misses can be lined up with what the game was doing.
		if (s.recent[0].state[0] && strcmp(s.recent[0].state, m_gameState) != 0)
		{
			memcpy(m_gameState, s.recent[0].state, sizeof(m_gameState));
			Log("game state: %s", m_gameState);
		}
	}

	if (m_jitterFailed && (m_pauseLeft -= dt) <= 0.0)
	{
		Log("camera jitter tried again");
		m_jitterFailed = false;
		m_landCount = m_landAt = m_jitterChecks = m_missLogged = 0;
	}

	ID3D11Buffer* cbs[4] = {m_cbFrame, m_cbPass, m_cbReproject, m_cbSync};
	ID3D11SamplerState* samplers[2] = {m_linear, m_point};

	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->IASetInputLayout(nullptr);
	ctx->VSSetShader(m_vs, nullptr, 0);
	ctx->RSSetState(m_raster);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(m_depth, 0);
	ctx->PSSetSamplers(0, 2, samplers);
	ctx->PSSetConstantBuffers(0, 4, cbs);

	// Depth as distance, once: every pass after this reads m_depthLinear in slot 14.
	if (haveDepth)
		Pass(ctx, m_psLinearDepth, m_depthLinear.rtv, m_depthLinear.w, m_depthLinear.h, (float)m_depthCopy.w, (float)m_depthCopy.h, &m_depthCopy.srv, 1);

	if ((taa || t2x) && haveDepth)
		TaaSync(ctx, s, back);
	else if (m_syncOut.tex)
		ReleaseSync();

	GpuMark(ctx, GPU_DEPTH);

	ID3D11ShaderResourceView* current = m_src.srv;

	if (p[P_AA] > 0.5f && !(taaNow && jittered))
	{
		if (p[P_AA_MODE] > 0.5f && SmaaReady())
		{
			// SMAA discards pixels with no edge, so both of its buffers start each frame empty.
			const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			ctx->ClearRenderTargetView(m_smaaEdges.rtv, clear);
			ctx->ClearRenderTargetView(m_smaaWeights.rtv, clear);

			Pass(ctx, m_psSmaaEdges, m_smaaEdges.rtv, m_smaaEdges.w, m_smaaEdges.h, w, h, &current, 1);

			// SMAA T2x: the area textures for the quarter pixel this frame was drawn at (smaa.hlsl,
			// @SUBSAMPLE_INDICES); 0 is SMAA 1x.
			float subsample = 0.0f;

			if (t2xNow && jittered)
				subsample = pose.jitter[0] > 0.0f ? kT2xSubsampleRight : 3.0f - kT2xSubsampleRight;

			ID3D11ShaderResourceView* weightInputs[3] = {m_smaaEdges.srv, m_smaaAreaSrv, m_smaaSearchSrv};
			Pass(ctx, m_psSmaaWeights, m_smaaWeights.rtv, m_smaaWeights.w, m_smaaWeights.h, w, h, weightInputs, 3, subsample);

			// Linear light blending through the sRGB views when there are both.
			bool linear = m_srcSrgbSrv && m_aaSrgbRtv;
			ID3D11ShaderResourceView* blendInputs[2] = {linear ? m_srcSrgbSrv : current, m_smaaWeights.srv};
			Pass(ctx, m_psSmaaBlend, linear ? m_aaSrgbRtv : m_aa.rtv, m_aa.w, m_aa.h, w, h, blendInputs, 2);
		}
		else
		{
			Pass(ctx, m_psFxaa, m_aa.rtv, m_aa.w, m_aa.h, w, h, &current, 1);
		}

		current = m_aa.srv;
	}

	GpuMark(ctx, GPU_AA);

	// Motion vectors: this frame's brightness and depth pyramid, then a coarse to fine search against
	// last frame's. Nothing to compare against on the first frame, so there is no motion that frame.
	// The search is skipped when only the depth effects want it and they follow the camera; they still
	// read last frame's depth from the pyramid.
	bool motionOk = false, pyramidOk = false;
	bool search = mblur || motionView || surfaceGrain || !cameraMotion;
	ID3D11ShaderResourceView* previousPyramid = advanced ? m_motionPyr[1 - m_motionIndex].srv : nullptr;

	if (needMotion)
	{
		int now = m_motionIndex;
		ID3D11ShaderResourceView* motionInputs[MAX_INPUTS] = {};
		motionInputs[0] = current;
		motionInputs[14] = haveDepth ? m_depthLinear.srv : nullptr;
		Pass(ctx, m_psMotionInput, m_motionPyr[now].rtv, m_motionPyr[now].w, m_motionPyr[now].h, w, h, motionInputs, MAX_INPUTS);
		ctx->OMSetRenderTargets(0, nullptr, nullptr);
		ctx->GenerateMips(m_motionPyr[now].srv);

		if (!m_motionReset)
		{
			pyramidOk = true;

			for (int level = 2; level >= 0 && search; level--)
			{
				ID3D11ShaderResourceView* inputs[3] = {m_motionPyr[now].srv, previousPyramid, level < 2 ? m_motion[level + 1].srv : nullptr};
				const RenderTexture& out = m_motion[level];
				Pass(ctx, m_psMotionSearch, out.rtv, out.w, out.h, (float)out.w, (float)out.h, inputs, 3, (float)level);
			}

			motionOk = search;
		}
		else
		{
			const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			ctx->ClearRenderTargetView(m_motion[0].rtv, zero);
		}
	}

	GpuMark(ctx, GPU_MOTION);

	// Depth of field, early, so bloom, grading and everything after see the blurred frame just as
	// they would a real lens's. Focus first: where you are looking, eased from last frame.
	// Either size counts: the hip size may be zero while the aiming one is not.
	bool dof = haveDepth && (p[P_DOF] > 0.001f || p[P_DOF_AIM] > 0.001f);

	// Nothing to blur from the hip, and your sights have been down long enough for the lens to have
	// eased shut: skip the passes. Not a reset, so raising your sights eases open again.
	bool lensShut = false;

	if (dof && p[P_DOF] <= 0.001f)
	{
		bool aiming = p[P_CAMERA_VALID] > 0.5f && p[P_CAMERA_AIM] > 0.5f;
		m_dofIdle = aiming ? 0.0 : m_dofIdle + dt;
		lensShut = !m_focusReset && m_dofIdle > 0.5;
	}
	else
	{
		m_dofIdle = 0.0;
	}

	if (dof && !lensShut)
	{
		float speed = p[P_DOF_SPEED] > 0.05f ? p[P_DOF_SPEED] : 0.05f;
		float focusBlend = m_focusReset ? 1.0f : (float)(1.0 - exp(-dt * speed));
		int nextFocus = 1 - m_focusIndex;

		ID3D11ShaderResourceView* focusInputs[MAX_INPUTS] = {};
		focusInputs[0] = m_focus[m_focusIndex].srv;
		focusInputs[14] = m_depthLinear.srv;
		Pass(ctx, m_psFocus, m_focus[nextFocus].rtv, 1, 1, 1.0f, 1.0f, focusInputs, MAX_INPUTS, focusBlend, m_focusReset ? 1.0f : 0.0f);
		m_focusIndex = nextFocus;
		m_focusReset = false;

		ID3D11ShaderResourceView* prepareInputs[MAX_INPUTS] = {};
		prepareInputs[0] = current;
		prepareInputs[1] = m_focus[m_focusIndex].srv;
		prepareInputs[14] = m_depthLinear.srv;
		Pass(ctx, m_psDofPrepare, m_dofHalf[0].rtv, m_dofHalf[0].w, m_dofHalf[0].h, w, h, prepareInputs, MAX_INPUTS);

		ID3D11ShaderResourceView* blurInputs[MAX_INPUTS] = {};
		blurInputs[0] = m_dofHalf[0].srv;
		blurInputs[1] = m_focus[m_focusIndex].srv;
		blurInputs[14] = m_depthLinear.srv;
		Pass(ctx, m_psDofBlur, m_dofHalf[1].rtv, m_dofHalf[1].w, m_dofHalf[1].h, (float)m_dofHalf[0].w, (float)m_dofHalf[0].h, blurInputs, MAX_INPUTS);

		Pass(ctx, m_psDofFill, m_dofHalf[0].rtv, m_dofHalf[0].w, m_dofHalf[0].h, (float)m_dofHalf[1].w, (float)m_dofHalf[1].h, &m_dofHalf[1].srv, 1);

		ID3D11ShaderResourceView* compositeInputs[MAX_INPUTS] = {};
		compositeInputs[0] = current;
		compositeInputs[1] = m_dofHalf[0].srv;
		compositeInputs[2] = m_focus[m_focusIndex].srv;
		compositeInputs[14] = m_depthLinear.srv;
		Pass(ctx, m_psDofComposite, m_dof.rtv, m_dof.w, m_dof.h, w, h, compositeInputs, MAX_INPUTS);

		current = m_dof.srv;
	}
	else if (!dof)
	{
		m_focusReset = true;
	}

	GpuMark(ctx, GPU_DOF);

	bool adapt = p[P_ADAPT] > 0.001f;

	if (adapt)
	{
		Pass(ctx, m_psLuma, m_lum.rtv, m_lum.w, m_lum.h, w, h, &current, 1);
		ctx->GenerateMips(m_lum.srv);

		float blend = m_adaptReset ? 1.0f : (float)(1.0 - exp(-dt * p[P_ADAPT_SPEED]));
		int next = 1 - m_adaptIndex;
		ID3D11ShaderResourceView* inputs[3] = {m_adapt[m_adaptIndex].srv, nullptr, m_lum.srv};

		Pass(ctx, m_psAdapt, m_adapt[next].rtv, 1, 1, 256.0f, 256.0f, inputs, 3, (float)(m_lumMips - 1), blend);
		m_adaptIndex = next;
		m_adaptReset = false;
	}
	else
	{
		m_adaptReset = true;
	}

	ID3D11ShaderResourceView* bloom = nullptr;

	if (p[P_BLOOM] > 0.001f)
	{
		Pass(ctx, m_psBloomPrefilter, m_down[0].rtv, m_down[0].w, m_down[0].h, w, h, &current, 1);

		for (int i = 1; i < BLOOM_LEVELS; i++)
			Pass(ctx, m_psBloomDown, m_down[i].rtv, m_down[i].w, m_down[i].h, (float)m_down[i - 1].w, (float)m_down[i - 1].h, &m_down[i - 1].srv, 1);

		for (int i = BLOOM_LEVELS - 2; i >= 0; i--)
		{
			const RenderTexture& coarse = i == BLOOM_LEVELS - 2 ? m_down[BLOOM_LEVELS - 1] : m_up[i + 1];
			ID3D11ShaderResourceView* inputs[2] = {m_down[i].srv, coarse.srv};

			Pass(ctx, m_psBloomUp, m_up[i].rtv, m_up[i].w, m_up[i].h, (float)coarse.w, (float)coarse.h, inputs, 2);
		}

		bloom = m_up[0].srv;
	}


	// Half and quarter resolution copies. Clarity and the cues read the quarter one; the wide
	// chains below start from it, so everything shares these two passes.
	if (quarter || wide || colorBloom || halation || lightInfo || ssr)
	{
		Pass(ctx, m_psBloomDown, m_blur[0].rtv, m_blur[0].w, m_blur[0].h, w, h, &current, 1);
		Pass(ctx, m_psBloomDown, m_blur[1].rtv, m_blur[1].w, m_blur[1].h, (float)m_blur[0].w, (float)m_blur[0].h, &m_blur[0].srv, 1);
	}

	// The wide blur for obscurance, the cues and local tone mapping: 1/8, 1/16 and 1/32 of the screen.
	if (wide)
	{
		const RenderTexture* from = &m_blur[1];

		for (int i = 0; i < WIDE_LEVELS; i++)
		{
			Pass(ctx, m_psWideDown, m_wide[i].rtv, m_wide[i].w, m_wide[i].h, (float)from->w, (float)from->h, &from->srv, 1);
			from = &m_wide[i];
		}
	}

	// Colour bloom: only strongly coloured areas go into this chain, as colour without brightness.
	if (colorBloom)
	{
		Pass(ctx, m_psTintPrefilter, m_tint[0].rtv, m_tint[0].w, m_tint[0].h, (float)m_blur[1].w, (float)m_blur[1].h, &m_blur[1].srv, 1);

		for (int i = 1; i < WIDE_LEVELS; i++)
			Pass(ctx, m_psWideDown, m_tint[i].rtv, m_tint[i].w, m_tint[i].h, (float)m_tint[i - 1].w, (float)m_tint[i - 1].h, &m_tint[i - 1].srv, 1);
	}

	// Halation: only the brightest highlights, blurred wider still and tinted warm in the grade.
	if (halation)
	{
		Pass(ctx, m_psHaloPrefilter, m_halo[0].rtv, m_halo[0].w, m_halo[0].h, (float)m_blur[1].w, (float)m_blur[1].h, &m_blur[1].srv, 1);

		for (int i = 1; i < WIDE_LEVELS; i++)
			Pass(ctx, m_psWideDown, m_halo[i].rtv, m_halo[i].w, m_halo[i].h, (float)m_halo[i - 1].w, (float)m_halo[i - 1].h, &m_halo[i - 1].srv, 1);
	}

	// One pixel holding where the frame's bright light is and how strong it is, so the lens dirt
	// can sit opposite it. 256 taps, one pixel: the cost does not depend on the screen size.
	if (lightInfo)
		Pass(ctx, m_psLightInfo, m_light.rtv, 1, 1, (float)m_blur[1].w, (float)m_blur[1].h, &m_blur[1].srv, 1);

	// Surface grain: carried along by the motion vectors, re-seeded where that cannot be trusted.
	ID3D11ShaderResourceView* grainSrv = nullptr;

	if (surfaceGrain)
	{
		int next = 1 - m_grainHist.index;
		ID3D11ShaderResourceView* grainInputs[MAX_INPUTS] = {};
		grainInputs[0] = m_grainHist.t[m_grainHist.index].srv;
		grainInputs[1] = m_motion[0].srv;
		grainInputs[3] = previousPyramid;
		grainInputs[14] = haveDepth ? m_depthLinear.srv : nullptr;

		const RenderTexture& out = m_grainHist.t[next];
		Pass(ctx, m_psGrain, out.rtv, out.w, out.h, w, h, grainInputs, MAX_INPUTS, 0.0f, m_grainHist.reset || !motionOk ? 1.0f : 0.0f);
		m_grainHist.index = next;
		m_grainHist.reset = false;
		grainSrv = out.srv;
	}
	else
	{
		m_grainHist.reset = true;
	}

	GpuMark(ctx, GPU_BLUR);

	// Ambient occlusion: the taps, accumulated over frames when temporal is on, then a blur that
	// stops at edges. Everything here reads the depth copy from slot 14, as the grade does.
	bool accumulate = temporal && advanced;

	if (ao)
	{
		ID3D11ShaderResourceView* aoInputs[MAX_INPUTS] = {};
		aoInputs[14] = m_depthLinear.srv;
		Pass(ctx, m_psAo, m_ao.rtv, m_ao.w, m_ao.h, (float)m_ao.w, (float)m_ao.h, aoInputs, MAX_INPUTS);

		ID3D11ShaderResourceView* aoSource = m_ao.srv;

		if (accumulate)
			aoSource = Accumulate(ctx, m_aoHist, m_ao.srv, 0.9f, cameraMotion ? pyramidOk : motionOk, previousPyramid);
		else
			m_aoHist.reset = true;

		ID3D11ShaderResourceView* blurInputs[MAX_INPUTS] = {};
		blurInputs[14] = m_depthLinear.srv;
		blurInputs[15] = aoSource;
		Pass(ctx, m_psAoBlur, m_aoBlur.rtv, m_aoBlur.w, m_aoBlur.h, (float)m_ao.w, (float)m_ao.h, blurInputs, MAX_INPUTS);
	}
	else
	{
		m_aoHist.reset = true;
	}

	GpuMark(ctx, GPU_AO);

	// Indirect light and ray traced lighting: gathered at half resolution, accumulated over frames,
	// then blurred along surfaces. Reflections are accumulated but not blurred, to stay sharp.
	auto gather = [&](bool on, ID3D11PixelShader* ps, RenderTexture& raw, History& history, float keep, RenderTexture* blurred) -> ID3D11ShaderResourceView* {
		if (!on)
		{
			history.reset = true;
			return nullptr;
		}

		ID3D11ShaderResourceView* inputs[MAX_INPUTS] = {};
		inputs[0] = current;
		inputs[1] = m_blur[0].srv;
		inputs[14] = m_depthLinear.srv;
		Pass(ctx, ps, raw.rtv, raw.w, raw.h, (float)raw.w, (float)raw.h, inputs, MAX_INPUTS);

		ID3D11ShaderResourceView* result = raw.srv;

		if (accumulate)
			result = Accumulate(ctx, history, raw.srv, keep, cameraMotion ? pyramidOk : motionOk, previousPyramid);
		else
			history.reset = true;

		if (blurred)
		{
			ID3D11ShaderResourceView* blurInputs[MAX_INPUTS] = {};
			blurInputs[0] = result;
			blurInputs[14] = m_depthLinear.srv;
			Pass(ctx, m_psDepthBlur, blurred->rtv, blurred->w, blurred->h, (float)blurred->w, (float)blurred->h, blurInputs, MAX_INPUTS);
			result = blurred->srv;
		}

		return result;
	};

	ID3D11ShaderResourceView* ilSrv = gather(il, m_psIl, m_il, m_ilHist, 0.85f, &m_ilBlur);
	GpuMark(ctx, GPU_IL);
	ID3D11ShaderResourceView* rtgiSrv = gather(rtgi, m_psRtgi, m_rtgi, m_rtgiHist, 0.94f, &m_rtgiBlur);
	GpuMark(ctx, GPU_RTGI);
	ID3D11ShaderResourceView* ssrSrv = gather(ssr, m_psSsr, m_ssr, m_ssrHist, 0.8f, nullptr);
	GpuMark(ctx, GPU_SSR);

	// Light shafts: what can shine, then a march towards the light.
	if (shafts)
	{
		ID3D11ShaderResourceView* maskInputs[MAX_INPUTS] = {};
		maskInputs[0] = current;
		maskInputs[14] = haveDepth ? m_depthLinear.srv : nullptr;
		Pass(ctx, m_psShaftMask, m_shaftMask.rtv, m_shaftMask.w, m_shaftMask.h, w, h, maskInputs, MAX_INPUTS);

		ID3D11ShaderResourceView* shaftInputs[2] = {m_shaftMask.srv, m_light.srv};
		Pass(ctx, m_psShafts, m_shafts.rtv, m_shafts.w, m_shafts.h, (float)m_shaftMask.w, (float)m_shaftMask.h, shaftInputs, 2);
	}

	GpuMark(ctx, GPU_SHAFTS);

	// This frame's pyramid becomes next frame's "last frame".
	if (needMotion)
	{
		m_motionIndex = 1 - m_motionIndex;
		m_motionReset = false;
	}
	else
	{
		m_motionReset = true;
	}

	{
		ID3D11ShaderResourceView* inputs[MAX_INPUTS] = {
			current,
			bloom,
			adapt ? m_adapt[m_adaptIndex].srv : nullptr,
			quarter ? m_blur[1].srv : nullptr,
			m_lutSrv,
			nullptr,
			wide ? m_wide[0].srv : nullptr,
			wide ? m_wide[WIDE_LEVELS - 1].srv : nullptr,
			colorBloom ? m_tint[1].srv : nullptr,
			colorBloom ? m_tint[WIDE_LEVELS - 1].srv : nullptr,
			dirt ? m_dirtSrv : nullptr,
			lightInfo ? m_light.srv : nullptr,
			halation ? m_halo[1].srv : nullptr,
			halation ? m_halo[WIDE_LEVELS - 1].srv : nullptr,
			haveDepth ? m_depthLinear.srv : nullptr,
			ao ? m_aoBlur.srv : nullptr,
		};
		inputs[16] = ilSrv;
		inputs[17] = rtgiSrv;
		inputs[18] = ssrSrv;
		inputs[19] = shafts ? m_shafts.srv : nullptr;
		inputs[20] = motionView && motionOk ? m_motion[0].srv : nullptr;
		inputs[21] = grainSrv;
		Pass(ctx, m_psGrade, m_grade.rtv, m_grade.w, m_grade.h, w, h, inputs, MAX_INPUTS, adapt ? 1.0f : 0.0f, bloom ? 1.0f : 0.0f);
	}

	// Temporal antialiasing (PS_Taa), on the graded frame: everything up to here, occlusion and
	// reflections included, was drawn from the same jittered camera and depth, so it is all put back
	// together. Sharpening, FSR and grain come after, so they never feed back into the history. Last
	// frame's result is reprojected with the camera, clipped to this frame's neighbourhood and blended
	// in; strength sets how little of each new frame goes in.
	ID3D11ShaderResourceView* graded = m_grade.srv;
	m_taaRunning = taaNow || t2xNow;
	m_t2xRunning = t2xNow;

	// SMAA T2x (PS_SmaaT2x): this frame half and half with last frame, both graded, each jittered a
	// quarter pixel the other way. Written where TAA keeps its history, which T2x does not need.
	if (t2x)
	{
		int next = 1 - m_taaHist.index;
		ID3D11ShaderResourceView* inputs[MAX_INPUTS] = {graded, m_t2xPrev.srv, prevDepth ? m_depthPrev.srv : nullptr};
		inputs[14] = m_depthLinear.srv;

		const RenderTexture& out = m_taaHist.t[next];
		Pass(ctx, m_psSmaaT2x, out.rtv, out.w, out.h, w, h, inputs, MAX_INPUTS, 0.0f, m_t2xReset || !reproject ? 1.0f : 0.0f);
		m_taaHist.index = next;

		// This frame's graded picture is last frame's next time.
		ID3D11ShaderResourceView* nulls[MAX_INPUTS] = {};
		ctx->PSSetShaderResources(0, MAX_INPUTS, nulls);
		ctx->CopyResource(m_t2xPrev.tex, m_grade.tex);
		m_t2xReset = false;
		graded = out.srv;
	}
	else
	{
		m_t2xReset = true;
	}

	if (taa)
	{
		int next = 1 - m_taaHist.index;
		float strength = p[P_TAA] < 1.0f ? p[P_TAA] : 1.0f;
		ID3D11ShaderResourceView* inputs[MAX_INPUTS] = {graded, m_taaHist.t[m_taaHist.index].srv, prevDepth ? m_depthPrev.srv : nullptr};
		inputs[14] = haveDepth ? m_depthLinear.srv : nullptr;

		const RenderTexture& out = m_taaHist.t[next];
		Pass(ctx, m_psTaa, out.rtv, out.w, out.h, w, h, inputs, MAX_INPUTS, 0.25f - 0.2f * strength, m_taaHist.reset || !reproject ? 1.0f : 0.0f);
		m_taaHist.index = next;
		m_taaHist.reset = false;
		graded = out.srv;
	}
	else
	{
		m_taaHist.reset = true;
	}

	// This frame's distance becomes last frame's, for TaaSync and PS_Taa next frame.
	if (m_depthPrev.tex && haveDepth)
	{
		ID3D11ShaderResourceView* nulls[MAX_INPUTS] = {};
		ctx->PSSetShaderResources(0, MAX_INPUTS, nulls);
		ctx->CopyResource(m_depthPrev.tex, m_depthLinear.tex);
		m_depthPrevFrame = s.recent[0].valid ? s.recent[0].frame : -1.0f;
	}

	GpuMark(ctx, GPU_GRADE);

	// FSR 1.0: EASU resamples the graded frame into the larger buffer along the edges it finds.
	// PS_Final resolves it back to the screen and runs RCAS there, at the size the frame is shown at.
	// Render scale cleanup: shrink the graded frame and rebuild it at screen size. What is lost is
	// detail; what goes with it is the aliasing and shimmer the engine baked into the image.
	float renderScale = p[P_RENDER_SCALE] < 1.0f ? (p[P_RENDER_SCALE] > 0.5f ? p[P_RENDER_SCALE] : 0.5f) : 1.0f;

	if (renderScale < 0.999f && EnsureScaled(desc.Width, desc.Height, renderScale))
	{
		Pass(ctx, m_psDownsample, m_scaled.rtv, m_scaled.w, m_scaled.h, w, h, &graded, 1);
		Pass(ctx, m_psEasu, m_rescaled.rtv, m_rescaled.w, m_rescaled.h, (float)m_scaled.w, (float)m_scaled.h, &m_scaled.srv, 1);
		graded = m_rescaled.srv;
	}
	else if (m_scaled.tex)
	{
		ReleaseScaled();
	}


	if (mblur && motionOk)
	{
		ID3D11ShaderResourceView* blurInputs[2] = {graded, m_motion[0].srv};
		Pass(ctx, m_psMotionBlur, m_mblur.rtv, m_mblur.w, m_mblur.h, w, h, blurInputs, 2);
		graded = m_mblur.srv;
	}

	ID3D11ShaderResourceView* finalSrv = graded;
	float finalW = w;
	float finalH = h;

	if (upscale)
	{
		Pass(ctx, m_psEasu, m_upscale.rtv, m_upW, m_upH, w, h, &graded, 1);
		finalSrv = m_upscale.srv;
		finalW = (float)m_upW;
		finalH = (float)m_upH;
	}

	{
		// Tex5 is the untouched frame for the split view.
		// Depth rides along so the sharpener can ease off across a silhouette.
		ID3D11ShaderResourceView* inputs[MAX_INPUTS] = {finalSrv, nullptr, nullptr, nullptr, nullptr, m_src.srv};
		inputs[14] = haveDepth ? m_depthLinear.srv : nullptr;
		// Extra.x 1: the frame went through SMAA T2x or TAA, so it is dithered again (PS_Final).
		Pass(ctx, m_psFinal, targetRtv, desc.Width, desc.Height, finalW, finalH, inputs, MAX_INPUTS, m_taaRunning ? 1.0f : 0.0f);
	}

	GpuMark(ctx, GPU_FINAL);
	GpuEnd(ctx);

	// Leave nothing of ours (and no reference to the back buffer) bound in our state object.
	ID3D11ShaderResourceView* nulls[MAX_INPUTS] = {};
	ctx->PSSetShaderResources(0, MAX_INPUTS, nulls);
	ctx->OMSetRenderTargets(0, nullptr, nullptr);

	ctx->SwapDeviceContextState(previous, nullptr);

	if (previous)
		previous->Release();

	return true;
}

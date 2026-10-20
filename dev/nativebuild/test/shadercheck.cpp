// Compiles every entry point of shaders/pss.hlsl with the d3dcompiler_47 found at run time: under
// Wine or Proton that is vkd3d-shader, the compiler the game gets there, so this is the real test.
// Usage: shadercheck.exe <path to pss.hlsl>   (run from any directory; includes resolve beside it)
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

typedef HRESULT(WINAPI* CompileFn)(LPCWSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

int main(int argc, char** argv)
{
	if (argc < 2) { printf("usage: shadercheck.exe <pss.hlsl>\n"); return 2; }
	HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
	CompileFn compile = lib ? (CompileFn)GetProcAddress(lib, "D3DCompileFromFile") : nullptr;
	if (!compile) { printf("FAIL: no d3dcompiler_47.dll / D3DCompileFromFile\n"); return 1; }
	std::string path = argv[1];
	std::wstring wpath(path.begin(), path.end());
	// The entry points the plugin compiles (native/src/effects.cpp).
	const char* entries[] = {"VS_Fullscreen", "PS_FXAA", "PS_Luma", "PS_Adapt", "PS_BloomPrefilter", "PS_BloomDown", "PS_BloomUp", "PS_Grade", "PS_Final", "PS_WideDown", "PS_TintPrefilter", "PS_HaloPrefilter", "PS_LightInfo", "PS_Easu", "PS_LinearDepth", "PS_Taa", "PS_SmaaT2x", "PS_TaaSync", "PS_Ao", "PS_AoBlur", "PS_SmaaEdges", "PS_SmaaWeights", "PS_SmaaBlend", "PS_Focus", "PS_DofPrepare", "PS_DofBlur", "PS_DofFill", "PS_DofComposite", "PS_Il", "PS_Rtgi", "PS_Ssr", "PS_DepthBlur", "PS_MotionInput", "PS_MotionSearch", "PS_Temporal", "PS_MotionBlur", "PS_ShaftMask", "PS_Shafts", "PS_Grain", "PS_Downsample"};
	int failed = 0, ok = 0;
	for (const char* entry : entries)
	{
		bool vs = entry[0] == 'V';
		ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
		HRESULT hr = compile(wpath.c_str(), nullptr, (ID3DInclude*)(UINT_PTR)1 /* D3D_COMPILE_STANDARD_FILE_INCLUDE */, entry, vs ? "vs_5_0" : "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
		if (FAILED(hr) || !code)
		{
			failed++;
			printf("FAIL %s (hr 0x%08lx)\n%s\n", entry, (unsigned long)hr, err ? (const char*)err->GetBufferPointer() : "(no message)");
		}
		else
		{
			ok++;
			printf("ok   %-18s %6u bytes\n", entry, (unsigned)code->GetBufferSize());
		}
		if (code) code->Release();
		if (err) err->Release();
	}
	printf("%d compiled, %d failed\n", ok, failed);
	return failed ? 1 : 0;
}

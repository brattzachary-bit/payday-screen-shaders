// Payday Screen Shaders - the whole effect chain.
//
// Runs on the finished World image, before the HUD is drawn. This file is compiled at runtime by
// the game's own d3dcompiler, so it can be edited and reloaded from the options menu without a
// restart.
//
// Most effects here work from colour alone. The ones under "Depth" instead read the game's own
// depth, which the plugin finds among Diesel's render targets and copies (hooks.cpp). When no
// depth target was found the plugin writes DEPTH_MODE = -1 and every one of them switches itself
// off, so this file is always safe to run.
//
// Pass order (effects.cpp): FXAA -> luminance + adaptation -> bloom chain -> half/quarter blur ->
// wide blur (obscurance, cues) and colour chain (colour bloom) -> ambient occlusion -> grade ->
// sharpen.

cbuffer Frame : register(b0)
{
	float4 Screen; // w, h, 1/w, 1/h
	float4 Time;   // seconds, dt, render target is sRGB, LUT size
	float4 P[32];  // parameters, layout matches native/src/params.h
};

cbuffer PassConstants : register(b1) // not "Pass": a keyword to Microsoft's compiler
{
	float4 Texel; // 1/srcW, 1/srcH, srcW, srcH
	float4 Extra; // per-pass values, then 1/dstW, 1/dstH
};

// Temporal antialiasing (lua/core.lua measures the cameras as the frame starts rendering). Axes are
// world directions for screen right, up and forward. Cur is the camera this frame was drawn with,
// jitter included; Out the same camera without it, the grid the result is kept on; Prev last frame's Out.
cbuffer Reproject : register(b2)
{
	float4 CurRight;
	float4 CurUp;
	float4 CurFwd;
	float4 OutRight; // w is 1 when the cameras are known
	float4 OutUp;
	float4 OutFwd;
	float4 PrevRight; // w is 1 when Tex2 holds last frame's distance
	float4 PrevUp;
	float4 PrevFwd;
	float4 CamDelta; // this camera's position minus last frame's, world units; w is 1 when both are known
	float4 CamTan;   // tangents of half the field of view: this frame's in xy, last frame's in zw
	float4 PrevDrawnRight; // last frame's camera as drawn, jitter included: the grid the effects' histories
	                       // are on; w is 1 when the effects may follow the camera (PS_Temporal)
	float4 PrevDrawnUp;
	float4 PrevDrawnFwd;
};

// Which camera line a frame was drawn with (effects.cpp, Effects::TaaSync): four candidate pairs of
// cameras, this frame's and last frame's, 8 rows each: right, up and forward of this frame's camera,
// the same of last frame's, the position change, and the tangents (this frame's xy, last frame's zw).
cbuffer Sync : register(b3)
{
	float4 SyncCameras[32];
};

Texture2D Tex0 : register(t0);
Texture2D Tex1 : register(t1);
Texture2D Tex2 : register(t2);
Texture2D Tex3 : register(t3);
Texture3D Lut : register(t4);
Texture2D Orig : register(t5);
Texture2D Tex6 : register(t6); // wide blur, 1/8
Texture2D Tex7 : register(t7); // wide blur, 1/32
Texture2D Tex8 : register(t8); // colour chain, 1/16
Texture2D Tex9 : register(t9); // colour chain, 1/32
Texture2D Dirt : register(t10);      // the lens dirt photograph (one channel)
Texture2D LightInfo : register(t11); // 1x1: where the frame's bright light is, and how much
Texture2D Tex12 : register(t12);     // halation chain, 1/16
Texture2D Tex13 : register(t13);     // halation chain, 1/32
Texture2D Depth : register(t14);     // distance from the camera in world units (PS_LinearDepth)
Texture2D AoTex : register(t15);     // ambient occlusion, half resolution, blurred
Texture2D IlTex : register(t16);     // indirect light, half resolution
Texture2D RtgiTex : register(t17);   // ray traced lighting, half resolution
Texture2D SsrTex : register(t18);    // reflections: colour, and how much of it to use in alpha
Texture2D ShaftTex : register(t19);  // light shafts, half resolution
Texture2D MotionTex : register(t20); // where each pixel was last frame, as a uv offset
Texture2D GrainTex : register(t21);  // surface grain, carried along with the surfaces it sits on

SamplerState Linear : register(s0);
SamplerState Point : register(s1);

#define ENABLED          P[0].x
#define SPLIT            P[0].y
#define AA               P[0].z
#define AA_SUBPIX        P[0].w
#define AA_EDGE          P[1].x
#define SHARPEN          P[1].y
#define BLOOM            P[1].z
#define BLOOM_THRESHOLD  P[1].w
#define BLOOM_RADIUS     P[2].x
#define ADAPT            P[2].y
#define ADAPT_SPEED      P[2].z
#define ADAPT_MIN        P[2].w
#define ADAPT_MAX        P[3].x
#define ADAPT_KEY        P[3].y
#define LIFT             P[3].z
#define GAMMA            P[3].w
#define GAIN             P[4].x
#define CONTRAST         P[4].y
#define SATURATION       P[4].z
#define VIBRANCE         P[4].w
#define LUT_MIX          P[5].x
#define VIGNETTE         P[5].y
#define GRAIN            P[5].z
// P[5].w inject_after and P[6].x require_fullscreen are read by the hook, not here.
#define DEBUG_VIEW       P[6].y
#define SHADOWS          P[6].z
#define HIGHLIGHTS       P[6].w
#define EXPOSURE         P[7].x
#define TEMPERATURE      P[7].y
#define TINT             P[7].z
#define CLARITY          P[7].w
#define DEBAND           P[8].x
#define DITHER           P[8].y
#define CHROMATIC        P[8].z
#define OBSCURANCE       P[8].w
#define OBSCURANCE_RADIUS P[9].x
#define COLOR_BLOOM      P[9].y
#define COLOR_BLOOM_RADIUS P[9].z
#define CUES             P[9].w
#define CUES_SEPARATION  P[10].x
#define CUES_DEPTH       P[10].y
#define CUES_FOCUS       P[10].z
#define DIRT             P[10].w
#define DIRT_OFFSET      P[11].x
#define HALATION         P[11].y
#define HALATION_THRESHOLD P[11].z
#define HALATION_TINT    P[11].w
#define HALATION_RADIUS  P[12].x
// P[12].y and P[12].z are unused (they were the Panini and spherical projections).
// The plugin rewrites these two in the constant buffer: UPSCALE is 1 only when the larger buffer
// really exists, and UPSCALE_SCALE is the factor it was actually created at.
#define UPSCALE          P[12].w
#define UPSCALE_SCALE    P[13].x
#define UPSCALE_SHARPNESS P[13].y
// Depth. The plugin rewrites DEPTH_MODE to -1 when it has no depth target, which is what
// HaveDepth() tests: every effect below it then costs nothing and changes nothing.
#define DEPTH_MODE       P[13].z
#define DEPTH_NEAR       P[13].w
#define DEPTH_FAR        P[14].x
#define AO               P[14].y
#define AO_RADIUS        P[14].z
#define FOG              P[14].w
#define FOG_START        P[15].x
#define FOG_TINT         P[15].y
#define DOF              P[15].z
#define DOF_FOCUS        P[15].w
#define DOF_RANGE        P[16].x
#define DEPTH_VIEW       P[16].y
#define AA_MODE          P[16].z  // 0 FXAA, 1 SMAA
#define DOF_SPEED        P[16].w
#define DOF_NEAR         P[17].x
#define DOF_WEAPON       P[17].y
// The camera, measured by Lua (see PSS:camera_line). CAMERA_VALID is 0 when it has stopped arriving.
#define CAMERA_TAN_X     P[17].z
#define CAMERA_TAN_Y     P[17].w
#define CAMERA_PITCH     P[18].x
#define CAMERA_ROLL      P[18].y
#define CAMERA_HEIGHT    P[18].z
#define CAMERA_VALID     P[18].w
#define FRAME            P[19].x
#define IL               P[19].y
#define IL_RADIUS        P[19].z
#define RTGI             P[19].w
#define RTGI_DISTANCE    P[20].x
#define SSR              P[20].y
#define SSR_BLUR         P[20].z
#define TEMPORAL         P[20].w
#define MOTION_VIEW      P[21].x
#define MBLUR            P[21].y
#define SHAFTS           P[21].z
#define SHAFTS_LENGTH    P[21].w
#define HFOG             P[22].x
#define HFOG_HEIGHT      P[22].y
#define FOG_GLOW         P[22].z
#define LTM              P[22].w
#define LTM_DETAIL       P[23].x
#define CAMERA_AIM       P[23].y
#define DOF_AIM          P[23].z
#define DEMOIRE          P[23].w
#define DETAIL           P[24].x
#define SURFACE_GRAIN    P[24].y
#define SURFACE_GRAIN_SIZE P[24].z
// Temporal antialiasing: the plugin uses these, the shader only needs them declared so the three
// parameter lists stay in step.
#define TAA              P[24].w
#define RENDER_SCALE     P[25].x
#define JITTER_X         P[25].y
#define JITTER_Y         P[25].z
#define CAMERA_FRAME     P[25].w

// 9-tap tent over a low resolution chain level, `o` = offset in uv. A plain bilinear read of a
// 1/32 level only averages its nearest texels; the tent makes it a genuinely wide, smooth blur.
#define TENT9(T, uv, o) ((T.SampleLevel(Linear, (uv), 0) * 4.0 \
	+ (T.SampleLevel(Linear, (uv) + float2((o).x, 0.0), 0) + T.SampleLevel(Linear, (uv) - float2((o).x, 0.0), 0) \
	+ T.SampleLevel(Linear, (uv) + float2(0.0, (o).y), 0) + T.SampleLevel(Linear, (uv) - float2(0.0, (o).y), 0)) * 2.0 \
	+ T.SampleLevel(Linear, (uv) + (o), 0) + T.SampleLevel(Linear, (uv) - (o), 0) \
	+ T.SampleLevel(Linear, (uv) + float2((o).x, -(o).y), 0) + T.SampleLevel(Linear, (uv) + float2(-(o).x, (o).y), 0)) / 16.0)

#define TIME        Time.x
#define SRGB_TARGET Time.z
#define LUT_SIZE    Time.w

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

// One triangle that covers the screen; no vertex buffer needed.
VSOut VS_Fullscreen(uint id : SV_VertexID)
{
	VSOut o;
	float x = (id == 1) ? 2.0 : 0.0;
	float y = (id == 2) ? 2.0 : 0.0;

	o.uv = float2(x, y);
	o.pos = float4(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);

	return o;
}

// ---------------------------------------------------------------------------------------------
// Colour space. Diesel writes gamma-encoded values into a UNORM back buffer; if the target is an
// sRGB view instead, sampling already decodes and writing encodes, so the conversions flip.

float3 SrgbToLinear(float3 c)
{
	c = saturate(c);
	float3 lo = c / 12.92;
	float3 hi = pow((c + 0.055) / 1.055, 2.4);

	return lerp(hi, lo, step(c, 0.04045));
}

float3 LinearToSrgb(float3 c)
{
	c = max(c, 0.0);
	float3 lo = c * 12.92;
	float3 hi = 1.055 * pow(max(c, 0.0000001), 1.0 / 2.4) - 0.055;

	return lerp(hi, lo, step(c, 0.0031308));
}

float3 ReadLinear(float3 s) { return SRGB_TARGET > 0.5 ? s : SrgbToLinear(s); }
float3 ReadGamma(float3 s) { return SRGB_TARGET > 0.5 ? LinearToSrgb(s) : s; }
float3 WriteGamma(float3 g) { return SRGB_TARGET > 0.5 ? SrgbToLinear(g) : g; }
float3 WriteLinear(float3 l) { return SRGB_TARGET > 0.5 ? l : LinearToSrgb(l); }

float Luma(float3 g) { return dot(g, float3(0.299, 0.587, 0.114)); }
float LumaLinear(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float LumaAt(float2 uv)
{
	return Luma(ReadGamma(Tex0.SampleLevel(Linear, uv, 0).rgb));
}

float Hash(float2 p)
{
	p = frac(p * float2(443.897, 441.423));
	p += dot(p, p.yx + 19.19);

	return frac((p.x + p.y) * p.x);
}

// ---------------------------------------------------------------------------------------------
// SMAA 1x, after Jimenez, Echevarria, Masia, Navarro and Gutierrez (MIT; the full notice is at the
// top of shaders/smaa.hlsl). The plugin pastes that file in where the #include line below is, so
// nothing depends on the compiler's own include support. Its two lookup textures are made from the
// published headers by dev/nativebuild/make_smaa.py.
//
// Three passes: find edges, work out how much of each pixel an edge covers (the area texture holds
// the answer for every edge shape), then blend each pixel with its neighbours by that much. Unlike
// FXAA it follows edges for up to 32 pixels, so long shallow edges get a smooth ramp instead of
// soft steps, and it leaves texture detail alone.

#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_ULTRA 1
#define SMAA_RT_METRICS float4(Screen.zw, Screen.xy)
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(Linear, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(Point, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(Linear, coord, 0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(Linear, coord, 0)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(Linear, coord, 0, offset)
// Edge detection must see gamma encoded colour. ReadGamma makes that true on an sRGB target too.
#define SMAASamplePoint(tex, coord) float4(ReadGamma(tex.SampleLevel(Point, coord, 0).rgb), 1.0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]

#include "smaa.hlsl"

// Colour edge detection rather than luma: it also finds edges whose two sides are about as bright
// but differ in colour, which luma misses and leaves stepped.
float4 PS_SmaaEdges(VSOut i) : SV_Target
{
	float4 offset[3];
	SMAAEdgeDetectionVS(i.uv, offset);

	return float4(SMAAColorEdgeDetectionPS(i.uv, offset, Tex0), 0.0, 0.0);
}

// Tex0 edges, Tex1 area texture, Tex2 search texture. Extra.x: for SMAA T2x the subsample set of the
// quarter pixel this frame was drawn at, 1 or 2 (smaa.hlsl, @SUBSAMPLE_INDICES); 0 for SMAA 1x.
float4 PS_SmaaWeights(VSOut i) : SV_Target
{
	float2 pixcoord;
	float4 offset[3];
	SMAABlendingWeightCalculationVS(i.uv, pixcoord, offset);
	float s = Extra.x;

	return SMAABlendingWeightCalculationPS(i.uv, pixcoord, offset, Tex0, Tex1, Tex2, float4(s, s, s, 0.0));
}

// Tex0 colour, Tex1 blend weights.
float4 PS_SmaaBlend(VSOut i) : SV_Target
{
	float4 offset;
	SMAANeighborhoodBlendingVS(i.uv, offset);

	return SMAANeighborhoodBlendingPS(i.uv, offset, Tex0, Tex1);
}

// ---------------------------------------------------------------------------------------------
// FXAA 3.11 quality, after Timothy Lottes (public domain reference implementation).

float4 PS_FXAA(VSOut i) : SV_Target
{
	float2 px = Texel.xy;
	float2 uv = i.uv;

	float4 rgbM = Tex0.SampleLevel(Linear, uv, 0);
	float lumaM = Luma(ReadGamma(rgbM.rgb));
	float lumaS = LumaAt(uv + float2(0.0, px.y));
	float lumaE = LumaAt(uv + float2(px.x, 0.0));
	float lumaN = LumaAt(uv - float2(0.0, px.y));
	float lumaW = LumaAt(uv - float2(px.x, 0.0));

	float maxL = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
	float minL = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
	float range = maxL - minL;

	if (range < max(0.0312, maxL * AA_EDGE))
		return rgbM;

	float lumaNW = LumaAt(uv + float2(-px.x, -px.y));
	float lumaSE = LumaAt(uv + float2(px.x, px.y));
	float lumaNE = LumaAt(uv + float2(px.x, -px.y));
	float lumaSW = LumaAt(uv + float2(-px.x, px.y));

	float lumaNS = lumaN + lumaS;
	float lumaWE = lumaW + lumaE;
	float subpixRcpRange = 1.0 / range;
	float subpixNSWE = lumaNS + lumaWE;
	float edgeHorz1 = -2.0 * lumaM + lumaNS;
	float edgeVert1 = -2.0 * lumaM + lumaWE;

	float lumaNESE = lumaNE + lumaSE;
	float lumaNWNE = lumaNW + lumaNE;
	float edgeHorz2 = -2.0 * lumaE + lumaNESE;
	float edgeVert2 = -2.0 * lumaN + lumaNWNE;

	float lumaNWSW = lumaNW + lumaSW;
	float lumaSWSE = lumaSW + lumaSE;
	float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
	float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
	float edgeHorz3 = -2.0 * lumaW + lumaNWSW;
	float edgeVert3 = -2.0 * lumaS + lumaSWSE;
	float edgeHorz = abs(edgeHorz3) + edgeHorz4;
	float edgeVert = abs(edgeVert3) + edgeVert4;

	float subpixNWSWNESE = lumaNWSW + lumaNESE;
	float lengthSign = px.x;
	bool horzSpan = edgeHorz >= edgeVert;
	float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;

	if (!horzSpan)
	{
		lumaN = lumaW;
		lumaS = lumaE;
	}
	else
	{
		lengthSign = px.y;
	}

	float subpixB = subpixA * (1.0 / 12.0) - lumaM;
	float gradientN = lumaN - lumaM;
	float gradientS = lumaS - lumaM;
	float lumaNN = lumaN + lumaM;
	float lumaSS = lumaS + lumaM;
	bool pairN = abs(gradientN) >= abs(gradientS);
	float gradient = max(abs(gradientN), abs(gradientS));

	if (pairN)
		lengthSign = -lengthSign;

	float subpixC = saturate(abs(subpixB) * subpixRcpRange);

	float2 posB = uv;
	float2 offNP = horzSpan ? float2(px.x, 0.0) : float2(0.0, px.y);

	if (!horzSpan)
		posB.x += lengthSign * 0.5;
	else
		posB.y += lengthSign * 0.5;

	float2 posN = posB - offNP;
	float2 posP = posB + offNP;
	float subpixD = -2.0 * subpixC + 3.0;
	float lumaEndN = LumaAt(posN);
	float subpixE = subpixC * subpixC;
	float lumaEndP = LumaAt(posP);

	if (!pairN)
		lumaNN = lumaSS;

	float gradientScaled = gradient * 0.25;
	float lumaMM = lumaM - lumaNN * 0.5;
	float subpixF = subpixD * subpixE;
	bool lumaMLTZero = lumaMM < 0.0;

	lumaEndN -= lumaNN * 0.5;
	lumaEndP -= lumaNN * 0.5;

	bool doneN = abs(lumaEndN) >= gradientScaled;
	bool doneP = abs(lumaEndP) >= gradientScaled;

	if (!doneN)
		posN -= offNP * 1.5;

	if (!doneP)
		posP += offNP * 1.5;

	for (int k = 0; k < 10; k++)
	{
		if (doneN && doneP)
			break;

		float stepSize = k < 4 ? 2.0 : (k < 6 ? 4.0 : 8.0);

		if (!doneN)
		{
			lumaEndN = LumaAt(posN) - lumaNN * 0.5;
			doneN = abs(lumaEndN) >= gradientScaled;

			if (!doneN)
				posN -= offNP * stepSize;
		}

		if (!doneP)
		{
			lumaEndP = LumaAt(posP) - lumaNN * 0.5;
			doneP = abs(lumaEndP) >= gradientScaled;

			if (!doneP)
				posP += offNP * stepSize;
		}
	}

	float dstN = horzSpan ? (uv.x - posN.x) : (uv.y - posN.y);
	float dstP = horzSpan ? (posP.x - uv.x) : (posP.y - uv.y);

	bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
	bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
	float spanLength = dstP + dstN;
	bool directionN = dstN < dstP;
	float dst = min(dstN, dstP);
	bool goodSpan = directionN ? goodSpanN : goodSpanP;
	float subpixG = subpixF * subpixF;
	float pixelOffset = dst * (-1.0 / max(spanLength, 0.000001)) + 0.5;
	float subpixH = subpixG * AA_SUBPIX;
	float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
	float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);

	float2 posM = uv;

	if (!horzSpan)
		posM.x += pixelOffsetSubpix * lengthSign;
	else
		posM.y += pixelOffsetSubpix * lengthSign;

	return float4(Tex0.SampleLevel(Linear, posM, 0).rgb, 1.0);
}

// ---------------------------------------------------------------------------------------------
// Eye adaptation. Log luminance, weighted towards the centre of the screen where you look,
// averaged by the mip chain and eased over time.

float4 PS_Luma(VSOut i) : SV_Target
{
	float3 c = ReadLinear(Tex0.SampleLevel(Linear, i.uv, 0).rgb);
	float l = max(LumaLinear(c), 0.0005);
	float2 d = (i.uv - 0.5) * 1.6;
	float w = 1.0 - 0.75 * saturate(dot(d, d));

	return float4(log2(l) * w, w, 0.0, 1.0);
}

float4 PS_Adapt(VSOut i) : SV_Target
{
	float previous = Tex0.SampleLevel(Point, float2(0.5, 0.5), 0).r;
	float4 m = Tex2.SampleLevel(Point, float2(0.5, 0.5), Extra.x);
	float current = m.r / max(m.g, 0.0001);

	return float4(lerp(previous, current, Extra.y), 0.0, 0.0, 1.0);
}

// ---------------------------------------------------------------------------------------------
// Bloom: soft-knee threshold, then a dual filter down/up chain. PS_BloomDown doubles as the
// plain downsample for clarity.

float4 PS_BloomPrefilter(VSOut i) : SV_Target
{
	// Karis average: each tap weighted by 1 / (1 + brightness), so one very bright pixel (a spark, a
	// specular glint on a gun) cannot dominate its block and flicker in and out of bloom as it moves.
	float2 t = Texel.xy;
	float3 a = ReadLinear(Tex0.SampleLevel(Linear, i.uv + float2(-t.x, -t.y), 0).rgb);
	float3 b = ReadLinear(Tex0.SampleLevel(Linear, i.uv + float2(t.x, -t.y), 0).rgb);
	float3 d = ReadLinear(Tex0.SampleLevel(Linear, i.uv + float2(-t.x, t.y), 0).rgb);
	float3 e = ReadLinear(Tex0.SampleLevel(Linear, i.uv + float2(t.x, t.y), 0).rgb);
	float wa = 1.0 / (1.0 + LumaLinear(a));
	float wb = 1.0 / (1.0 + LumaLinear(b));
	float wd = 1.0 / (1.0 + LumaLinear(d));
	float we = 1.0 / (1.0 + LumaLinear(e));
	float3 c = (a * wa + b * wb + d * wd + e * we) / (wa + wb + wd + we);

	float brightness = max(c.r, max(c.g, c.b));
	float knee = BLOOM_THRESHOLD * 0.5 + 0.0001;
	float soft = clamp(brightness - BLOOM_THRESHOLD + knee, 0.0, 2.0 * knee);
	soft = soft * soft / (4.0 * knee);

	float contribution = max(soft, brightness - BLOOM_THRESHOLD) / max(brightness, 0.0001);

	return float4(c * contribution, 1.0);
}

float4 PS_BloomDown(VSOut i) : SV_Target
{
	float2 t = Texel.xy;
	float3 s = Tex0.SampleLevel(Linear, i.uv, 0).rgb * 4.0;
	s += Tex0.SampleLevel(Linear, i.uv + float2(-t.x, -t.y), 0).rgb;
	s += Tex0.SampleLevel(Linear, i.uv + float2(t.x, -t.y), 0).rgb;
	s += Tex0.SampleLevel(Linear, i.uv + float2(-t.x, t.y), 0).rgb;
	s += Tex0.SampleLevel(Linear, i.uv + float2(t.x, t.y), 0).rgb;

	return float4(s / 8.0, 1.0);
}

// Tex0 is this level going down, Tex1 the coarser level coming up; Texel is Tex1's.
float4 PS_BloomUp(VSOut i) : SV_Target
{
	float2 t = Texel.xy * BLOOM_RADIUS;
	float3 s = Tex1.SampleLevel(Linear, i.uv + float2(-t.x * 2.0, 0.0), 0).rgb;
	s += Tex1.SampleLevel(Linear, i.uv + float2(t.x * 2.0, 0.0), 0).rgb;
	s += Tex1.SampleLevel(Linear, i.uv + float2(0.0, -t.y * 2.0), 0).rgb;
	s += Tex1.SampleLevel(Linear, i.uv + float2(0.0, t.y * 2.0), 0).rgb;
	s += Tex1.SampleLevel(Linear, i.uv + float2(-t.x, -t.y), 0).rgb * 2.0;
	s += Tex1.SampleLevel(Linear, i.uv + float2(t.x, -t.y), 0).rgb * 2.0;
	s += Tex1.SampleLevel(Linear, i.uv + float2(-t.x, t.y), 0).rgb * 2.0;
	s += Tex1.SampleLevel(Linear, i.uv + float2(t.x, t.y), 0).rgb * 2.0;

	return float4(Tex0.SampleLevel(Linear, i.uv, 0).rgb + s / 12.0, 1.0);
}

// Plain 5-tap downsample that keeps alpha (the colour chain keeps its weight there).
float4 PS_WideDown(VSOut i) : SV_Target
{
	float2 t = Texel.xy;
	float4 s = Tex0.SampleLevel(Linear, i.uv, 0) * 4.0;
	s += Tex0.SampleLevel(Linear, i.uv + float2(-t.x, -t.y), 0);
	s += Tex0.SampleLevel(Linear, i.uv + float2(t.x, -t.y), 0);
	s += Tex0.SampleLevel(Linear, i.uv + float2(-t.x, t.y), 0);
	s += Tex0.SampleLevel(Linear, i.uv + float2(t.x, t.y), 0);

	return s / 8.0;
}

// Colour bloom prefilter: keeps only strong colour, and only its hue and saturation. rgb is the
// colour minus its own luminance (so adding it back changes no brightness), a is how much
// strong colour there was. White and grey have no chroma, so highlights never enter.
float4 PS_TintPrefilter(VSOut i) : SV_Target
{
	float2 t = Texel.xy;
	float3 s = Tex0.SampleLevel(Linear, i.uv + float2(-t.x, -t.y), 0).rgb;
	s += Tex0.SampleLevel(Linear, i.uv + float2(t.x, -t.y), 0).rgb;
	s += Tex0.SampleLevel(Linear, i.uv + float2(-t.x, t.y), 0).rgb;
	s += Tex0.SampleLevel(Linear, i.uv + float2(t.x, t.y), 0).rgb;

	float3 c = ReadLinear(s * 0.25);
	float lum = LumaLinear(c);
	float mx = max(c.r, max(c.g, c.b));
	float mn = min(c.r, min(c.g, c.b));
	float chroma = (mx - mn) / max(mx, 0.0001);
	float weight = smoothstep(0.2, 0.65, chroma) * smoothstep(0.01, 0.06, lum);

	return float4((c - lum) * weight, weight);
}

// Halation prefilter: only the brightest highlights feed this chain, and they carry their own
// colour, so a white lamp bleeds warm and a red sign bleeds red. Bloom's threshold is far lower;
// this one starts where the image is already near white.
float4 PS_HaloPrefilter(VSOut i) : SV_Target
{
	float2 t = Texel.xy;
	float3 s = Tex0.SampleLevel(Linear, i.uv + float2(-t.x, -t.y), 0).rgb;
	s += Tex0.SampleLevel(Linear, i.uv + float2(t.x, -t.y), 0).rgb;
	s += Tex0.SampleLevel(Linear, i.uv + float2(-t.x, t.y), 0).rgb;
	s += Tex0.SampleLevel(Linear, i.uv + float2(t.x, t.y), 0).rgb;

	float3 c = ReadLinear(s * 0.25);
	float l = LumaLinear(c);
	float over = max(l - HALATION_THRESHOLD, 0.0);

	return float4(c * (over / max(l, 0.0001)), 1.0);
}

// Where the bright light in this frame is, and how much of it there is. One pixel, 256 taps over
// the quarter resolution blur: xy is the light's offset from the middle of the screen weighted by
// how far above "this is a light source" each sample is, z is that weight, w the brightest sample.
// Everything is judged in linear light so an sRGB and a UNORM back buffer agree.
float4 PS_LightInfo(VSOut i) : SV_Target
{
	float2 sum = float2(0.0, 0.0);
	float weight = 0.0;
	float brightest = 0.0;

	[unroll] for (int y = 0; y < 16; y++)
	{
		[unroll] for (int x = 0; x < 16; x++)
		{
			float2 uv = (float2(x, y) + 0.5) / 16.0;
			float l = LumaLinear(ReadLinear(Tex0.SampleLevel(Linear, uv, 0).rgb));
			float w = max(l - 0.6, 0.0);

			sum += (uv - 0.5) * w;
			weight += w;
			brightest = max(brightest, l);
		}
	}

	return float4(sum / 256.0, weight / 256.0, brightest);
}

// ---------------------------------------------------------------------------------------------
// Grade. In order: chromatic aberration and debanding on the source, then in linear light
// exposure, adaptation, white balance, bloom and colour bloom, then in gamma clarity, obscurance,
// monocular cues, shadows, highlights, lift/gamma/gain, contrast, saturation, vibrance, LUT,
// vignette, grain and dither.

float3 Deband(float2 uv, float3 centre)
{
	float angle = Hash(uv * Screen.xy) * 6.2831853;
	float radius = lerp(4.0, 16.0, Hash(uv * Screen.xy + 17.31));
	float2 o = float2(cos(angle), sin(angle)) * radius * Screen.zw;
	float2 p = float2(-o.y, o.x);

	float3 a = Tex0.SampleLevel(Point, uv + o, 0).rgb;
	float3 b = Tex0.SampleLevel(Point, uv - o, 0).rgb;
	float3 c = Tex0.SampleLevel(Point, uv + p, 0).rgb;
	float3 d = Tex0.SampleLevel(Point, uv - p, 0).rgb;

	// Only flat gradients are smoothed: any real edge nearby keeps the original pixel.
	float threshold = DEBAND * 0.012;
	float3 diff = max(max(abs(a - centre), abs(b - centre)), max(abs(c - centre), abs(d - centre)));
	float flat = step(max(diff.r, max(diff.g, diff.b)), threshold);

	return lerp(centre, (a + b + c + d) * 0.25, flat);
}

// ---------------------------------------------------------------------------------------------
// Depth. Diesel draws the world's distance into one of its own full screen render targets, which
// the plugin finds and copies before the HUD goes down (hooks.cpp). What is actually IN that
// buffer is not something a mod can be told, so DEPTH_MODE covers the encodings a renderer can
// use and "automatic" picks between them: a buffer holding world units reads far outside 0..1,
// while a projected one cannot leave it. The Depth view under Tools shows what is arriving.

bool HaveDepth()
{
	return DEPTH_MODE > -0.5;
}

float LinearizeHyperbolic(float d, float n, float f)
{
	return (n * f) / max(f - d * (f - n), 0.00001);
}

// The game's depth as it arrives (Tex0, the plugin's copy of it), as distance from the camera in
// world units. Worked out once a frame by PS_LinearDepth; everything else reads that result.
float DepthEyeRaw(float2 uv)
{
	float d = Tex0.SampleLevel(Point, saturate(uv), 0).r;
	float n = max(DEPTH_NEAR, 0.01);
	float f = max(DEPTH_FAR, n + 1.0);

	if (DEPTH_MODE > 2.5)
		d = 1.0 - d; // reversed Z

	if (DEPTH_MODE > 1.5)
		return clamp(LinearizeHyperbolic(d, n, f), n, f);

	if (DEPTH_MODE > 0.5)
		return clamp(d, 0.0, f);

	return d > 1.0001 ? min(d, f) : clamp(LinearizeHyperbolic(d, n, f), n, f);
}

float4 PS_LinearDepth(VSOut i) : SV_Target
{
	return float4(DepthEyeRaw(i.uv), 0.0, 0.0, 1.0);
}

// Distance from the camera, in the game's own world units.
float DepthEye(float2 uv)
{
	return Depth.SampleLevel(Point, saturate(uv), 0).r;
}

float DepthFraction(float2 uv)
{
	return saturate(DepthEye(uv) / max(DEPTH_FAR, 1.0));
}

// ---------------------------------------------------------------------------------------------
// View space. x right, y up, z forward: the distance along the view axis, which is what Diesel's
// depth target holds. The projection comes from the camera Lua measures every frame; without it (a
// menu, or nothing that needs it switched on) a 75 degree horizontal view is assumed, which is only
// ever used for a frame or two.

static const float PI = 3.14159265;

float2 TanHalf()
{
	if (CAMERA_VALID > 0.5)
		return float2(CAMERA_TAN_X, CAMERA_TAN_Y);

	return float2(0.7673, 0.7673 * Screen.y / Screen.x);
}

float3 ViewPos(float2 uv, float depth)
{
	return float3(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * TanHalf() * depth, depth);
}

// The surface position under uv, at any sub-pixel position. Passes that run at half resolution, and
// every sample they take, land between depth pixels; pairing the nearest pixel's depth with the
// in-between uv puts points half a pixel off their surface, enough to tilt a flat floor into a
// crease. Snapping the uv to the pixel instead moves samples off their line and does the same thing.
// So depth is interpolated between the four surrounding pixels in 1/depth, which is exact across any
// flat surface. Across an edge that would invent a surface in mid air, so there the nearest is used.
float3 ViewPosAt(float2 uv)
{
	float2 pixel = saturate(uv) * Screen.xy - 0.5;
	float2 corner = floor(pixel);
	float2 f = pixel - corner;
	float2 t = Screen.zw;
	// The four surrounding pixels in one fetch, gathered at the point between them so the footprint
	// is exactly these four: w top left, z top right, x bottom left, y bottom right.
	float4 q = Depth.GatherRed(Point, (corner + 1.0) * t);
	float d00 = max(q.w, 0.001);
	float d10 = max(q.z, 0.001);
	float d01 = max(q.x, 0.001);
	float d11 = max(q.y, 0.001);
	float nearest = min(min(d00, d10), min(d01, d11));
	float farthest = max(max(d00, d10), max(d01, d11));
	float depth;

	if (farthest > nearest * 1.1 + 5.0)
	{
		// The nearest pixel, the one a point sample at uv would read.
		depth = f.y < 0.5 ? (f.x < 0.5 ? q.w : q.z) : (f.x < 0.5 ? q.x : q.y);
	}
	else
	{
		float inverse = lerp(lerp(1.0 / d00, 1.0 / d10, f.x), lerp(1.0 / d01, 1.0 / d11, f.x), f.y);
		depth = 1.0 / max(inverse, 0.000001);
	}

	return ViewPos(uv, depth);
}

float2 ViewToUv(float3 p)
{
	float2 ndc = p.xy / (max(p.z, 0.001) * TanHalf());

	return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// The surface's facing, from depth alone. On each axis the neighbour nearer in depth is used, so
// the edge of something in front never tilts the surface behind it.
float3 ViewNormal(float2 uv)
{
	float2 t = Screen.zw;
	float3 c = ViewPosAt(uv);
	float3 l = ViewPosAt(uv - float2(t.x, 0.0));
	float3 r = ViewPosAt(uv + float2(t.x, 0.0));
	float3 u = ViewPosAt(uv - float2(0.0, t.y));
	float3 d = ViewPosAt(uv + float2(0.0, t.y));
	float3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
	float3 dy = abs(u.z - c.z) < abs(c.z - d.z) ? u - c : c - d;
	float3 n = normalize(cross(dy, dx));

	// Point it back towards the camera. Testing the normal against the view position, not its own z:
	// a floor seen at a shallow angle has a normal almost exactly across the view axis, where z is
	// zero give or take rounding, and flipping on that sign sent reflections into the floor.
	return dot(n, c) > 0.0 ? -n : n;
}

// World up, in view space, from the camera's pitch and roll. Straight up without a camera.
float3 WorldUpInView()
{
	if (CAMERA_VALID < 0.5)
		return float3(0.0, 1.0, 0.0);

	float pitch = radians(CAMERA_PITCH);
	float roll = radians(CAMERA_ROLL);
	float3 up = float3(0.0, cos(pitch), sin(pitch));

	return float3(-up.y * sin(roll), up.y * cos(roll), up.z);
}

// Interleaved gradient noise (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare", 2014). Neighbouring pixels get well spread values, so even a blurred single frame looks
// even rather than speckled, and stepping it along a fixed offset each frame gives temporal
// accumulation a different, well distributed sample every frame - it settles in a handful of frames
// where white noise needs dozens. Still when temporal accumulation is off, so nothing shimmers.
float InterleavedGradient(float2 pixel)
{
	return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float Noise(float2 uv, float salt)
{
	float frame = TEMPORAL > 0.5 ? fmod(FRAME, 64.0) : 0.0;

	return InterleavedGradient(floor(uv * Screen.xy) + salt * float2(47.0, 17.0) + 5.588238 * frame);
}

float PixelsAcross(float worldSize, float depth)
{
	return worldSize / max(depth * TanHalf().y, 0.001) * 0.5 * Screen.y;
}

// ---------------------------------------------------------------------------------------------
// Ambient occlusion: GTAO (Jimenez, Wu, Pesce and Jarabo, "Practical Realtime Strategies for
// Accurate Indirect Occlusion", 2016). For a few slices around the view direction it finds how far
// the surrounding geometry rises above the surface on either side, then integrates cosine weighted
// visibility over what is left open. A flat surface, at any angle, gets exactly none: occlusion only
// comes from real creases, corners and contact. Half resolution; blurred along surfaces after.

float4 PS_Ao(VSOut i) : SV_Target
{
	if (!HaveDepth() || AO <= 0.0)
		return float4(1.0, 1.0, 1.0, 1.0);

	float3 p = ViewPosAt(i.uv);
	float3 n = ViewNormal(i.uv);
	float3 v = normalize(-p);
	float radius = max(AO_RADIUS, 1.0);
	float pixelRadius = min(PixelsAcross(radius, p.z), Screen.y * 0.15);

	if (pixelRadius < 1.5)
		return float4(1.0, 1.0, 1.0, 1.0);

	// Two slices a frame when frames are accumulated (they rotate every frame, so the history holds
	// many), three when each frame has to stand alone.
	int sliceCount = TEMPORAL > 0.5 ? 2 : 3;
	float slices = sliceCount;
	float visibility = 0.0;
	float weight = 0.0;
	float jitter = Noise(i.uv, 1.7);
	float stepJitter = Noise(i.uv, 8.3);

	[loop]
	for (int slice = 0; slice < sliceCount; slice++)
	{
		float angle = (slice + jitter) * PI / slices;
		float2 dir = float2(cos(angle), sin(angle));
		float3 dirV = float3(dir.x, -dir.y, 0.0);
		float3 axis = normalize(cross(dirV, v));
		float3 ortho = cross(v, axis);
		float3 projN = n - axis * dot(n, axis);
		float projLen = max(length(projN), 0.0001);
		float cosN = saturate(dot(projN, v) / projLen);
		float gamma = sign(dot(ortho, projN)) * acos(cosN);
		float horizons[2] = {-1.0, -1.0};

		[unroll]
		for (int side = 0; side < 2; side++)
		{
			float direction = side == 0 ? -1.0 : 1.0;
			float best = -1.0;

			[unroll]
			for (int k = 1; k <= 6; k++)
			{
				float t = (k - 1.0 + stepJitter) / 6.0;
				// At least a pixel and a half out: nearer, a diagonal step lands back on this pixel's own
				// depth, a zero length offset that reads as a horizon straight across and occludes a
				// perfectly flat wall.
				float2 uv = i.uv + dir * direction * (t * t * pixelRadius + 1.5) * Screen.zw;
				float3 delta = ViewPosAt(uv) - p;
				float len = length(delta);
				float cosH = dot(delta / max(len, 0.0001), v);
				// Off the screen there is no geometry to know about. Sampling there would read the edge
				// pixel's depth again, which looks like a wall rising at the screen's border.
				float usable = all(uv >= 0.0) && all(uv <= 1.0) && len > 0.001 ? 1.0 : 0.0;
				float falloff = saturate(1.0 - len * len / (radius * radius)) * usable;

				best = max(best, lerp(-1.0, cosH, falloff));
			}

			horizons[side] = best;
		}

		float h0 = gamma + max(-acos(horizons[0]) - gamma, -PI * 0.5);
		float h1 = gamma + min(acos(horizons[1]) - gamma, PI * 0.5);
		float sinG = sin(gamma);
		float arc = 0.25 * (-cos(2.0 * h0 - gamma) + cosN + 2.0 * h0 * sinG)
			+ 0.25 * (-cos(2.0 * h1 - gamma) + cosN + 2.0 * h1 * sinG);

		visibility += projLen * arc;
		weight += projLen;
	}

	float open = weight > 0.0 ? saturate(visibility / weight) : 1.0;
	float result = lerp(1.0, open, saturate(AO));

	return float4(result, result, result, 1.0);
}

// ---------------------------------------------------------------------------------------------
// Indirect light: one bounce from nearby surfaces. The same horizon walk as the occlusion, but
// every time the horizon rises the surface that raised it is lit and visible from here, so its
// colour is gathered, weighted by how directly it faces this point. Tex0 the frame.

float4 PS_Il(VSOut i) : SV_Target
{
	if (!HaveDepth() || IL <= 0.0)
		return float4(0.0, 0.0, 0.0, 1.0);

	float3 p = ViewPosAt(i.uv);
	float3 n = ViewNormal(i.uv);
	float3 v = normalize(-p);
	float radius = max(IL_RADIUS, 1.0);
	float pixelRadius = min(PixelsAcross(radius, p.z), Screen.y * 0.2);

	if (pixelRadius < 2.0)
		return float4(0.0, 0.0, 0.0, 1.0);

	float3 light = float3(0.0, 0.0, 0.0);
	float jitter = Noise(i.uv, 3.1);
	float stepJitter = Noise(i.uv, 5.9);
	int sliceCount = TEMPORAL > 0.5 ? 2 : 3;

	[loop]
	for (int slice = 0; slice < sliceCount; slice++)
	{
		float angle = (slice + jitter) * PI / sliceCount;
		float2 dir = float2(cos(angle), sin(angle));

		[unroll]
		for (int side = 0; side < 2; side++)
		{
			float direction = side == 0 ? -1.0 : 1.0;
			float best = -1.0;

			[loop]
			for (int k = 1; k <= 8; k++)
			{
				float t = (k - 1.0 + stepJitter) / 8.0;
				float2 uv = i.uv + dir * direction * (t * t * pixelRadius + 1.5) * Screen.zw;

				if (any(uv < 0.0) || any(uv > 1.0))
					break;

				float3 delta = ViewPosAt(uv) - p;
				float len = max(length(delta), 0.0001);
				float3 towards = delta / len;
				float cosH = dot(towards, v);

				if (len >= 1.0 && cosH > best)
				{
					float rise = cosH - best;
					float facing = saturate(dot(n, towards));
					float fade = saturate(1.0 - len / radius);

					light += ReadLinear(Tex0.SampleLevel(Linear, uv, 0).rgb) * rise * facing * fade;
					best = cosH;
				}
			}
		}
	}

	return float4(light / (sliceCount * 2.0), 1.0);
}

// ---------------------------------------------------------------------------------------------
// Ray traced lighting, in screen space. Two rays a pixel leave the surface in cosine weighted
// directions and march through view space; where one passes behind the depth buffer it has hit
// something, and the light on that surface is gathered. Longer reach than the indirect light, and
// far noisier on its own: it is meant for temporal accumulation, which turns two rays a frame into
// dozens. What is off screen or hidden behind something cannot be hit - the honest limit of doing
// this after the frame rather than inside the engine.

float4 PS_Rtgi(VSOut i) : SV_Target
{
	if (!HaveDepth() || RTGI <= 0.0)
		return float4(0.0, 0.0, 0.0, 1.0);

	float3 p = ViewPosAt(i.uv);
	float3 n = ViewNormal(i.uv);
	float3 tangent = normalize(abs(n.y) < 0.99 ? cross(n, float3(0.0, 1.0, 0.0)) : cross(n, float3(1.0, 0.0, 0.0)));
	float3 bitangent = cross(n, tangent);
	float distance = max(RTGI_DISTANCE, 10.0);
	// Indirect light already gathers everything within its own reach. When it is on, the rays start
	// where it stops, so the same bounce is never added twice - they take the far field, it takes the
	// near one.
	float start = IL > 0.0 ? min(IL_RADIUS, distance * 0.8) : 0.0;
	float3 origin = p + n * max(p.z * 0.002, 1.0);
	float3 light = float3(0.0, 0.0, 0.0);

	[unroll]
	for (int ray = 0; ray < 2; ray++)
	{
		float u1 = Noise(i.uv, 11.3 + ray * 5.1);
		float u2 = Noise(i.uv, 29.7 + ray * 3.3);
		float phi = 2.0 * PI * u1;
		float r = sqrt(u2);
		float3 dir = normalize(tangent * cos(phi) * r + bitangent * sin(phi) * r + n * sqrt(1.0 - u2));
		float jitter = Noise(i.uv, 41.9 + ray);
		float3 previous = origin;

		[loop]
		for (int k = 1; k <= 16; k++)
		{
			float s = (k - 1.0 + jitter) / 16.0;
			float3 q = origin + dir * (start + (distance - start) * s * s);

			if (q.z <= 1.0)
				break;

			float2 uv = ViewToUv(q);

			if (any(uv < 0.0) || any(uv > 1.0))
				break;

			float scene = DepthEye(uv);
			float thickness = max(q.z * 0.02, 10.0) + length(q - previous);

			if (q.z > scene && q.z - scene < thickness)
			{
				light += ReadLinear(Tex0.SampleLevel(Linear, uv, 0).rgb);
				break;
			}

			previous = q;
		}
	}

	return float4(light * 0.5, 1.0);
}

// ---------------------------------------------------------------------------------------------
// Screen space reflections. The view ray is mirrored about the surface and marched until it passes
// behind the depth buffer, then refined by bisection. How much shows follows Fresnel (glancing
// angles reflect most, as on a real floor), plus a little more on surfaces that face up; it fades
// towards the screen's edges and where a ray turns back at the camera, the two places screen space
// runs out of picture. Tex0 the frame, Tex1 its half resolution blur (rough reflections).

float4 PS_Ssr(VSOut i) : SV_Target
{
	if (!HaveDepth() || SSR <= 0.0)
		return float4(0.0, 0.0, 0.0, 0.0);

	float3 p = ViewPosAt(i.uv);
	float3 n = ViewNormal(i.uv);
	float3 view = normalize(p);
	float3 r = reflect(view, n);
	float facing = saturate(dot(-view, n));
	float fresnel = 0.04 + 0.96 * pow(1.0 - facing, 5.0);
	float up = saturate(dot(n, WorldUpInView()));

	// There is no material information after the frame is drawn, so smoothness is guessed from the
	// surface itself: polished stone, tile, glass and water are smooth-looking, carpet, grass, gravel
	// and dirt are busy with fine detail. Busy surfaces reflect little and blurrily, which is what
	// stops every floor looking wet - the usual giveaway of screen space reflections.
	// Measured as the spread of brightness across neighbouring full resolution pixels, which no
	// alignment between a texture's pattern and a blur's grid can hide.
	float2 px = Screen.zw;
	float l0 = Luma(ReadGamma(Tex0.SampleLevel(Point, i.uv, 0).rgb));
	float l1 = Luma(ReadGamma(Tex0.SampleLevel(Point, i.uv + float2(px.x, 0.0), 0).rgb));
	float l2 = Luma(ReadGamma(Tex0.SampleLevel(Point, i.uv - float2(px.x, 0.0), 0).rgb));
	float l3 = Luma(ReadGamma(Tex0.SampleLevel(Point, i.uv + float2(0.0, px.y), 0).rgb));
	float l4 = Luma(ReadGamma(Tex0.SampleLevel(Point, i.uv - float2(0.0, px.y), 0).rgb));
	float mean = (l0 + l1 + l2 + l3 + l4) * 0.2;
	float spread = sqrt(max((l0 * l0 + l1 * l1 + l2 * l2 + l3 * l3 + l4 * l4) * 0.2 - mean * mean, 0.0));
	float rough = saturate(spread * 14.0);
	float amount = saturate(fresnel + up * 0.35) * saturate(SSR) * lerp(1.0, 0.15, rough);

	if (amount < 0.01)
		return float4(0.0, 0.0, 0.0, 0.0);

	float reach = min(p.z * 2.0 + 500.0, 6000.0);
	float3 origin = p + n * max(p.z * 0.002, 1.0);
	float jitter = Noise(i.uv, 51.7);
	float3 previous = origin;
	float2 hitUv = float2(0.0, 0.0);
	float hitAt = -1.0;

	[loop]
	for (int k = 1; k <= 32; k++)
	{
		float s = (k - 1.0 + jitter) / 32.0;
		float3 q = origin + r * reach * s * s;

		if (q.z <= 1.0)
			break;

		float2 uv = ViewToUv(q);

		if (any(uv < 0.0) || any(uv > 1.0))
			break;

		float scene = DepthEye(uv);
		float thickness = max(q.z * 0.03, 10.0) + length(q - previous);

		if (q.z > scene && q.z - scene < thickness)
		{
			float3 a = previous;
			float3 b = q;

			[unroll]
			for (int j = 0; j < 5; j++)
			{
				float3 middle = (a + b) * 0.5;

				if (middle.z > DepthEye(ViewToUv(middle)))
					b = middle;
				else
					a = middle;
			}

			hitUv = ViewToUv(b);
			hitAt = s;
			break;
		}

		previous = q;
	}

	if (hitAt < 0.0)
		return float4(0.0, 0.0, 0.0, 0.0);

	float2 edge = saturate(min(hitUv, 1.0 - hitUv) * 12.0);
	float confidence = edge.x * edge.y * saturate(1.0 - hitAt) * saturate(r.z * 4.0 + 1.0);
	float3 sharp = ReadLinear(Tex0.SampleLevel(Linear, hitUv, 0).rgb);
	float3 blurred = ReadLinear(Tex1.SampleLevel(Linear, hitUv, 0).rgb);

	return float4(lerp(sharp, blurred, saturate(max(SSR_BLUR, rough))), amount * confidence);
}

// Render scale cleanup: shrink the finished frame with a tent over everything one smaller pixel now
// covers. Averaging those samples is what washes out the aliasing and shimmer the engine baked into
// the image; EASU then rebuilds it at screen size. Detail is lost, engine artefacts with it.
float4 PS_Downsample(VSOut i) : SV_Target
{
	float2 ratio = float2(Texel.z * Extra.z, Texel.w * Extra.w);

	return TENT9(Tex0, i.uv, Texel.xy * ratio * 0.5);
}

// A 5x5 blur that only averages neighbours at a similar distance. Tex0 anything, half resolution.
float4 PS_DepthBlur(VSOut i) : SV_Target
{
	float centre = DepthEye(i.uv);
	float4 total = float4(0.0, 0.0, 0.0, 0.0);
	float weight = 0.0;

	[unroll]
	for (int y = -2; y <= 2; y++)
	{
		[unroll]
		for (int x = -2; x <= 2; x++)
		{
			float2 uv = i.uv + float2(x, y) * Texel.xy;
			float w = saturate(1.0 - abs(DepthEye(uv) - centre) / max(centre * 0.05 + 2.0, 1.0));

			total += Tex0.SampleLevel(Point, uv, 0) * w;
			weight += w;
		}
	}

	return weight > 0.0 ? total / weight : Tex0.SampleLevel(Point, i.uv, 0);
}

// ---------------------------------------------------------------------------------------------
// Motion vectors, estimated from the picture. The game does not keep any, so like the ReShade
// motion shaders (this follows the idea of AlucardDH's block matching, GPL-2) each frame is compared
// with the last: for every small patch, which nearby offset in the previous frame looks most like it,
// by brightness and by depth together. A coarse search at 1/8 resolution guesses big movements; each
// finer level only refines its parent's guess, so fast turns are found without searching far.

// Half resolution: gamma brightness (noise and detail sit evenly there) and depth, with mips.
float4 PS_MotionInput(VSOut i) : SV_Target
{
	float3 c = float3(0.0, 0.0, 0.0);

	c += ReadGamma(Tex0.SampleLevel(Point, i.uv + float2(-0.5, -0.5) * Screen.zw, 0).rgb);
	c += ReadGamma(Tex0.SampleLevel(Point, i.uv + float2(0.5, -0.5) * Screen.zw, 0).rgb);
	c += ReadGamma(Tex0.SampleLevel(Point, i.uv + float2(-0.5, 0.5) * Screen.zw, 0).rgb);
	c += ReadGamma(Tex0.SampleLevel(Point, i.uv + float2(0.5, 0.5) * Screen.zw, 0).rgb);

	return float4(Luma(c * 0.25), HaveDepth() ? DepthFraction(i.uv) : 0.0, 0.0, 1.0);
}

static const float2 kCross[5] = {float2(0.0, 0.0), float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0)};

// Tex0 this frame's pyramid, Tex1 last frame's, Tex2 the coarser level's answer. Extra.x the mip
// level searched (2 is the coarsest, with nothing to refine). Output: the uv offset to where this
// pixel was last frame, and how poor the best match was.
float4 PS_MotionSearch(VSOut i) : SV_Target
{
	float level = Extra.x;
	float2 texel = Texel.xy;
	int radius = level > 1.5 ? 4 : (level > 0.5 ? 2 : 1);
	float2 guess = level > 1.5 ? float2(0.0, 0.0) : Tex2.SampleLevel(Linear, i.uv, 0).rg;
	float bestCost = 100000.0;
	float2 best = guess;

	[loop]
	for (int y = -radius; y <= radius; y++)
	{
		[loop]
		for (int x = -radius; x <= radius; x++)
		{
			float2 offset = guess + float2(x, y) * texel;
			float cost = 0.0;

			[unroll]
			for (int k = 0; k < 5; k++)
			{
				float2 o = kCross[k] * texel;
				float4 now = Tex0.SampleLevel(Point, i.uv + o, level);
				float4 then = Tex1.SampleLevel(Linear, i.uv + o + offset, level);

				cost += abs(now.r - then.r) + abs(now.g - then.g) * 4.0;
			}

			// Between equally good matches, the smaller movement wins: flat areas stay still.
			cost += length(float2(x, y)) * 0.002;

			if (cost < bestCost)
			{
				bestCost = cost;
				best = offset;
			}
		}
	}

	return float4(best, bestCost, 1.0);
}

static const float kWeaponDistance = 60.0; // world units (cm): your viewmodel sits closer than this

// The view direction through a point of a camera's screen (forward component 1).
float3 RayThrough(float2 uv, float3 right, float3 up, float3 fwd, float2 tan)
{
	return right * ((uv.x * 2.0 - 1.0) * tan.x) + up * ((1.0 - uv.y * 2.0) * tan.y) + fwd;
}

// Where a direction from a camera lands on its screen, as uv; z is its distance along the forward axis.
float3 ProjectTo(float3 v, float3 right, float3 up, float3 fwd, float2 tan)
{
	float3 c = float3(dot(v, right), dot(v, up), dot(v, fwd));
	float2 ndc = c.xy / (max(c.z, 1.0e-6) * tan);

	return float3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, c.z);
}

// Tex0 this frame's result, Tex1 the history, Tex2 motion (unused when the camera is known), Tex3 last
// frame's motion pyramid (its depth). Extra.x how much history to keep, Extra.y 1 when there is none to use. History is only
// trusted where the pixel really was the same surface last frame (the depths agree), and is clipped
// to what this frame's neighbourhood allows, so moving objects do not leave ghosts behind.
float4 PS_Temporal(VSOut i) : SV_Target
{
	float4 current = Tex0.SampleLevel(Point, i.uv, 0);

	if (Extra.y > 0.5)
		return current;

	float2 before = i.uv + Tex2.SampleLevel(Linear, i.uv, 0).rg;
	float depthNow = DepthFraction(i.uv);

	// With the camera known, where this surface was comes from its depth and the two cameras as drawn,
	// not from motion guessed from the picture (measured in dev/NOTES.md). Your weapon moves with the
	// camera, so it stays put; things moving by themselves fail the depth test below and start again.
	if (PrevDrawnRight.w > 0.5)
	{
		float z = DepthEye(i.uv);
		before = i.uv;

		if (z >= kWeaponDistance)
		{
			float3 w = RayThrough(i.uv, CurRight.xyz, CurUp.xyz, CurFwd.xyz, CamTan.xy) * z + CamDelta.xyz;
			float3 p = ProjectTo(w, PrevDrawnRight.xyz, PrevDrawnUp.xyz, PrevDrawnFwd.xyz, CamTan.zw);
			before = p.xy;
			depthNow = saturate(p.z / max(DEPTH_FAR, 1.0));
		}
	}

	if (any(before < 0.0) || any(before > 1.0))
		return current;

	float depthThen = Tex3.SampleLevel(Point, before, 0).g;

	if (abs(depthNow - depthThen) > 0.01 + depthNow * 0.05)
		return current;

	float4 sum = float4(0.0, 0.0, 0.0, 0.0);
	float4 sumSq = float4(0.0, 0.0, 0.0, 0.0);

	[unroll]
	for (int y = -1; y <= 1; y++)
	{
		[unroll]
		for (int x = -1; x <= 1; x++)
		{
			float4 c = Tex0.SampleLevel(Point, i.uv + float2(x, y) * Texel.xy, 0);
			sum += c;
			sumSq += c * c;
		}
	}

	float4 mean = sum / 9.0;
	float4 spread = sqrt(max(sumSq / 9.0 - mean * mean, 0.0)) * 1.5 + 0.02;
	float4 history = clamp(Tex1.SampleLevel(Linear, before, 0), mean - spread, mean + spread);

	return lerp(current, history, saturate(Extra.x));
}

// Full resolution. Tex0 the graded frame, Tex1 motion. Blurs along the way each pixel moved, in
// linear light, capped at 32 pixels. A still camera and still scene give no motion and no change.
float4 PS_MotionBlur(VSOut i) : SV_Target
{
	float4 centre = Tex0.SampleLevel(Point, i.uv, 0);
	float2 motion = Tex1.SampleLevel(Linear, i.uv, 0).rg * saturate(MBLUR);
	float pixels = length(motion * Screen.xy);

	if (pixels < 0.75)
		return centre;

	motion *= min(1.0, 32.0 / pixels);

	float3 sum = float3(0.0, 0.0, 0.0);
	float jitter = Noise(i.uv, 61.1);

	[unroll]
	for (int k = 0; k < 12; k++)
	{
		float t = (k + jitter) / 12.0 - 0.5;
		sum += ReadLinear(Tex0.SampleLevel(Linear, i.uv + motion * t, 0).rgb);
	}

	return float4(WriteLinear(sum / 12.0), 1.0);
}

// ---------------------------------------------------------------------------------------------
// Light shafts. Half resolution. The mask keeps what can shine - bright, and far away where there
// is depth (sky, windows, lamps across a room) - and the shafts march each pixel towards where the
// bright light in the frame is (the same one pixel measurement the lens dirt uses), gathering the
// mask with decay. Anything near and dark in between blocks them, because it is not in the mask.

float4 PS_ShaftMask(VSOut i) : SV_Target
{
	float3 c = ReadLinear(Tex0.SampleLevel(Linear, i.uv, 0).rgb);
	float bright = saturate((LumaLinear(c) - 0.55) * 3.0);
	float far = HaveDepth() ? smoothstep(0.2, 0.55, DepthFraction(i.uv)) : 1.0;

	return float4(c * bright * far, 1.0);
}

// Tex0 the mask, Tex1 the light information pixel.
float4 PS_Shafts(VSOut i) : SV_Target
{
	float4 light = Tex1.SampleLevel(Point, float2(0.5, 0.5), 0);
	float amount = saturate(light.z * 25.0);

	if (amount < 0.001 || SHAFTS <= 0.0)
		return float4(0.0, 0.0, 0.0, 1.0);

	float2 source = 0.5 + light.xy / max(light.z, 0.0001);
	float2 toward = (source - i.uv) * SHAFTS_LENGTH;
	float3 sum = float3(0.0, 0.0, 0.0);
	float decay = 1.0;
	float jitter = Noise(i.uv, 71.3);

	[unroll]
	for (int k = 0; k < 48; k++)
	{
		sum += Tex0.SampleLevel(Linear, i.uv + toward * ((k + jitter) / 48.0), 0).rgb * decay;
		decay *= 0.97;
	}

	return float4(sum / 48.0 * amount, 1.0);
}

// How much the frame's brightest light glows through haze at this pixel.
float3 FogGlow(float2 uv)
{
	float4 light = LightInfo.SampleLevel(Point, float2(0.5, 0.5), 0);
	float amount = saturate(light.z * 25.0) * saturate(FOG_GLOW);

	if (amount < 0.001)
		return float3(0.0, 0.0, 0.0);

	float2 aspect = float2(Screen.x / Screen.y, 1.0);
	float2 source = 0.5 + light.xy / max(light.z, 0.0001);
	float near = saturate(1.0 - length((uv - source) * aspect) * 1.2);

	return float3(1.0, 0.86, 0.68) * near * near * near * amount;
}

// Blur that stops at edges: only neighbours at a similar distance are averaged, so occlusion
// spreads along a wall and never leaks across a silhouette.
float4 PS_AoBlur(VSOut i) : SV_Target
{
	if (!HaveDepth() || AO <= 0.0)
		return float4(1.0, 1.0, 1.0, 1.0);

	float centre = DepthEye(i.uv);
	float total = 0.0;
	float weight = 0.0;

	[unroll]
	for (int y = -2; y <= 2; y++)
	{
		[unroll]
		for (int x = -2; x <= 2; x++)
		{
			float2 uv = i.uv + float2(x, y) * Texel.xy;
			float w = saturate(1.0 - abs(DepthEye(uv) - centre) / max(centre * 0.05 + 2.0, 1.0));

			total += AoTex.SampleLevel(Point, uv, 0).r * w;
			weight += w;
		}
	}

	float visibility = weight > 0.0 ? total / weight : 1.0;

	return float4(visibility, visibility, visibility, 1.0);
}

// ---------------------------------------------------------------------------------------------
// Depth of field.
//
// Focus is WHERE YOU ARE LOOKING: the distance under the crosshair, read from a small cluster of
// depth taps at the centre of the screen (a harmonic mean, so a thin railing in front of a wall
// wins over the wall behind it, as your eye would) and eased over time in log space, so refocusing
// from 2 m to 50 m takes as long as 50 m to 2 m, like a real eye or lens. Your own weapon and hands
// are ignored for focusing and never blurred.
//
// The blur is gathered at half resolution over a golden angle spiral whose rings get further apart
// as they grow, after Dennis Gustafsson's single pass bokeh: every sample asks "does its own blur
// disc reach me?" rather than "is it near me?", which is what lets a blurred foreground spread over
// a sharp background while a blurred background never bleeds onto a sharp foreground. Bright samples
// weigh more, so highlights open into discs instead of smearing. A small tent fills the gaps between
// samples, and the composite decides sharp or blurred per FULL resolution pixel so in-focus edges
// stay crisp.

static const float kGoldenAngle = 2.39996323;
static const float2 kFocusTaps[9] = {
	float2(0.0, 0.0), float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0),
	float2(0.7071, 0.7071), float2(-0.7071, 0.7071), float2(0.7071, -0.7071), float2(-0.7071, -0.7071)
};

// Full resolution pixels, the same look at any resolution. Two sizes: from the hip, and while
// aiming, eased between as you raise and lower your sights.
// The lens has a size at all: either from the hip or while aiming. Every pass tests this, not the
// hip size alone, or setting the hip to zero would switch the whole thing off.
bool DofOn()
{
	return DOF > 0.0 || DOF_AIM > 0.0;
}

float DofMaxRadius(float aim)
{
	return max(lerp(DOF, DOF_AIM, saturate(aim)), 0.0) * 24.0 * (Screen.y / 1080.0);
}

// Signed blur radius in full resolution pixels: negative in front of the focus, positive behind it.
// aim is how far your sights are up, eased: it picks between the two blur sizes.
float DofCoc(float2 uv, float focus, float aim)
{
	float d = DepthEye(uv);

	if (DOF_WEAPON > 0.5 && d < kWeaponDistance)
		return 0.0;

	// A thin lens blurs in proportion to 1 - focus / distance: zero at the focus, rising towards 1
	// far behind it, and growing without limit in front of it.
	float c = 1.0 - focus / max(d, 1.0);
	float sharp = saturate(DOF_RANGE) * 0.5;
	float behind = saturate((c - sharp) / max(1.0 - sharp, 0.001));
	float front = saturate((-c - sharp) / max(1.0 - sharp, 0.001)) * DOF_NEAR;

	return (behind - front) * DofMaxRadius(aim);
}

// 1x1: r the focus distance, eased; g how open the lens is. Tex0 last frame's. Extra.x how far focus
// moves this frame, Extra.y 1 to jump straight there (the first frame, or after the effect was off).
// g eases 0..1 as you raise and lower your sights over about a fifth of a second, which is what
// picks between the hip and aiming blur sizes.
float4 PS_Focus(VSOut i) : SV_Target
{
	float4 last = Tex0.SampleLevel(Point, float2(0.5, 0.5), 0);
	float previous = last.r;
	float wantAim = CAMERA_VALID > 0.5 && CAMERA_AIM > 0.5 ? 1.0 : 0.0;
	float open = Extra.y > 0.5 ? wantAim : lerp(last.g, wantAim, 1.0 - exp(-Time.y * 12.0));
	float current = max(DOF_FOCUS, 0.0) * max(DEPTH_FAR, 1.0);

	if (DOF_FOCUS <= 0.001)
	{
		float2 aspect = float2(Screen.y / Screen.x, 1.0);
		float inverse = 0.0;
		float weight = 0.0;

		[unroll]
		for (int t = 0; t < 9; t++)
		{
			float d = DepthEye(0.5 + kFocusTaps[t] * aspect * 0.025);

			if (DOF_WEAPON < 0.5 || d >= kWeaponDistance)
			{
				float w = t == 0 ? 3.0 : 1.0;
				inverse += w / max(d, 1.0);
				weight += w;
			}
		}

		// Nothing but your own weapon under the crosshair: keep the last focus.
		current = weight > 0.0 ? weight / inverse : (previous > 0.0 ? previous : max(DEPTH_FAR, 1.0));
	}

	current = max(current, 1.0);

	if (Extra.y > 0.5 || previous <= 0.0)
		return float4(current, open, 0.0, 1.0);

	return float4(exp(lerp(log(max(previous, 1.0)), log(current), saturate(Extra.x))), open, 0.0, 1.0);
}

// Half resolution: linear colour, and the blur radius (the strongest foreground blur in the 2x2, or
// else the weakest background blur, so no pixel is blurred more than its sharpest neighbour needs).
// Tex0 the frame, Tex1 the focus.
float4 PS_DofPrepare(VSOut i) : SV_Target
{
	if (!HaveDepth() || !DofOn())
		return float4(0.0, 0.0, 0.0, 0.0);

	float4 lens = Tex1.SampleLevel(Point, float2(0.5, 0.5), 0);
	float focus = lens.r;
	float3 colour = float3(0.0, 0.0, 0.0);
	float front = 0.0;
	float behind = 100000.0;

	// The four diagonal taps of the focus table, scaled to half a full resolution pixel: exactly the
	// four pixels this half resolution texel covers.
	[unroll]
	for (int t = 5; t < 9; t++)
	{
		float2 uv = i.uv + kFocusTaps[t] * 0.7071 * Screen.zw;
		float coc = DofCoc(uv, focus, lens.g);

		colour += ReadLinear(Tex0.SampleLevel(Point, uv, 0).rgb);
		front = min(front, coc);
		behind = min(behind, max(coc, 0.0));
	}

	return float4(colour * 0.25, front < 0.0 ? front : behind);
}

// Tex0 the prepared half resolution buffer. Alpha out: how much a blurred foreground covers this pixel.
float4 PS_DofBlur(VSOut i) : SV_Target
{
	float4 centre = Tex0.SampleLevel(Point, i.uv, 0);

	if (!HaveDepth() || !DofOn())
		return float4(centre.rgb, 0.0);

	float maxRadius = DofMaxRadius(Tex1.SampleLevel(Point, float2(0.5, 0.5), 0).g);
	float centreSize = abs(centre.a);
	float centreDepth = DepthEye(i.uv);
	// About 96 samples to reach the largest disc: the spiral's rings get further apart as it grows.
	float spacing = max(maxRadius * maxRadius / 192.0, 0.5);
	float firstWeight = 1.0 + 3.0 * LumaLinear(centre.rgb);
	float3 sum = centre.rgb * firstWeight;
	float total = firstWeight;
	float cover = 0.0;
	float radius = spacing;
	float angle = Hash(i.uv * Screen.xy) * 6.2831853;

	[loop]
	for (int k = 0; k < 96; k++)
	{
		if (radius >= maxRadius)
			break;

		float2 uv = i.uv + float2(cos(angle), sin(angle)) * radius * Screen.zw;
		float4 s = Tex0.SampleLevel(Linear, uv, 0);
		float size = abs(s.a);
		bool farther = DepthEye(uv) > centreDepth;

		// Something farther away may not blur over something nearer and sharper.
		if (farther)
			size = min(size, centreSize * 2.0);

		float reach = smoothstep(radius - 1.0, radius + 1.0, size);
		float w = reach * (1.0 + 3.0 * LumaLinear(s.rgb));

		sum += s.rgb * w;
		total += w;

		if (!farther && s.a < 0.0)
			cover = max(cover, reach);

		radius += spacing / radius;
		angle += kGoldenAngle;
	}

	return float4(sum / total, cover);
}

// A small tent over the half resolution blur, to hide the gaps between spiral samples.
float4 PS_DofFill(VSOut i) : SV_Target
{
	return TENT9(Tex0, i.uv, Texel.xy);
}

// Full resolution. Tex0 the sharp frame, Tex1 the filled blur, Tex2 the focus.
float4 PS_DofComposite(VSOut i) : SV_Target
{
	float4 sharp = Tex0.SampleLevel(Point, i.uv, 0);

	if (!HaveDepth() || !DofOn())
		return sharp;

	float4 lens = Tex2.SampleLevel(Point, float2(0.5, 0.5), 0);
	float4 blurred = Tex1.SampleLevel(Linear, i.uv, 0);
	float size = abs(DofCoc(i.uv, lens.r, lens.g));
	float amount = max(saturate((size - 0.5) / 1.5), blurred.a);

	return float4(lerp(sharp.rgb, WriteLinear(blurred.rgb), amount), 1.0);
}

// ---------------------------------------------------------------------------------------------
// Surface grain. Fine detail that belongs to the surfaces rather than to the screen: each frame the
// grain is looked up where this pixel was LAST frame and kept, so it travels with the wall or floor
// it sits on instead of crawling over them like film grain does. It is re-seeded wherever that
// lookup cannot be trusted (off screen, or a different distance, so a different surface), and the
// cells shrink with distance the way a real surface's own texture does. With no motion vectors it
// simply stands still.
//
// Tex0 last frame's grain, Tex1 the motion vectors, Tex3 last frame's depth. Extra.y 1 = start over.
float4 PS_Grain(VSOut i) : SV_Target
{
	float depth = HaveDepth() ? DepthEye(i.uv) : 1000.0;
	float size = max(lerp(1.0, 4.0, saturate(SURFACE_GRAIN_SIZE)) * saturate(600.0 / max(depth, 1.0)), 1.0);
	float fresh = Hash(floor(i.uv * Screen.xy / size) + 0.5);
	float2 before = i.uv + Tex1.SampleLevel(Linear, i.uv, 0).rg;
	float depthNow = HaveDepth() ? DepthFraction(i.uv) : 0.0;

	if (Extra.y > 0.5 || any(before < 0.0) || any(before > 1.0) || abs(depthNow - Tex3.SampleLevel(Point, before, 0).g) > 0.01 + depthNow * 0.05)
		return float4(fresh, fresh, fresh, 1.0);

	// Resampling blurs the pattern a little every frame; the contrast is put back so it stays grain.
	float history = saturate((Tex0.SampleLevel(Linear, before, 0).r - 0.5) * 1.12 + 0.5);
	float kept = lerp(fresh, history, 0.92);

	return float4(kept, kept, kept, 1.0);
}

float4 PS_Grade(VSOut i) : SV_Target
{
	float3 source = Tex0.SampleLevel(Point, i.uv, 0).rgb;

	if (CHROMATIC > 0.0)
	{
		float2 d = i.uv - 0.5;
		float2 offset = d * dot(d, d) * CHROMATIC * 0.025;

		source.r = Tex0.SampleLevel(Linear, i.uv + offset, 0).r;
		source.b = Tex0.SampleLevel(Linear, i.uv - offset, 0).b;
	}

	if (DEBAND > 0.0)
		source = Deband(i.uv, source);

	// The four neighbours, shared by moire reduction and texture detail below.
	float2 texel = Screen.zw;
	float3 left = Tex0.SampleLevel(Point, i.uv - float2(texel.x, 0.0), 0).rgb;
	float3 right = Tex0.SampleLevel(Point, i.uv + float2(texel.x, 0.0), 0).rgb;
	float3 above = Tex0.SampleLevel(Point, i.uv - float2(0.0, texel.y), 0).rgb;
	float3 below = Tex0.SampleLevel(Point, i.uv + float2(0.0, texel.y), 0).rgb;
	float3 neighbours = (left + right + above + below) * 0.25;
	float3 slope = abs(left - right) + abs(above - below);

	// Moire reduction. The shimmer a fine texture makes when its pattern lands near pixel size is
	// detail that ALTERNATES pixel to pixel while barely sloping across them - unlike a real edge,
	// which slopes. Where that is found, and only there, the pixel is eased towards its neighbours.
	// It is strongest far away, where a texture's pattern is squeezed smallest.
	if (DEMOIRE > 0.0)
	{
		float alternating = Luma(abs(source - neighbours));
		float across = Luma(slope);
		float shimmer = saturate((alternating * 4.0 - across * 0.8) * 8.0);
		float distance = HaveDepth() ? saturate(lerp(0.55, 1.0, DepthFraction(i.uv) * 4.0)) : 0.8;

		// Half way is the average of the four neighbours, which cancels an alternating pattern
		// exactly. Going further would push it past the average and invert the pattern instead.
		source = lerp(source, neighbours, saturate(DEMOIRE) * shimmer * distance * 0.5);
	}

	float3 c = ReadLinear(source);
	float ev = EXPOSURE;

	if (Extra.x > 0.5)
	{
		float averageLog = Tex2.SampleLevel(Point, float2(0.5, 0.5), 0).r;

		ev += clamp(log2(max(ADAPT_KEY, 0.001)) - averageLog, ADAPT_MIN, ADAPT_MAX) * ADAPT;
	}

	if (ev != 0.0)
		c *= pow(2.0, ev);

	// Local tone mapping. The picture is split into a base (the local average brightness, in stops)
	// and the detail on top of it. The base is pulled towards middle grey, so a dark corridor and a
	// bright window can both be seen at once, then the detail is put back - a little stronger - so
	// nothing goes flat. The wide base is only trusted where it agrees with the near one, which is
	// what keeps a halo from forming round a bright window. Tex3 quarter and Tex6 1/8 blur, taken
	// before exposure, so they are scaled to match.
	if (LTM > 0.0)
	{
		float scale = pow(2.0, ev);
		float here = log2(max(LumaLinear(c), 0.00001));
		float near = log2(max(LumaLinear(ReadLinear(Tex3.SampleLevel(Linear, i.uv, 0).rgb)) * scale, 0.00001));
		float wide = log2(max(LumaLinear(ReadLinear(TENT9(Tex6, i.uv, Screen.zw * 8.0).rgb)) * scale, 0.00001));
		float base = lerp(near, wide, exp(-abs(near - wide) * 1.5));
		const float middle = -2.47; // log2(0.18)
		float compressed = middle + (base - middle) * (1.0 - 0.5 * saturate(LTM));
		float target = compressed + (here - base) * (1.0 + LTM_DETAIL);

		c *= pow(2.0, clamp(target - here, -3.0, 3.0));
	}

	if (TEMPERATURE != 0.0 || TINT != 0.0)
	{
		float before = LumaLinear(c);
		c *= float3(1.0 + 0.12 * TEMPERATURE + 0.06 * TINT, 1.0 - 0.12 * TINT, 1.0 - 0.12 * TEMPERATURE + 0.06 * TINT);
		c *= before / max(LumaLinear(c), 0.00001);
	}

	// Ambient occlusion from real geometry. Applied before bloom, so a darkened crease does not
	// also stop glowing, and capped like Obscurance so it deepens contact rather than painting
	// black into corners.
	// Multi-bounce (from the GTAO paper): light that is occluded once bounces around a bright crease
	// and partly comes back, so bright surfaces keep more light in their corners than dark ones.
	// Without it occlusion reads as dirt on white walls.
	if (HaveDepth() && AO > 0.0)
	{
		float v = lerp(1.0, saturate(AoTex.SampleLevel(Linear, i.uv, 0).r), 0.85);
		float3 albedo = saturate(c * 1.2);
		float3 ka = 2.0404 * albedo - 0.3324;
		float3 kb = -4.7951 * albedo + 0.6417;
		float3 kc = 2.7552 * albedo + 0.6903;

		c *= max(float3(v, v, v), ((v * ka + kb) * v + kc) * v);
	}

	// Light from other surfaces lands in proportion to how much light this surface reflects, which
	// the frame only tells us through its own colour: dark stays dark, coloured takes colour.
	if (HaveDepth() && (IL > 0.0 || RTGI > 0.0))
	{
		float3 surface = saturate(c * 1.5 + 0.03);
		float3 gathered = float3(0.0, 0.0, 0.0);

		if (IL > 0.0)
			gathered += IlTex.SampleLevel(Linear, i.uv, 0).rgb * IL * 2.0;

		if (RTGI > 0.0)
			gathered += RtgiTex.SampleLevel(Linear, i.uv, 0).rgb * RTGI;

		c += gathered * surface;
	}

	if (HaveDepth() && SSR > 0.0)
	{
		float4 reflection = SsrTex.SampleLevel(Linear, i.uv, 0);
		c = lerp(c, reflection.rgb, saturate(reflection.a));
	}

	// Debug views 3 to 6: one depth effect alone. Occlusion as grey (white is open, dark is occluded);
	// the light the others add, 4x brighter so faint light shows. Magenta and black checks: that
	// effect is not running (no depth, or switched off).
	if (DEBUG_VIEW > 2.5)
	{
		float3 v;
		float strength = DEBUG_VIEW < 3.5 ? AO : (DEBUG_VIEW < 4.5 ? IL : (DEBUG_VIEW < 5.5 ? RTGI : SSR));

		if (!HaveDepth() || strength <= 0.0)
		{
			float2 cell = floor(i.uv * Screen.xy / 16.0);
			float check = frac((cell.x + cell.y) * 0.5) > 0.25 ? 1.0 : 0.0;
			v = float3(check, 0.0, check);
		}
		else if (DEBUG_VIEW < 3.5)
		{
			v = saturate(AoTex.SampleLevel(Linear, i.uv, 0).rrr);
		}
		else if (DEBUG_VIEW < 4.5)
		{
			v = IlTex.SampleLevel(Linear, i.uv, 0).rgb * IL * 2.0 * 4.0;
		}
		else if (DEBUG_VIEW < 5.5)
		{
			v = RtgiTex.SampleLevel(Linear, i.uv, 0).rgb * RTGI * 4.0;
		}
		else
		{
			float4 reflection = SsrTex.SampleLevel(Linear, i.uv, 0);
			v = reflection.rgb * saturate(reflection.a) * 4.0;
		}

		return float4(WriteLinear(saturate(v)), 1.0);
	}

	if (SHAFTS > 0.0)
		c += ShaftTex.SampleLevel(Linear, i.uv, 0).rgb * SHAFTS * 2.0;

	float3 bloom = float3(0.0, 0.0, 0.0);

	if (Extra.y > 0.5)
	{
		bloom = Tex1.SampleLevel(Linear, i.uv, 0).rgb / 6.0;
		c += bloom * BLOOM;
	}

	// Colour bloom: strong colour spreads softly into its surroundings, like light bouncing off a
	// painted wall. The spread carries no luminance of its own, and it lands more on lit surfaces
	// than on black, so the overall brightness stays where it was.
	if (COLOR_BLOOM > 0.0)
	{
		float reach = saturate((COLOR_BLOOM_RADIUS - 0.5) / 1.5);
		float3 spread = lerp(TENT9(Tex8, i.uv, Screen.zw * 16.0).rgb, TENT9(Tex9, i.uv, Screen.zw * 32.0).rgb, reach);
		float receiver = 0.3 + 0.7 * saturate(LumaLinear(c) * 3.0);

		c = max(c + spread * COLOR_BLOOM * 2.0 * receiver, 0.0);
	}

	// Halation: the warm bleed film gets around its brightest highlights, from light scattering
	// through the emulsion. Only near-white highlights feed it, it is blurred far wider than
	// bloom, and it lands warm rather than white.
	if (HALATION > 0.0)
	{
		float reach = saturate((HALATION_RADIUS - 0.5) / 1.5);
		float3 halo = lerp(TENT9(Tex12, i.uv, Screen.zw * 16.0).rgb, TENT9(Tex13, i.uv, Screen.zw * 32.0).rgb, reach);
		float3 warm = halo * lerp(float3(1.0, 1.0, 1.0), float3(1.0, 0.42, 0.20), HALATION_TINT);

		c += warm * HALATION * 0.6;
	}

	// Lens dirt: dust and smears on the front element, lit by the bright light that is actually in
	// the frame and thrown to the opposite side of the picture, the way a real lens does. With no
	// bright source there is nothing to light it, so it disappears completely instead of greying
	// the image: exactly what makes vanilla lens dirt wash out a dark scene.
	if (DIRT > 0.0)
	{
		float4 light = LightInfo.SampleLevel(Point, float2(0.5, 0.5), 0);
		float amount = saturate(light.z * 25.0);

		if (amount > 0.001)
		{
			float2 towards = light.xy / max(light.z, 0.0001);
			float2 uv = saturate(i.uv + towards * DIRT_OFFSET);
			float speck = Dirt.SampleLevel(Linear, uv, 0).r;
			float side = saturate(dot(i.uv - 0.5, -towards) * 8.0 + 0.35);

			// Squared, so the sparse bright specks show and the dark field stays dark.
			c += speck * speck * side * amount * DIRT * 0.2;
		}
	}

	// Aerial perspective: the air between you and a distant surface scatters light into the line of
	// sight, so far things lose contrast and take the colour of the air. This is the one depth
	// effect that changes how deep a space reads, rather than how it is lit. It follows how bright
	// the scene is, so a night heist gets a dark haze instead of a grey wash.
	if (HaveDepth() && FOG > 0.0)
	{
		float t = saturate((DepthFraction(i.uv) - FOG_START) / max(1.0 - FOG_START, 0.001));
		float level = Extra.x > 0.5 ? saturate(exp2(Tex2.SampleLevel(Point, float2(0.5, 0.5), 0).r) * 3.0) : 0.5;
		float3 air = lerp(float3(0.42, 0.52, 0.68), float3(0.68, 0.58, 0.44), FOG_TINT);

		c = lerp(c, air * lerp(0.15, 1.0, level) + FogGlow(i.uv), t * t * FOG * 0.85);
	}

	// Height fog: thickest around your feet and thinning upwards, integrated along each view ray
	// through that exponential layer, so looking down into a room thickens it and looking up thins
	// it. The layer moves with you: PAYDAY's maps sit at every height, so there is no one floor
	// level to anchor it to. Needs the camera's pitch.
	if (HaveDepth() && HFOG > 0.0 && CAMERA_VALID > 0.5)
	{
		float3 pv = ViewPosAt(i.uv);
		float distance = length(pv);
		float rise = dot(pv, WorldUpInView());
		float k = 1.0 / max(HFOG_HEIGHT, 1.0);
		const float eye = 150.0; // your eye above your feet, in world units
		float slope = rise * k;
		float along = abs(slope) > 0.0001 ? (1.0 - exp(-slope)) / slope : 1.0;
		float thickness = HFOG * 0.002 * distance * exp(-eye * k) * along;
		float level = Extra.x > 0.5 ? saturate(exp2(Tex2.SampleLevel(Point, float2(0.5, 0.5), 0).r) * 3.0) : 0.5;
		float3 air = lerp(float3(0.42, 0.52, 0.68), float3(0.68, 0.58, 0.44), FOG_TINT) * lerp(0.15, 1.0, level);

		c = lerp(air + FogGlow(i.uv), c, exp(-max(thickness, 0.0)));
	}

	if (DEBUG_VIEW > 0.5 && DEBUG_VIEW < 1.5)
		return float4(WriteLinear(saturate(bloom * BLOOM * 4.0)), 1.0);

	// Soft shoulder, only when exposure or bloom pushed values past white. Neutral settings
	// leave the image bit-for-bit alone.
	float shoulder = saturate(max(ev, 0.0) * 1.5 + BLOOM * 2.0);

	if (shoulder > 0.0)
	{
		float m = max(c.r, max(c.g, c.b));

		if (m > 0.8)
		{
			float mapped = 0.8 + 0.2 * (1.0 - exp(-(m - 0.8) / 0.2));
			c *= lerp(1.0, mapped / m, shoulder);
		}
	}

	float3 g = LinearToSrgb(c);

	if (CLARITY != 0.0)
	{
		float blurred = Luma(ReadGamma(Tex3.SampleLevel(Linear, i.uv, 0).rgb));
		float detail = Luma(ReadGamma(source)) - blurred;
		float midtones = saturate(1.0 - abs(Luma(g) * 2.0 - 1.0));

		g += detail * CLARITY * 1.5 * midtones;
	}

	// Texture detail. Surfaces in an older game are flat because their textures are low resolution,
	// not because the light is wrong, so the fine contrast that IS there is amplified: the pixel
	// against its immediate neighbours, held back at real edges (where it would ring) and in the
	// darkest and brightest parts (where there is nothing to show). It invents nothing; it makes
	// what the texture already has read as texture.
	if (DETAIL > 0.0)
	{
		float3 fine = ReadGamma(source) - ReadGamma(neighbours);
		float edge = saturate(Luma(slope) * 8.0);
		float middle = saturate(1.0 - abs(Luma(g) * 2.0 - 1.0));

		g += fine * DETAIL * 1.5 * (1.0 - edge * 0.75) * (0.35 + 0.65 * middle);
	}

	// Obscurance: bloom in reverse. A pixel whose surroundings are darker than it is pulled down,
	// so creases, contact areas and recesses deepen. Colour only, no depth. In a dark scene (judged
	// over the widest blur) there is no effect at all, so a light in a dark room or the edge of a
	// shadow you need to see into is never dimmed; the darkening is capped, and the shadow recovery
	// below runs after it.
	// Obscurance guesses recesses from brightness. With real ambient occlusion running it would only
	// darken the same creases twice, so it steps aside.
	if (OBSCURANCE > 0.0 && !(HaveDepth() && AO > 0.0))
	{
		float reach = saturate((OBSCURANCE_RADIUS - 0.5) / 1.5);
		float around = Luma(ReadGamma(lerp(TENT9(Tex6, i.uv, Screen.zw * 8.0).rgb, TENT9(Tex7, i.uv, Screen.zw * 32.0).rgb, reach)));
		float here = Luma(ReadGamma(source));
		// How lit this part of the scene is, over about an eighth of the screen around the pixel.
		// Judged in linear light, so it reads the same whether the blur was averaged in gamma
		// (a UNORM back buffer) or in linear light (an sRGB view).
		float scene = LumaLinear(ReadLinear(TENT9(Tex7, i.uv, Screen.zw * 128.0).rgb));
		float contrast = saturate((here - around) / max(here, 0.05));

		// Moderate contrast is a surface next to a darker one: deepen it. Extreme contrast is a
		// light against darkness: leave it, as the eye would.
		float occlusion = contrast * (1.0 - smoothstep(0.45, 0.7, contrast));
		float protect = smoothstep(0.05, 0.2, scene);

		g *= 1.0 - min(saturate(OBSCURANCE) * occlusion * protect * 0.9, 0.35);
	}

	// Monocular cues: depth-free hints that help shapes read against their background, from the
	// image alone. Separation is local contrast compressed so edges do not halo; flat, low-contrast
	// areas recede a little (aerial perspective); the edges of the screen soften slightly so the
	// centre, where you look, keeps full definition.
	if (CUES > 0.0)
	{
		float3 srcGamma = ReadGamma(source);
		float3 nearGamma = ReadGamma(Tex3.SampleLevel(Linear, i.uv, 0).rgb);
		float here = Luma(srcGamma);
		float near = Luma(nearGamma);
		float far = Luma(ReadGamma(TENT9(Tex7, i.uv, Screen.zw * 32.0).rgb));
		float mid = saturate(1.0 - abs(Luma(g) * 2.0 - 1.0));

		float detail = here - near;
		float separation = detail / (1.0 + abs(detail) * 8.0);
		g += separation * CUES * CUES_SEPARATION * 1.5 * (0.3 + 0.7 * mid);

		float flatness = 1.0 - saturate((abs(detail) + abs(here - far)) * 6.0);
		// Receding flat areas is a guess at distance haze; with real aerial perspective on, it steps aside.
		float recede = CUES * CUES_DEPTH * flatness * (HaveDepth() && FOG > 0.0 ? 0.0 : 1.0);
		float lg = Luma(g);
		g = lerp(g, float3(lg, lg, lg), recede * 0.15);
		g += (far - here) * recede * 0.12;

		float2 fromCentre = (i.uv - 0.5) * float2(Screen.x / Screen.y, 1.0);
		float edge = smoothstep(0.25, 0.85, length(fromCentre));
		g -= (srcGamma - nearGamma) * CUES * CUES_FOCUS * 0.6 * edge;
	}

	g += SHADOWS * 3.0 * g * pow(1.0 - saturate(g), 4.0);
	g -= HIGHLIGHTS * 3.0 * (1.0 - saturate(g)) * pow(saturate(g), 4.0);
	g = GAIN * (g + LIFT * (1.0 - g));
	g = pow(saturate(g), 1.0 / max(GAMMA, 0.05));

	if (CONTRAST >= 0.0)
		g = lerp(g, g * g * (3.0 - 2.0 * g), CONTRAST);
	else
		g = lerp(g, float3(0.5, 0.5, 0.5), -CONTRAST * 0.5);

	float l = Luma(g);
	g = lerp(float3(l, l, l), g, SATURATION);

	float mx = max(g.r, max(g.g, g.b));
	float mn = min(g.r, min(g.g, g.b));
	g = lerp(float3(l, l, l), g, 1.0 + VIBRANCE * (1.0 - (mx - mn)));
	g = saturate(g);

	if (LUT_MIX > 0.0 && LUT_SIZE > 1.0)
	{
		float3 uvw = g * ((LUT_SIZE - 1.0) / LUT_SIZE) + 0.5 / LUT_SIZE;
		g = lerp(g, Lut.SampleLevel(Linear, uvw, 0).rgb, LUT_MIX);
	}

	if (VIGNETTE > 0.0)
	{
		float2 d = (i.uv - 0.5) * float2(Screen.x / Screen.y, 1.0);
		g *= 1.0 - VIGNETTE * smoothstep(0.35, 1.0, length(d) * 1.1);
	}

	// Surface grain from PS_Grain: it travels with the surfaces, so it reads as texture rather than
	// as a dirty screen. Kept out of black, where there is nothing to texture, and faded with
	// distance, where it would only add shimmer.
	if (SURFACE_GRAIN > 0.0)
	{
		float grain = GrainTex.SampleLevel(Point, i.uv, 0).r - 0.5;
		float lit = saturate(Luma(g) * 2.5);
		float close = HaveDepth() ? saturate(1.0 - DepthFraction(i.uv) * 4.0) : 1.0;

		g += grain * SURFACE_GRAIN * 0.16 * lit * close;
	}

	// Breaks up 8-bit banding that shadow lifting and grading make visible.
	if (DITHER > 0.0)
		g += (Hash(i.uv * Screen.xy + frac(TIME * 7.0) * 61.0) - 0.5) * DITHER * (1.5 / 255.0);

	// The depth view has its own switch, so it can never be confused with the exposure mode of the
	// debug list (which tints the whole screen red and looks exactly like a failure warning).
	// Near is bright, far is dark, and the bands make the falloff readable. A magenta and black
	// checkerboard - a pattern no effect in this mod can produce - means no depth was found.
	// Motion vectors: the direction a pixel came from, as colour, brighter for faster.
	if (MOTION_VIEW > 0.5)
	{
		float2 motion = MotionTex.SampleLevel(Point, i.uv, 0).rg * Screen.xy / 16.0;
		return float4(WriteGamma(saturate(float3(0.5 + motion.x, 0.5 + motion.y, 0.5 - length(motion) * 0.5))), 1.0);
	}

	if (DEPTH_VIEW > 0.5)
	{
		if (!HaveDepth())
		{
			float2 cell = floor(i.uv * Screen.xy / 16.0);
			float check = frac((cell.x + cell.y) * 0.5) > 0.25 ? 1.0 : 0.0;

			return float4(WriteGamma(float3(check, 0.0, check)), 1.0);
		}

		// Log scale, near to far in ten bands of the same distance ratio, so a room a few metres across
		// is not all one shade.
		float n = max(DEPTH_NEAR, 0.01);
		float f = saturate(log(max(DepthEye(i.uv), n) / n) / log(max(DEPTH_FAR, n + 1.0) / n));
		float band = frac(f * 10.0) > 0.5 ? 1.0 : 0.82;

		return float4(WriteGamma(saturate(float3(1.0 - f, 1.0 - f, 1.0 - f) * band)), 1.0);
	}

	if (DEBUG_VIEW > 1.5)
		g = g * 0.5 + float3(saturate(ev), 0.0, saturate(-ev)) * 0.5;

	return float4(WriteGamma(saturate(g)), 1.0);
}

// ---------------------------------------------------------------------------------------------
// Temporal antialiasing. Every pixel's surface is put back where it was last frame from its depth and
// the two cameras, so last frame's result can be read from the right place even at a fraction of a
// pixel; the camera's own motion then supplies new sample positions each frame, which is what steadies
// crawling edges and brings back detail thinner than a pixel. Following Karis ("High Quality Temporal
// Supersampling", 2014) and Salvi ("An Excursion in Temporal Supersampling", 2016): last frame is read
// with a Catmull-Rom filter so it does not soften frame after frame, and clipped towards this frame's
// neighbourhood (mean +- one standard deviation, in YCoCg) so things that move by themselves, which the
// camera cannot account for, do not leave trails. Tex0 this frame, Tex1 last frame's result.

float3 ToYCoCg(float3 c)
{
	return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 FromYCoCg(float3 c)
{
	return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Catmull-Rom from five bilinear taps (the four corners of the 4x4 footprint carry almost no weight).
float3 HistoryCatmullRom(float2 uv)
{
	float2 size = Texel.zw;
	float2 pos = uv * size;
	float2 c = floor(pos - 0.5) + 0.5;
	float2 f = pos - c;
	float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	float2 w3 = f * f * (-0.5 + 0.5 * f);
	float2 w12 = w1 + w2;
	float2 t0 = (c - 1.0) / size;
	float2 t3 = (c + 2.0) / size;
	float2 t12 = (c + w2 / w12) / size;

	float3 sum = Tex1.SampleLevel(Linear, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y);
	sum += Tex1.SampleLevel(Linear, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y);
	sum += Tex1.SampleLevel(Linear, t12, 0).rgb * (w12.x * w12.y);
	sum += Tex1.SampleLevel(Linear, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y);
	sum += Tex1.SampleLevel(Linear, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);

	float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;

	return max(sum / weight, 0.0);
}

// Pulls h towards the centre of the box until it is inside (Playdead's clip, INSIDE, 2016): unlike a
// clamp it keeps the colour's direction, so a clipped pixel does not change hue.
float3 ClipToBox(float3 h, float3 lo, float3 hi)
{
	float3 centre = 0.5 * (hi + lo);
	float3 extent = 0.5 * (hi - lo) + 0.0001;
	float3 v = h - centre;
	float3 a = abs(v / extent);
	float m = max(a.x, max(a.y, a.z));

	return m > 1.0 ? centre + v / m : h;
}

// Measured against a 16-sample supersampled render of a moving 3D room (tools in the source notes):
// How sharply the samples around a pixel are weighted by their distance from it, exp(-k * d * d) with d
// in pixels. Karis (2014) uses a Gaussian fit of Blackman-Harris (k = 2.29); 8 kept more detail here.
static const float kTaaSampleSharpness = 8.0;
// How far outside this frame's neighbourhood the history may stay, in standard deviations.
static const float kTaaClip = 1.5;
// Moving pixels take more of the new frame, per pixel moved, up to kTaaMovingMax: history read at a
// fraction of a pixel softens a little every frame it is carried.
static const float kTaaMoving = 0.1;
static const float kTaaMovingMax = 0.3;
// A surface farther than everything last frame had where it was, by more than this, was hidden then:
// whatever stood in front moved away by itself, or the camera has just uncovered it.
static const float kTaaDepthTolerance = 0.02;

// Extra.x: how much of this frame goes in. Extra.y: 1 starts over from this frame.
// Alpha carries how many frames the history holds (1 to 31) from frame to frame.
float4 PS_Taa(VSOut i) : SV_Target
{
	if (OutRight.w < 0.5)
		return float4(Tex0.SampleLevel(Point, i.uv, 0).rgb, 1.0);

	// Where this output pixel's view direction landed in the frame as drawn. Without jitter that is the
	// pixel itself; with it, a fraction of a pixel away, the same everywhere for the weapon and the world.
	float2 size = Texel.zw;
	float2 juv = ProjectTo(RayThrough(i.uv, OutRight.xyz, OutUp.xyz, OutFwd.xyz, CamTan.xy), CurRight.xyz, CurUp.xyz, CurFwd.xyz, CamTan.xy).xy;
	float2 centre = floor(juv * size) + 0.5;

	// This frame at that point, from the nine nearest samples weighted by their distance from it; their
	// spread for clipping the history; and the nearest surface among them, which moves the edge.
	float3 sum = float3(0.0, 0.0, 0.0);
	float weights = 0.0;
	float3 m1 = float3(0.0, 0.0, 0.0);
	float3 m2 = float3(0.0, 0.0, 0.0);
	float nearest = 1.0e30;
	float2 nearUv = juv;

	[unroll]
	for (int y = -1; y <= 1; y++)
	{
		[unroll]
		for (int x = -1; x <= 1; x++)
		{
			float2 at = centre + float2(x, y);
			float2 uv = at / size;
			float3 c = ToYCoCg(ReadLinear(Tex0.SampleLevel(Point, uv, 0).rgb));
			float2 d = at - juv * size;
			float w = exp(-kTaaSampleSharpness * dot(d, d));

			sum += c * w;
			weights += w;
			m1 += c;
			m2 += c * c;

			float z = HaveDepth() ? DepthEye(uv) : 1.0e30;

			if (z < nearest)
			{
				nearest = z;
				nearUv = uv;
			}
		}
	}

	float3 current = sum / weights;

	float4 fresh = float4(WriteLinear(max(FromYCoCg(current), 0.0)), 1.0);

	if (Extra.y > 0.5 || CamDelta.w < 0.5 || !HaveDepth())
		return fresh;

	// Where this surface was last frame. Your own weapon and hands move with the camera, so they stay put.
	float2 prevUv = i.uv;

	if (nearest >= kWeaponDistance)
	{
		float3 w = RayThrough(nearUv, CurRight.xyz, CurUp.xyz, CurFwd.xyz, CamTan.xy) * nearest + CamDelta.xyz;
		float3 before = ProjectTo(w, PrevRight.xyz, PrevUp.xyz, PrevFwd.xyz, CamTan.zw);

		if (before.z < 1.0)
			return fresh;

		prevUv = before.xy + (juv - nearUv);

		// Was it hidden last frame? If last frame's distance where it was (the four nearest, so a fraction
		// of a pixel cannot fake it at an edge) is all nearer, something stood in front of it that has
		// moved away (an enemy, a door) or the camera has just uncovered it, and last frame's colour here
		// is that something's: take this frame instead, so it leaves no trail (FSR 2's depth clip does the
		// same). Only that way round: a thin wire the jitter catches in one frame and misses in the next
		// looks just like something new arriving in front, and rejecting that made wires flicker.
		if (PrevRight.w > 0.5)
		{
			float2 at = before.xy * size - 0.5;
			float4 q = Tex2.GatherRed(Point, (floor(at) + 1.0) / size);
			float hi = max(max(q.x, q.y), max(q.z, q.w));

			if (before.z > hi * (1.0 + kTaaDepthTolerance))
				return fresh;
		}
	}

	if (any(prevUv < 0.0) || any(prevUv > 1.0))
		return fresh;

	float3 mean = m1 / 9.0;
	float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
	float3 history = ClipToBox(ToYCoCg(ReadLinear(HistoryCatmullRom(prevUv))), mean - kTaaClip * sigma, mean + kTaaClip * sigma);

	// Frames held since a fresh start (FSR 2's accumulation weight, ffx_fsr2_accumulate.h): a young history
	// is the average of its frames, 1/2, 1/3, ... rather than mostly its first, aliased frame. Never less
	// of the new frame than before. FSR 2 also resets the count where clipping moves the history; that
	// doubled the flicker of a still view here, so it is left out.
	float frames = Tex1.SampleLevel(Linear, prevUv, 0).a;
	float moved = length((prevUv - i.uv) * size);
	float take = min(saturate(Extra.x + kTaaMoving * moved), max(Extra.x, kTaaMovingMax));
	float3 result = lerp(history, current, max(take, 1.0 / (frames + 1.0)));

	return float4(WriteLinear(max(FromYCoCg(result), 0.0)), min(frames + 1.0, 31.0));
}

// SMAA T2x's resolve (smaa.hlsl, SMAAResolvePS with reprojection): this frame, drawn a quarter pixel one
// way, half and half with last frame, drawn a quarter pixel the other way, so on each pixel the two
// average out to its centre; both had SMAA with the area textures for their quarter pixel. Last frame
// is moved by the camera's own motion with the jitter left out, as the reference's velocity is. Two
// frames only, so a mistake lasts one frame, not a trail. The reference's velocity weighting needs each
// object's own motion, which the game does not give; instead, as PS_Taa: your weapon stays put, a
// surface hidden last frame takes this frame only, and last frame is clamped to this frame's 3x3
// (something moving by itself). Tex0 this frame, graded; Tex1 last frame, graded; Tex2 last frame's
// distance. Extra.y 1 when there is no last frame to use.
float4 PS_SmaaT2x(VSOut i) : SV_Target
{
	float3 current = ReadLinear(Tex0.SampleLevel(Point, i.uv, 0).rgb);
	float4 fresh = float4(WriteLinear(current), 1.0);

	if (Extra.y > 0.5 || CamDelta.w < 0.5 || !HaveDepth())
		return fresh;

	float2 size = Texel.zw;
	float z = DepthEye(i.uv);
	float2 prevUv = i.uv;

	if (z >= kWeaponDistance)
	{
		float3 ray = RayThrough(i.uv, CurRight.xyz, CurUp.xyz, CurFwd.xyz, CamTan.xy);
		float2 here = ProjectTo(ray, OutRight.xyz, OutUp.xyz, OutFwd.xyz, CamTan.xy).xy;
		float3 before = ProjectTo(ray * z + CamDelta.xyz, PrevRight.xyz, PrevUp.xyz, PrevFwd.xyz, CamTan.zw);

		if (before.z < 1.0)
			return fresh;

		prevUv = i.uv + (before.xy - here);

		if (PrevRight.w > 0.5)
		{
			float2 at = before.xy * size - 0.5;
			float4 q = Tex2.GatherRed(Point, (floor(at) + 1.0) / size);
			float hi = max(max(q.x, q.y), max(q.z, q.w));

			if (before.z > hi * (1.0 + kTaaDepthTolerance))
				return fresh;
		}
	}

	if (any(prevUv < 0.0) || any(prevUv > 1.0))
		return fresh;

	float3 lo = current;
	float3 hi = current;

	[unroll]
	for (int y = -1; y <= 1; y++)
	{
		[unroll]
		for (int x = -1; x <= 1; x++)
		{
			float3 c = ReadLinear(Tex0.SampleLevel(Point, i.uv + float2(x, y) / size, 0).rgb);
			lo = min(lo, c);
			hi = max(hi, c);
		}
	}

	float3 previous = clamp(ReadLinear(Tex1.SampleLevel(Linear, prevUv, 0).rgb), lo, hi);

	return float4(WriteLinear(lerp(current, previous, 0.5)), 1.0);
}

// How badly each candidate pair of cameras predicts last frame's distance from this frame's, at 128x64
// points: relative difference, capped so one silhouette cannot outweigh the rest. Your own weapon does
// not move with the world, so it is left out. Tex1 last frame's distance.
//
// Only flat surfaces count: 1/distance must change evenly across the neighbouring pixels, as it does
// on any plane. At an edge, a camera that lands between last frame's pixels reads a blend of both
// sides, while one that lands on them (the camera without its jitter, standing still) reads one side
// exactly. So a still view full of edges favoured the camera without its jitter even when every frame
// had it: in a heist with 0.6.6, and in the harness with a fence of thin poles (seen in 75 of 138
// checks at 200 poles).
static const float kSyncFlat = 0.002;

float4 PS_TaaSync(VSOut i) : SV_Target
{
	// The depth pixel's own centre, so the ray goes exactly where that depth was drawn.
	float2 size = Texel.zw;
	float2 uv = (floor(i.uv * size) + 0.5) / size;
	float d = DepthEye(uv);
	float4 r = float4(0.0, 0.0, 0.0, 0.0);

	if (d < kWeaponDistance)
		return r;

	float w = 1.0 / d;
	float bendX = 1.0 / DepthEye(uv - float2(Texel.x, 0.0)) + 1.0 / DepthEye(uv + float2(Texel.x, 0.0)) - 2.0 * w;
	float bendY = 1.0 / DepthEye(uv - float2(0.0, Texel.y)) + 1.0 / DepthEye(uv + float2(0.0, Texel.y)) - 2.0 * w;

	if (max(abs(bendX), abs(bendY)) > kSyncFlat * w)
		return r;

	[unroll]
	for (int c = 0; c < 4; c++)
	{
		int b = c * 8;
		float3 w = RayThrough(uv, SyncCameras[b].xyz, SyncCameras[b + 1].xyz, SyncCameras[b + 2].xyz, SyncCameras[b + 7].xy) * d + SyncCameras[b + 6].xyz;
		float3 p = ProjectTo(w, SyncCameras[b + 3].xyz, SyncCameras[b + 4].xyz, SyncCameras[b + 5].xyz, SyncCameras[b + 7].zw);
		float e = 0.05;

		if (p.z > 1.0 && all(p.xy > 0.0) && all(p.xy < 1.0))
		{
			// Bilinear by hand (Gather: w top left, z top right, x bottom left, y bottom right), so a
			// fraction of a pixel shows on smooth surfaces whatever the hardware filters.
			float2 at = p.xy * size - 0.5;
			float2 f = frac(at);
			float4 q = Tex1.GatherRed(Point, (floor(at) + 1.0) / size);
			float prev = lerp(lerp(q.w, q.z, f.x), lerp(q.x, q.y, f.x), f.y);
			e = min(abs(prev - p.z) / p.z, 0.05);
		}

		r[c] = e;
	}

	return r;
}

// ---------------------------------------------------------------------------------------------
// FSR 1.0, after AMD FidelityFX Super Resolution 1.0 (MIT). EASU redraws the finished frame into a
// larger buffer by resampling along the edges it finds; PS_Final fits it back to the screen and
// runs RCAS, the sharpener, there.
//
// Be clear about what this is. The plugin is handed an image the game has ALREADY rendered at your
// screen resolution, so this cannot recover detail that was never drawn, and it is not the same
// thing as running the game at a lower resolution and upscaling to native. What it does give is
// edge-directed resampling (which smooths jagged edges) and sharpening that judges its own limit
// per pixel.

float3 EasuFetch(float2 base, float2 off)
{
	return Tex0.SampleLevel(Point, base + off * Texel.xy, 0).rgb;
}

// AMD's luma for EASU (r/2 + g + b/2), in perceptual space. ReadGamma keeps that true whether the
// render target is UNORM (already gamma) or an sRGB view (linear).
float EasuLuma(float3 c)
{
	float3 p = ReadGamma(c);

	return p.r * 0.5 + p.g + p.b * 0.5;
}

// One of the four 2x2 corners: how strong the gradient through C is, and which way it runs.
void EasuSet(inout float2 dir, inout float len, float2 pp, bool biS, bool biT, bool biU, bool biV,
	float lA, float lB, float lC, float lD, float lE)
{
	float w = 0.0;

	if (biS)
		w = (1.0 - pp.x) * (1.0 - pp.y);

	if (biT)
		w = pp.x * (1.0 - pp.y);

	if (biU)
		w = (1.0 - pp.x) * pp.y;

	if (biV)
		w = pp.x * pp.y;

	float lenX = rcp(max(max(abs(lD - lC), abs(lC - lB)), 0.000001));
	float dirX = lD - lB;

	dir.x += dirX * w;
	lenX = saturate(abs(dirX) * lenX);
	len += lenX * lenX * w;

	float lenY = rcp(max(max(abs(lE - lC), abs(lC - lA)), 0.000001));
	float dirY = lE - lA;

	dir.y += dirY * w;
	lenY = saturate(abs(dirY) * lenY);
	len += lenY * lenY * w;
}

// One tap of the kernel, which is a 2-lobe window stretched and turned along the edge: taps across
// the edge fall off fast, taps along it count for much more.
void EasuTap(inout float3 acc, inout float accW, float2 off, float2 dir, float2 len2, float lob, float clp, float3 c)
{
	float2 v = float2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x);
	v *= len2;

	float d2 = min(dot(v, v), clp);
	float wB = 2.0 / 5.0 * d2 - 1.0;
	float wA = lob * d2 - 1.0;

	wB *= wB;
	wA *= wA;
	wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);

	float w = wB * wA;

	acc += c * w;
	accW += w;
}

float4 PS_Easu(VSOut i) : SV_Target
{
	// Where this output pixel lands in the input, and the 4x4 block around it without its corners.
	float2 pp = i.uv * Texel.zw - 0.5;
	float2 fp = floor(pp);
	float2 fr = pp - fp;
	float2 base = (fp + 0.5) * Texel.xy;

	float3 cb = EasuFetch(base, float2(0.0, -1.0));
	float3 cc = EasuFetch(base, float2(1.0, -1.0));
	float3 ce = EasuFetch(base, float2(-1.0, 0.0));
	float3 cf = EasuFetch(base, float2(0.0, 0.0));
	float3 cg = EasuFetch(base, float2(1.0, 0.0));
	float3 ch = EasuFetch(base, float2(2.0, 0.0));
	float3 ci = EasuFetch(base, float2(-1.0, 1.0));
	float3 cj = EasuFetch(base, float2(0.0, 1.0));
	float3 ck = EasuFetch(base, float2(1.0, 1.0));
	float3 cl = EasuFetch(base, float2(2.0, 1.0));
	float3 cn = EasuFetch(base, float2(0.0, 2.0));
	float3 co = EasuFetch(base, float2(1.0, 2.0));

	float lb = EasuLuma(cb), lc = EasuLuma(cc), le = EasuLuma(ce), lf = EasuLuma(cf);
	float lg = EasuLuma(cg), lh = EasuLuma(ch), li = EasuLuma(ci), lj = EasuLuma(cj);
	float lk = EasuLuma(ck), ll = EasuLuma(cl), ln = EasuLuma(cn), lo = EasuLuma(co);

	float2 dir = float2(0.0, 0.0);
	float len = 0.0;

	EasuSet(dir, len, fr, true, false, false, false, lb, le, lf, lg, lj);
	EasuSet(dir, len, fr, false, true, false, false, lc, lf, lg, lh, lk);
	EasuSet(dir, len, fr, false, false, true, false, lf, li, lj, lk, ln);
	EasuSet(dir, len, fr, false, false, false, true, lg, lj, lk, ll, lo);

	float dirR = dot(dir, dir);
	bool zero = dirR < (1.0 / 32768.0);

	dir = zero ? float2(1.0, 0.0) : dir * rsqrt(max(dirR, 0.000000001));
	len = len * 0.5;
	len *= len;

	// A sharp diagonal stretches the kernel along itself; a flat area leaves it round.
	float stretch = dot(dir, dir) * rcp(max(max(abs(dir.x), abs(dir.y)), 0.000001));
	float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
	float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
	float clp = rcp(lob);

	float3 acc = float3(0.0, 0.0, 0.0);
	float accW = 0.0;

	EasuTap(acc, accW, float2(0.0, -1.0) - fr, dir, len2, lob, clp, cb);
	EasuTap(acc, accW, float2(1.0, -1.0) - fr, dir, len2, lob, clp, cc);
	EasuTap(acc, accW, float2(-1.0, 0.0) - fr, dir, len2, lob, clp, ce);
	EasuTap(acc, accW, float2(0.0, 0.0) - fr, dir, len2, lob, clp, cf);
	EasuTap(acc, accW, float2(1.0, 0.0) - fr, dir, len2, lob, clp, cg);
	EasuTap(acc, accW, float2(2.0, 0.0) - fr, dir, len2, lob, clp, ch);
	EasuTap(acc, accW, float2(-1.0, 1.0) - fr, dir, len2, lob, clp, ci);
	EasuTap(acc, accW, float2(0.0, 1.0) - fr, dir, len2, lob, clp, cj);
	EasuTap(acc, accW, float2(1.0, 1.0) - fr, dir, len2, lob, clp, ck);
	EasuTap(acc, accW, float2(2.0, 1.0) - fr, dir, len2, lob, clp, cl);
	EasuTap(acc, accW, float2(0.0, 2.0) - fr, dir, len2, lob, clp, cn);
	EasuTap(acc, accW, float2(1.0, 2.0) - fr, dir, len2, lob, clp, co);

	// Held inside the four nearest pixels, so the negative lobe can sharpen an edge but never ring
	// past what was actually there.
	float3 mn4 = min(min(cf, cg), min(cj, ck));
	float3 mx4 = max(max(cf, cg), max(cj, ck));

	return float4(min(mx4, max(mn4, acc / max(accW, 0.0001))), 1.0);
}

// ---------------------------------------------------------------------------------------------
// Final: the FSR resolve and RCAS, contrast adaptive sharpening (after AMD FidelityFX CAS, MIT),
// film grain and split view.

// Fitting the upscaled buffer back to the screen. One screen pixel now covers about UPSCALE_SCALE
// of its texels, so a 5-tap tent over that footprint resolves it; a plain point sample would throw
// most of those pixels away and alias.
float3 ResolveUpscaled(float2 uv)
{
	float2 o = Texel.xy * UPSCALE_SCALE * 0.25;
	float3 c = Tex0.SampleLevel(Linear, uv, 0).rgb * 4.0;

	c += Tex0.SampleLevel(Linear, uv + float2(-o.x, -o.y), 0).rgb;
	c += Tex0.SampleLevel(Linear, uv + float2(o.x, -o.y), 0).rgb;
	c += Tex0.SampleLevel(Linear, uv + float2(-o.x, o.y), 0).rgb;
	c += Tex0.SampleLevel(Linear, uv + float2(o.x, o.y), 0).rgb;

	return c / 8.0;
}

// RCAS, as in AMD's ffx_fsr1.h: the same idea as CAS, with the sharpening limit worked out per pixel
// from how close the pixel and its four neighbours are to black or white, so it cannot clip. It runs
// on the screen-size image, as AMD intends; on the larger buffer the resolve averaged most of it away.
float3 Rcas(float2 screenUv, float3 e)
{
	float2 s = Extra.zw; // one output pixel
	float3 b = ResolveUpscaled(screenUv + float2(0.0, -s.y));
	float3 d = ResolveUpscaled(screenUv + float2(-s.x, 0.0));
	float3 f = ResolveUpscaled(screenUv + float2(s.x, 0.0));
	float3 h = ResolveUpscaled(screenUv + float2(0.0, s.y));

	float3 mn4 = min(min(b, d), min(f, h));
	float3 mx4 = max(max(b, d), max(f, h));
	float3 hitMin = min(mn4, e) / (4.0 * mx4 + 0.0001);
	float3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0 - 0.0001);
	float3 lobeRGB = max(-hitMin, hitMax);
	float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * saturate(UPSCALE_SHARPNESS);

	return (lobe * (b + d + f + h) + e) * rcp(4.0 * lobe + 1.0);
}

float4 PS_Final(VSOut i) : SV_Target
{
	float2 t = Texel.xy;
	float2 uv = i.uv;
	bool upscaled = UPSCALE > 0.5;
	// With the FSR stage on, Tex0 is the larger buffer, resolved back down to the screen here.
	float3 e = upscaled ? ResolveUpscaled(uv) : Tex0.SampleLevel(Point, uv, 0).rgb;
	float3 result = e;

	if (upscaled && UPSCALE_SHARPNESS > 0.001)
		result = Rcas(i.uv, e);

	// CAS and RCAS are the same sharpener. While FSR upscaling is on, RCAS above does the sharpening
	// and this one is bypassed rather than doubling up on it.
	if (SHARPEN > 0.001 && !upscaled)
	{
		float3 a = Tex0.SampleLevel(Point, uv + float2(-t.x, -t.y), 0).rgb;
		float3 b = Tex0.SampleLevel(Point, uv + float2(0.0, -t.y), 0).rgb;
		float3 c = Tex0.SampleLevel(Point, uv + float2(t.x, -t.y), 0).rgb;
		float3 d = Tex0.SampleLevel(Point, uv + float2(-t.x, 0.0), 0).rgb;
		float3 f = Tex0.SampleLevel(Point, uv + float2(t.x, 0.0), 0).rgb;
		float3 g = Tex0.SampleLevel(Point, uv + float2(-t.x, t.y), 0).rgb;
		float3 h = Tex0.SampleLevel(Point, uv + float2(0.0, t.y), 0).rgb;
		float3 k = Tex0.SampleLevel(Point, uv + float2(t.x, t.y), 0).rgb;

		float3 mnRGB = min(min(min(d, e), min(f, b)), h);
		float3 mnRGB2 = min(mnRGB, min(min(a, c), min(g, k)));
		mnRGB += mnRGB2;

		float3 mxRGB = max(max(max(d, e), max(f, b)), h);
		float3 mxRGB2 = max(mxRGB, max(max(a, c), max(g, k)));
		mxRGB += mxRGB2;

		float3 amp = sqrt(saturate(min(mnRGB, 2.0 - mxRGB) / max(mxRGB, 0.0001)));
		float peak = -1.0 / lerp(8.0, 5.0, saturate(SHARPEN));

		// Never sharpen across a silhouette. Haloing the edge between two surfaces at different
		// distances is what makes sharpening look crunchy, and it is the one thing depth can fix
		// here for free.
		if (HaveDepth())
		{
			float centreDepth = DepthEye(uv);
			float jump = 0.0;

			jump = max(jump, abs(DepthEye(uv + float2(t.x, 0.0)) - centreDepth));
			jump = max(jump, abs(DepthEye(uv - float2(t.x, 0.0)) - centreDepth));
			jump = max(jump, abs(DepthEye(uv + float2(0.0, t.y)) - centreDepth));
			jump = max(jump, abs(DepthEye(uv - float2(0.0, t.y)) - centreDepth));

			peak *= 1.0 - saturate(jump / max(centreDepth * 0.06 + 2.0, 1.0));
		}

		float3 w = amp * peak;

		result = saturate((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w));
	}

	// Film grain goes last, after FSR and sharpening, as AMD's FSR guide asks: anything after it
	// (resampling, sharpening, motion blur) would smear or harden the grain. Not on the debug views.
	if (GRAIN > 0.0 && DEPTH_VIEW < 0.5 && MOTION_VIEW < 0.5)
	{
		float3 p = ReadGamma(result);
		float n = Hash(i.uv * Screen.xy + frac(TIME) * 1000.0) - 0.5;
		float mid = 1.0 - abs(Luma(p) * 2.0 - 1.0);

		result = WriteGamma(saturate(p + n * GRAIN * 0.12 * (0.35 + 0.65 * mid)));
	}

	// SMAA T2x and TAA blend frames, which averages the grade's dither away. Dither again here,
	// at the last 8-bit write (Extra.x 1 when one of them ran).
	if (Extra.x > 0.5 && DITHER > 0.0)
		result = WriteGamma(saturate(ReadGamma(result) + (Hash(i.uv * Screen.xy + frac(TIME * 7.0) * 61.0 + 17.0) - 0.5) * DITHER * (1.5 / 255.0)));

	if (SPLIT > 0.5)
	{
		if (i.uv.x < 0.5)
			result = Orig.SampleLevel(Point, i.uv, 0).rgb;

		// Extra.z is one OUTPUT pixel, which is not Tex0's texel size while upscaling.
		if (abs(i.uv.x - 0.5) < Extra.z)
			result = WriteGamma(float3(0.9, 0.62, 0.2));
	}

	return float4(result, 1.0);
}

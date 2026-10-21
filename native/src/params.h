#pragma once

// One flat list of float parameters shared by Lua, the render thread and the shader.
// The index IS the constant buffer slot: P[i / 4][i % 4] in pss.hlsl. Never reorder without
// updating the #defines at the top of shaders/pss.hlsl and PSS.PARAMS in lua/core.lua.
enum ParamIndex
{
	P_ENABLED = 0,
	P_SPLIT,
	P_AA,
	P_AA_SUBPIX,
	P_AA_EDGE,
	P_SHARPEN,
	P_BLOOM,
	P_BLOOM_THRESHOLD,
	P_BLOOM_RADIUS,
	P_ADAPT,
	P_ADAPT_SPEED,
	P_ADAPT_MIN,
	P_ADAPT_MAX,
	P_ADAPT_KEY,
	P_LIFT,
	P_GAMMA,
	P_GAIN,
	P_CONTRAST,
	P_SATURATION,
	P_VIBRANCE,
	P_LUT_MIX,
	P_VIGNETTE,
	P_GRAIN,
	P_INJECT_AFTER,       // native only: run after this fullscreen pass; 0 = the last one (automatic)
	P_REQUIRE_FULLSCREEN, // native only: only count passes whose viewport covers the back buffer
	P_DEBUG_VIEW,
	P_SHADOWS,
	P_HIGHLIGHTS,
	P_EXPOSURE,
	P_TEMPERATURE,
	P_TINT,
	P_CLARITY,
	P_DEBAND,
	P_DITHER,
	P_CHROMATIC,
	P_UNUSED_35, // was obscurance (removed in 0.11: real occlusion made it redundant and it haloed the sky)
	P_UNUSED_36, // was obscurance_radius
	P_COLOR_BLOOM,
	P_COLOR_BLOOM_RADIUS,
	P_CUES,
	P_CUES_SEPARATION,
	P_CUES_DEPTH,
	P_CUES_FOCUS,
	P_DIRT,
	P_DIRT_OFFSET,
	P_FLARE,       // lens flare: ghosts and an anamorphic streak from the bright light in the frame
	P_UNUSED_46,   // was halation_threshold
	P_BLOOM_TINT,  // warmth of the bloom (halation folded into it in 0.11)
	P_FLARE_LENGTH, // how long the streak is
	P_PANINI,    // cylindrical (Panini) projection of the finished frame, from the real field of view
	P_SPHERICAL, // stereographic (fisheye) projection, likewise
	P_UPSCALE,           // FSR 1.0: redraw the finished frame larger, then fit it back to the screen
	P_UPSCALE_SCALE,     // how much larger, 1.25x to 2x
	P_UPSCALE_SHARPNESS, // RCAS strength, applied at screen size; it replaces CAS while FSR is on

	// Depth. The game draws linear depth into one of its own render targets (a full screen
	// R32_FLOAT), which the hook finds and copies. Everything below does nothing at all when no
	// depth target was found, so these are safe to leave on.
	P_DEPTH_MODE,  // 0 automatic, 1 linear already, 2 hyperbolic, 3 hyperbolic reversed
	P_DEPTH_NEAR,  // near plane, only used to linearise a hyperbolic buffer
	P_DEPTH_FAR,   // far plane, in the same units
	P_AO,          // ambient occlusion from real geometry, not from brightness
	P_AO_RADIUS,   // how far around each point it looks, in world units
	P_FOG,         // aerial perspective: distant surfaces fade towards the air's colour
	P_FOG_START,   // the distance the fade begins, as a fraction of the far plane
	P_FOG_TINT,    // cool blue haze through to warm dust
	P_DOF,         // depth of field
	P_DOF_FOCUS,   // focus distance as a fraction of the far plane; 0 focuses on what is centred
	P_DOF_RANGE,   // how much stays sharp either side of the focus

	// The depth view is its OWN switch, not a mode in the shared debug list. The exposure
	// mode in that list paints the screen red, which is indistinguishable from a "no depth"
	// warning, and that cost an evening of chasing a bug that was not there.
	P_DEPTH_VIEW,

	P_AA_MODE,     // 0 FXAA, 1 SMAA, 2 SMAA T2x
	P_DOF_SPEED,   // how quickly focus follows what you look at
	P_DOF_NEAR,    // how much things in front of the focus blur
	P_DOF_WEAPON,  // 1 keeps your own weapon and hands sharp, and out of the focusing

	// The camera, measured by Lua every frame and read by the plugin from mods/saves/pss_camera.txt.
	// Never written by the settings file. CAMERA_VALID is decided by the plugin: 0 once no fresh
	// camera line has arrived for half a second, and everything that needs the camera then stops.
	P_CAMERA_TAN_X,  // tangent of half the horizontal field of view, MEASURED from the projection
	P_CAMERA_TAN_Y,  // and of half the vertical one
	P_CAMERA_PITCH,  // degrees, positive looking up
	P_CAMERA_ROLL,   // degrees
	P_CAMERA_HEIGHT, // world units
	P_CAMERA_VALID,
	P_FRAME,         // written by the plugin every frame: a counter, for noise that changes over time

	P_IL,              // screen space indirect light: one bounce from nearby surfaces
	P_IL_RADIUS,       // world units
	P_RTGI,            // ray traced lighting: longer rays, accumulated over frames
	P_RTGI_DISTANCE,   // world units a ray may travel
	P_SSR,             // screen space reflections
	P_SSR_BLUR,        // how rough reflections look
	P_TEMPORAL,        // 1 accumulates occlusion, light and reflections over frames using motion vectors
	P_MOTION_VIEW,     // draws the motion vectors instead of the picture
	P_MBLUR,           // motion blur
	P_SHAFTS,          // light shafts from the brightest light in view
	P_SHAFTS_LENGTH,   // how far towards the light each pixel looks
	P_HFOG,            // height fog: thickest at your feet, thinning with height
	P_HFOG_HEIGHT,     // world units over which it thins out
	P_FOG_GLOW,        // light glowing through either fog
	P_LTM,             // local tone mapping
	P_LTM_DETAIL,      // how much local detail is lifted on top
	P_CAMERA_AIM,      // 1 while aiming down sights; from the camera line like the rest of the camera
	P_DOF_AIM,         // blur size while aiming; P_DOF is the size from the hip, eased between

	P_DEMOIRE,         // softens the shimmering pattern a texture makes at pixel scale
	P_DETAIL,          // micro-contrast: makes surfaces read as having more texture than they do
	P_SURFACE_GRAIN,   // fine grain on surfaces, carried along with them by the motion vectors
	P_SURFACE_GRAIN_SIZE, // how coarse that grain is

	// Temporal antialiasing strength. Lua turns the camera by a fraction of a pixel each frame for the
	// draw (jitter_x/y, in pixels, for the status file); the cameras themselves come with the pose.
	P_TAA,
	P_RENDER_SCALE, // 1 = off; below that the frame is shrunk and rebuilt, to wash out engine aliasing
	P_JITTER_X,
	P_JITTER_Y,
	P_CAMERA_FRAME,
	P_COUNT
};

// 32 float4s in the shader. Growing this is cheap; it must stay a multiple of 4 and match P[] in
// shaders/pss.hlsl.
const int PARAM_SLOTS = 128;

struct ParamInfo
{
	const char* name;
	float def;
	float min;
	float max;
};

extern const ParamInfo kParams[P_COUNT];

int FindParam(const char* name);

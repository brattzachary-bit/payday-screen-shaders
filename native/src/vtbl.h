#pragma once

// COM vtable slots, computed in vtbl.c from the C interface declarations with offsetof,
// so no index in this project is typed in by hand.
#ifdef __cplusplus
extern "C" {
#endif

extern const int VT_CTX_DRAW_INDEXED;
extern const int VT_CTX_DRAW;
extern const int VT_CTX_DRAW_INDEXED_INSTANCED;
extern const int VT_CTX_DRAW_INSTANCED;
extern const int VT_CTX_PS_SET_SHADER;
extern const int VT_CTX_OM_SET_RENDER_TARGETS;
extern const int VT_CTX_OM_SET_RENDER_TARGETS_AND_UAVS;
extern const int VT_CTX_RS_SET_VIEWPORTS;
extern const int VT_CTX_COPY_RESOURCE;
extern const int VT_CTX_COPY_SUBRESOURCE_REGION;
extern const int VT_CTX_RESOLVE_SUBRESOURCE;
extern const int VT_CTX_CLEAR_RENDER_TARGET_VIEW;
extern const int VT_CTX_EXECUTE_COMMAND_LIST;
extern const int VT_SWAP_PRESENT;
extern const int VT_SWAP1_PRESENT1;
extern const int VT_CTX_COUNT;

#ifdef __cplusplus
}
#endif

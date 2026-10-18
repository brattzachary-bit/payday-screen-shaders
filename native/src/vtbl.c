/* Compiled as C on purpose: the MinGW headers only declare the Vtbl structs for C. */
#define CINTERFACE
#define COBJMACROS

#include <stddef.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <dxgi1_2.h>

#include "vtbl.h"

#define SLOT(iface, method) (int)(offsetof(iface##Vtbl, method) / sizeof(void*))

const int VT_CTX_DRAW_INDEXED = SLOT(ID3D11DeviceContext, DrawIndexed);
const int VT_CTX_DRAW = SLOT(ID3D11DeviceContext, Draw);
const int VT_CTX_DRAW_INDEXED_INSTANCED = SLOT(ID3D11DeviceContext, DrawIndexedInstanced);
const int VT_CTX_DRAW_INSTANCED = SLOT(ID3D11DeviceContext, DrawInstanced);
const int VT_CTX_PS_SET_SHADER = SLOT(ID3D11DeviceContext, PSSetShader);
const int VT_CTX_OM_SET_RENDER_TARGETS = SLOT(ID3D11DeviceContext, OMSetRenderTargets);
const int VT_CTX_OM_SET_RENDER_TARGETS_AND_UAVS = SLOT(ID3D11DeviceContext, OMSetRenderTargetsAndUnorderedAccessViews);
const int VT_CTX_RS_SET_VIEWPORTS = SLOT(ID3D11DeviceContext, RSSetViewports);
const int VT_CTX_COPY_RESOURCE = SLOT(ID3D11DeviceContext, CopyResource);
const int VT_CTX_COPY_SUBRESOURCE_REGION = SLOT(ID3D11DeviceContext, CopySubresourceRegion);
const int VT_CTX_RESOLVE_SUBRESOURCE = SLOT(ID3D11DeviceContext, ResolveSubresource);
const int VT_CTX_CLEAR_RENDER_TARGET_VIEW = SLOT(ID3D11DeviceContext, ClearRenderTargetView);
const int VT_CTX_EXECUTE_COMMAND_LIST = SLOT(ID3D11DeviceContext, ExecuteCommandList);
const int VT_SWAP_PRESENT = SLOT(IDXGISwapChain, Present);
const int VT_SWAP1_PRESENT1 = SLOT(IDXGISwapChain1, Present1);
// Slots in the newest context interface: how big a table that stands in for the context must be.
const int VT_CTX_COUNT = (int)(sizeof(ID3D11DeviceContext4Vtbl) / sizeof(void*));

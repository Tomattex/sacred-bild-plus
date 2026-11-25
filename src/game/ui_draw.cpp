#include "game/ui_draw.h"

#include <ddraw.h>
#include <d3d.h>

void UiDraw::quad(IDirect3DDevice7* device, IDirectDrawSurface7* surface, float left, float top, float right, float bottom,
    const float* uv, uint32_t color)
{
    struct Vertex
    {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    const float u0 = uv ? uv[0] : 0.0f, v0 = uv ? uv[1] : 0.0f, u1 = uv ? uv[2] : 0.0f, v1 = uv ? uv[3] : 0.0f;
    const Vertex vertices[4] = {
        {left, top, 0.0f, 1.0f, color, u0, v0}, {right, top, 0.0f, 1.0f, color, u1, v0},
        {left, bottom, 0.0f, 1.0f, color, u0, v1}, {right, bottom, 0.0f, 1.0f, color, u1, v1},
    };
    struct RenderState
    {
        D3DRENDERSTATETYPE state;
        DWORD value, saved;
    };
    RenderState states[] = {
        {D3DRENDERSTATE_ALPHABLENDENABLE, TRUE}, {D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA},
        {D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA}, {D3DRENDERSTATE_ALPHATESTENABLE, FALSE},
        {D3DRENDERSTATE_ZENABLE, FALSE},
    };
    struct StageState
    {
        DWORD stage;
        D3DTEXTURESTAGESTATETYPE type;
        DWORD value, saved;
    };
    const DWORD arg = surface ? D3DTA_TEXTURE : D3DTA_DIFFUSE;
    const DWORD op = surface ? D3DTOP_MODULATE : D3DTOP_SELECTARG1;
    StageState stages[] = {
        {0, D3DTSS_COLOROP, op}, {0, D3DTSS_COLORARG1, arg}, {0, D3DTSS_COLORARG2, D3DTA_DIFFUSE},
        {0, D3DTSS_ALPHAOP, op}, {0, D3DTSS_ALPHAARG1, arg}, {0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE},
        {0, D3DTSS_MINFILTER, D3DTFN_LINEAR}, {0, D3DTSS_MAGFILTER, D3DTFG_LINEAR},
        {0, D3DTSS_ADDRESS, D3DTADDRESS_CLAMP}, {1, D3DTSS_COLOROP, D3DTOP_DISABLE},
    };
    IDirectDrawSurface7* texture = nullptr;
    device->GetTexture(0, &texture);
    for (RenderState& s : states)
    {
        device->GetRenderState(s.state, &s.saved);
        device->SetRenderState(s.state, s.value);
    }
    for (StageState& s : stages)
    {
        device->GetTextureStageState(s.stage, s.type, &s.saved);
        device->SetTextureStageState(s.stage, s.type, s.value);
    }
    device->SetTexture(0, surface);
    device->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
        const_cast<Vertex*>(vertices), 4, 0);
    device->SetTexture(0, texture);
    if (texture)
    {
        texture->Release();
    }
    for (const StageState& s : stages)
    {
        device->SetTextureStageState(s.stage, s.type, s.saved);
    }
    for (const RenderState& s : states)
    {
        device->SetRenderState(s.state, s.saved);
    }
}

void UiDraw::fillRect(IDirect3DDevice7* device, float left, float top, float right, float bottom, uint32_t color)
{
    quad(device, nullptr, left, top, right, bottom, nullptr, color);
}

void UiDraw::panel(IDirect3DDevice7* device, float left, float top, float right, float bottom)
{
    fillRect(device, left - 1, top - 1, right + 1, bottom + 1, 0xFF8C6A3A);
    fillRect(device, left, top, right, bottom, 0xEC140F0A);
}

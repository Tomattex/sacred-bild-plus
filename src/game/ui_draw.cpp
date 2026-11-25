#include "game/ui_draw.h"

#include <ddraw.h>
#include <d3d.h>

void UiDraw::fillRect(IDirect3DDevice7* device, float left, float top, float right, float bottom, uint32_t color)
{
    struct Vertex
    {
        float x, y, z, rhw;
        DWORD color;
    };
    const Vertex quad[4] = {
        {left, top, 0.0f, 1.0f, color}, {right, top, 0.0f, 1.0f, color},
        {left, bottom, 0.0f, 1.0f, color}, {right, bottom, 0.0f, 1.0f, color},
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
    StageState stages[] = {
        {0, D3DTSS_COLOROP, D3DTOP_SELECTARG1}, {0, D3DTSS_COLORARG1, D3DTA_DIFFUSE},
        {0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1}, {0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE},
        {1, D3DTSS_COLOROP, D3DTOP_DISABLE},
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
    device->SetTexture(0, nullptr);
    device->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, const_cast<Vertex*>(quad), 4, 0);
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

void UiDraw::panel(IDirect3DDevice7* device, float left, float top, float right, float bottom)
{
    fillRect(device, left - 1, top - 1, right + 1, bottom + 1, 0xFF8C6A3A);
    fillRect(device, left, top, right, bottom, 0xEC140F0A);
}

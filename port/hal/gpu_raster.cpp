/* THE OPTIONAL GRAPHICS-CARD RENDERER. See hal/gpu_raster.h for what the
 * setting is and hal/gpu_device.h for the one device it shares with the
 * present path.
 *
 * ============================ THE SEAM ====================================
 *
 * ntr/gx.cpp draws a frame in two passes over the same buffers: an OPAQUE pass
 * (colour into the framebuffer, a float depth with a LESS test, a per-pixel
 * polygon ID, the 3D coverage mask) and then a TRANSLUCENT pass (blended
 * polygons with the equal-ID refusal, and mode-3 shadow volumes with their
 * stencil protocol) which READS what the opaque pass left. The opaque pass is
 * nearly all of the fill and none of the awkward state.
 *
 * So this file draws the opaque pass, and only the opaque pass, into three
 * offscreen targets and reads them straight back into the very buffers the
 * software opaque pass would have filled. gx.cpp then runs its translucent
 * pass, unchanged, over them. Everything downstream -- the edge smoothing, the
 * 2D compositor, the DS display capture, the selftest bitmap, the present path
 * -- sees an ordinary finished frame and cannot tell which rasteriser drew it.
 *
 * ========================= MATCHING THE SOFTWARE ==========================
 *
 * GEOMETRY arrives already in host screen space: x and y in pixels of the
 * active render size, z in [0,1], w kept, u and v in texel units. The clip
 * position is rebuilt on the CPU as (x_ndc * w, y_ndc * w, z * w, w) so that
 *   - u and v interpolate perspective-correct, which is what gx.cpp does
 *     (it interpolates u/w, v/w and 1/w and divides), and
 *   - SV_Position.z comes out as the plain screen-space interpolation of z,
 *     which is what gx.cpp's depth is (l0*a.z + l1*b.z + l2*c.z, no 1/w).
 * The vertex colour is declared noperspective for the same reason: gx.cpp
 * interpolates colour screen-linear. All three were read out of the raster
 * before this was written; see hal/gpu_raster.hlsl.
 *
 * DEPTH is D32_FLOAT with a LESS comparison, which keeps the FIRST triangle
 * submitted at a tie exactly as gx.cpp's `if (z >= depth) continue` does.
 * DepthClipEnable is FALSE because the software raster has no z clipping at
 * all: the near clip is done on the CPU before a triangle ever gets here and
 * there is no far clip.
 *
 * CULLING stays on the CPU, from the same signed screen area and the same
 * POLYGON_ATTR bits, and the card is told to cull nothing. A degenerate
 * triangle is dropped by the same 1e-6 test.
 *
 * THE SCISSOR is the present rectangle, which is what gx.cpp clamps its
 * bounding box to.
 *
 * COVERAGE TRAVELS IN THE POLYGON-ID TARGET. gx.cpp keeps a one-byte coverage
 * mask beside the framebuffer, and the 2D compositor needs it to know which
 * pixels the 3D engine wrote. It cannot come from the colour target's alpha
 * here because that alpha drives the blend, so the R8_UINT ID target carries
 * 0x80 | polygon_id: bit 7 is "this engine wrote this pixel" and the low six
 * bits are the DS's six-bit polygon ID.
 *
 * THE CLEAR COLOUR is read out of the framebuffer at the top of the frame
 * rather than hard-coded, and the readback COPIES ONLY COVERED PIXELS, so a
 * pixel the opaque pass did not reach keeps exactly the bytes the caller's own
 * clear put there. That is what makes this independent of whether the caller's
 * clear is one colour or many.
 *
 * ========================== WHERE IT DIFFERS ==============================
 *
 * 1. THE FILL RULE. This card uses Direct3D's top-left rule, one pixel one
 *    triangle; gx.cpp accepts a pixel on every edge, so two triangles sharing
 *    an edge both cover it and the depth test settles them. Along a shared
 *    edge both triangles interpolate to the same values, so the colour agrees
 *    to within float rounding; on a silhouette the two rasterisers disagree by
 *    whole pixels. That is why this is not a byte gate and why the in-process
 *    A/B excludes pixels within one pixel of a coverage or ID edge.
 *
 * 2. TEXTUREFILTER 1 AND 2. With filtering on, a filtered texel's alpha can
 *    land between 0 and full. This file used to let that partial alpha reach
 *    the blend unit AND the depth write, so the rim of every cut-out came out
 *    darkened by the clear colour and, because depth was written, whatever
 *    was drawn after it could not fill the rim in (the skybox showed through
 *    as an outline). Run hunt7 (lane FILTEREDGE1) changed the filtered arms
 *    to the rule TextureFilter 0 follows: a SECOND, point sampler pinned to
 *    the top level picks the texel the DS would have picked, and that texel
 *    alone decides whether the pixel exists and whether it is fully opaque;
 *    the filter supplies only the colour. The texture is held premultiplied
 *    (16 bits a channel) on these arms and the pixel shader divides the
 *    alpha back out, which is gx.cpp's alpha-weighted blend, so a transparent
 *    texel's colour never bleeds into a visible edge. See ps_filt in
 *    gpu_raster.hlsl and filtered_texel_cover in ntr/gx.cpp. At TextureFilter
 *    0 -- the arm this is gated on -- none of this runs: the shader, the
 *    sampler and the texture are the ones from before.
 *
 * 3. MIP CHAINS. TextureFilter 2 asks the card to generate the chain, which is
 *    a plain box filter; gx.cpp builds its own chain in premultiplied alpha so
 *    a cut-out does not drag its transparent colour outward. Same arm, same
 *    reason, not gated.
 *
 * 4. THE DEGENERATE-W FALLBACK. gx.cpp falls back to an affine UV when the
 *    interpolated 1/w is under 1e-9. The card has no such case; a vertex with
 *    w at zero is left to the hardware.
 *
 * ============================== FAILURE ===================================
 *
 * Every call is checked. The first failure prints one plain line and sets a
 * flag; from then on this file answers "I did not draw" and gx.cpp runs its
 * own opaque pass for that frame and every frame after. Nothing is left half
 * written, because the CPU buffers are only touched after all three readbacks
 * have succeeded.
 */
#if defined(_WIN32)

#include <windows.h>
#include <d3d11.h>
#include <emmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <vector>

#include "hal/gpu_raster.h"
#include "hal/gpu_device.h"
#include "hal/gpu_raster_shaders.h"
#include "hal/host_settings.h"
#include "ntr/gx.h"

namespace {

/* ---- the settings and the test knobs, read once -------------------------- */

int g_on;              /* the key said yes */
int g_started;
int g_down;            /* fallen back for the rest of the run */
int g_want_warp = -1;  /* SM64DS_RENDERER_DEVICE */
int g_perf;            /* SM64DS_RENDERER_PERF: the cost breakdown */
int g_fail_after;      /* SM64DS_RENDERER_FAIL_AFTER: the fallback drill */
int g_fail_readback;   /* SM64DS_RENDERER_FAIL_READBACK: the other drill */
/* SM64DS_RENDERER_FAIL_DEVICE: the third drill, and the only one that cannot
   be reached any other way on a machine that has a working card. It takes the
   SAME branch, with the same message, that a box with no Direct3D 11 at all
   takes: the setting is on, no device starts, the game draws every frame the
   ordinary way and says so once. */
int g_fail_device;

/* ---- THE DEPTH RANGE, AND WHY IT IS HALVED ------------------------------
   ntr/gx.cpp clears its depth buffer to 1e30 and tests LESS, so "nothing has
   been drawn here" is a value no geometry can reach. A depth target has no
   such value: the card's viewport transform clamps to [MinDepth, MaxDepth],
   both of which must lie in [0, 1], so a clear of 1.0 is a value geometry CAN
   reach -- and does. The screen z ntr/gx.cpp computes is
   (clip.z / clip.w + 1) * 0.5 with no far clip at all, so anything past the
   projection's far plane comes out ABOVE 1.0. Measured, Bob-omb Battlefield's
   sky on the first frame of the level, through SM64DS_PROBE_PX:
     z = 1.00022 at all three vertices
   Clamped to 1.0 that fails LESS against a 1.0 clear, so the whole sky and
   every far surface simply was not drawn: the card's first frames of levels
   6, 8 and 12 came back empty and filled in over about thirty frames as the
   intro camera brought the geometry in under z = 1.

   So the card is handed HALF the depth and the readback doubles it back.
   Halving and doubling are exact in binary floating point, so ordering, the
   LESS tie-break (equal z stays equal, and the first submitted keeps the
   pixel) and the value the software translucent pass reads are all unchanged
   to the last bit. The range this buys is z < 2.0, against a measured maximum
   of about 1.0002: a surface at z >= 2.0 would still clamp and lose, and a
   projection that produced one would be far outside anything this game has
   been measured to make. */
const float kDepthScale = 0.5f;     /* z on the card */
const float kDepthUnscale = 2.0f;   /* and back again, both exact */
int g_addrcheck;       /* SM64DS_RENDERER_ADDRCHECK: the six DS address ranges */

/* ---- what the run learned, for the one line at exit ---------------------- */

long long g_frames, g_tris, g_batches, g_verts;
double    g_ms_build, g_ms_draw, g_ms_read;
int       g_perf_n;
double    g_perf_build, g_perf_draw, g_perf_read;
/* the readback split in two: the Map's wait for the card, and the copy of the
   covered pixels into the software rasteriser's buffers */
double    g_perf_wait, g_perf_copy;
long long g_perf_tris, g_perf_batches;

/* ---- the device and the pipeline ----------------------------------------- */

ID3D11Device        *g_dev;
ID3D11DeviceContext *g_ctx;

ID3D11VertexShader   *g_vs;
ID3D11PixelShader    *g_ps;
ID3D11PixelShader    *g_ps_filt;   /* the same pixel under TextureFilter 1 and 2 */
ID3D11InputLayout    *g_layout;
ID3D11RasterizerState *g_rast;
ID3D11DepthStencilState *g_dss;
ID3D11BlendState     *g_blend;
ID3D11Buffer         *g_vb;
UINT                  g_vb_verts;
/* THE TOON TABLE the pixel shader reads for a mode-2 polygon (the
   ToonTable constant buffer, 32 float4, rgb 0..255): ntr/gx.cpp's frame table,
   uploaded only on a frame that has one and only when it changed */
ID3D11Buffer         *g_toon_cb;
float                 g_toon_seen[32 * 3];
bool                  g_toon_valid;

/* [filter][addressU][addressV]; address 0 clamp, 1 wrap, 2 mirror */
ID3D11SamplerState   *g_smp[3][3][3];
/* the point sampler pinned to the top level that ps_filt takes coverage from,
   [addressU][addressV] */
ID3D11SamplerState   *g_psmp[3][3];

/* the one-pixel opaque white texture an untextured polygon binds, so the
   shader has no branch and gx.cpp's own `texel = 0xFFFFFFFF` is literally
   what it samples */
ID3D11Texture2D          *g_white;
ID3D11ShaderResourceView *g_white_srv;

/* ---- the three targets and their staging copies -------------------------- */

ID3D11Texture2D        *g_col, *g_col_stage;
ID3D11RenderTargetView *g_col_rtv;
ID3D11Texture2D        *g_id, *g_id_stage;
ID3D11RenderTargetView *g_id_rtv;
ID3D11Texture2D        *g_dep, *g_dep_stage;
ID3D11DepthStencilView *g_dep_dsv;
int                     g_rt_w, g_rt_h;

/* ---- the texture cache on the card --------------------------------------- */

struct GTex {
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
    int w, h;
};
std::map<uint32_t, GTex> g_tex;
uint32_t g_tex_gen_seen;
size_t   g_tex_bytes;

/* ---- small helpers ------------------------------------------------------- */

long long qpc()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
long long g_qpf;
double ms_between(long long a, long long b)
{
    if (!g_qpf) return 0.0;
    return (double)(b - a) * 1000.0 / (double)g_qpf;
}

int env_int(const char *name, int dflt)
{
    const char *e = getenv(name);
    if (!e || !*e) return dflt;
    char *end = 0;
    const long v = strtol(e, &end, 10);
    return (end != e) ? (int)v : dflt;
}

/* ONE LINE, AND THEN THIS RUN DRAWS IN SOFTWARE FOR GOOD. Never an exit, never
   a stall: the caller returns 0 and gx.cpp's own opaque pass draws the frame
   that was about to be drawn here, so not even one picture is lost. */
void fall_back(const char *why, HRESULT hr)
{
    if (g_down) return;
    g_down = 1;
    if (hr)
        fprintf(stderr, "[renderer] the graphics card stopped drawing the 3D "
                        "picture (%s, code %08x). This run goes back to drawing "
                        "it the ordinary way; nothing else changes.\n",
                why, (unsigned)hr);
    else
        fprintf(stderr, "[renderer] the graphics card cannot draw the 3D picture "
                        "(%s). This run draws it the ordinary way; nothing else "
                        "changes.\n", why);
}

/* ---- the pipeline objects, made once ------------------------------------- */

bool make_pipeline()
{
    HRESULT hr = g_dev->CreateVertexShader(kGpuRasterVS, sizeof kGpuRasterVS, 0,
                                           &g_vs);
    if (FAILED(hr)) { fall_back("the vertex shader would not load", hr); return false; }
    hr = g_dev->CreatePixelShader(kGpuRasterPS, sizeof kGpuRasterPS, 0, &g_ps);
    if (FAILED(hr)) { fall_back("the pixel shader would not load", hr); return false; }
    hr = g_dev->CreatePixelShader(kGpuRasterPSF, sizeof kGpuRasterPSF, 0, &g_ps_filt);
    if (FAILED(hr)) { fall_back("the filtered pixel shader would not load", hr); return false; }

    const D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 36,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = g_dev->CreateInputLayout(el, 4, kGpuRasterVS, sizeof kGpuRasterVS,
                                  &g_layout);
    if (FAILED(hr)) { fall_back("the vertex layout was refused", hr); return false; }

    {
        D3D11_BUFFER_DESC cb;
        memset(&cb, 0, sizeof cb);
        cb.ByteWidth = 32 * 4 * sizeof(float);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = g_dev->CreateBuffer(&cb, 0, &g_toon_cb);
        if (FAILED(hr) || !g_toon_cb) { fall_back("no toon table buffer", hr); return false; }
        g_toon_valid = false;
    }

    /* CULL_NONE: the cull test is done on the CPU from the same signed screen
       area and the same POLYGON_ATTR bits gx.cpp uses, so by the time a
       triangle reaches the card it has already been kept or dropped.
       DepthClipEnable FALSE: the software raster has no z clip at all.
       ScissorEnable TRUE: the present rectangle. */
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;
    hr = g_dev->CreateRasterizerState(&rd, &g_rast);
    if (FAILED(hr)) { fall_back("no rasterizer state", hr); return false; }

    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    dd.StencilEnable = FALSE;
    hr = g_dev->CreateDepthStencilState(&dd, &g_dss);
    if (FAILED(hr)) { fall_back("no depth state", hr); return false; }

    /* The colour target blends by the shader's own alpha, which is the DS's
       effective alpha over 31. At TextureFilter 0 that alpha is always 1 and
       this is an exact replacement; see WHERE IT DIFFERS at the top. The ID
       target has blending off because an integer target cannot blend, which is
       what IndependentBlendEnable is here for. */
    D3D11_BLEND_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.IndependentBlendEnable = TRUE;
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bd.RenderTarget[1].BlendEnable = FALSE;
    bd.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = g_dev->CreateBlendState(&bd, &g_blend);
    if (FAILED(hr)) { fall_back("no blend state", hr); return false; }

    const uint32_t white = 0xFFFFFFFFu;
    D3D11_TEXTURE2D_DESC wd;
    memset(&wd, 0, sizeof wd);
    wd.Width = wd.Height = 1;
    wd.MipLevels = wd.ArraySize = 1;
    wd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    wd.SampleDesc.Count = 1;
    wd.Usage = D3D11_USAGE_IMMUTABLE;
    wd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA wsd;
    memset(&wsd, 0, sizeof wsd);
    wsd.pSysMem = &white;
    wsd.SysMemPitch = 4;
    hr = g_dev->CreateTexture2D(&wd, &wsd, &g_white);
    if (FAILED(hr)) { fall_back("no untextured stand-in", hr); return false; }
    hr = g_dev->CreateShaderResourceView(g_white, 0, &g_white_srv);
    if (FAILED(hr)) { fall_back("no untextured stand-in view", hr); return false; }
    return true;
}

/* WRAP, CLAMP AND MIRROR ARE PER AXIS, which is ntr/gx.cpp's tex_coord_i: a
   cleared repeat bit is CLAMP, repeat without flip is WRAP and repeat with
   flip is MIRROR. Built on demand and kept, so a level pays for the handful of
   combinations it actually uses. */
ID3D11SamplerState *sampler(int filter, int au, int av)
{
    if (filter < 0) filter = 0;
    if (filter > 2) filter = 2;
    if (g_smp[filter][au][av]) return g_smp[filter][au][av];
    static const D3D11_TEXTURE_ADDRESS_MODE kMode[3] = {
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP,
        D3D11_TEXTURE_ADDRESS_MIRROR,
    };
    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.AddressU = kMode[au];
    sd.AddressV = kMode[av];
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    /* 0 is the DS's own nearest sampling; 1 is bilinear with no chain, so the
       mip level is pinned at the top; 2 is the chain plus the blend between
       two of its levels, which is what trilinear is. */
    if (filter == 0) sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    else if (filter == 1) {
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.MaxLOD = 0.0f;
    } else sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(g_dev->CreateSamplerState(&sd, &g_smp[filter][au][av])))
        return 0;
    return g_smp[filter][au][av];
}

/* THE POINT SAMPLER ps_filt TAKES COVERAGE FROM: filter 0's own sampler, except
   that the mip level is pinned at the top (a chain exists under filter 2, and
   a point sampler left free would pick a coarser level for a distant surface,
   where gx.cpp's nearest texel is always the full-size one). */
ID3D11SamplerState *psampler(int au, int av)
{
    if (g_psmp[au][av]) return g_psmp[au][av];
    static const D3D11_TEXTURE_ADDRESS_MODE kMode[3] = {
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP,
        D3D11_TEXTURE_ADDRESS_MIRROR,
    };
    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.AddressU = kMode[au];
    sd.AddressV = kMode[av];
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.MaxLOD = 0.0f;
    if (FAILED(g_dev->CreateSamplerState(&sd, &g_psmp[au][av]))) return 0;
    return g_psmp[au][av];
}

/* ---- the three targets --------------------------------------------------- */

void release_targets()
{
    if (g_col_rtv) { g_col_rtv->Release(); g_col_rtv = 0; }
    if (g_col_stage) { g_col_stage->Release(); g_col_stage = 0; }
    if (g_col) { g_col->Release(); g_col = 0; }
    if (g_id_rtv) { g_id_rtv->Release(); g_id_rtv = 0; }
    if (g_id_stage) { g_id_stage->Release(); g_id_stage = 0; }
    if (g_id) { g_id->Release(); g_id = 0; }
    if (g_dep_dsv) { g_dep_dsv->Release(); g_dep_dsv = 0; }
    if (g_dep_stage) { g_dep_stage->Release(); g_dep_stage = 0; }
    if (g_dep) { g_dep->Release(); g_dep = 0; }
    g_rt_w = g_rt_h = 0;
}

bool ensure_targets(int w, int h)
{
    if (g_col && g_rt_w == w && g_rt_h == h) return true;
    release_targets();

    D3D11_TEXTURE2D_DESC d;
    memset(&d, 0, sizeof d);
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;

    /* colour: the framebuffer's own byte order */
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = g_dev->CreateTexture2D(&d, 0, &g_col);
    if (FAILED(hr)) { fall_back("no colour target", hr); return false; }
    hr = g_dev->CreateRenderTargetView(g_col, 0, &g_col_rtv);
    if (FAILED(hr)) { fall_back("no colour target view", hr); return false; }

    /* polygon ID plus the coverage bit */
    d.Format = DXGI_FORMAT_R8_UINT;
    hr = g_dev->CreateTexture2D(&d, 0, &g_id);
    if (FAILED(hr)) { fall_back("no polygon-id target", hr); return false; }
    hr = g_dev->CreateRenderTargetView(g_id, 0, &g_id_rtv);
    if (FAILED(hr)) { fall_back("no polygon-id target view", hr); return false; }

    /* depth: typeless so the same texture can be a depth view for drawing and
       a plain float copy for reading back */
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = g_dev->CreateTexture2D(&d, 0, &g_dep);
    if (FAILED(hr)) { fall_back("no depth target", hr); return false; }
    D3D11_DEPTH_STENCIL_VIEW_DESC vd;
    memset(&vd, 0, sizeof vd);
    vd.Format = DXGI_FORMAT_D32_FLOAT;
    vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = g_dev->CreateDepthStencilView(g_dep, &vd, &g_dep_dsv);
    if (FAILED(hr)) { fall_back("no depth target view", hr); return false; }

    /* the three staging copies the frame is read back through */
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    hr = g_dev->CreateTexture2D(&d, 0, &g_col_stage);
    if (FAILED(hr)) { fall_back("no colour readback buffer", hr); return false; }
    d.Format = DXGI_FORMAT_R8_UINT;
    hr = g_dev->CreateTexture2D(&d, 0, &g_id_stage);
    if (FAILED(hr)) { fall_back("no polygon-id readback buffer", hr); return false; }
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    hr = g_dev->CreateTexture2D(&d, 0, &g_dep_stage);
    if (FAILED(hr)) { fall_back("no depth readback buffer", hr); return false; }

    g_rt_w = w;
    g_rt_h = h;
    return true;
}

/* ---- the texture cache --------------------------------------------------- */

void release_textures()
{
    for (std::map<uint32_t, GTex>::iterator it = g_tex.begin();
         it != g_tex.end(); ++it) {
        if (it->second.srv) it->second.srv->Release();
        if (it->second.tex) it->second.tex->Release();
    }
    g_tex.clear();
    g_tex_bytes = 0;
}

/* KEYED ON A MONOTONIC ID, NOT ON THE PIXEL POINTER. ntr/gx.cpp's decode cache
   frees and reuses its buffers, so a pointer that named one texture a moment
   ago can name a different one now; the id is handed out at bind time and
   never reused, and gx_invalidate_textures bumps a generation that throws this
   whole cache away. */
const GTex *upload_texture(uint32_t id, const uint32_t *px, int w, int h,
                           int filter)
{
    std::map<uint32_t, GTex>::iterator it = g_tex.find(id);
    if (it != g_tex.end()) return &it->second;
    if (!px || w <= 0 || h <= 0) return 0;

    GTex t;
    memset(&t, 0, sizeof t);
    t.w = w;
    t.h = h;

    D3D11_TEXTURE2D_DESC d;
    memset(&d, 0, sizeof d);
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   /* 0xAARRGGBB in memory is BGRA */
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    const bool mips = (filter >= 2);
    /* UNDER FILTER 1 AND 2 THE TEXTURE IS HELD PREMULTIPLIED, 16 BITS A
       CHANNEL (run hunt7, lane FILTEREDGE1), so the card's own bilinear and
       box-filtered mips are alpha-weighted exactly as ntr/gx.cpp's are, and
       ps_filt divides the alpha out. Sixteen bits because a covered texel
       next to transparent ones can carry a small alpha, and an 8-bit
       premultiplied value would be a coarse colour once divided back up.
       Filter 0 keeps the straight 8-bit texture it always had. */
    const bool premult = (filter >= 1);
    std::vector<uint16_t> pm;
    if (premult) {
        d.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        pm.resize((size_t)w * (size_t)h * 4u);
        for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
            const uint32_t c = px[i];
            const uint32_t a = c >> 24;
            const uint32_t r = (c >> 16) & 0xFFu, g = (c >> 8) & 0xFFu, b = c & 0xFFu;
            /* c * a / 255 scaled from 0..255 to 0..65535: (c * a * 257 + 127) / 255 */
            pm[i * 4 + 0] = (uint16_t)((r * a * 257u + 127u) / 255u);
            pm[i * 4 + 1] = (uint16_t)((g * a * 257u + 127u) / 255u);
            pm[i * 4 + 2] = (uint16_t)((b * a * 257u + 127u) / 255u);
            pm[i * 4 + 3] = (uint16_t)(a * 257u);
        }
    }
    const void *src_px = premult ? (const void *)pm.data() : (const void *)px;
    const UINT src_pitch = (UINT)w * (premult ? 8u : 4u);
    HRESULT hr;
    if (mips) {
        /* the card builds the chain; see WHERE IT DIFFERS at the top for how
           that chain differs from the one ntr/gx.cpp builds for itself */
        d.MipLevels = 0;
        d.BindFlags |= D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        hr = g_dev->CreateTexture2D(&d, 0, &t.tex);
    } else {
        d.MipLevels = 1;
        D3D11_SUBRESOURCE_DATA sd;
        memset(&sd, 0, sizeof sd);
        sd.pSysMem = src_px;
        sd.SysMemPitch = src_pitch;
        hr = g_dev->CreateTexture2D(&d, &sd, &t.tex);
    }
    if (FAILED(hr) || !t.tex) { fall_back("a texture would not upload", hr); return 0; }
    hr = g_dev->CreateShaderResourceView(t.tex, 0, &t.srv);
    if (FAILED(hr) || !t.srv) {
        t.tex->Release();
        fall_back("a texture view was refused", hr);
        return 0;
    }
    if (mips) {
        g_ctx->UpdateSubresource(t.tex, 0, 0, src_px, src_pitch, 0);
        g_ctx->GenerateMips(t.srv);
    }
    g_tex_bytes += (size_t)w * (size_t)h * (premult ? 8u : 4u) * (mips ? 2u : 1u);
    return &g_tex.insert(std::make_pair(id, t)).first->second;
}

/* ---- the vertex buffer --------------------------------------------------- */

struct GVert {
    float x, y, z, w;
    float u, v;
    float r, g, b;      /* 0..255, the units ntr/gx.cpp interpolates in */
    float pa, pid;      /* polygon alpha 0..31 and polygon ID 0..63 */
};
std::vector<GVert> g_scratch;

struct Batch {
    const GTex *tex;
    int au, av;
    UINT first, count;
};
std::vector<Batch> g_batch;

bool ensure_vb(size_t verts)
{
    if (g_vb && g_vb_verts >= verts) return true;
    if (g_vb) { g_vb->Release(); g_vb = 0; g_vb_verts = 0; }
    size_t want = verts + verts / 2 + 1024;
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (UINT)(want * sizeof(GVert));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const HRESULT hr = g_dev->CreateBuffer(&bd, 0, &g_vb);
    if (FAILED(hr) || !g_vb) { fall_back("no vertex buffer", hr); return false; }
    g_vb_verts = (UINT)want;
    return true;
}

/* ---- the one-time start-up ----------------------------------------------- */

void at_exit();
void aa_at_exit();   /* with the edge-smoothing pass, below */

bool start()
{
    if (g_down) return false;
    if (g_dev) return true;
    if (g_fail_device) {
        fprintf(stderr, "[renderer] SM64DS_RENDERER_FAIL_DEVICE is set; "
                "pretending no device exists\n");
        fall_back("no Direct3D device would start", 0);
        return false;
    }
    if (port_gpu_device_failed()) { fall_back("no Direct3D device would start", 0); return false; }
    port_gpu_device_address_rows("before", g_addrcheck);
    if (!port_gpu_device_acquire(g_want_warp)) {
        fall_back("no Direct3D device would start", 0);
        return false;
    }
    g_dev = (ID3D11Device *)port_gpu_device();
    g_ctx = (ID3D11DeviceContext *)port_gpu_device_context();
    if (!g_dev || !g_ctx) { fall_back("the Direct3D device went missing", 0); return false; }
    LARGE_INTEGER f;
    if (QueryPerformanceFrequency(&f)) g_qpf = f.QuadPart;
    fprintf(stderr, "[renderer] the 3D picture is drawn by \"%s\"%s, feature "
            "level %u_%u, ready in %.1f ms. The translucent polygons, the "
            "shadows and everything after them still run here.\n",
            port_gpu_device_adapter()[0] ? port_gpu_device_adapter() : "(unnamed)",
            port_gpu_device_is_warp() ? " (WARP, the software device Windows ships)"
                                      : "",
            (unsigned)((port_gpu_device_feature_level() >> 12) & 0xf),
            (unsigned)((port_gpu_device_feature_level() >> 8) & 0xf),
            port_gpu_device_create_ms());
    if (!make_pipeline()) return false;
    port_gpu_device_address_rows("after ", g_addrcheck);
    atexit(at_exit);
    return true;
}

void at_exit()
{
    if (g_frames)
        fprintf(stderr, "[renderer] %lld frame(s) drawn on the card: %lld "
                "triangle(s) in %lld batch(es), %.3f ms building them, %.3f ms "
                "submitting, %.3f ms reading the picture back, per frame. "
                "Textures held on the card: %u KB.\n",
                g_frames, g_tris, g_batches, g_ms_build / (double)g_frames,
                g_ms_draw / (double)g_frames, g_ms_read / (double)g_frames,
                (unsigned)(g_tex_bytes / 1024));
    /* THE ADDRESS ROWS WITH THE THREE TARGETS STILL ALIVE, which is the point
       of taking them here rather than after the release below: a 32-bit
       process that lost one of the six DS ranges to a driver's own mapping is
       a process whose game cannot run, and the render targets are the biggest
       thing this file ever asks a driver for. */
    port_gpu_device_address_rows("exit  ", g_addrcheck);
    aa_at_exit();
    release_textures();
    release_targets();
    if (g_vb) { g_vb->Release(); g_vb = 0; }
    if (g_white_srv) { g_white_srv->Release(); g_white_srv = 0; }
    if (g_white) { g_white->Release(); g_white = 0; }
    if (g_blend) { g_blend->Release(); g_blend = 0; }
    if (g_dss) { g_dss->Release(); g_dss = 0; }
    if (g_rast) { g_rast->Release(); g_rast = 0; }
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            for (int c = 0; c < 3; ++c)
                if (g_smp[a][b][c]) { g_smp[a][b][c]->Release(); g_smp[a][b][c] = 0; }
    if (g_layout) { g_layout->Release(); g_layout = 0; }
    if (g_toon_cb) { g_toon_cb->Release(); g_toon_cb = 0; }
    g_toon_valid = false;
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            if (g_psmp[a][b]) { g_psmp[a][b]->Release(); g_psmp[a][b] = 0; }
    if (g_ps_filt) { g_ps_filt->Release(); g_ps_filt = 0; }
    if (g_ps) { g_ps->Release(); g_ps = 0; }
    if (g_vs) { g_vs->Release(); g_vs = 0; }
    /* the device itself belongs to hal/gpu_device.cpp and is released there */
}

/* ---- the readback -------------------------------------------------------- */

/* THE TEST KNOBS OF THE READBACK, off in every ordinary run.
   SM64DS_RENDERER_RBCHECK=1: every frame the card draws is read back twice,
   this file's way and the old way (the whole targets, the whole present
   rectangle scanned one pixel at a time), from the same buffers as handed
   over; the two results are compared byte for byte -- colour, coverage,
   polygon id, depth -- and the old one is kept. The count is printed at exit.
   SM64DS_RENDERER_RBTIME=1: see draw_frame. */
int g_rbcheck, g_rbtime;
long long g_rbc_frames, g_rbc_bad_frames, g_rbc_bad_bytes;
std::vector<uint32_t> g_rbc_fb0, g_rbc_fb1;
std::vector<uint8_t>  g_rbc_cv0, g_rbc_cv1, g_rbc_id0, g_rbc_id1;
std::vector<float>    g_rbc_dp0, g_rbc_dp1;
double    g_rbt_ms[2], g_rbt_wait[2];
long long g_rbt_n[2];

/* THE COPY INTO THE CPU BUFFERS, SIXTEEN PIXELS AT A TIME where it can. The
   coverage bit is bit 7 of the id byte, which is exactly the bit SSE2's byte
   movemask collects, so one load and one movemask say whether a run of
   sixteen pixels is all covered (a course: nearly every run), all empty (the
   clear around a menu's model) or mixed. An all-covered run is written with
   whole-register stores, an empty one is skipped, and a mixed one and the
   ragged end of a row take the one-pixel loop. Every store writes the value
   the one-pixel loop writes -- the colour with its alpha forced to 0xFF, a
   coverage byte of 1, the low six bits of the id, the depth times two (one
   IEEE single multiply either way) -- so the buffers come out byte for byte
   the same; SM64DS_RENDERER_RBCHECK=1 compares them with the one-pixel loop
   on every frame. */
void copy_box(const ntr::GxGpuFrame *f, int bx0, int by0, int bx1, int by1,
              const D3D11_MAPPED_SUBRESOURCE &mc,
              const D3D11_MAPPED_SUBRESOURCE &mi,
              const D3D11_MAPPED_SUBRESOURCE &md)
{
    const __m128i alpha = _mm_set1_epi32((int)0xFF000000u);
    const __m128i ones = _mm_set1_epi8(1);
    const __m128i low6 = _mm_set1_epi8(0x3F);
    const __m128 two = _mm_set1_ps(kDepthUnscale);
    const bool want_id = f->want_attrid != 0;
    for (int y = by0; y < by1; ++y) {
        const uint32_t *crow =
            (const uint32_t *)((const unsigned char *)mc.pData + (size_t)y * mc.RowPitch);
        const unsigned char *irow =
            (const unsigned char *)mi.pData + (size_t)y * mi.RowPitch;
        const float *drow = f->want_depth
            ? (const float *)((const unsigned char *)md.pData + (size_t)y * md.RowPitch)
            : 0;
        uint32_t *fbrow = f->fb + (size_t)y * f->stride;
        uint8_t *cvrow = f->cover + (size_t)y * f->stride;
        uint8_t *idrow = f->attrid + (size_t)y * f->stride;
        float *dprow = f->depth + (size_t)y * f->stride;
        int x = bx0;
        for (; x + 16 <= bx1; x += 16) {
            const __m128i ids = _mm_loadu_si128((const __m128i *)(irow + x));
            const int m = _mm_movemask_epi8(ids);
            if (m == 0) continue;
            if (m == 0xFFFF) {
                for (int q = 0; q < 16; q += 4) {
                    const __m128i c = _mm_loadu_si128((const __m128i *)(crow + x + q));
                    _mm_storeu_si128((__m128i *)(fbrow + x + q), _mm_or_si128(c, alpha));
                    if (drow)
                        _mm_storeu_ps(dprow + x + q,
                                      _mm_mul_ps(_mm_loadu_ps(drow + x + q), two));
                }
                _mm_storeu_si128((__m128i *)(cvrow + x), ones);
                if (want_id)
                    _mm_storeu_si128((__m128i *)(idrow + x), _mm_and_si128(ids, low6));
                continue;
            }
            for (int k = x; k < x + 16; ++k) {
                const unsigned char idv = irow[k];
                if (!(idv & 0x80u)) continue;
                fbrow[k] = 0xFF000000u | (crow[k] & 0x00FFFFFFu);
                cvrow[k] = 1;
                if (want_id) idrow[k] = (uint8_t)(idv & 0x3Fu);
                if (drow) dprow[k] = drow[k] * kDepthUnscale;
            }
        }
        for (; x < bx1; ++x) {
            const unsigned char idv = irow[x];
            if (!(idv & 0x80u)) continue;
            fbrow[x] = 0xFF000000u | (crow[x] & 0x00FFFFFFu);
            cvrow[x] = 1;
            if (want_id) idrow[x] = (uint8_t)(idv & 0x3Fu);
            if (drow) dprow[x] = drow[x] * kDepthUnscale;
        }
    }
}

/* Read the box [bx0,bx1) x [by0,by1) back into the software rasteriser's
   buffers: covered pixels only, which is what keeps every pixel the card did
   not reach exactly as the caller's clear left it. `whole` is the old way,
   kept for the two knobs above: whole-target copies and a scan of whatever
   box is passed (the present rectangle).

   The colour and the id come out through a boxed copy into the same place in
   their full-size staging copies, so a pixel's staging address does not
   depend on the box. The DEPTH is always copied whole: Direct3D 11 refuses a
   boxed copy out of a depth-stencil resource (and refuses it silently: the
   staging copy just keeps what it held), and it is only read back on frames
   a second pass will read it on (want_depth) in the first place.

   Every Map happens before a single CPU byte is written, so a failure leaves
   the buffers exactly as gx.cpp cleared them. Returns 0 after fall_back. */
int read_back(const ntr::GxGpuFrame *f, int bx0, int by0, int bx1, int by1,
              bool whole, double *wait_ms)
{
    if (whole) {
        g_ctx->CopyResource(g_col_stage, g_col);
        g_ctx->CopyResource(g_id_stage, g_id);
    } else {
        D3D11_BOX box;
        box.left = (UINT)bx0;
        box.right = (UINT)bx1;
        box.top = (UINT)by0;
        box.bottom = (UINT)by1;
        box.front = 0;
        box.back = 1;
        g_ctx->CopySubresourceRegion(g_col_stage, 0, (UINT)bx0, (UINT)by0, 0,
                                     g_col, 0, &box);
        g_ctx->CopySubresourceRegion(g_id_stage, 0, (UINT)bx0, (UINT)by0, 0,
                                     g_id, 0, &box);
    }
    if (f->want_depth) g_ctx->CopyResource(g_dep_stage, g_dep);
    port_gpu_timer_span_end(PORT_GPU_SPAN_OPAQUE);

    const long long tw0 = qpc();
    D3D11_MAPPED_SUBRESOURCE mc, mi, md;
    memset(&mc, 0, sizeof mc);
    memset(&mi, 0, sizeof mi);
    memset(&md, 0, sizeof md);
    HRESULT hr = g_ctx->Map(g_col_stage, 0, D3D11_MAP_READ, 0, &mc);
    if (FAILED(hr)) { fall_back("the picture could not be read back", hr); return 0; }
    hr = g_ctx->Map(g_id_stage, 0, D3D11_MAP_READ, 0, &mi);
    if (FAILED(hr)) {
        g_ctx->Unmap(g_col_stage, 0);
        fall_back("the polygon ids could not be read back", hr);
        return 0;
    }
    if (f->want_depth) {
        hr = g_ctx->Map(g_dep_stage, 0, D3D11_MAP_READ, 0, &md);
        if (FAILED(hr)) {
            g_ctx->Unmap(g_id_stage, 0);
            g_ctx->Unmap(g_col_stage, 0);
            fall_back("the depth could not be read back", hr);
            return 0;
        }
    }
    *wait_ms += ms_between(tw0, qpc());

    /* NOTHING ON THE CPU SIDE HAS BEEN TOUCHED UNTIL HERE, which is what makes
       a failure above safe: the software pass then draws the frame over
       buffers that are still exactly as it cleared them. The old way keeps
       its one-pixel loop, which is what RBCHECK compares copy_box against. */
    if (!whole) copy_box(f, bx0, by0, bx1, by1, mc, mi, md);
    else for (int y = by0; y < by1; ++y) {
        const uint32_t *crow =
            (const uint32_t *)((const unsigned char *)mc.pData + (size_t)y * mc.RowPitch);
        const unsigned char *irow =
            (const unsigned char *)mi.pData + (size_t)y * mi.RowPitch;
        const float *drow = f->want_depth
            ? (const float *)((const unsigned char *)md.pData + (size_t)y * md.RowPitch)
            : 0;
        uint32_t *fbrow = f->fb + (size_t)y * f->stride;
        uint8_t *cvrow = f->cover + (size_t)y * f->stride;
        uint8_t *idrow = f->attrid + (size_t)y * f->stride;
        float *dprow = f->depth + (size_t)y * f->stride;
        for (int x = bx0; x < bx1; ++x) {
            const unsigned char idv = irow[x];
            if (!(idv & 0x80u)) continue;
            fbrow[x] = 0xFF000000u | (crow[x] & 0x00FFFFFFu);
            cvrow[x] = 1;
            if (f->want_attrid) idrow[x] = (uint8_t)(idv & 0x3Fu);
            if (drow) dprow[x] = drow[x] * kDepthUnscale;
        }
    }

    if (f->want_depth) g_ctx->Unmap(g_dep_stage, 0);
    g_ctx->Unmap(g_id_stage, 0);
    g_ctx->Unmap(g_col_stage, 0);
    return 1;
}

void rbt_report()
{
    for (int a = 0; a < 2; ++a) {
        const double n = g_rbt_n[a] ? (double)g_rbt_n[a] : 1.0;
        fprintf(stderr, "[renderer-rbtime] %s: %lld frame(s), readback %.3f ms "
                "(wait %.3f copy %.3f)\n", a ? "old" : "new", g_rbt_n[a],
                g_rbt_ms[a] / n, g_rbt_wait[a] / n,
                (g_rbt_ms[a] - g_rbt_wait[a]) / n);
    }
}

void rbc_report()
{
    fprintf(stderr, "[renderer-rbcheck] %lld frame(s) read back both ways, %lld "
            "differing, %lld differing byte(s)\n",
            g_rbc_frames, g_rbc_bad_frames, g_rbc_bad_bytes);
}

/* the present rectangle of the four buffers, into or out of a side copy */
void rbc_save(const ntr::GxGpuFrame *f, std::vector<uint32_t> &fb,
              std::vector<uint8_t> &cv, std::vector<uint8_t> &id,
              std::vector<float> &dp, bool out)
{
    const size_t n = (size_t)f->pw * (size_t)f->ph;
    fb.resize(n);
    cv.resize(n);
    id.resize(n);
    dp.resize(n);
    for (int y = 0; y < f->ph; ++y) {
        const size_t o = (size_t)(f->py0 + y) * f->stride + f->px0;
        const size_t r = (size_t)y * f->pw;
        if (out) {
            memcpy(&fb[r], f->fb + o, f->pw * sizeof(uint32_t));
            memcpy(&cv[r], f->cover + o, f->pw);
            memcpy(&id[r], f->attrid + o, f->pw);
            memcpy(&dp[r], f->depth + o, f->pw * sizeof(float));
        } else {
            memcpy(f->fb + o, &fb[r], f->pw * sizeof(uint32_t));
            memcpy(f->cover + o, &cv[r], f->pw);
            memcpy(f->attrid + o, &id[r], f->pw);
            memcpy(f->depth + o, &dp[r], f->pw * sizeof(float));
        }
    }
}

/* After this frame's readback: set its result aside (fb0), put the buffers
   back as they were handed over (fb1, saved before the readback), read the
   frame back the old way, compare, keep the old result. On a frame the card
   had nothing to draw the old way reads nothing either, so the comparison is
   against the buffers as handed over. */
void rbc_frame(const ntr::GxGpuFrame *f, bool drew)
{
    rbc_save(f, g_rbc_fb0, g_rbc_cv0, g_rbc_id0, g_rbc_dp0, true);
    rbc_save(f, g_rbc_fb1, g_rbc_cv1, g_rbc_id1, g_rbc_dp1, false);
    double w = 0.0;
    if (drew && !read_back(f, f->px0, f->py0, f->px0 + f->pw, f->py0 + f->ph,
                           true, &w))
        return;
    long long bytes = 0;
    for (int y = 0; y < f->ph; ++y) {
        const size_t o = (size_t)(f->py0 + y) * f->stride + f->px0;
        const size_t r = (size_t)y * f->pw;
        if (!memcmp(&g_rbc_fb0[r], f->fb + o, f->pw * 4) &&
            !memcmp(&g_rbc_cv0[r], f->cover + o, f->pw) &&
            !memcmp(&g_rbc_id0[r], f->attrid + o, f->pw) &&
            !memcmp(&g_rbc_dp0[r], f->depth + o, f->pw * 4))
            continue;
        for (int x = 0; x < f->pw; ++x) {
            const unsigned char *a = (const unsigned char *)&g_rbc_fb0[r + x];
            const unsigned char *b = (const unsigned char *)(f->fb + o + x);
            const unsigned char *c = (const unsigned char *)&g_rbc_dp0[r + x];
            const unsigned char *e = (const unsigned char *)(f->depth + o + x);
            for (int k = 0; k < 4; ++k) bytes += (a[k] != b[k]) + (c[k] != e[k]);
            bytes += g_rbc_cv0[r + x] != f->cover[o + x];
            bytes += g_rbc_id0[r + x] != f->attrid[o + x];
        }
    }
    if (bytes) {
        ++g_rbc_bad_frames;
        g_rbc_bad_bytes += bytes;
        if (g_rbc_bad_frames <= 5)
            fprintf(stderr, "[renderer-rbcheck] frame %lld: %lld byte(s) "
                    "differ\n", g_frames, bytes);
    }
    ++g_rbc_frames;
}

#if defined(GX_HAS_GPU_AA)
/* ---- THE EDGE-SMOOTHING PASS ON THE CARD (run perf2) --------------------
 *
 * ntr/gx.cpp's aa_pass smooths the finished 3D picture on the CPU after the
 * translucent pass: five luma values per covered pixel, a contrast test, a
 * direction, a blend. At RenderScale 4 that is 4 ms of a frame on a
 * twelve-core machine and 15 ms on two cores. With the card drawing, gx.cpp
 * hands the pass here instead (gx_set_gpu_aa): the picture as it stands
 * (aa_pass has already copied it aside for the display capture, that copy
 * is what is uploaded), and the coverage mask, go up; hal/gpu_raster.hlsl's
 * aa_ps runs aa_band's rule on every pixel; the result comes back and every
 * pixel that changed is written into the framebuffer and counted.
 *
 * THE RULE IS aa_band's, OPERATION FOR OPERATION, in exact arithmetic: see
 * the shader. The frame goes up and comes back as packed 32-bit words, so no
 * colour conversion stands between the CPU's bytes and the shader's.
 * SM64DS_RENDERER_AACHECK=1 runs this file's own copy of aa_band (aa_ref,
 * below) on the same input every frame and counts the pixels that differ;
 * the count is printed at exit.
 *
 * The pass needs shader model 5 (the `precise` marks that forbid a fused
 * multiply-add are only encoded there), so a device below feature level 11_0
 * answers 0 and gx.cpp runs its own pass; so does any failure, the card
 * having fallen back, and SM64DS_RENDERER_AA=0 (the knob that keeps the CPU
 * pass with the card on, for timing). The software renderer never calls
 * this: nothing registers it unless the "Renderer" setting is on. */
ID3D11VertexShader       *g_aa_vs;
ID3D11PixelShader        *g_aa_ps;
ID3D11Texture2D          *g_aa_in, *g_aa_cv, *g_aa_out, *g_aa_st;
ID3D11ShaderResourceView *g_aa_in_srv, *g_aa_cv_srv;
ID3D11RenderTargetView   *g_aa_rtv;
int g_aa_w, g_aa_h;
/* SM64DS_RENDERER_AA: 0 the processor always, 1 the card always (where it
   can), absent = WHICHEVER IS FASTER HERE, decided once at the first smoothed
   frame. Both give the same picture, so this is a speed choice only.
   Measured on level 6, same exe, card vs processor, median of 3 (RTX 4070):
     2 cores  RenderScale 3  9.56 -> 4.02 ms   RenderScale 4 19.61 -> 6.67 ms
     12 cores RenderScale 3  2.42 -> 3.35 ms   RenderScale 4  4.04 -> 5.26 ms
     WARP, 12 cores           S4 4.10 -> 11.81 ms
   The card's round trip (the picture up, the pass, the result back) costs a
   roughly fixed couple of milliseconds; the processor's pass divides by its
   cores. So the card takes it when this process has four processors or fewer
   to run on (the affinity mask, which is what a two-core machine and the
   two-core stand-in both report) and the device is a real card, not WARP. */
int g_aa_on = -1;
int g_aa_dead;         /* the pass cannot run on this device: CPU from now on */
int g_aacheck;         /* SM64DS_RENDERER_AACHECK */
long long g_aac_frames, g_aac_bad_frames, g_aac_bad_px;
std::vector<uint32_t> g_aac_ref;
long long g_aa_frames;
double g_aa_ms;

void release_aa()
{
    if (g_aa_rtv) { g_aa_rtv->Release(); g_aa_rtv = 0; }
    if (g_aa_in_srv) { g_aa_in_srv->Release(); g_aa_in_srv = 0; }
    if (g_aa_cv_srv) { g_aa_cv_srv->Release(); g_aa_cv_srv = 0; }
    if (g_aa_in) { g_aa_in->Release(); g_aa_in = 0; }
    if (g_aa_cv) { g_aa_cv->Release(); g_aa_cv = 0; }
    if (g_aa_out) { g_aa_out->Release(); g_aa_out = 0; }
    if (g_aa_st) { g_aa_st->Release(); g_aa_st = 0; }
    g_aa_w = g_aa_h = 0;
}

/* The CPU pass takes over for the rest of the run, with one line saying why.
   Not fall_back: the opaque pass on the card is unaffected by this. */
void aa_give_up(const char *why, HRESULT hr)
{
    if (g_aa_dead) return;
    g_aa_dead = 1;
    fprintf(stderr, "[renderer] the edge smoothing goes back to the processor "
            "(%s, code %08x); the picture is the same, only slower.\n", why,
            (unsigned)hr);
}

bool ensure_aa(int w, int h)
{
    if (!g_aa_vs) {
        if (port_gpu_device_feature_level() < 0xb000) {   /* D3D_FEATURE_LEVEL_11_0 */
            aa_give_up("the card is older than Direct3D 11", 0);
            return false;
        }
        HRESULT hr = g_dev->CreateVertexShader(kGpuAaVS, sizeof kGpuAaVS, 0, &g_aa_vs);
        if (FAILED(hr)) { aa_give_up("its vertex shader would not load", hr); return false; }
        hr = g_dev->CreatePixelShader(kGpuAaPS, sizeof kGpuAaPS, 0, &g_aa_ps);
        if (FAILED(hr)) { aa_give_up("its pixel shader would not load", hr); return false; }
    }
    if (g_aa_in && g_aa_w == w && g_aa_h == h) return true;
    release_aa();
    D3D11_TEXTURE2D_DESC d;
    memset(&d, 0, sizeof d);
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.Format = DXGI_FORMAT_R32_UINT;
    HRESULT hr = g_dev->CreateTexture2D(&d, 0, &g_aa_in);
    if (FAILED(hr)) { aa_give_up("no picture texture", hr); return false; }
    hr = g_dev->CreateShaderResourceView(g_aa_in, 0, &g_aa_in_srv);
    if (FAILED(hr)) { aa_give_up("no picture view", hr); return false; }
    d.Format = DXGI_FORMAT_R8_UINT;
    hr = g_dev->CreateTexture2D(&d, 0, &g_aa_cv);
    if (FAILED(hr)) { aa_give_up("no coverage texture", hr); return false; }
    hr = g_dev->CreateShaderResourceView(g_aa_cv, 0, &g_aa_cv_srv);
    if (FAILED(hr)) { aa_give_up("no coverage view", hr); return false; }
    d.Format = DXGI_FORMAT_R32_UINT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = g_dev->CreateTexture2D(&d, 0, &g_aa_out);
    if (FAILED(hr)) { aa_give_up("no result target", hr); return false; }
    hr = g_dev->CreateRenderTargetView(g_aa_out, 0, &g_aa_rtv);
    if (FAILED(hr)) { aa_give_up("no result target view", hr); return false; }
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = g_dev->CreateTexture2D(&d, 0, &g_aa_st);
    if (FAILED(hr)) { aa_give_up("no result readback buffer", hr); return false; }
    g_aa_w = w;
    g_aa_h = h;
    return true;
}

/* THIS FILE'S COPY OF ntr/gx.cpp's aa_band, for SM64DS_RENDERER_AACHECK only:
   the same luma, the same tests, the same blends, into `out`. */
inline float aa_luma(uint32_t p)
{
    return 0.299f * (float)((p >> 16) & 0xFF) +
           0.587f * (float)((p >> 8) & 0xFF) +
           0.114f * (float)(p & 0xFF);
}
inline uint32_t aa_mix2(uint32_t a, uint32_t b, float t)
{
    const float s = 1.0f - t;
    uint32_t r = 0xFF000000u;
    for (int sh = 16; sh >= 0; sh -= 8) {
        const float v = (float)((a >> sh) & 0xFF) * s + (float)((b >> sh) & 0xFF) * t;
        const int i = (int)(v + 0.5f);
        r |= (uint32_t)(i < 0 ? 0 : (i > 255 ? 255 : i)) << sh;
    }
    return r;
}
void aa_ref(const ntr::GxGpuAa *a, uint32_t *out)
{
    for (int y = 0; y < a->h; ++y) {
        const uint8_t *crow = a->cover + (size_t)y * a->stride;
        const uint32_t *rm = a->src + (size_t)y * a->stride;
        const uint32_t *rn = a->src + (size_t)(y > 0 ? y - 1 : 0) * a->stride;
        const uint32_t *rs = a->src + (size_t)(y + 1 < a->h ? y + 1 : y) * a->stride;
        uint32_t *orow = out + (size_t)y * a->w;
        for (int x = 0; x < a->w; ++x) {
            orow[x] = rm[x];
            if (!crow[x]) continue;
            const int xw = x > 0 ? x - 1 : 0;
            const int xe = x + 1 < a->w ? x + 1 : x;
            const uint32_t pM = rm[x], pN = rn[x], pS = rs[x];
            const uint32_t pW = rm[xw], pE = rm[xe];
            const float lM = aa_luma(pM), lN = aa_luma(pN), lS = aa_luma(pS);
            const float lW = aa_luma(pW), lE = aa_luma(pE);
            float lo = lM, hi = lM;
            const float ls[4] = {lN, lS, lW, lE};
            for (int i = 0; i < 4; ++i) {
                if (ls[i] < lo) lo = ls[i];
                if (ls[i] > hi) hi = ls[i];
            }
            const float range = hi - lo;
            if (range < 8.0f || range < hi * 0.125f) continue;
            const float d2x = fabsf(lW + lE - 2.0f * lM);
            const float d2y = fabsf(lN + lS - 2.0f * lM);
            const uint32_t n1 = (d2x >= d2y) ? pW : pN;
            const uint32_t n2 = (d2x >= d2y) ? pE : pS;
            const float avg = 0.25f * (lN + lS + lW + lE);
            float t = fabsf(avg - lM) / range;
            t = t * t;
            if (t > 0.5f) t = 0.5f;
            if (t <= 0.002f) continue;
            orow[x] = aa_mix2(pM, aa_mix2(n1, n2, 0.5f), t);
        }
    }
}

void aac_report()
{
    fprintf(stderr, "[renderer-aacheck] %lld frame(s) smoothed on the card and "
            "checked against the processor's rule: %lld differing, %lld "
            "differing pixel(s)\n", g_aac_frames, g_aac_bad_frames, g_aac_bad_px);
}

int aa_backend(ntr::GxGpuAa *a)
{
    if (!g_aa_on || g_aa_dead || g_down || !g_dev || !a || a->w <= 0 || a->h <= 0)
        return 0;
    if (g_aa_on < 0) {
        DWORD_PTR pm = 0, sm = 0;
        int n = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm))
            for (; pm; pm &= pm - 1) ++n;
        const bool warp = port_gpu_device_is_warp() != 0;
        g_aa_on = (n > 0 && n <= 4 && !warp) ? 1 : 0;
        fprintf(stderr, "[renderer] edge smoothing: %s (%d processor(s)%s)\n",
                g_aa_on ? "on the card" : "on the processor, which is faster here",
                n, warp ? ", WARP" : "");
        if (!g_aa_on) return 0;
    }
    if (!ensure_aa(a->w, a->h)) return 0;
    const long long t0 = qpc();

    D3D11_BOX box;
    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = (UINT)a->w;
    box.bottom = (UINT)a->h;
    box.back = 1;
    g_ctx->UpdateSubresource(g_aa_in, 0, &box, a->src, (UINT)a->stride * 4u, 0);
    g_ctx->UpdateSubresource(g_aa_cv, 0, &box, a->cover, (UINT)a->stride, 0);

    g_ctx->OMSetRenderTargets(1, &g_aa_rtv, 0);
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)a->w;
    vp.Height = (float)a->h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    g_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = a->w;
    sc.bottom = a->h;
    g_ctx->RSSetScissorRects(1, &sc);
    g_ctx->RSSetState(g_rast);
    g_ctx->OMSetDepthStencilState(0, 0);
    g_ctx->OMSetBlendState(0, 0, 0xffffffffu);
    g_ctx->IASetInputLayout(0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_aa_vs, 0, 0);
    g_ctx->PSSetShader(g_aa_ps, 0, 0);
    ID3D11ShaderResourceView *srvs[2] = { g_aa_in_srv, g_aa_cv_srv };
    g_ctx->PSSetShaderResources(0, 2, srvs);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView *none[2] = { 0, 0 };
    g_ctx->PSSetShaderResources(0, 2, none);
    g_ctx->OMSetRenderTargets(0, 0, 0);
    g_ctx->CopyResource(g_aa_st, g_aa_out);

    D3D11_MAPPED_SUBRESOURCE m;
    memset(&m, 0, sizeof m);
    const HRESULT hr = g_ctx->Map(g_aa_st, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        /* nothing has been written yet: the CPU pass does this frame */
        aa_give_up("its result could not be read back", hr);
        return 0;
    }
    if (g_aacheck) {
        g_aac_ref.resize((size_t)a->w * (size_t)a->h);
        aa_ref(a, &g_aac_ref[0]);
        long long bad = 0;
        for (int y = 0; y < a->h; ++y) {
            const uint32_t *orow =
                (const uint32_t *)((const unsigned char *)m.pData + (size_t)y * m.RowPitch);
            const uint32_t *rrow = &g_aac_ref[(size_t)y * a->w];
            for (int x = 0; x < a->w; ++x) bad += orow[x] != rrow[x];
        }
        ++g_aac_frames;
        if (bad) {
            ++g_aac_bad_frames;
            g_aac_bad_px += bad;
            if (g_aac_bad_frames <= 5)
                fprintf(stderr, "[renderer-aacheck] frame %lld: %lld pixel(s) "
                        "differ\n", g_frames, bad);
        }
    }
    /* THE WRITE, exactly aa_band's: a pixel is written, and counted, only
       where the smoothed value differs from the frame as it stood. The live
       framebuffer still holds that frame (a->src is aa_pass's copy of it). */
    unsigned long long changed = 0;
    for (int y = 0; y < a->h; ++y) {
        const uint32_t *orow =
            (const uint32_t *)((const unsigned char *)m.pData + (size_t)y * m.RowPitch);
        const uint32_t *srow = a->src + (size_t)y * a->stride;
        uint32_t *frow = a->fb + (size_t)y * a->stride;
        int x = 0;
        for (; x + 4 <= a->w; x += 4) {
            const __m128i o = _mm_loadu_si128((const __m128i *)(orow + x));
            const __m128i s = _mm_loadu_si128((const __m128i *)(srow + x));
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(o, s)) == 0xFFFF) continue;
            for (int k = x; k < x + 4; ++k)
                if (orow[k] != srow[k]) { frow[k] = orow[k]; ++changed; }
        }
        for (; x < a->w; ++x)
            if (orow[x] != srow[x]) { frow[x] = orow[x]; ++changed; }
    }
    g_ctx->Unmap(g_aa_st, 0);
    a->changed = changed;
    ++g_aa_frames;
    g_aa_ms += ms_between(t0, qpc());
    return 1;
}

void aa_at_exit()
{
    if (g_aa_frames)
        fprintf(stderr, "[renderer] %lld frame(s) smoothed on the card, %.3f ms "
                "per frame for the whole round trip.\n", g_aa_frames,
                g_aa_ms / (double)g_aa_frames);
    release_aa();
    if (g_aa_vs) { g_aa_vs->Release(); g_aa_vs = 0; }
    if (g_aa_ps) { g_aa_ps->Release(); g_aa_ps = 0; }
}
#else   /* ntr/gx.h without the edge-smoothing hook: the CPU pass always */
void aa_at_exit() {}
#endif

/* ---- the frame ----------------------------------------------------------- */

int draw_frame(const ntr::GxGpuFrame *f)
{
    if (g_down || !f) return 0;
    if (!start()) return 0;
    if (f->cw <= 0 || f->ch <= 0 || f->pw <= 0 || f->ph <= 0) return 0;

    if (g_fail_after && g_frames >= g_fail_after) {
        fprintf(stderr, "[renderer] SM64DS_RENDERER_FAIL_AFTER=%d reached after "
                "%lld frame(s); forcing the fallback on purpose\n",
                g_fail_after, g_frames);
        fall_back("a test asked for it", 0);
        return 0;
    }

    if (f->tex_generation != g_tex_gen_seen) {
        g_tex_gen_seen = f->tex_generation;
        release_textures();
    }
    if (!ensure_targets(f->cw, f->ch)) return 0;

    const long long t0 = qpc();

    /* ---- build the batches, in submission order ---------------------------
       Consecutive triangles that share a texture and a wrap mode go into one
       draw. The order within the list is the order the game submitted them,
       which is the order ntr/gx.cpp draws them, and the depth test settles the
       rest. */
    g_scratch.clear();
    g_batch.clear();
    const GTex *cur_tex = 0;
    int cur_au = -1, cur_av = -1;
    bool open = false;

    const float inv_cw = 2.0f / (float)f->cw;
    const float inv_ch = 2.0f / (float)f->ch;

    /* THE SCREEN BOX OF EVERY TRIANGLE THE CARD WILL DRAW. A pixel outside it
       cannot be covered, so the readback below only copies and scans the box:
       on a course that is the whole picture, around a menu's or a minigame's
       model it is often a small part of it. A coordinate that is not a finite
       number widens the box to the whole present rectangle rather than
       trusting it. */
    float bminx = 1e30f, bminy = 1e30f, bmaxx = -1e30f, bmaxy = -1e30f;
    bool bwide = false;

    for (size_t i = 0; i < f->count; ++i) {
        const ntr::GxTriangle &t = f->tris[i];
        if (t.translucent) continue;          /* the second pass' business */
        if (t.mode == 3) continue;            /* a shadow volume, likewise */

        const ntr::GxVertex &a = t.v[0], &b = t.v[1], &c = t.v[2];
        /* THE SAME CULL TEST, IN THE SAME ORDER, as ntr/gx.cpp's band loop. */
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (area < 1e-6f && area > -1e-6f) continue;
        const bool backface = area > 0.0f;
        if (backface && !(t.cull & 1)) continue;
        if (!backface && !(t.cull & 2)) continue;

        for (int k = 0; k < 3; ++k) {
            const float vx = t.v[k].x, vy = t.v[k].y;
            if (!(vx > -1e20f && vx < 1e20f && vy > -1e20f && vy < 1e20f)) {
                bwide = true;
                continue;
            }
            if (vx < bminx) bminx = vx;
            if (vx > bmaxx) bmaxx = vx;
            if (vy < bminy) bminy = vy;
            if (vy > bmaxy) bmaxy = vy;
        }

        const bool textured = t.tex && t.tw > 0 && t.th > 0;
        const GTex *gt = 0;
        int au = 0, av = 0;
        if (textured) {
            gt = upload_texture(t.tex_id, t.tex, t.tw, t.th, f->tex_filter);
            if (!gt) return 0;                /* fall_back already said why */
            const bool rep_s = (t.wrap & 1) != 0, rep_t = (t.wrap & 2) != 0;
            const bool flip_s = (t.wrap & 4) != 0, flip_t = (t.wrap & 8) != 0;
            au = rep_s ? (flip_s ? 2 : 1) : 0;
            av = rep_t ? (flip_t ? 2 : 1) : 0;
        }
        if (!open || gt != cur_tex || au != cur_au || av != cur_av) {
            if (open) g_batch.back().count =
                (UINT)g_scratch.size() - g_batch.back().first;
            Batch nb;
            nb.tex = gt;
            nb.au = au;
            nb.av = av;
            nb.first = (UINT)g_scratch.size();
            nb.count = 0;
            g_batch.push_back(nb);
            cur_tex = gt;
            cur_au = au;
            cur_av = av;
            open = true;
        }

        const float tsc = (float)(t.tex_scale ? t.tex_scale : 1);
        const float iw = textured ? 1.0f / (float)t.tw : 0.0f;
        const float ih = textured ? 1.0f / (float)t.th : 0.0f;
        const float pa = (float)((t.alpha >= 31 || t.alpha == 0) ? 31u : t.alpha);
        /* the polygon ID, and above it the toon mode for a mode-2 polygon
           (1 toon, 2 highlight; the shader's attr.y is id + 64 * mode) */
        const float pid = (float)(t.polyid +
            ((t.mode == 2 && f->toon_shade) ? 64 * f->toon_shade : 0));

        for (int k = 0; k < 3; ++k) {
            const ntr::GxVertex &v = t.v[k];
            GVert o;
            /* SCREEN PIXELS TO CLIP SPACE. The viewport is the whole active
               picture, so a screen x maps to 2x/cw - 1 and a screen y to
               1 - 2y/ch; multiplying through by w is what makes the card
               interpolate u and v perspective-correct and z screen-linear,
               which is the pair ntr/gx.cpp produces. */
            const float xn = (float)v.x * inv_cw - 1.0f;
            const float yn = 1.0f - (float)v.y * inv_ch;
            o.x = xn * v.w;
            o.y = yn * v.w;
            o.z = v.z * kDepthScale * v.w;   /* see kDepthScale above */
            o.w = v.w;
            o.u = textured ? v.u * tsc * iw : 0.0f;
            o.v = textured ? v.v * tsc * ih : 0.0f;
            o.r = (float)((v.color >> 16) & 0xFF);
            o.g = (float)((v.color >> 8) & 0xFF);
            o.b = (float)(v.color & 0xFF);
            o.pa = pa;
            o.pid = pid;
            g_scratch.push_back(o);
        }
    }
    if (open)
        g_batch.back().count = (UINT)g_scratch.size() - g_batch.back().first;

    const long long t1 = qpc();

    /* ---- clear, submit ---------------------------------------------------- */

    /* A FRAME WITH NOTHING FOR THE CARD TO DRAW READS NOTHING BACK. With no
       opaque triangle no pixel is covered, and the copy below only ever
       writes covered pixels, so the buffers already hold exactly what the
       whole round trip would have left in them. The file select, the menus
       and every frame whose 3D is all translucent take this path and never
       wait for the card at all. */
    const bool nothing = g_scratch.empty();

    /* The card's own clock over the opaque pass: the clears, every draw and
       the copy out of the targets, which is all of the work the Map below
       then waits for. A timestamp pair costs an End() at each end and is
       collected a picture or two later (hal/gpu_device.h). */
    if (!nothing) port_gpu_timer_span_begin(PORT_GPU_SPAN_OPAQUE);

    /* THE CLEAR COLOUR IS THE FRAMEBUFFER'S OWN, read at the top of the frame
       rather than assumed, and the alpha is meaningless here because coverage
       travels in the ID target. It only matters at all on a filtered arm,
       where a partly transparent texel blends against it; the readback below
       copies covered pixels only, so an uncovered pixel keeps exactly the
       bytes the caller's clear left. */
    const float clr[4] = {
        (float)((f->clear_argb >> 16) & 0xFF) / 255.0f,
        (float)((f->clear_argb >> 8) & 0xFF) / 255.0f,
        (float)(f->clear_argb & 0xFF) / 255.0f,
        0.0f,
    };
    const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    if (!nothing) {
        g_ctx->ClearRenderTargetView(g_col_rtv, clr);
        g_ctx->ClearRenderTargetView(g_id_rtv, zero);
        g_ctx->ClearDepthStencilView(g_dep_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

        if (!ensure_vb(g_scratch.size())) return 0;
        D3D11_MAPPED_SUBRESOURCE ms;
        memset(&ms, 0, sizeof ms);
        HRESULT hr = g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        if (FAILED(hr)) { fall_back("the vertex buffer would not open", hr); return 0; }
        memcpy(ms.pData, &g_scratch[0], g_scratch.size() * sizeof(GVert));
        g_ctx->Unmap(g_vb, 0);

        ID3D11RenderTargetView *rtvs[2] = { g_col_rtv, g_id_rtv };
        g_ctx->OMSetRenderTargets(2, rtvs, g_dep_dsv);
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = (float)f->cw;
        vp.Height = (float)f->ch;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        g_ctx->RSSetViewports(1, &vp);
        D3D11_RECT sc;
        sc.left = f->px0;
        sc.top = f->py0;
        sc.right = f->px0 + f->pw;
        sc.bottom = f->py0 + f->ph;
        g_ctx->RSSetScissorRects(1, &sc);
        g_ctx->RSSetState(g_rast);
        g_ctx->OMSetDepthStencilState(g_dss, 0);
        const float bf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_ctx->OMSetBlendState(g_blend, bf, 0xffffffffu);
        g_ctx->IASetInputLayout(g_layout);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const UINT stride = (UINT)sizeof(GVert), offset = 0;
        g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
        g_ctx->VSSetShader(g_vs, 0, 0);
        /* the filtered arms take their own pixel shader and a second, point
           sampler; filter 0 binds exactly what it always did */
        const bool filt_arm = f->tex_filter >= 1;
        g_ctx->PSSetShader(filt_arm ? g_ps_filt : g_ps, 0, 0);
        if (f->toon_shade && f->toon_rgb &&
            (!g_toon_valid ||
             memcmp(g_toon_seen, f->toon_rgb, sizeof g_toon_seen) != 0)) {
            D3D11_MAPPED_SUBRESOURCE tm;
            memset(&tm, 0, sizeof tm);
            HRESULT thr = g_ctx->Map(g_toon_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &tm);
            if (FAILED(thr)) { fall_back("the toon table would not open", thr); return 0; }
            float *dst = (float *)tm.pData;
            for (int i = 0; i < 32; ++i) {
                dst[i * 4 + 0] = f->toon_rgb[i * 3 + 0];
                dst[i * 4 + 1] = f->toon_rgb[i * 3 + 1];
                dst[i * 4 + 2] = f->toon_rgb[i * 3 + 2];
                dst[i * 4 + 3] = 0.0f;
            }
            g_ctx->Unmap(g_toon_cb, 0);
            memcpy(g_toon_seen, f->toon_rgb, sizeof g_toon_seen);
            g_toon_valid = true;
        }
        g_ctx->PSSetConstantBuffers(0, 1, &g_toon_cb);

        for (size_t bi = 0; bi < g_batch.size(); ++bi) {
            const Batch &bt = g_batch[bi];
            if (!bt.count) continue;
            ID3D11ShaderResourceView *srv = bt.tex ? bt.tex->srv : g_white_srv;
            ID3D11SamplerState *smp =
                bt.tex ? sampler(f->tex_filter, bt.au, bt.av) : sampler(0, 0, 0);
            if (!smp) { fall_back("no sampler state", 0); return 0; }
            g_ctx->PSSetShaderResources(0, 1, &srv);
            g_ctx->PSSetSamplers(0, 1, &smp);
            if (filt_arm) {
                ID3D11SamplerState *psm = bt.tex ? psampler(bt.au, bt.av) : psampler(0, 0);
                if (!psm) { fall_back("no point sampler state", 0); return 0; }
                g_ctx->PSSetSamplers(1, 1, &psm);
            }
            g_ctx->Draw(bt.count, bt.first);
            ++g_batches;
        }
        ID3D11ShaderResourceView *none = 0;
        g_ctx->PSSetShaderResources(0, 1, &none);
        g_ctx->OMSetRenderTargets(0, 0, 0);
    }

    const long long t2 = qpc();

    /* ---- read the picture back ------------------------------------------- */

    if (g_fail_readback && g_frames >= g_fail_readback) {
        fprintf(stderr, "[renderer] SM64DS_RENDERER_FAIL_READBACK=%d reached "
                "after %lld frame(s); forcing a readback failure on purpose\n",
                g_fail_readback, g_frames);
        fall_back("a test asked the readback to fail", 0);
        return 0;
    }

    /* THE BOX TO READ: the present rectangle cut down to the triangles' own
       screen box plus a pixel of margin on every side (the card snaps a
       vertex to a 1/256 pixel grid, far inside that margin). */
    int bx0 = f->px0, by0 = f->py0;
    int bx1 = f->px0 + f->pw, by1 = f->py0 + f->ph;
    if (!nothing && !bwide) {
        if (bminx - 1.0f > (float)bx0) bx0 = (int)(bminx - 1.0f);
        if (bminy - 1.0f > (float)by0) by0 = (int)(bminy - 1.0f);
        if (bmaxx + 2.0f < (float)bx1) bx1 = (int)(bmaxx + 2.0f);
        if (bmaxy + 2.0f < (float)by1) by1 = (int)(bmaxy + 2.0f);
    }
    double wait_ms = 0.0;
    if (g_rbcheck) rbc_save(f, g_rbc_fb1, g_rbc_cv1, g_rbc_id1, g_rbc_dp1, true);
    if (!nothing && bx1 > bx0 && by1 > by0) {
        /* SM64DS_RENDERER_RBTIME=1: odd frames read back the old way (the
           whole targets, the whole present rectangle scanned one pixel at a
           time), even frames this way, in one process over the same scenes,
           so a busy machine slows both halves alike; the two averages are
           printed at exit. Both leave the same bytes (RBCHECK). */
        const int arm = (g_rbtime && (g_frames & 1)) ? 1 : 0;
        const long long ta = qpc();
        const bool ok = arm
            ? read_back(f, f->px0, f->py0, f->px0 + f->pw, f->py0 + f->ph,
                        true, &wait_ms)
            : read_back(f, bx0, by0, bx1, by1, false, &wait_ms);
        if (!ok) return 0;
        if (g_rbtime) {
            g_rbt_ms[arm] += ms_between(ta, qpc());
            g_rbt_wait[arm] += wait_ms;
            ++g_rbt_n[arm];
        }
    } else if (!nothing) {
        port_gpu_timer_span_end(PORT_GPU_SPAN_OPAQUE);
    }
    if (g_rbcheck) rbc_frame(f, !nothing);

    const long long t3 = qpc();
    ++g_frames;
    g_tris += (long long)(g_scratch.size() / 3);
    g_verts += (long long)g_scratch.size();
    g_ms_build += ms_between(t0, t1);
    g_ms_draw += ms_between(t1, t2);
    g_ms_read += ms_between(t2, t3);

    if (g_perf) {
        g_perf_build += ms_between(t0, t1);
        g_perf_draw += ms_between(t1, t2);
        g_perf_read += ms_between(t2, t3);
        g_perf_wait += wait_ms;
        g_perf_copy += ms_between(t2, t3) - wait_ms;
        g_perf_tris += (long long)(g_scratch.size() / 3);
        g_perf_batches += (long long)g_batch.size();
        if (++g_perf_n >= 30) {
            fprintf(stderr, "[renderer] build %6.3fms submit %6.3fms readback "
                    "%6.3fms (wait %6.3fms copy %6.3fms) tris %6lld batches "
                    "%4lld\n",
                    g_perf_build / g_perf_n, g_perf_draw / g_perf_n,
                    g_perf_read / g_perf_n, g_perf_wait / g_perf_n,
                    g_perf_copy / g_perf_n, g_perf_tris / g_perf_n,
                    g_perf_batches / g_perf_n);
            g_perf_build = g_perf_draw = g_perf_read = 0.0;
            g_perf_wait = g_perf_copy = 0.0;
            g_perf_tris = g_perf_batches = 0;
            g_perf_n = 0;
        }
    }
    return 1;
}

int backend(const ntr::GxGpuFrame *f) { return draw_frame(f); }

}  // namespace

extern "C" void port_gpu_raster_configure(void)
{
    if (g_started) return;
    g_started = 1;
    g_on = host_setting_renderer();
    if (!g_on) return;                 /* the whole default path stops here */

    const char *dev = getenv("SM64DS_RENDERER_DEVICE");
    if (dev && *dev) {
        if (!_stricmp(dev, "warp")) g_want_warp = 1;
        else if (!_stricmp(dev, "hw") || !_stricmp(dev, "hardware")) g_want_warp = 0;
    }
    g_perf = env_int("SM64DS_RENDERER_PERF", 0);
    g_fail_after = env_int("SM64DS_RENDERER_FAIL_AFTER", 0);
    g_fail_readback = env_int("SM64DS_RENDERER_FAIL_READBACK", 0);
    g_addrcheck = env_int("SM64DS_RENDERER_ADDRCHECK", 0);
    g_fail_device = env_int("SM64DS_RENDERER_FAIL_DEVICE", 0);
    g_rbcheck = env_int("SM64DS_RENDERER_RBCHECK", 0);
    if (g_rbcheck) atexit(rbc_report);
    g_rbtime = env_int("SM64DS_RENDERER_RBTIME", 0);
    if (g_rbtime) atexit(rbt_report);
#if defined(GX_HAS_GPU_AA)
    g_aa_on = env_int("SM64DS_RENDERER_AA", -1);
    g_aacheck = env_int("SM64DS_RENDERER_AACHECK", 0);
    if (g_aacheck) atexit(aac_report);
#endif
    ntr::gx_set_gpu_opaque(&backend);
#if defined(GX_HAS_GPU_AA)
    ntr::gx_set_gpu_aa(&aa_backend);
#endif
}

extern "C" int port_gpu_raster_active(void)
{
    return (g_on && !g_down) ? 1 : 0;
}

extern "C" double port_gpu_raster_readback_ms(void)
{
    return g_ms_read;
}

#else   /* not Windows: the port ships on Windows and there is no card here */

#include "hal/gpu_raster.h"

void port_gpu_raster_configure(void) {}
int port_gpu_raster_active(void) { return 0; }
double port_gpu_raster_readback_ms(void) { return 0.0; }

#endif

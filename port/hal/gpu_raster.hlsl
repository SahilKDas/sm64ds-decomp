// THE OPAQUE 3D PASS, ON THE CARD. Compiled offline with the Windows SDK's
// fxc.exe into hal/gpu_raster_shaders.h; d3dcompiler.dll never loads at run
// time. The exact commands are in that header's own comment.
//
// EVERY DECISION HERE IS A COPY OF ntr/gx.cpp's SOFTWARE RASTER, not an
// improvement on it. The software path is the byte gate and the default, so
// where this file and that loop disagree the software one is right and this is
// the bug. The three places that matter:
//
//  1. VERTEX COLOUR INTERPOLATES SCREEN-LINEAR, which is why it is declared
//     noperspective. gx.cpp interpolates the colour with the plain screen-space
//     barycentrics (l0 * acol + l1 * bcol + l2 * ccol, no 1/w anywhere) while it
//     interpolates u and v through 1/w. A perspective-correct colour would be
//     the more "correct" picture and the wrong one.
//
//  2. THE TEXEL ARITHMETIC IS THE SAME ARITHMETIC. gx.cpp computes
//     round(vertex_colour * texel / 255) per channel with the vertex colour
//     carried as a 0..255 float, so the colour comes in here as 0..255 floats
//     rather than a packed byte colour, and the texel arrives as an 8-bit UNORM
//     sample, whose conversion to float is exactly n/255.
//
//  3. THE ALPHA RULE IS THE SAME RULE. A texel with alpha 0 is no pixel at all:
//     no colour, no coverage and no depth (gx.cpp's "transparent texel"
//     continue). The effective alpha is then the integer (poly_alpha * texel_a +
//     127) / 255 in 0..31, and at the setting this is gated on -- point sampling
//     -- an opaque-class polygon always lands on exactly 31, which makes the
//     blend below an exact replacement.
//
//  4. MODE 2 IS THE SAME TABLE LOOKUP (run hunt2, lane RENDER2). A toon or
//     highlight polygon (POLYGON_ATTR mode 2) takes its colour through the
//     frame's TOON TABLE, picked per pixel by the interpolated vertex red:
//     toon modulates the entry instead of the vertex colour, highlight
//     modulates the vertex red on all three channels and adds the entry after.
//     gx.cpp's band loop does the same arithmetic; the mode rides in the
//     polygon-ID attribute as id + 64 * mode (1 toon, 2 highlight), so the
//     vertex layout did not change.
//
// COVERAGE TRAVELS IN THE POLYGON-ID TARGET, not in the colour target's alpha,
// because the colour target's alpha has to drive the blend. Bit 7 of the R8
// target says "this engine wrote this pixel" and bits 0..5 are the polygon ID
// the shadow pass reads back; a DS polygon ID is six bits, so bit 7 is free.

struct VSIn {
    float4 pos  : POSITION;    // clip space, built on the CPU as (xn*w, yn*w, z*w, w)
    float2 uv   : TEXCOORD0;   // already divided by the bound buffer's size
    float3 col  : COLOR0;      // vertex colour, 0..255 per channel
    float2 attr : TEXCOORD1;   // x = polygon alpha 0..31, y = polygon ID 0..63
                               //     + 64 * toon mode (0 none, 1 toon, 2 highlight)
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    noperspective float3 col : COLOR0;
    nointerpolation float2 attr : TEXCOORD1;
};

VSOut vs_main(VSIn i)
{
    VSOut o;
    o.pos = i.pos;
    o.uv = i.uv;
    o.col = i.col;
    o.attr = i.attr;
    return o;
}

Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);

// the frame's toon table, 32 entries, rgb in 0..255 (gx.cpp's g_toon8)
cbuffer ToonTable : register(b0)
{
    float4 toon[32];
};

struct PSOut {
    float4 col : SV_TARGET0;
    uint id : SV_TARGET1;
};

PSOut shade(VSOut i, float4 t, float ta)
{
    PSOut o;
    float sa = floor((i.attr.x * ta + 127.0) / 255.0);
    uint a = (uint)(i.attr.y + 0.5);
    uint m = a >> 6;
    float3 v = i.col;
    float3 e = float3(0.0, 0.0, 0.0);
    if (m != 0u) {
        // gx.cpp's toon_index: round(red * 31 / 255), 0..31
        int k = clamp((int)(i.col.r * (31.0f / 255.0f) + 0.5f), 0, 31);
        e = toon[k].rgb;
        v = (m == 1u) ? e : i.col.rrr;
    }
    float3 f = floor(v * t.rgb + 0.5);
    if (m == 2u) f += e;
    float3 c = clamp(f, 0.0, 255.0) * (1.0 / 255.0);
    o.col = float4(c, min(sa, 31.0) * (1.0 / 31.0));
    o.id = 0x80u | (a & 63u);
    return o;
}

PSOut ps_main(VSOut i)
{
    float4 t = tex0.Sample(smp0, i.uv);
    // An untextured polygon binds a one-pixel opaque white texture, so this is
    // the same code path with t == 1 and no branch: gx.cpp's own untextured
    // case is literally texel = 0xFFFFFFFF.
    float ta = floor(t.a * 255.0 + 0.5);
    clip(ta - 0.5);                       // alpha-0 texel: not a pixel
    return shade(i, t, ta);
}

// THE SAME PIXEL UNDER TEXTUREFILTER 1 OR 2 (run hunt7, lane FILTEREDGE1). The
// texture bound here is held PREMULTIPLIED (rgb * a, 16 bits a channel), so the
// card's own bilinear / trilinear blend is the alpha-weighted blend gx.cpp's
// sample_bilinear does by hand, and dividing by the blended alpha gives the
// straight colour back: a transparent texel's colour (black, usually) never
// reaches the visible edge of a cut-out. COVERAGE IS NOT THE FILTER'S: s1 is a
// point sampler pinned to the top level, and its texel is the one TextureFilter
// 0 would have taken. Alpha 0 there is no pixel (no colour, no depth), a fully
// opaque texel is a fully opaque pixel that writes depth like any other, and
// only a texel with an alpha of its own (an A3I5 / A5I3 fade) keeps the
// filtered alpha. gx.cpp's filtered_texel_cover is this same rule.
SamplerState smpP : register(s1);

PSOut ps_filt(VSOut i)
{
    float4 tf = tex0.Sample(smp0, i.uv);
    float4 tp = tex0.SampleLevel(smpP, i.uv, 0.0);
    float pa = floor(tp.a * 255.0 + 0.5);
    clip(pa - 0.5);                       // the DS's texel is alpha 0: not a pixel
    float fa = floor(tf.a * 255.0 + 0.5);
    float ta = (pa > 254.5) ? 255.0 : ((fa > 0.5) ? fa : pa);
    float4 t = float4(saturate(tf.rgb / max(tf.a, 1.0 / 65535.0)), ta * (1.0 / 255.0));
    return shade(i, t, ta);
}

// ============================================================================
// THE EDGE-SMOOTHING PASS, ON THE CARD (run perf2, lane GPURB). A copy of
// ntr/gx.cpp's aa_band, operation for operation, not an improvement on it:
// the CPU pass stays the reference and runs whenever the card is not drawing.
//
// EXACT ARITHMETIC. Every value is computed in the same order as the C++, each
// operation rounded once to a 32-bit float, which is what `precise` asks of the
// compiler (no fused multiply-add, no reassociation) and what Direct3D 11
// requires of an add and a multiply. The pixels are integers in and integers
// out: the frame arrives as one packed 0xAARRGGBB word per texel (R32_UINT) and
// the coverage mask as one byte, and the result leaves the same way, so no
// UNORM conversion stands between the CPU's bytes and the shader's.
//
// THE ONE OPERATION DIRECT3D DOES NOT ROUND EXACTLY is the divide (2.5 ULP).
// aa_band divides once, t = |avg - lM| / range, so div_rn below finds the
// correctly rounded quotient: it tries the few floats around the card's own
// answer and keeps the one whose remainder a - q * b, computed exactly with
// Dekker's split product, is smallest. The quotient of two floats is never
// exactly half way between two floats, so there is always one winner.

Texture2D<uint> aa_src : register(t0);
Texture2D<uint> aa_cov : register(t1);

float4 aa_vs(uint id : SV_VertexID) : SV_Position
{
    // one triangle over the whole picture: (-1,1) (3,1) (-1,-3)
    float x = (id == 1) ? 3.0 : -1.0;
    float y = (id == 2) ? -3.0 : 1.0;
    return float4(x, y, 0.0, 1.0);
}

float luma(uint p)
{
    precise float r = (float)((p >> 16) & 0xFFu);
    precise float g = (float)((p >> 8) & 0xFFu);
    precise float b = (float)(p & 0xFFu);
    precise float v = 0.299f * r + 0.587f * g;
    precise float l = v + 0.114f * b;
    return l;
}

// the exact error of p = x * y, p already rounded (Dekker, no fused ops)
float prod_err(float x, float y, float p)
{
    precise float cx = 4097.0f * x;
    precise float xh = cx - (cx - x);
    precise float xl = x - xh;
    precise float cy = 4097.0f * y;
    precise float yh = cy - (cy - y);
    precise float yl = y - yh;
    precise float e = ((xh * yh - p) + xh * yl + xl * yh) + xl * yl;
    return e;
}

// a >= 0, b > 0: the float nearest a / b
float div_rn(float a, float b)
{
    if (a == 0.0f) return 0.0f;
    precise float q0 = a / b;
    float best = q0;
    precise float bestr = 1e30f;
    [unroll] for (int k = -3; k <= 3; ++k) {
        precise float q = asfloat((uint)((int)asuint(q0) + k));
        precise float p = q * b;
        precise float e = prod_err(q, b, p);
        precise float r = (a - p) - e;
        precise float ar = abs(r);
        if (ar < bestr) { bestr = ar; best = q; }
    }
    return best;
}

uint mix_ch(uint a, uint b, float s, float t)
{
    precise float v = (float)a * s + (float)b * t;
    precise float w = v + 0.5f;
    int i = (int)w;
    return (uint)clamp(i, 0, 255);
}

uint mix2(uint a, uint b, float t)
{
    precise float s = 1.0f - t;
    return 0xFF000000u |
           (mix_ch((a >> 16) & 0xFFu, (b >> 16) & 0xFFu, s, t) << 16) |
           (mix_ch((a >> 8) & 0xFFu, (b >> 8) & 0xFFu, s, t) << 8) |
           mix_ch(a & 0xFFu, b & 0xFFu, s, t);
}

uint aa_ps(float4 pos : SV_Position) : SV_Target0
{
    int w, h;
    aa_src.GetDimensions(w, h);
    int x = (int)pos.x, y = (int)pos.y;
    uint pM = aa_src.Load(int3(x, y, 0));
    if (aa_cov.Load(int3(x, y, 0)) == 0u) return pM;
    int xw = x > 0 ? x - 1 : 0;
    int xe = x + 1 < w ? x + 1 : x;
    int yn = y > 0 ? y - 1 : 0;
    int ys = y + 1 < h ? y + 1 : y;
    uint pN = aa_src.Load(int3(x, yn, 0));
    uint pS = aa_src.Load(int3(x, ys, 0));
    uint pW = aa_src.Load(int3(xw, y, 0));
    uint pE = aa_src.Load(int3(xe, y, 0));
    precise float lM = luma(pM), lN = luma(pN), lS = luma(pS);
    precise float lW = luma(pW), lE = luma(pE);
    float lo = min(min(min(min(lM, lN), lS), lW), lE);
    float hi = max(max(max(max(lM, lN), lS), lW), lE);
    precise float range = hi - lo;
    precise float hr = hi * 0.125f;
    if (range < 8.0f || range < hr) return pM;
    precise float d2x = abs((lW + lE) - 2.0f * lM);
    precise float d2y = abs((lN + lS) - 2.0f * lM);
    uint n1 = (d2x >= d2y) ? pW : pN;
    uint n2 = (d2x >= d2y) ? pE : pS;
    precise float avg = 0.25f * (((lN + lS) + lW) + lE);
    precise float t = div_rn(abs(avg - lM), range);
    t = t * t;
    if (t > 0.5f) t = 0.5f;
    if (t <= 0.002f) return pM;
    uint nb = mix2(n1, n2, 0.5f);
    return mix2(pM, nb, t);
}

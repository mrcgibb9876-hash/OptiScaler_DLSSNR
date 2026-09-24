// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"
#include "OS_Upsamplers.h"

#include <Config.h>

#include <string>

namespace
{

// Shared declarations. Repeated into each shader below at first use rather than kept as one
// translation-wide string that the others reference: these are assembled inside a function-local
// static, so there is no order-of-initialisation question to get wrong.
const char* kPreamble = R"(
#ifdef VK_MODE
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    int _SrcWidth;
    int _SrcHeight;
    int _DstWidth;
    int _DstHeight;
    float _AntiRinging;
    float _Sharpness;
    float _Sigmoid;
    float _Dither;
    int _Frame;
};

#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> InputTexture : register(t0);

#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
RWTexture2D<float4> OutputTexture : register(u0);

#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
SamplerState LinearClampSampler : register(s0);

static int ClampInt(int v, int lo, int hi)
{
    return min(max(v, lo), hi);
}

// Source texel by integer coordinate, clamped at the edges. Sampling the exact texel centre through
// the linear sampler is what the downsamplers in OS_Common.h do -- it is point sampling in all but
// name, and it keeps one static sampler serving every shader here.
static float3 FetchTexel(int x, int y)
{
    float2 uv = float2((float) ClampInt(x, 0, _SrcWidth - 1) + 0.5f,
                       (float) ClampInt(y, 0, _SrcHeight - 1) + 0.5f) /
                float2((float) _SrcWidth, (float) _SrcHeight);
    return InputTexture.SampleLevel(LinearClampSampler, uv, 0.0f).rgb;
}

static float2 SourceScale()
{
    return float2((float) _SrcWidth / (float) _DstWidth, (float) _SrcHeight / (float) _DstHeight);
}
)";

// Sigmoidal light. Resampling in a space whose transfer curve is an S compresses the overshoot a
// ringing filter leaves near black and near white, instead of letting it clip into a visible band.
// Forward transform and inverse are exact, so with the filter itself unchanged the only thing that
// moves is where the overshoot lands.
//
// The transform is defined on [0, 1] only. This pass can be handed HDR values well above 1, and
// squashing those into range would be a worse artefact than the ringing it is meant to fix, so a
// component outside (0, 1) passes through untouched. That makes it an SDR control in practice,
// which is why it defaults to off.
//
// The strength slider is the curve's SLOPE, with libplacebo's 6.5 at the top of the range. That is
// what makes it a real zero-to-max control rather than a switch wearing a slider: a low slope is a
// curve that barely bends, not a weaker version of a fixed one. Below about 2% of the range the
// scale factor the inverse divides by gets small enough to be worth avoiding, and a curve that
// shallow is doing nothing anyway, so the caller treats that as off.
const char* kSigmoid = R"(
static float2 SigmoidTerms(float centre, float slope)
{
    float offset = 1.0f / (1.0f + exp(slope * centre));
    float scale = 1.0f / (1.0f + exp(slope * (centre - 1.0f))) - offset;
    return float2(offset, scale);
}

static float SigmoidFwd1(float c, float centre, float slope, float2 terms)
{
    if (c <= 0.0f || c >= 1.0f)
        return c;
    return centre - log(max(1.0f / (c * terms.y + terms.x) - 1.0f, 1e-6f)) / slope;
}

static float SigmoidInv1(float s, float centre, float slope, float2 terms)
{
    if (s <= 0.0f || s >= 1.0f)
        return s;
    return (1.0f / (1.0f + exp(slope * (centre - s))) - terms.x) / terms.y;
}

static float3 SigmoidFwd(float3 c, float centre, float slope, float2 terms)
{
    return float3(SigmoidFwd1(c.r, centre, slope, terms), SigmoidFwd1(c.g, centre, slope, terms),
                  SigmoidFwd1(c.b, centre, slope, terms));
}

static float3 SigmoidInv(float3 c, float centre, float slope, float2 terms)
{
    return float3(SigmoidInv1(c.r, centre, slope, terms), SigmoidInv1(c.g, centre, slope, terms),
                  SigmoidInv1(c.b, centre, slope, terms));
}
)";

// EWA Lanczos -- a polar filter rather than a separable one.
//
// Every other filter in this folder is separable: it weights horizontally, then vertically, which
// makes its support a SQUARE. A pixel two across and two up is as close, in the filter's eyes, as a
// pixel 2.83 away in a straight line is not -- so a diagonal edge is reconstructed from a kernel
// that reaches further along the diagonal than along the axes, and gets the staircase separable
// filters are known for. This one weights by true radial distance, so its support is a DISC and a
// diagonal is treated exactly like a horizontal.
//
// The cost is that there is no separable trick to exploit: 64 taps per destination pixel against
// Lanczos3's 36, and all of them real samples rather than two cheap passes. It is the most expensive
// thing in this folder by a wide margin. That is the known price of EWA, not a bug to tune out.
const char* kEwaLanczos = R"(
static const float JINC_ZERO1 = 1.2196698912665045f;

// The third and fourth zeros of jinc, which are the radii of the two filters libplacebo names at
// either end of the sharpness slider, and the kernel stretch of the sharper one.
static const float JINC_ZERO3 = 3.2383154841662362f;
static const float JINC_ZERO4 = 4.2410628637960699f;
static const float BLUR_SHARPEST = 0.8845120932605482f;

// 4x4 Bayer, for the dither at the end. Written out rather than derived, because the bit trick that
// produces it is harder to check than the matrix is.
static const float BAYER4[16] = {
     0.0f,  8.0f,  2.0f, 10.0f,
    12.0f,  4.0f, 14.0f,  6.0f,
     3.0f, 11.0f,  1.0f,  9.0f,
    15.0f,  7.0f, 13.0f,  5.0f
};

// Bessel J1, Abramowitz and Stegun 9.4.4 and 9.4.6 -- the same family of polynomial approximation
// the Kaiser downsamplers already use for I0, and good to about 1e-7 across the range this needs.
// Called with a non-negative argument only, so the odd-function branch for x < 0 is not written.
static float BesselJ1(float x)
{
    if (x < 3.0f)
    {
        float t = x / 3.0f;
        float t2 = t * t;
        return x * (0.5f +
                    t2 * (-0.56249985f +
                          t2 * (0.21093573f +
                                t2 * (-0.03954289f + t2 * (0.00443319f + t2 * (-0.00031761f + t2 * 0.00001109f))))));
    }

    float t = 3.0f / x;
    float f1 = 0.79788456f +
               t * (0.00000156f +
                    t * (0.01659667f + t * (0.00017105f + t * (-0.00249511f + t * (0.00113653f + t * -0.00020033f)))));
    float th = x - 2.35619449f +
               t * (0.12499612f +
                    t * (0.00005650f + t * (-0.00637879f + t * (0.00074348f + t * (0.00079824f + t * -0.00029166f)))));
    return f1 * cos(th) / sqrt(x);
}

// Jinc, normalised so that its first zero lands at JINC_ZERO1 -- the radial analogue of sinc.
static float Jinc(float x)
{
    if (x < 1e-6f)
        return 1.0f;
    float px = 3.14159265358979f * x;
    return 2.0f * BesselJ1(px) / px;
}

// Lanczos windowing, done radially: the kernel is jinc, and the window is jinc stretched so that
// its own first zero falls exactly on the filter radius. The same construction as sinc(x)*sinc(x/a)
// in one dimension, with every 1 replaced by the first zero of jinc.
//
// blur stretches the KERNEL without moving the window, so below 1 the kernel is evaluated further
// out than the distance asks for and the filter sharpens. This is libplacebo's blur coefficient and
// it is what separates its three named EWA filters from each other.
static float EwaWeight(float d, float radius, float blur)
{
    if (d >= radius)
        return 0.0f;
    return Jinc(d / blur) * Jinc(d * JINC_ZERO1 / radius);
}

// Ordered dither, advanced per frame by the golden ratio so the pattern is well spread over time
// rather than sitting still as a texture you can pick out. Returns -0.5 to +0.5.
static float DitherOffset(uint2 p, int frame)
{
    float cell = BAYER4[(p.y & 3) * 4 + (p.x & 3)] / 16.0f;
    return frac(cell + (float) frame * 0.61803398875f) - 0.5f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint ox = id.x;
    uint oy = id.y;
    if (ox >= (uint) _DstWidth || oy >= (uint) _DstHeight)
        return;

    float2 srcPos = (float2((float) ox + 0.5f, (float) oy + 0.5f)) * SourceScale() - 0.5f;

    int2 ip = (int2) floor(srcPos);
    float2 f = srcPos - (float2) ip;

    // One slider, three filters. At 0 this is libplacebo's ewa_lanczos. At 1 it is
    // ewa_lanczos4sharpest -- that filter's radius AND its kernel stretch. ewa_lanczossharp is a
    // stretch of 0.98125 at ewa_lanczos's radius, so the radius is held flat until the stretch is
    // already past that point and only then opens up: that puts all three of them on the line,
    // instead of two of them on it and the third somewhere beside it.
    float sharpness = saturate(_Sharpness);
    float blur = lerp(1.0f, BLUR_SHARPEST, sharpness);
    float radius = lerp(JINC_ZERO3, JINC_ZERO4, smoothstep(0.5f, 1.0f, sharpness));

    // Sharpening widens the support, so the tap count follows the slider instead of always paying
    // for the widest setting. 8x8 at the soft end, 10x10 at the sharp one.
    int taps = (int) ceil(radius);

    // The slope of the sigmoid curve, with libplacebo's 6.5 at the top of the slider. Too shallow a
    // curve is doing nothing and divides the inverse by a scale factor small enough to be worth
    // avoiding, so the bottom of the range is off rather than nearly off.
    float sigmoidStrength = saturate(_Sigmoid);
    bool useSigmoid = sigmoidStrength >= 0.02f;
    float sigmoidCentre = 0.75f;
    float sigmoidSlope = 6.5f * sigmoidStrength;
    float2 terms = SigmoidTerms(sigmoidCentre, sigmoidSlope);

    float3 acc = 0.0f;
    float wsum = 0.0f;

    // The four texels the sample point sits between, kept for the anti-ringing clamp below.
    float3 lo = 1e30f;
    float3 hi = -1e30f;

    for (int j = -taps + 1; j <= taps; ++j)
    {
        for (int i = -taps + 1; i <= taps; ++i)
        {
            float w = EwaWeight(length(float2((float) i - f.x, (float) j - f.y)), radius, blur);
            if (w == 0.0f)
                continue;

            float3 s = FetchTexel(ip.x + i, ip.y + j);
            if (useSigmoid)
                s = SigmoidFwd(s, sigmoidCentre, sigmoidSlope, terms);

            if (i >= 0 && i <= 1 && j >= 0 && j <= 1)
            {
                lo = min(lo, s);
                hi = max(hi, s);
            }

            acc += s * w;
            wsum += w;
        }
    }

    float3 outRgb = (wsum != 0.0f) ? (acc / wsum) : FetchTexel(ip.x, ip.y);

    // Anti-ringing. Pull the answer back toward the range the four texels it sits between already
    // covered. Clamping to the whole 64-tap kernel's range instead would almost never bite -- a
    // kernel that wide nearly always contains the overshoot somewhere -- and a ring is visible
    // against its IMMEDIATE neighbours, so those are what bound it.
    outRgb = lerp(outRgb, clamp(outRgb, lo, hi), saturate(_AntiRinging));

    if (useSigmoid)
        outRgb = SigmoidInv(outRgb, sigmoidCentre, sigmoidSlope, terms);

    // Dither last, after the clamp, because it is the final thing before the store that can break a
    // band -- and because anything applied after it would quantise it straight back out again.
    //
    // The amplitude is half a step of an 8-BIT output at the top of the slider. Eight bits is the
    // assumption, not a reading: this pass cannot see what the swapchain will do with the frame, and
    // banding is an SDR complaint. On a wider output the dither is simply below the step size and
    // costs nothing.
    float ditherStrength = saturate(_Dither);
    if (ditherStrength > 0.0f)
        outRgb += DitherOffset(uint2(ox, oy), _Frame) * (ditherStrength / 255.0f);

    OutputTexture[uint2(ox, oy)] = float4(outRgb, 1.0f);
}
)";

// xBR-lv2, Hyllian's shader, ported from the Cg in libretro/common-shaders (MIT -- the notice is in
// Licenses/xBR-lv2_LICENSE.txt and repeated at the head of the source below).
//
// This is a pixel-art scaler, and it is the reason it is worth having: every other filter here
// reconstructs a band-limited signal, which is the right model for a rendered frame and the wrong
// one for art that was drawn one pixel at a time. On a sprite or a 2D UI drawn at 320x240, Lanczos
// gives you a blurred sprite with ringing on it; xBR finds the edge the artist drew and follows it.
//
// It needs no adapting to arbitrary scale factors. The original already works from the sub-texel
// position (its fp), so it produces a result for any destination pixel rather than a fixed 2x or 4x
// block -- all this port changes is where the neighbourhood comes from and how the result is
// written.
//
// Two things the port has to do that Cg did not: HLSL has no component-wise && or || on bool4 that
// survives both fxc and dxc, so those are spelled out; and a bool4 used as a multiplier is cast
// rather than promoted.
const char* kXbr = R"(
// Hyllian's xBR-lv2 Shader
// Copyright (C) 2011-2016 Hyllian - sergiogdb@gmail.com
// Released under the MIT licence; see Licenses/xBR-lv2_LICENSE.txt for the full notice.
// Incorporates some of the ideas from the SABR shader. Thanks to Joshua Street.

static const float XBR_SCALE = 4.0f;
static const float XBR_Y_WEIGHT = 48.0f;
static const float XBR_EQ_THRESHOLD = 25.0f;
static const float XBR_LV2_COEFFICIENT = 2.0f;

static const float4 Ao = float4(1.0f, -1.0f, -1.0f, 1.0f);
static const float4 Bo = float4(1.0f, 1.0f, -1.0f, -1.0f);
static const float4 Co = float4(1.5f, 0.5f, -0.5f, 0.5f);
static const float4 Ax = float4(1.0f, -1.0f, -1.0f, 1.0f);
static const float4 Bx = float4(0.5f, 2.0f, -0.5f, -2.0f);
static const float4 Cx = float4(1.0f, 1.0f, -0.5f, 0.0f);
static const float4 Ay = float4(1.0f, -1.0f, -1.0f, 1.0f);
static const float4 By = float4(2.0f, 0.5f, -2.0f, -0.5f);
static const float4 Cy = float4(2.0f, 0.0f, -1.0f, 0.5f);
static const float4 Ci = float4(0.25f, 0.25f, 0.25f, 0.25f);

static const float3 YWeights = float3(0.2126f, 0.7152f, 0.0722f);

static float4 df4(float4 A, float4 B)
{
    return abs(A - B);
}

static float c_df(float3 c1, float3 c2)
{
    float3 d = abs(c1 - c2);
    return d.r + d.g + d.b;
}

// Component-wise boolean helpers. Cg wrote these as && / || / ! on bool4 directly; fxc accepts that
// and dxc rejects it, so spelling them out keeps one source good for both and costs nothing.
static bool4 And4(bool4 a, bool4 b)
{
    return bool4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w);
}

static bool4 Or4(bool4 a, bool4 b)
{
    return bool4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w);
}

static bool4 Not4(bool4 a)
{
    return bool4(!a.x, !a.y, !a.z, !a.w);
}

static bool4 eq4(float4 A, float4 B)
{
    return df4(A, B) < float4(XBR_EQ_THRESHOLD, XBR_EQ_THRESHOLD, XBR_EQ_THRESHOLD, XBR_EQ_THRESHOLD);
}

static float4 weighted_distance(float4 a, float4 b, float4 c, float4 d, float4 e, float4 f, float4 g, float4 h)
{
    return df4(a, b) + df4(a, c) + df4(d, e) + df4(d, f) + 4.0f * df4(g, h);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint ox = id.x;
    uint oy = id.y;
    if (ox >= (uint) _DstWidth || oy >= (uint) _DstHeight)
        return;

    float2 srcPos = (float2((float) ox + 0.5f, (float) oy + 0.5f)) * SourceScale();

    int2 t = (int2) floor(srcPos);
    float2 fp = srcPos - (float2) t;

    //    A1 B1 C1
    // A0  A  B  C C4
    // D0  D  E  F F4
    // G0  G  H  I I4
    //    G5 H5 I5
    float3 A1 = FetchTexel(t.x - 1, t.y - 2);
    float3 B1 = FetchTexel(t.x + 0, t.y - 2);
    float3 C1 = FetchTexel(t.x + 1, t.y - 2);

    float3 A = FetchTexel(t.x - 1, t.y - 1);
    float3 B = FetchTexel(t.x + 0, t.y - 1);
    float3 C = FetchTexel(t.x + 1, t.y - 1);

    float3 D = FetchTexel(t.x - 1, t.y + 0);
    float3 E = FetchTexel(t.x + 0, t.y + 0);
    float3 F = FetchTexel(t.x + 1, t.y + 0);

    float3 G = FetchTexel(t.x - 1, t.y + 1);
    float3 H = FetchTexel(t.x + 0, t.y + 1);
    float3 I = FetchTexel(t.x + 1, t.y + 1);

    float3 G5 = FetchTexel(t.x - 1, t.y + 2);
    float3 H5 = FetchTexel(t.x + 0, t.y + 2);
    float3 I5 = FetchTexel(t.x + 1, t.y + 2);

    float3 A0 = FetchTexel(t.x - 2, t.y - 1);
    float3 D0 = FetchTexel(t.x - 2, t.y + 0);
    float3 G0 = FetchTexel(t.x - 2, t.y + 1);

    float3 C4 = FetchTexel(t.x + 2, t.y - 1);
    float3 F4 = FetchTexel(t.x + 2, t.y + 0);
    float3 I4 = FetchTexel(t.x + 2, t.y + 1);

    float3 yw = XBR_Y_WEIGHT * YWeights;

    float4 b = mul(float4x3(B, D, H, F), yw);
    float4 c = mul(float4x3(C, A, G, I), yw);
    float4 e = mul(float4x3(E, E, E, E), yw);
    float4 d = b.yzwx;
    float4 f = b.wxyz;
    float4 g = c.zwxy;
    float4 h = b.zwxy;
    float4 i = c.wxyz;

    float4 i4 = mul(float4x3(I4, C1, A0, G5), yw);
    float4 i5 = mul(float4x3(I5, C4, A1, G0), yw);
    float4 h5 = mul(float4x3(H5, F4, B1, D0), yw);
    float4 f4 = h5.yzwx;

    float4 delta = float4(1.0f / XBR_SCALE, 1.0f / XBR_SCALE, 1.0f / XBR_SCALE, 1.0f / XBR_SCALE);
    float4 deltaL = float4(0.5f / XBR_SCALE, 1.0f / XBR_SCALE, 0.5f / XBR_SCALE, 1.0f / XBR_SCALE);
    float4 deltaU = deltaL.yxwz;

    // These inequations define the line below which interpolation occurs.
    float4 fx = Ao * fp.y + Bo * fp.x;
    float4 fx_left = Ax * fp.y + Bx * fp.x;
    float4 fx_up = Ay * fp.y + By * fp.x;

    // CORNER_A, the original's default of the four corner rules.
    bool4 interp_restriction_lv0 = And4(e != f, e != h);
    bool4 interp_restriction_lv1 = interp_restriction_lv0;
    bool4 interp_restriction_lv2_left = And4(e != g, d != g);
    bool4 interp_restriction_lv2_up = And4(e != c, b != c);

    float4 fx45i = saturate((fx + delta - Co - Ci) / (2.0f * delta));
    float4 fx45 = saturate((fx + delta - Co) / (2.0f * delta));
    float4 fx30 = saturate((fx_left + deltaL - Cx) / (2.0f * deltaL));
    float4 fx60 = saturate((fx_up + deltaU - Cy) / (2.0f * deltaU));

    float4 wd1 = weighted_distance(e, c, g, i, h5, f4, h, f);
    float4 wd2 = weighted_distance(h, d, i5, f, i4, b, e, i);

    bool4 edri = And4(wd1 <= wd2, interp_restriction_lv0);
    bool4 edr = And4(wd1 < wd2, interp_restriction_lv1);
    edr = And4(edr, Or4(Not4(edri.yzwx), Not4(edri.wxyz)));

    bool4 edr_left = And4(And4(XBR_LV2_COEFFICIENT * df4(f, g) <= df4(h, c), interp_restriction_lv2_left),
                          And4(edr, And4(Not4(edri.yzwx), eq4(e, c))));
    bool4 edr_up = And4(And4(df4(f, g) >= XBR_LV2_COEFFICIENT * df4(h, c), interp_restriction_lv2_up),
                        And4(edr, And4(Not4(edri.wxyz), eq4(e, g))));

    fx45 = (float4) edr * fx45;
    fx30 = (float4) edr_left * fx30;
    fx60 = (float4) edr_up * fx60;
    fx45i = (float4) edri * fx45i;

    bool4 px = df4(e, f) <= df4(e, h);

    float4 maximos = max(max(fx30, fx60), max(fx45, fx45i));

    float3 res1 = E;
    res1 = lerp(res1, lerp(H, F, px.x ? 1.0f : 0.0f), maximos.x);
    res1 = lerp(res1, lerp(B, D, px.z ? 1.0f : 0.0f), maximos.z);

    float3 res2 = E;
    res2 = lerp(res2, lerp(F, B, px.y ? 1.0f : 0.0f), maximos.y);
    res2 = lerp(res2, lerp(D, H, px.w ? 1.0f : 0.0f), maximos.w);

    float3 res = lerp(res1, res2, step(c_df(E, res1), c_df(E, res2)));

    OutputTexture[uint2(ox, oy)] = float4(res, 1.0f);
}
)";

// Sharp bilinear. Nearest at the largest whole factor that fits, with the hardware's bilinear used
// only across the last fractional step of the seam between one source texel's block and the next.
//
// It is the answer to a specific complaint: nearest neighbour at a non-integer factor gives some
// source rows one output pixel more than their neighbours, so a scrolling sprite visibly wobbles;
// bilinear fixes the wobble by blurring everything. This keeps the block flat and softens only the
// seam, so there is nothing to wobble and nothing much to blur.
//
// The one filter here that leans on the sampler rather than gathering texels itself -- the whole
// method is a coordinate warp feeding a single bilinear tap.
const char* kSharpBilinear = R"(
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint ox = id.x;
    uint oy = id.y;
    if (ox >= (uint) _DstWidth || oy >= (uint) _DstHeight)
        return;

    float2 srcSize = float2((float) _SrcWidth, (float) _SrcHeight);
    float2 texel = (float2((float) ox + 0.5f, (float) oy + 0.5f)) * SourceScale();

    float2 scale = max(floor(float2((float) _DstWidth, (float) _DstHeight) / srcSize), 1.0f);

    float2 texelFloored = floor(texel);
    float2 s = texel - texelFloored;

    // Everything except the outermost 1/scale of the block is flat; only that margin ramps.
    float2 regionRange = 0.5f - 0.5f / scale;
    float2 centreDist = s - 0.5f;
    float2 warped = (centreDist - clamp(centreDist, -regionRange, regionRange)) * scale + 0.5f;

    float3 outRgb = InputTexture.SampleLevel(LinearClampSampler, (texelFloored + warped) / srcSize, 0.0f).rgb;
    OutputTexture[uint2(ox, oy)] = float4(outRgb, 1.0f);
}
)";

// Integer scale. Nearest neighbour at the largest whole multiple that fits inside the destination,
// the remainder left as a black border.
//
// The point is that EVERY source pixel becomes a block of exactly the same size. A 3.4x enlargement
// drawn at 3.4x gives two rows out of five an extra pixel of height, which on pixel art reads as the
// image being subtly wrong everywhere; drawn at 3x and centred it is exactly right, just smaller.
// Anyone who asks for this wants the border.
const char* kIntegerScale = R"(
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    int ox = (int) id.x;
    int oy = (int) id.y;
    if (ox >= _DstWidth || oy >= _DstHeight)
        return;

    int factor = max(min(_DstWidth / max(_SrcWidth, 1), _DstHeight / max(_SrcHeight, 1)), 1);

    int2 drawn = int2(_SrcWidth * factor, _SrcHeight * factor);
    int2 origin = (int2(_DstWidth, _DstHeight) - drawn) / 2;
    int2 p = int2(ox, oy) - origin;

    if (p.x < 0 || p.y < 0 || p.x >= drawn.x || p.y >= drawn.y)
    {
        OutputTexture[uint2(ox, oy)] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    OutputTexture[uint2(ox, oy)] = float4(FetchTexel(p.x / factor, p.y / factor), 1.0f);
}
)";

// Nearest neighbour. No filtering of any kind, at whatever factor the destination asks for.
//
// Kept separate from Integer scale because they answer different questions: this one fills the
// destination and accepts uneven blocks, that one keeps the blocks even and accepts a border.
const char* kNearest = R"(
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint ox = id.x;
    uint oy = id.y;
    if (ox >= (uint) _DstWidth || oy >= (uint) _DstHeight)
        return;

    int2 sp = (int2) floor((float2((float) ox + 0.5f, (float) oy + 0.5f)) * SourceScale());
    OutputTexture[uint2(ox, oy)] = float4(FetchTexel(sp.x, sp.y), 1.0f);
}
)";

std::string Assemble(const char* body, bool withSigmoid)
{
    std::string source = kPreamble;
    if (withSigmoid)
        source += kSigmoid;
    source += body;
    return source;
}

} // namespace

namespace
{

// Every tuning control is a plain 0..1, so they all come through here rather than each carrying its
// own clamp. The ini reader clamps too; this is for a value set live from the menu.
float Unit(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

} // namespace

UpsamplerTuning UpsamplerTuningFor(bool neuralRendering)
{
    auto& cfg = *Config::Instance();

    if (neuralRendering)
        return { Unit(cfg.DlssNrScalingSharpness.value_or_default()),
                 Unit(cfg.DlssNrScalingAntiRinging.value_or_default()),
                 Unit(cfg.DlssNrScalingSigmoid.value_or_default()), Unit(cfg.DlssNrScalingDither.value_or_default()) };

    return { Unit(cfg.OutputScalingSharpness.value_or_default()), Unit(cfg.OutputScalingAntiRinging.value_or_default()),
             Unit(cfg.OutputScalingSigmoid.value_or_default()), Unit(cfg.OutputScalingDither.value_or_default()) };
}

const char* UpsamplerShaderSource(Upsampler which)
{
    // Function-local statics: assembled once, on first use, in a defined order. Doing this at
    // namespace scope would make the result depend on the order two inline variables in different
    // translation units happen to be initialised in, which is exactly the class of bug that only
    // shows up in a release build on someone else's machine.
    static const std::string ewaLanczos = Assemble(kEwaLanczos, true);
    static const std::string xbr = Assemble(kXbr, false);
    static const std::string sharpBilinear = Assemble(kSharpBilinear, false);
    static const std::string integerScale = Assemble(kIntegerScale, false);
    static const std::string nearest = Assemble(kNearest, false);

    switch (which)
    {
    case Upsampler::EwaLanczos:
        return ewaLanczos.c_str();
    case Upsampler::XBR:
        return xbr.c_str();
    case Upsampler::SharpBilinear:
        return sharpBilinear.c_str();
    case Upsampler::IntegerScale:
        return integerScale.c_str();
    case Upsampler::Nearest:
        return nearest.c_str();
    default:
        // Upsampler::Bicubic is not here: it is the existing upsampleCode in OS_Common.h, unchanged.
        return nullptr;
    }
}

const char* UpsamplerName(Upsampler which)
{
    switch (which)
    {
    case Upsampler::EwaLanczos:
        return "EwaLanczos";
    case Upsampler::XBR:
        return "xBR-lv2";
    case Upsampler::SharpBilinear:
        return "SharpBilinear";
    case Upsampler::IntegerScale:
        return "IntegerScale";
    case Upsampler::Nearest:
        return "Nearest";
    default:
        return "BicubicUp";
    }
}

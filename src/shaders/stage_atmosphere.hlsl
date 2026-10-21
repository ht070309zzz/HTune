// MusicStage GPU stage: full-screen atmosphere (low resolution) and composite.
#include "stage_common.hlsli"
#include "stage_detail.hlsli"

struct FullscreenOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

FullscreenOut FullscreenVS(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    FullscreenOut output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

// Cover color with a palette fallback. Missing current/previous sources fade
// through the palette instead of flashing black.
float3 SourceColor(float2 uv, float mip, float3 fallback)
{
    float3 current = fallback;
    float3 previous = fallback;
    if (gTime.z > 0.5) current = gCover.SampleLevel(gMirrorSampler, uv, mip).rgb;
    if (gTime.w > 0.5) previous = gPreviousCover.SampleLevel(gMirrorSampler, uv, mip).rgb;
    return lerp(previous, current, saturate(gTime.y));
}

float3 PaletteField(float2 p, float t, float2 warp)
{
    float3 sum = 0.0;
    float weight = 0.0;
    [unroll] for (int i = 0; i < 5; ++i)
    {
        float fi = (float)i;
        float2 center = float2(sin(t * (0.050 + 0.013 * fi) + fi * 1.7), cos(t * (0.043 + 0.011 * fi) + fi * 2.3)) * float2(0.58, 0.44);
        float d = length(p + warp * 0.45 - center);
        float w = exp(-d * d * (2.0 + fi * 0.35)) * (1.15 - fi * 0.12);
        sum += gPalette[i].rgb * w;
        weight += w;
    }
    return sum / max(weight, 1e-3);
}

float3 FlowAtmosphere(float2 uv, float t)
{
    float aspect = gResolution.x / max(gResolution.y, 1.0);
    float2 p = (uv - 0.5) * float2(aspect, 1.0);
    float slow = t * 0.05;
    float2 q = float2(Fbm(float3(p * 1.1, slow)), Fbm(float3(p * 1.1 + 4.3, slow * 0.9)));
    float2 r = float2(Fbm(float3(p * 1.6 + q * 1.8 + 1.7, slow * 1.2)), Fbm(float3(p * 1.6 + q * 1.8 + 8.3, slow * 1.1)));
    float3 palette = PaletteField(p, t, r);
    float2 c1 = Rotate2(p * 0.58 + r * 0.34, t * 0.021) + 0.5;
    float2 c2 = Rotate2(p * 0.92 - q * 0.30, -t * 0.017 + 1.9) + 0.5;
    float3 layer1 = SourceColor(c1, 4.6, palette);
    float3 layer2 = SourceColor(c2, 5.4, palette);
    float3 color = lerp(layer1, layer2, smoothstep(-0.25, 0.35, r.x + q.y * 0.5));
    float energy = gAudio.x * gParams.x;
    color = Saturate3(color, 1.22);
    // Normalize first, then modulate, so audio and flow stay visible.
    color = gStage.y > 0.5 ? CompressLuma(color, 0.55, 0.82) : CompressLuma(color, 0.035, 0.17);
    float modulation = 0.88 + r.y * 0.30 + energy * 0.24 + gAudio.w * 0.10 * gParams.x;
    color *= gStage.y > 0.5 ? lerp(1.0, modulation, 0.35) : modulation;
    return lerp(color, gBase.rgb, 0.12);
}

float3 CinemaAtmosphere(float2 uv, float t)
{
    float aspect = gResolution.x / max(gResolution.y, 1.0);
    float2 p = (uv - 0.5) * float2(aspect, 1.0);
    float2 pixel = uv * gResolution.xy;
    float2 f = (pixel - gField.xy) / max(gField.z, 1.0);
    float d = length(f);
    float2 warp = float2(Fbm(float3(f * 1.4, t * 0.04)), Fbm(float3(f * 1.4 + 5.1, t * 0.035)));
    float3 palette = lerp(gPalette[0].rgb, gPalette[1].rgb, saturate(0.5 + warp.x));
    float3 nebula = SourceColor(Rotate2(f * 0.45 + warp * 0.25, t * 0.012) + 0.5, 5.2, palette);
    nebula = CompressLuma(Saturate3(nebula, 1.25), 0.08, 0.30);
    float halo = exp(-d * d * 1.25) * (0.50 + 0.35 * gAudio.x * gParams.x + 0.15 * gAudio.w);
    float3 base = gBase.rgb;
    float3 color = base * 0.9 + nebula * halo * 0.55 * gField.w;
    float smoke = Fbm(float3(p * 1.3 + float2(t * 0.018, -t * 0.006), t * 0.025));
    color += lerp(gPalette[2].rgb, gAccent.rgb, 0.35) * smoothstep(0.02, 0.32, smoke) * 0.05;
    // A soft key light falling diagonally from the upper right.
    float2 beamOrigin = float2(aspect * 0.42, -0.62);
    float2 beamDir = normalize(float2(-0.55, 1.0));
    float2 rel = p - beamOrigin;
    float along = dot(rel, beamDir);
    float across = abs(rel.x * beamDir.y - rel.y * beamDir.x);
    float beam = exp(-across * across * 9.0) * smoothstep(0.0, 0.5, along) * exp(-along * 0.9);
    color += lerp(gAccent.rgb, float3(1.0, 1.0, 1.0), 0.4) * beam * (0.035 + 0.03 * gAudio.w);
    return gStage.y > 0.5 ? lerp(color, base, 0.6) : color;
}

float3 PaperAtmosphere(float2 uv, float t)
{
    float aspect = gResolution.x / max(gResolution.y, 1.0);
    float2 p = (uv - 0.5) * float2(aspect, 1.0);
    float3 base = gBase.rgb;
    float mottle = Fbm(float3(p * 2.6, 3.1)) * 0.06 + Fbm(float3(p * 9.0, 7.7)) * 0.02;
    float3 color = base * (1.0 + mottle);
    float wash = smoothstep(0.0, 0.42, Fbm(float3(p * 0.75 + float2(t * 0.012, -t * 0.009), t * 0.018)) + 0.12);
    float3 washPalette = lerp(gPalette[0].rgb, gPalette[1].rgb, saturate(0.5 + Fbm(float3(p * 1.2, t * 0.02))));
    float3 washColor = Saturate3(SourceColor(Rotate2(p * 0.5, t * 0.01) + 0.5, 5.6, washPalette), 1.3);
    if (gStage.y > 0.5)
    {
        float3 tinted = color * lerp(float3(1.0, 1.0, 1.0), saturate(washColor * 1.35 + 0.25), 0.55);
        color = lerp(color, tinted, wash * (0.32 + 0.12 * gAudio.x * gParams.x));
    }
    else
    {
        color += ToneToBand(washColor, 0.05, 0.16) * wash * (0.45 + 0.2 * gAudio.x * gParams.x);
    }
    return color;
}

float4 AtmospherePS(FullscreenOut input) : SV_Target
{
    float2 uv = input.position.xy * gShift.zw;
    float t = gTime.x;
    float3 color;
    if (gStage.x < 0.5) color = FlowAtmosphere(uv, t);
    else if (gStage.x < 1.5) color = CinemaAtmosphere(uv, t);
    else color = PaperAtmosphere(uv, t);
    float moving=gDust.w>=4?0:1;
    float4 detail=ThemeDetail(uv,gResolution.x/gResolution.y,t*moving,gAudio.x*gParams.x*moving,gAudio.z*gParams.x*moving,gAudio.w*gParams.x*moving,gAccent.rgb,gBase.rgb,(int)gStage.x,fmod(gDust.w,4));
    color=color*(1-detail.a)+detail.rgb;
    return float4(max(color, 0.0), 1.0);
}

float4 CompositePS(FullscreenOut input) : SV_Target
{
    float2 uv = input.uv;
    float3 color = gAtmosphere.SampleLevel(gClampSampler, uv, 0.0).rgb;
    float style = gStage.x;
    float2 v = (uv - 0.5) * 2.0;
    float radius = length(v * float2(0.82, 1.0));
    float vignetteAmount = style < 0.5 ? 0.34 : (style < 1.5 ? 0.62 : 0.16);
    color *= 1.0 - smoothstep(0.55, 1.5, radius) * vignetteAmount * gStage.z;
    if (style > 0.5 && style < 1.5)
    {
        // Cinematic top/bottom falloff rather than hard letterbox bars.
        float bars = smoothstep(0.0, 0.08, uv.y) * smoothstep(1.0, 0.92, uv.y);
        color *= lerp(0.62, 1.0, bars);
    }
    color = lerp(color, gBase.rgb, saturate(gParams.w) * 0.85);
    float2 pixel = input.position.xy;
    float grainAmount = style < 0.5 ? 0.016 : (style < 1.5 ? 0.034 : 0.026);
    float grain = Hash12(pixel + frac(gTime.x * 7.31) * float2(97.1, 41.3)) - 0.5;
    color += grain * grainAmount * gStage.z;
    color += (Hash12(pixel * 1.618 + 3.7) - 0.5) / 255.0;
    return float4(saturate(color), 1.0);
}

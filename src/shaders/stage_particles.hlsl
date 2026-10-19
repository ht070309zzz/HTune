// MusicStage GPU stage: cover particle field and depth dust (instanced sprites).
#include "stage_common.hlsli"

struct ParticleOut
{
    float4 position : SV_Position;
    float4 color : COLOR0;   // premultiplied color, alpha for non-additive blending
    float2 corner : TEXCOORD0;
};

float2 SpriteCorner(uint vertexId)
{
    return float2((vertexId & 1) ? 1.0 : -1.0, (vertexId & 2) ? -1.0 : 1.0);
}

ParticleOut Culled()
{
    ParticleOut output;
    output.position = float4(4.0, 4.0, 0.5, 1.0);
    output.color = 0.0;
    output.corner = 0.0;
    return output;
}

float3 CoverSample(float2 uv, float mip)
{
    float3 current = 0.0;
    float3 previous = 0.0;
    if (gTime.z > 0.5) current = gCover.SampleLevel(gClampSampler, uv, mip).rgb;
    if (gTime.w > 0.5) previous = gPreviousCover.SampleLevel(gClampSampler, uv, mip).rgb;
    return lerp(previous, current, saturate(gTime.y));
}

ParticleOut FieldVS(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    float n = gGrid.x;
    float index = (float)instanceId;
    // Half-cell bias keeps exact multiples of n on the correct row despite
    // the rounding of 1/n.
    float gy = floor((index + 0.5) * gGrid.y);
    float gx = index - gy * n;
    if (gy >= n || gx < 0.0 || gx >= n) return Culled();
    float2 uv = (float2(gx, gy) + 0.5) * gGrid.y;
    float rnd = Hash12(float2(gx, gy) * 1.731 + 4.1);
    float rnd2 = Hash12(float2(gy, gx) * 2.117 + 9.3);
    float3 color = CoverSample(uv, 0.75);
    float lum = dot(color, kLuma);
    float t = gTime.x;
    float motion = gParams.z;
    float k = gParams.x * motion;
    float2 local = (uv - 0.5) * float2(2.0, -2.0);
    float d = length(local);
    float3 pos = float3(local, 0.0);
    pos.z += (lum - 0.45) * 0.16;
    pos.z += (sin(local.x * 2.6 + t * 0.55) + cos(local.y * 2.2 - t * 0.47)) * 0.025 * motion;
    pos.z += (ValueNoise(float3(local * 1.15, t * 0.32)) - 0.5) * gAudio.x * 0.68 * k;
    pos.z += sin(d * 10.0 - t * 3.4) * gAudio.y * 0.085 * k * exp(-d * 0.9);
    float2 shimmer = float2(Hash12(float2(rnd, floor(t * 24.0))), Hash12(float2(rnd2, floor(t * 24.0) + 3.1))) - 0.5;
    pos.xy += shimmer * gAudio.z * 0.018 * k;
    pos.xy *= 1.0 + gAudio.w * 0.04 * k;
    pos.z += gAudio.w * 0.10 * k * (rnd - 0.3);
    // Boundary particles slowly detach into a mist so the field never reads as a hard square.
    float edge = smoothstep(0.78, 1.0, max(abs(local.x), abs(local.y)));
    float2 outward = local / max(d, 1e-3);
    float drift = frac(rnd + t * 0.03 * (0.4 + rnd2) * (0.35 + 0.65 * motion));
    pos.xy += outward * edge * drift * 0.45;
    pos.z += edge * (rnd2 - 0.5) * 0.5;
    float alpha = 1.0 - edge * drift;

    float3 view = float3(dot(gRotation0.xyz, pos), dot(gRotation1.xyz, pos), dot(gRotation2.xyz, pos));
    float w = gCamera.x - view.z;
    if (w < 0.25) return Culled();
    float4 clip = float4(view.x * gCamera.y, view.y * gCamera.z, 0.5 * w, w);
    clip.xy += gShift.xy * w;
    float size = gGrid.z * (0.70 + lum * 0.55) * (1.0 + gAudio.w * 0.35 * k) * (gCamera.w / w);
    float2 corner = SpriteCorner(vertexId);
    clip.xy += corner * size * gResolution.zw * w;

    ParticleOut output;
    output.position = clip;
    float brightness = gStage.w * (0.80 + lum * 0.40 + gAudio.w * 0.25 * k);
    float opacity = alpha * gField.w;
    output.color = float4(color * brightness * opacity, opacity);
    output.corner = corner;
    return output;
}

ParticleOut DustVS(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    float i = (float)instanceId;
    if (i >= gDust.z) return Culled();
    float3 seed = float3(Hash12(float2(i, 1.3)), Hash12(float2(i, 7.9)), Hash12(float2(i, 13.7)));
    float t = gTime.x;
    float travel = frac(seed.z + t * (0.010 + seed.x * 0.012) * (1.0 + gAudio.x * 0.6 * gParams.x));
    float3 pos;
    pos.x = (seed.x * 2.0 - 1.0) * 3.2 + sin(t * 0.07 + i) * 0.05;
    pos.y = (seed.y * 2.0 - 1.0) * 2.0 + cos(t * 0.05 + i * 1.3) * 0.05;
    pos.z = lerp(-3.0, 1.7, travel);
    float w = 3.4 - pos.z;
    if (w < 0.3) return Culled();
    float aspectInverse = gResolution.y / max(gResolution.x, 1.0);
    float4 clip = float4(pos.x * 1.55 * aspectInverse, pos.y * 1.55, 0.5 * w, w);
    float fade = smoothstep(0.0, 0.18, travel) * smoothstep(1.0, 0.82, travel);
    float twinkle = 0.55 + 0.45 * sin(t * (1.3 + seed.y * 2.1) + i);
    float size = gDust.y * (0.6 + seed.y * 1.2) * (3.4 / w) * (1.0 + gAudio.z * 0.4 * gParams.x);
    float2 corner = SpriteCorner(vertexId);
    clip.xy += corner * size * gResolution.zw * w;
    ParticleOut output;
    output.position = clip;
    float3 tint = lerp(float3(1.0, 0.97, 0.92), gAccent.rgb, seed.x * 0.5);
    float opacity = gDust.x * fade * twinkle * (0.55 + 0.45 * gAudio.z * gParams.x);
    output.color = float4(tint * opacity, opacity);
    output.corner = corner;
    return output;
}

float4 ParticlePS(ParticleOut input) : SV_Target
{
    float r2 = dot(input.corner, input.corner);
    float a = exp(-r2 * 3.0) * saturate((1.0 - r2) * 4.0);
    return input.color * a;
}

// MusicStage stage compositor: the Backstage camera and the blurred stage copy
// used by glass panels. Original implementation for this project; compiled
// offline by fxc (SM 4.0).

cbuffer CompositeConstants : register(b0)
{
    float4 gTarget;    // x,y = target pixels, z,w = 1/x, 1/y
    float4 gInverse0;  // rows of the inverse homography: target pixel -> card uv
    float4 gInverse1;
    float4 gInverse2;
    float4 gCard;      // x,y = card size (stage pixels), z = corner radius (stage pixels), w = presence 0..1
    float4 gScene;     // x = backdrop brightness, y = composite mix (0 plain stage), z = reflection, w = shadow
    float4 gLight;     // rgb = spotlight color, a = intensity
    float4 gSpot;      // x,y = spotlight apex (px), z = cone spread per px of drop, w = cone length (px)
    float4 gPool;      // x,y = floor light pool center (px), z,w = its radii (px)
    float4 gRim;       // rgb = rim light color, a = strength
    float4 gStep;      // x,y = blur direction step (uv), z,w = downsample tap offset (uv)
};

Texture2D gStage : register(t0);
Texture2D gSource : register(t1);  // pass input; in PresentPS the glass blur (1/8)
Texture2D gAmbient : register(t2); // PresentPS: the room light (1/32, very soft)
SamplerState gLinear : register(s0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
// Gaussian (sigma 5 texels, radius 14) folded into bilinear fetch pairs.
static const float kOffsets[8] = { 0.0, 1.485004, 3.465057, 5.445221, 7.425557, 9.406127, 11.386986, 13.368188 };
static const float kWeights[8] = { 0.080083, 0.152423, 0.125043, 0.087553, 0.052322, 0.026686, 0.011617, 0.004316 };

struct CompositeOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

CompositeOut CompositeVS(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    CompositeOut output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

float Hash12(float2 p)
{
    float3 q = frac(float3(p.xyx) * float3(0.1371, 0.1193, 0.1279));
    q += dot(q, q.yzx + 27.41);
    return frac((q.x + q.y) * q.z);
}

// Box downsample with four bilinear taps: an offset of one source texel
// averages a 4x4 block, half a texel averages a 2x2 block.
float4 DownsamplePS(CompositeOut input) : SV_Target
{
    float2 uv = input.uv;
    float2 offset = gStep.zw;
    float3 color = gSource.SampleLevel(gLinear, uv + offset * float2(-1.0, -1.0), 0).rgb;
    color += gSource.SampleLevel(gLinear, uv + offset * float2(1.0, -1.0), 0).rgb;
    color += gSource.SampleLevel(gLinear, uv + offset * float2(-1.0, 1.0), 0).rgb;
    color += gSource.SampleLevel(gLinear, uv + offset * float2(1.0, 1.0), 0).rgb;
    return float4(color * 0.25, 1.0);
}

// One separable Gaussian direction (gStep.xy is one texel along it).
float4 BlurPS(CompositeOut input) : SV_Target
{
    float2 uv = input.uv;
    float3 color = gSource.SampleLevel(gLinear, uv, 0).rgb * kWeights[0];
    [unroll] for (int i = 1; i < 8; ++i)
    {
        float2 offset = gStep.xy * kOffsets[i];
        color += gSource.SampleLevel(gLinear, uv + offset, 0).rgb * kWeights[i];
        color += gSource.SampleLevel(gLinear, uv - offset, 0).rgb * kWeights[i];
    }
    return float4(color, 1.0);
}

float RoundedRect(float2 p, float2 halfSize, float radius)
{
    float2 q = abs(p) - halfSize + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// Four rotated-grid taps across the pixel footprint on the card, so a
// minified stage keeps small text steady instead of shimmering. At or below
// one stage texel per pixel the taps collapse into a single sample.
float3 SampleCard(float2 uv, float2 dx, float2 dy)
{
    float footprint = max(length(dx * gCard.xy), length(dy * gCard.xy));
    float spread = saturate(footprint - 1.0);
    dx *= spread;
    dy *= spread;
    float3 color = gStage.SampleLevel(gLinear, uv + dx * -0.125 + dy * -0.375, 0).rgb;
    color += gStage.SampleLevel(gLinear, uv + dx * 0.375 + dy * -0.125, 0).rgb;
    color += gStage.SampleLevel(gLinear, uv + dx * 0.125 + dy * 0.375, 0).rgb;
    color += gStage.SampleLevel(gLinear, uv + dx * -0.375 + dy * 0.125, 0).rgb;
    return color * 0.25;
}

// The live stage as a tilted card in a dark room lit by the stage's own
// colors, with a soft contact shadow, a floor reflection and a spotlight.
float4 PresentPS(CompositeOut input) : SV_Target
{
    float2 pixel = input.position.xy;
    float2 screenUv = pixel * gTarget.zw;
    float presence = gCard.w;
    float3 plain = gStage.SampleLevel(gLinear, screenUv, 0).rgb;
    // Reduced motion dips through the room: the stage dims into it first,
    // then the card fades in (closing runs the same path backwards).
    float toRoom = saturate(gScene.y * 2.0);
    float cardIn = saturate(gScene.y * 2.0 - 1.0);

    float3 haze = gAmbient.SampleLevel(gLinear, screenUv, 0).rgb;
    float3 backdrop = lerp(haze, dot(haze, kLuma).xxx, 0.15 * presence) * gScene.x;
    float2 centered = screenUv * 2.0 - 1.0;
    backdrop *= 1.0 - 0.50 * presence * smoothstep(0.30, 1.50, length(centered * float2(0.85, 1.0)));

    // Perspective-correct card coordinates; the third row is 1/depth.
    float3 h = float3(pixel, 1.0);
    float3 q = float3(dot(gInverse0.xyz, h), dot(gInverse1.xyz, h), dot(gInverse2.xyz, h));
    bool front = q.z > 1e-9;
    float2 uv = front ? q.xy / q.z : float2(-4.0, -4.0);
    float2 dx = ddx(uv);
    float2 dy = ddy(uv);
    float2 size = gCard.xy;
    float sd = RoundedRect(uv * size - size * 0.5, size * 0.5, gCard.z);
    float aa = max(fwidth(sd), 1e-3);
    float card = front ? saturate(0.5 - sd / aa) : 0.0;

    // Contact shadow: darkens the room around the card, heavier below it.
    float outside = max(sd, 0.0) / max(size.y, 1.0);
    float bottomBias = saturate(uv.y * 1.2 - 0.2);
    float shadow = front ? exp(-outside * (11.0 - 4.0 * bottomBias)) * (1.0 - card) : 0.0;
    float3 color = backdrop * (1.0 - gScene.w * shadow * (0.55 + 0.30 * bottomBias));

    // Spotlight: a soft cone from above and a pool of light on the floor.
    float2 rel = pixel - gSpot.xy;
    float reach = max(rel.y, 0.0);
    float across = abs(rel.x) / max(reach * gSpot.z + 24.0, 1.0);
    float cone = exp(-across * across * 2.4) * smoothstep(0.0, 0.2 * gSpot.w, reach) * exp(-reach / max(gSpot.w, 1.0));
    float2 poolRel = (pixel - gPool.xy) / max(gPool.zw, 1.0);
    float pool = exp(-dot(poolRel, poolRel));
    color += gLight.rgb * gLight.a * presence * (cone * 0.20 + pool * 0.16);

    // Floor reflection: the card plane continues below its bottom edge, so
    // the mirrored stage fades and softens with distance from that edge.
    float below = uv.y - 1.0;
    float2 mirrorUv = float2(uv.x, 1.0 - below);
    float mirrorSd = RoundedRect(mirrorUv * size - size * 0.5, size * 0.5, gCard.z);
    float mirror = front && below > 0.0 ? saturate(0.5 - mirrorSd / aa) : 0.0;
    float fall = exp(-max(below, 0.0) * 6.5);
    float3 sharp = gStage.SampleLevel(gLinear, mirrorUv, 0).rgb;
    float3 soft = gSource.SampleLevel(gLinear, mirrorUv, 0).rgb;
    float3 reflection = lerp(sharp, soft, saturate(below * 5.0));
    color = lerp(color, reflection, mirror * fall * gScene.z * presence * cardIn);

    // The card: footprint-filtered stage, a slight falloff toward the far
    // edge and a thin rim light that is brightest along the top.
    float3 surface = SampleCard(uv, dx, dy);
    surface *= 1.0 - presence * 0.14 * (1.0 - saturate(uv.x));
    float rim = exp(-abs(sd + 0.8 * aa) / (0.9 * aa)) * lerp(0.35, 1.0, saturate(1.0 - uv.y * 1.4));
    surface += gRim.rgb * gRim.a * presence * rim;
    color = lerp(color, surface, card * cardIn);

    color = lerp(plain, color, toRoom);
    color += (Hash12(pixel + 0.37) - 0.5) / 255.0;
    return float4(saturate(color), 1.0);
}

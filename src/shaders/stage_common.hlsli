// MusicStage GPU stage: shared constants and procedural helpers.
// Original implementation for this project; compiled offline by fxc (SM 4.0).

cbuffer StageConstants : register(b0)
{
    float4 gResolution; // x,y = stage pixels, z,w = 2/x, 2/y (pixel -> NDC)
    float4 gTime;       // x = animation seconds, y = cover blend, z = current cover, w = previous cover
    float4 gAudio;      // x = low, y = mid, z = high, w = beat envelope
    float4 gParams;     // x = response, y = camera, z = motion 0..1, w = background shade
    float4 gPalette[5]; // sRGB atmosphere colors ordered by visual weight
    float4 gBase;       // rgb = theme background, a = its relative luminance
    float4 gAccent;     // rgb = accent, a = rms
    float4 gField;      // x,y = particle field center (px), z = field size (px), w = opacity
    float4 gStage;      // x = style, y = light background (0/1), z = finish strength, w = particle gain
    float4 gGrid;       // x = grid size N, y = 1/N, z = sprite half size (px), w = DPI scale
    float4 gDust;       // x = opacity, y = sprite half size (px), z = count, w = unused
    float4 gCamera;     // x = distance, y = NDC scale x, z = NDC scale y, w = reference distance
    float4 gShift;      // x,y = NDC field center, z = atmosphere texel x, w = atmosphere texel y
    float4 gRotation0;  // rows of the particle field rotation
    float4 gRotation1;
    float4 gRotation2;
};

Texture2D gCover : register(t0);
Texture2D gPreviousCover : register(t1);
Texture2D gAtmosphere : register(t2);
SamplerState gClampSampler : register(s0);
SamplerState gMirrorSampler : register(s1);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float Hash12(float2 p)
{
    float3 q = frac(float3(p.xyx) * float3(0.1371, 0.1193, 0.1279));
    q += dot(q, q.yzx + 27.41);
    return frac((q.x + q.y) * q.z);
}

float Hash13(float3 p)
{
    p = frac(p * float3(0.1297, 0.1073, 0.1189));
    p += dot(p, p.zyx + 23.17);
    return frac((p.x + p.y) * p.z);
}

// Smooth 3D value noise in [0, 1].
float ValueNoise(float3 p)
{
    float3 cell = floor(p);
    float3 f = frac(p);
    float3 u = f * f * (3.0 - 2.0 * f);
    float n000 = Hash13(cell);
    float n100 = Hash13(cell + float3(1, 0, 0));
    float n010 = Hash13(cell + float3(0, 1, 0));
    float n110 = Hash13(cell + float3(1, 1, 0));
    float n001 = Hash13(cell + float3(0, 0, 1));
    float n101 = Hash13(cell + float3(1, 0, 1));
    float n011 = Hash13(cell + float3(0, 1, 1));
    float n111 = Hash13(cell + float3(1, 1, 1));
    float x00 = lerp(n000, n100, u.x), x10 = lerp(n010, n110, u.x);
    float x01 = lerp(n001, n101, u.x), x11 = lerp(n011, n111, u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

// Three octave fractal value noise, centered on zero (about -0.5..0.5).
float Fbm(float3 p)
{
    float value = 0.0;
    float amplitude = 0.55;
    [unroll] for (int octave = 0; octave < 3; ++octave)
    {
        value += (ValueNoise(p) - 0.5) * amplitude;
        p = p * 2.03 + float3(17.1, 9.7, 3.3);
        amplitude *= 0.5;
    }
    return value;
}

float2 Rotate2(float2 p, float angle)
{
    float s = sin(angle), c = cos(angle);
    return float2(c * p.x - s * p.y, s * p.x + c * p.y);
}

// Keeps hue while placing the luminance in a band that keeps lyrics legible.
float3 ToneToBand(float3 color, float low, float high)
{
    float luminance = max(dot(color, kLuma), 1e-3);
    float target = clamp(luminance, low, high);
    return color * (target / luminance);
}

// Monotonic compression into [low, high]: bright and dark regions keep their
// order (unlike a hard clamp), so the cover's light structure survives.
float3 CompressLuma(float3 color, float low, float high)
{
    float luminance = max(dot(color, kLuma), 1e-4);
    float t = saturate(luminance / (luminance + 0.25) * 1.25);
    float target = low + (high - low) * t;
    return color * (target / luminance);
}

float3 Saturate3(float3 color, float amount)
{
    float luminance = dot(color, kLuma);
    return max(0.0, lerp(luminance.xxx, color, amount));
}

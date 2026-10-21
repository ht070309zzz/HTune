// SPDX-License-Identifier: Apache-2.0
// Copyright Lost Coast Labs, Inc. (https://paper.design).
// Native HLSL adaptation by HTune; Paper 43cd68db79fa0b1759f72ffc941b3238e2a3954c.
cbuffer PaperConstants:register(b0){float4 nativeUniforms[64];float4 u_colors[10];};

SamplerState linearClamp:register(s0);
#define u_time nativeUniforms[0].x
#define u_colorsCount nativeUniforms[1].x
#define u_distortion nativeUniforms[2].x
#define u_swirl nativeUniforms[3].x
#define u_grainMixer nativeUniforms[4].x
#define u_grainOverlay nativeUniforms[5].x
#define u_resolution nativeUniforms[6].xy

float4 samplePaper(Texture2D tex,float2 uv){return tex.SampleLevel(linearClamp,uv,0);}
float4 samplePaperLod(Texture2D tex,float2 uv,float lod){return tex.SampleLevel(linearClamp,uv,lod);}
struct PaperPixel{float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
float2 V2(float a){return a.xx;}float2 V2(float2 a){return a;}float2 V2(float a,float b){return float2(a,b);}
float3 V3(float a){return a.xxx;}float3 V3(float3 a){return a;}float3 V3(float a,float b,float c){return float3(a,b,c);}float3 V3(float2 a,float b){return float3(a,b);}float3 V3(float a,float2 b){return float3(a,b);}
float4 V4(float2 a,float b,float c){return float4(a,b,c);}float4 V4(float a,float2 b,float c){return float4(a,b,c);}float4 V4(float a,float b,float2 c){return float4(a,b,c);}
float4 V4(float a){return a.xxxx;}float4 V4(float4 a){return a;}float4 V4(float a,float b,float c,float d){return float4(a,b,c,d);}float4 V4(float3 a,float b){return float4(a,b);}float4 V4(float2 a,float2 b){return float4(a,b);}
float Gatan(float a){return atan(a);}float Gatan(float a,float b){return atan2(a,b);}
float Gpow(float a,float b){return pow(abs(a),b);}float2 Gpow(float2 a,float2 b){return pow(abs(a),b);}float2 Gpow(float2 a,float b){return pow(abs(a),b);}float3 Gpow(float3 a,float3 b){return pow(abs(a),b);}float3 Gpow(float3 a,float b){return pow(abs(a),b);}float4 Gpow(float4 a,float4 b){return pow(abs(a),b);}
float Gmod(float a,float b){return a-b*floor(a/b);}float2 Gmod(float2 a,float b){return a-b*floor(a/b);}float2 Gmod(float2 a,float2 b){return a-b*floor(a/b);}float3 Gmod(float3 a,float b){return a-b*floor(a/b);}float3 Gmod(float3 a,float3 b){return a-b*floor(a/b);}float4 Gmod(float4 a,float b){return a-b*floor(a/b);}






#define TWO_PI 6.28318530718
#define PI 3.14159265358979323846


float2 rotate(float2 uv, float th) {
  return mul(uv,float2x2(cos(th), sin(th), -sin(th), cos(th)));
}


  float hash21(float2 p) {
    p = frac(p * V2(0.3183099, 0.3678794)) + 0.1;
    p += dot(p, p + 19.19);
    return frac(p.x * p.y);
  }


float valueNoise(float2 st) {
  float2 i = floor(st);
  float2 f = frac(st);
  float a = hash21(i);
  float b = hash21(i + V2(1.0, 0.0));
  float c = hash21(i + V2(0.0, 1.0));
  float d = hash21(i + V2(1.0, 1.0));
  float2 u = f * f * (3.0 - 2.0 * f);
  float x1 = lerp(a, b, u.x);
  float x2 = lerp(c, d, u.x);
  return lerp(x1, x2, u.y);
}

float noise(float2 n, float2 seedOffset) {
  return valueNoise(n + seedOffset);
}

float2 getPosition(int i, float t) {
  float a = float(i) * .37;
  float b = .6 + frac(float(i) / 3.) * .9;
  float c = .8 + frac(float(i + 1) / 4.);

  float x = sin(t * b + a);
  float y = cos(t * c + a * 1.5);

  return .5 + .5 * V2(x, y);
}

float4 PaperPS1(PaperPixel input):SV_TARGET {
float4 fragColor=0;float2 v_objectUV=(input.uv-.5)*float2(u_resolution.x/u_resolution.y,1);float2 v_imageUV=(float2(input.uv.x,1-input.uv.y)-.5)*nativeUniforms[63].xy+.5;float4 gl_FragCoord=float4(input.position.x,u_resolution.y-input.position.y,0,1);
  float2 uv = v_objectUV;
  uv += .5;
  float2 grainUV = uv * 1000.;

  float mixerGrain = 0.;
  if (u_grainMixer > 0.) {
    mixerGrain = .4 * u_grainMixer * (noise(grainUV, V2(0.)) - .5);
  }

  const float firstFrameOffset = 41.5;
  float t = .5 * (u_time + firstFrameOffset);

  float radius = smoothstep(0., 1., length(uv - .5));
  float center = 1. - radius;
  for (float i0 = 1.; i0 <= 2.; i0++) {
    uv.x += u_distortion * center / i0 * sin(t + i0 * .4 * smoothstep(.0, 1., uv.y)) * cos(.2 * t + i0 * 2.4 * smoothstep(.0, 1., uv.y));
    uv.y += u_distortion * center / i0 * cos(t + i0 * 2. * smoothstep(.0, 1., uv.x));
  }

  float2 uvRotated = uv;
  uvRotated -= V2(.5);
  float angle = 3. * u_swirl * radius;
  uvRotated = rotate(uvRotated, -angle);
  uvRotated += V2(.5);

  float3 color = V3(0.);
  float opacity = 0.;
  float totalWeight = 0.;

  for (int i1 = 0; i1 < 10; i1++) {
    if (i1 >= int(u_colorsCount)) break;

    float2 pos = getPosition(i1, t) + mixerGrain;
    float3 colorFraction = u_colors[i1].rgb * u_colors[i1].a;
    float opacityFraction = u_colors[i1].a;

    float dist = length(uvRotated - pos);

    dist = Gpow(dist, 3.5);
    float weight = 1. / (dist + 1e-3);
    color += colorFraction * weight;
    opacity += opacityFraction * weight;
    totalWeight += weight;
  }

  color /= max(1e-4, totalWeight);
  opacity /= max(1e-4, totalWeight);

  if (u_grainOverlay > 0.) {
    float grainOverlay = valueNoise(rotate(grainUV, 1.) + V2(3.));
    grainOverlay = lerp(grainOverlay, valueNoise(rotate(grainUV, 2.) + V2(-1.)), .5);
    grainOverlay = Gpow(grainOverlay, 1.3);

    float grainOverlayV = grainOverlay * 2. - 1.;
    float3 grainOverlayColor = V3(step(0., grainOverlayV));
    float grainOverlayStrength = u_grainOverlay * abs(grainOverlayV);
    grainOverlayStrength = Gpow(grainOverlayStrength, .8);
    color = lerp(color, grainOverlayColor, .35 * grainOverlayStrength);

    opacity += .5 * grainOverlayStrength;
  }
  opacity = clamp(opacity, 0., 1.);

  fragColor = V4(color, opacity);
return fragColor;
}

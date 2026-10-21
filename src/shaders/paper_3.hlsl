// SPDX-License-Identifier: Apache-2.0
// Copyright Lost Coast Labs, Inc. (https://paper.design).
// Native HLSL adaptation by HTune; Paper 43cd68db79fa0b1759f72ffc941b3238e2a3954c.
cbuffer PaperConstants:register(b0){float4 nativeUniforms[64];float4 u_colors[10];};
Texture2D u_image:register(t0);
SamplerState linearClamp:register(s0);
#define u_colorFront nativeUniforms[0].xyzw
#define u_colorBack nativeUniforms[1].xyzw
#define u_radius nativeUniforms[2].x
#define u_contrast nativeUniforms[3].x
#define u_imageAspectRatio nativeUniforms[4].x
#define u_size nativeUniforms[5].x
#define u_grainMixer nativeUniforms[6].x
#define u_grainOverlay nativeUniforms[7].x
#define u_grainSize nativeUniforms[8].x
#define u_grid nativeUniforms[9].x
#define u_originalColors (nativeUniforms[10].x>.5)
#define u_inverted (nativeUniforms[11].x>.5)
#define u_type nativeUniforms[12].x
#define u_resolution nativeUniforms[13].xy

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

float lst(float edge0, float edge1, float x) {
  return clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
}

float sst(float edge0, float edge1, float x) {
  return smoothstep(edge0, edge1, x);
}

float getCircle(float2 uv, float r, float baseR) {
  r = lerp(.25 * baseR, 0., r);
  float d = length(uv - .5);
  float aa=2/max(1,u_resolution.y);
  return 1. - smoothstep(r - aa, r + aa, d);
}

float getCell(float2 uv) {
  float insideX = step(0.0, uv.x) * (1.0 - step(1.0, uv.x));
  float insideY = step(0.0, uv.y) * (1.0 - step(1.0, uv.y));
  return insideX * insideY;
}

float getCircleWithHole(float2 uv, float r, float baseR) {
  float cell = getCell(uv);

  r = lerp(.75 * baseR, 0., r);
  float rMod = Gmod(r, .5);

  float d = length(uv - .5);
  float aa=2/max(1,u_resolution.y);
  float circle = 1. - smoothstep(rMod - aa, rMod + aa, d);
  if (r < .5) {
    return circle;
  } else {
    return cell - circle;
  }
}

float getGooeyBall(float2 uv, float r, float baseR) {
  float d = length(uv - .5);
  float sizeRadius = .3;
  if (u_grid == 1.) {
    sizeRadius = .42;
  }
  sizeRadius = lerp(sizeRadius * baseR, 0., r);
  d = 1. - sst(0., sizeRadius, d);

  d = Gpow(d, 2. + baseR);
  return d;
}

float getSoftBall(float2 uv, float r, float baseR) {
  float d = length(uv - .5);
  float sizeRadius = clamp(baseR, 0., 1.);
  sizeRadius = lerp(.5 * sizeRadius, 0., r);
  d = 1. - lst(0., sizeRadius, d);
  float powRadius = 1. - lst(0., 2., baseR);
  d = Gpow(d, 4. + 3. * powRadius);
  return d;
}

float getUvFrame(float2 uv, float2 pad) {
  float aa = 0.0001;

  float left   = smoothstep(-pad.x, -pad.x + aa, uv.x);
  float right  = smoothstep(1.0 + pad.x, 1.0 + pad.x - aa, uv.x);
  float bottom = smoothstep(-pad.y, -pad.y + aa, uv.y);
  float top    = smoothstep(1.0 + pad.y, 1.0 + pad.y - aa, uv.y);

  return left * right * bottom * top;
}

float sigmoid(float x, float k) {
  return 1.0 / (1.0 + exp(-k * (x - 0.5)));
}

float getLumAtPx(float2 uv, float contrast) {
  float4 tex = samplePaper(u_image, uv);
  float3 color = V3(
  sigmoid(tex.r, contrast),
  sigmoid(tex.g, contrast),
  sigmoid(tex.b, contrast)
  );
  float lum = dot(V3(0.2126, 0.7152, 0.0722), color);
  lum = lerp(1., lum, tex.a);
  lum = u_inverted ? (1. - lum) : lum;
  return lum;
}

float getLumBall(float2 p, float2 pad, float2 inCellOffset, float contrast, float baseR, float stepSize, out float4 ballColor) {
  p += inCellOffset;
  float2 uv_i = floor(p);
  float2 uv_f = frac(p);
  float2 samplingUV = (uv_i + .5 - inCellOffset) * pad + V2(.5);
  float outOfFrame = getUvFrame(samplingUV, pad * stepSize);

  float lum = getLumAtPx(samplingUV, contrast);
  ballColor = samplePaper(u_image, samplingUV);
  ballColor.rgb *= ballColor.a;
  ballColor *= outOfFrame;

  float ball = 0.;
  if (u_type == 0.) {
    // classic
    ball = getCircle(uv_f, lum, baseR);
  } else if (u_type == 1.) {
    // gooey
    ball = getGooeyBall(uv_f, lum, baseR);
  } else if (u_type == 2.) {
    // holes
    ball = getCircleWithHole(uv_f, lum, baseR);
  } else if (u_type == 3.) {
    // soft
    ball = getSoftBall(uv_f, lum, baseR);
  }

  return ball * outOfFrame;
}


float4 PaperPS3(PaperPixel input):SV_TARGET {
float4 fragColor=0;float2 v_objectUV=(input.uv-.5)*float2(u_resolution.x/u_resolution.y,1);float2 v_imageUV=(float2(input.uv.x,1-input.uv.y)-.5)*nativeUniforms[63].xy+.5;float4 gl_FragCoord=float4(input.position.x,u_resolution.y-input.position.y,0,1);

  float stepMultiplier = 1.;
  if (u_type == 0.) {
    // classic
    stepMultiplier = 2.;
  } else if (u_type == 1. || u_type == 3.) {
    // gooey & soft
    stepMultiplier = 6.;
  }

  float cellsPerSide = lerp(300., 7., Gpow(u_size, .7));
  cellsPerSide /= stepMultiplier;
  float cellSizeY = 1. / cellsPerSide;
  float2 pad = cellSizeY * V2(1. / u_imageAspectRatio, 1.);
  if (u_type == 1. && u_grid == 1.) {
    // gooey diagonal grid works differently
    pad *= .7;
  }

  float2 uv = v_imageUV;
  uv -= V2(.5);
  uv /= pad;

  float contrast = lerp(0., 15., Gpow(u_contrast, 1.5));
  float baseRadius = u_radius;
  if (u_originalColors == true) {
    contrast = lerp(.1, 4., Gpow(u_contrast, 2.));
    baseRadius = 2. * Gpow(.5 * u_radius, .3);
  }

  float totalShape = 0.;
  float3 totalColor = V3(0.);
  float totalOpacity = 0.;

  float4 ballColor=0;
  float shape=0;
  float stepSize = 1. / stepMultiplier;
  for (float x0 = -0.5; x0 < 0.5; x0 += stepSize) {
    for (float y1 = -0.5; y1 < 0.5; y1 += stepSize) {
      float2 offset = V2(x0, y1);

      if (u_grid == 1.) {
        float rowIndex = floor((y1 + .5) / stepSize);
        float colIndex = floor((x0 + .5) / stepSize);
        if (stepSize == 1.) {
          rowIndex = floor(uv.y + y1 + 1.);
          if (u_type == 1.) {
            colIndex = floor(uv.x + x0 + 1.);
          }
        }
        if (u_type == 1.) {
          if (Gmod(rowIndex + colIndex, 2.) == 1.) {
            continue;
          }
        } else {
          if (Gmod(rowIndex, 2.) == 1.) {
            offset.x += .5 * stepSize;
          }
        }
      }

      shape = getLumBall(uv, pad, offset, contrast, baseRadius, stepSize, ballColor);
      totalColor   += ballColor.rgb * shape;
      totalShape   += shape;
      totalOpacity += shape;
    }
  }

  const float eps = 1e-4;

  totalColor /= max(totalShape, eps);
  totalOpacity /= max(totalShape, eps);

  float finalShape = 0.;
  if (u_type == 0.) {
    finalShape = min(1., totalShape);
  } else if (u_type == 1.) {
    float aa = fwidth(totalShape);
    float th = .5;
    finalShape = smoothstep(th - aa, th + aa, totalShape);
  } else if (u_type == 2.) {
    finalShape = min(1., totalShape);
  } else if (u_type == 3.) {
    finalShape = totalShape;
  }

  float2 grainSize = lerp(2000., 200., u_grainSize) * V2(1., 1. / u_imageAspectRatio);
  float2 grainUV = v_imageUV - .5;
  grainUV *= grainSize;
  grainUV += .5;
  if (u_grainMixer > 0.) {
    float grain = valueNoise(grainUV);
    grain = smoothstep(.55, .7 + .2 * u_grainMixer, grain);
    grain *= u_grainMixer;
    finalShape = lerp(finalShape, 0., grain);
  }

  float3 color = V3(0.);
  float opacity = 0.;

  if (u_originalColors == true) {
    color = totalColor * finalShape;
    opacity = totalOpacity * finalShape;

    float3 bgColor = u_colorBack.rgb * u_colorBack.a;
    color = color + bgColor * (1. - opacity);
    opacity = opacity + u_colorBack.a * (1. - opacity);
  } else {
    float3 fgColor = u_colorFront.rgb * u_colorFront.a;
    float fgOpacity = u_colorFront.a;
    float3 bgColor = u_colorBack.rgb * u_colorBack.a;
    float bgOpacity = u_colorBack.a;

    color = fgColor * finalShape;
    opacity = fgOpacity * finalShape;
    color += bgColor * (1. - opacity);
    opacity += bgOpacity * (1. - opacity);
  }

  if (u_grainOverlay > 0.) {
    float grainOverlay = valueNoise(rotate(grainUV, 1.) + V2(3.));
    grainOverlay = lerp(grainOverlay, valueNoise(rotate(grainUV, 2.) + V2(-1.)), .5);
    grainOverlay = Gpow(grainOverlay, 1.3);

    float grainOverlayV = grainOverlay * 2. - 1.;
    float3 grainOverlayColor = V3(step(0., grainOverlayV));
    float grainOverlayStrength = u_grainOverlay * abs(grainOverlayV);
    grainOverlayStrength = Gpow(grainOverlayStrength, .8);
    color = lerp(color, grainOverlayColor, .5 * grainOverlayStrength);

    opacity += .5 * grainOverlayStrength;
  }
  opacity = clamp(opacity, 0., 1.);

  fragColor = V4(color, opacity);
return fragColor;
}

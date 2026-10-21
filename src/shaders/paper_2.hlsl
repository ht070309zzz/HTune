// SPDX-License-Identifier: Apache-2.0
// Copyright Lost Coast Labs, Inc. (https://paper.design).
// Native HLSL adaptation by HTune; Paper 43cd68db79fa0b1759f72ffc941b3238e2a3954c.
cbuffer PaperConstants:register(b0){float4 nativeUniforms[64];float4 u_colors[10];};
Texture2D u_image:register(t0);
SamplerState linearClamp:register(s0);
#define u_resolution nativeUniforms[0].xy
#define u_pixelRatio nativeUniforms[1].x
#define u_originX nativeUniforms[2].x
#define u_originY nativeUniforms[3].x
#define u_fit nativeUniforms[4].x
#define u_scale nativeUniforms[5].x
#define u_rotation nativeUniforms[6].x
#define u_offsetX nativeUniforms[7].x
#define u_offsetY nativeUniforms[8].x
#define u_colorFront nativeUniforms[9].xyzw
#define u_colorBack nativeUniforms[10].xyzw
#define u_colorHighlight nativeUniforms[11].xyzw
#define u_imageAspectRatio nativeUniforms[12].x
#define u_type nativeUniforms[13].x
#define u_pxSize nativeUniforms[14].x
#define u_originalColors (nativeUniforms[15].x>.5)
#define u_inverted (nativeUniforms[16].x>.5)
#define u_colorSteps nativeUniforms[17].x

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









  float hash21(float2 p) {
    p = frac(p * V2(0.3183099, 0.3678794)) + 0.1;
    p += dot(p, p + 19.19);
    return frac(p.x * p.y);
  }


#define TWO_PI 6.28318530718
#define PI 3.14159265358979323846


float getUvFrame(float2 uv, float2 pad) {
  float aa = 0.0001;

  float left   = smoothstep(-pad.x, -pad.x + aa, uv.x);
  float right  = smoothstep(1.0 + pad.x, 1.0 + pad.x - aa, uv.x);
  float bottom = smoothstep(-pad.y, -pad.y + aa, uv.y);
  float top    = smoothstep(1.0 + pad.y, 1.0 + pad.y - aa, uv.y);

  return left * right * bottom * top;
}

float2 getImageUV(float2 uv) {
  float2 boxOrigin = V2(.5 - u_originX, u_originY - .5);
  float r = u_rotation * PI / 180.;
  float2x2 graphicRotation = float2x2(cos(r), sin(r), -sin(r), cos(r));
  float2 graphicOffset = V2(-u_offsetX, u_offsetY);

  float2 imageBoxSize=0;
  if (u_fit == 1.) { // contain
    imageBoxSize.x = min(u_resolution.x / u_imageAspectRatio, u_resolution.y) * u_imageAspectRatio;
  } else if (u_fit == 2.) { // cover
    imageBoxSize.x = max(u_resolution.x / u_imageAspectRatio, u_resolution.y) * u_imageAspectRatio;
  } else {
    imageBoxSize.x = min(10.0, 10.0 / u_imageAspectRatio * u_imageAspectRatio);
  }
  imageBoxSize.y = imageBoxSize.x / u_imageAspectRatio;
  float2 imageBoxScale = u_resolution.xy / imageBoxSize;

  float2 imageUV = uv;
  imageUV *= imageBoxScale;
  imageUV += boxOrigin * (imageBoxScale - 1.);
  imageUV += graphicOffset;
  imageUV /= u_scale;
  imageUV.x *= u_imageAspectRatio;
  imageUV = mul(imageUV,graphicRotation);
  imageUV.x /= u_imageAspectRatio;

  imageUV += .5;
  imageUV.y = 1. - imageUV.y;

  return imageUV;
}

static const int bayer2x2[4] = {0, 2, 3, 1};
static const int bayer4x4[16] = {
0, 8, 2, 10,
12, 4, 14, 6,
3, 11, 1, 9,
15, 7, 13, 5
};

static const int bayer8x8[64] = {
0, 32, 8, 40, 2, 34, 10, 42,
48, 16, 56, 24, 50, 18, 58, 26,
12, 44, 4, 36, 14, 46, 6, 38,
60, 28, 52, 20, 62, 30, 54, 22,
3, 35, 11, 43, 1, 33, 9, 41,
51, 19, 59, 27, 49, 17, 57, 25,
15, 47, 7, 39, 13, 45, 5, 37,
63, 31, 55, 23, 61, 29, 53, 21
};

float getBayerValue(float2 uv, int size) {
  int2 pos = int2(frac(uv / float(size)) * float(size));
  int index = pos.y * size + pos.x;

  if (size == 2) {
    return float(bayer2x2[index]) / 4.0;
  } else if (size == 4) {
    return float(bayer4x4[index]) / 16.0;
  } else if (size == 8) {
    return float(bayer8x8[index]) / 64.0;
  }
  return 0.0;
}


float4 PaperPS2(PaperPixel input):SV_TARGET {
float4 fragColor=0;float2 v_objectUV=(input.uv-.5)*float2(u_resolution.x/u_resolution.y,1);float2 v_imageUV=(float2(input.uv.x,1-input.uv.y)-.5)*nativeUniforms[63].xy+.5;float4 gl_FragCoord=float4(input.position.x,u_resolution.y-input.position.y,0,1);

  float pxSize = u_pxSize * u_pixelRatio;
  float2 pxSizeUV = gl_FragCoord.xy - .5 * u_resolution;
  pxSizeUV /= pxSize;
  float2 canvasPixelizedUV = (floor(pxSizeUV) + .5) * pxSize;
  float2 normalizedUV = canvasPixelizedUV / u_resolution;

  float2 imageUV = getImageUV(normalizedUV);
  float2 ditheringNoiseUV = canvasPixelizedUV;
  float4 image = samplePaper(u_image, imageUV);
  float frame = getUvFrame(imageUV, pxSize / u_resolution);

  int type = int(floor(u_type));
  float dithering = 0.0;

  float lum = dot(V3(.2126, .7152, .0722), image.rgb);
  lum = u_inverted ? (1. - lum) : lum;

  switch (type) {
    case 1: {
      dithering = step(hash21(ditheringNoiseUV), lum);
    } break;
    case 2:
    dithering = getBayerValue(pxSizeUV, 2);
    break;
    case 3:
    dithering = getBayerValue(pxSizeUV, 4);
    break;
    default :
    dithering = getBayerValue(pxSizeUV, 8);
    break;
  }

  float colorSteps = max(floor(u_colorSteps), 1.);
  float3 color = V3(0.0);
  float opacity = 1.;

  dithering -= .5;
  float brightness = clamp(lum + dithering / colorSteps, 0.0, 1.0);
  brightness = lerp(0.0, brightness, frame);
  brightness = lerp(0.0, brightness, image.a);
  float quantLum = floor(brightness * colorSteps + 0.5) / colorSteps;
  quantLum = lerp(0.0, quantLum, frame);

  if (u_originalColors == true) {
    float3 normColor = image.rgb / max(lum, 0.001);
    color = normColor * quantLum;

    float quantAlpha = floor(image.a * colorSteps + 0.5) / colorSteps;
    opacity = lerp(quantLum, 1., quantAlpha);
  } else {
    float3 fgColor = u_colorFront.rgb * u_colorFront.a;
    float fgOpacity = u_colorFront.a;
    float3 bgColor = u_colorBack.rgb * u_colorBack.a;
    float bgOpacity = u_colorBack.a;
    float3 hlColor = u_colorHighlight.rgb * u_colorHighlight.a;
    float hlOpacity = u_colorHighlight.a;

    fgColor = lerp(fgColor, hlColor, step(1.02 - .02 * u_colorSteps, brightness));
    fgOpacity = lerp(fgOpacity, hlOpacity, step(1.02 - .02 * u_colorSteps, brightness));

    color = fgColor * quantLum;
    opacity = fgOpacity * quantLum;
    color += bgColor * (1.0 - opacity);
    opacity += bgOpacity * (1.0 - opacity);
  }

  fragColor = V4(color, opacity);
return fragColor;
}

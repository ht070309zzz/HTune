// SPDX-License-Identifier: Apache-2.0
// Copyright Lost Coast Labs, Inc. (https://paper.design).
// Native HLSL adaptation by HTune; Paper 43cd68db79fa0b1759f72ffc941b3238e2a3954c.
cbuffer PaperConstants:register(b0){float4 nativeUniforms[64];float4 u_colors[10];};
Texture2D u_image:register(t0);
SamplerState linearClamp:register(s0);
#define u_resolution nativeUniforms[0].xy
#define u_pixelRatio nativeUniforms[1].x
#define u_colorBack nativeUniforms[2].xyzw
#define u_colorShadow nativeUniforms[3].xyzw
#define u_colorHighlight nativeUniforms[4].xyzw
#define u_imageAspectRatio nativeUniforms[5].x
#define u_size nativeUniforms[6].x
#define u_shadows nativeUniforms[7].x
#define u_angle nativeUniforms[8].x
#define u_stretch nativeUniforms[9].x
#define u_shape nativeUniforms[10].x
#define u_distortion nativeUniforms[11].x
#define u_highlights nativeUniforms[12].x
#define u_distortionShape nativeUniforms[13].x
#define u_shift nativeUniforms[14].x
#define u_blur nativeUniforms[15].x
#define u_edges nativeUniforms[16].x
#define u_marginLeft nativeUniforms[17].x
#define u_marginRight nativeUniforms[18].x
#define u_marginTop nativeUniforms[19].x
#define u_marginBottom nativeUniforms[20].x
#define u_grainMixer nativeUniforms[21].x
#define u_grainOverlay nativeUniforms[22].x

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

float getUvFrame(float2 uv, float softness) {
  float aax = 2. * fwidth(uv.x);
  float aay = 2. * fwidth(uv.y);
  float left   = smoothstep(0., aax + softness, uv.x);
  float right  = 1. - smoothstep(1. - softness - aax, 1., uv.x);
  float bottom = smoothstep(0., aay + softness, uv.y);
  float top    = 1. - smoothstep(1. - softness - aay, 1., uv.y);
  return left * right * bottom * top;
}

static const int MAX_RADIUS = 50;
float4 samplePremultiplied(float2 uv) {
  float4 c = samplePaper(u_image, uv);
  c.rgb *= c.a;
  return c;
}
float4 getBlur(float2 uv, float2 texelSize, float2 dir, float sigma) {
  float4 nativeResult=samplePaper(u_image,uv);if(sigma>.5){
  int radius = int(min(float(MAX_RADIUS), ceil(3.0 * sigma)));

  float twoSigma2 = 2.0 * sigma * sigma;
  float gaussianNorm = 1.0 / sqrt(TWO_PI * sigma * sigma);

  float4 sum = samplePremultiplied(uv) * gaussianNorm;
  float weightSum = gaussianNorm;

  for (int i0 = 1; i0 <= MAX_RADIUS; i0++) {
    if (i0 > radius) break;

    float x = float(i0);
    float w = exp(-(x * x) / twoSigma2) * gaussianNorm;

    float2 offset = dir * texelSize * x;
    float4 s1 = samplePremultiplied(uv + offset);
    float4 s2 = samplePremultiplied(uv - offset);

    sum += (s1 + s2) * w;
    weightSum += 2.0 * w;
  }

  float4 result = sum / weightSum;
  if (result.a > 0.) {
    result.rgb /= result.a;
  }

  nativeResult=result;}return nativeResult;
}

float2 rotateAspect(float2 p, float a, float aspect) {
  p.x *= aspect;
  p = rotate(p, a);
  p.x /= aspect;
  return p;
}

float smoothFract(float x) {
  float f = frac(x);
  float w = fwidth(x);

  float edge = abs(f - 0.5) - 0.5;
  float band = smoothstep(-w, w, edge);

  return lerp(f, 1.0 - f, band);
}

float4 PaperPS4(PaperPixel input):SV_TARGET {
float4 fragColor=0;float2 v_objectUV=(input.uv-.5)*float2(u_resolution.x/u_resolution.y,1);float2 v_imageUV=(float2(input.uv.x,1-input.uv.y)-.5)*nativeUniforms[63].xy+.5;float4 gl_FragCoord=float4(input.position.x,u_resolution.y-input.position.y,0,1);

  float patternRotation = -u_angle * PI / 180.;
  float patternSize = lerp(200., 5., u_size);

  float2 uv = v_imageUV;

  float2 uvMask = gl_FragCoord.xy / u_resolution.xy;
  float2 sw = V2(.005);
  float4 margins = V4(u_marginLeft, u_marginTop, u_marginRight, u_marginBottom);
  float mask =
  smoothstep(margins[0], margins[0] + sw.x, uvMask.x + sw.x) *
  smoothstep(margins[2], margins[2] + sw.x, 1.0 - uvMask.x + sw.x) *
  smoothstep(margins[1], margins[1] + sw.y, uvMask.y + sw.y) *
  smoothstep(margins[3], margins[3] + sw.y, 1.0 - uvMask.y + sw.y);
  float maskOuter =
  smoothstep(margins[0] - sw.x, margins[0], uvMask.x + sw.x) *
  smoothstep(margins[2] - sw.x, margins[2], 1.0 - uvMask.x + sw.x) *
  smoothstep(margins[1] - sw.y, margins[1], uvMask.y + sw.y) *
  smoothstep(margins[3] - sw.y, margins[3], 1.0 - uvMask.y + sw.y);
  float maskStroke = maskOuter - mask;
  float maskInner =
  smoothstep(margins[0] - 2. * sw.x, margins[0], uvMask.x) *
  smoothstep(margins[2] - 2. * sw.x, margins[2], 1.0 - uvMask.x) *
  smoothstep(margins[1] - 2. * sw.y, margins[1], uvMask.y) *
  smoothstep(margins[3] - 2. * sw.y, margins[3], 1.0 - uvMask.y);
  float maskStrokeInner = maskInner - mask;

  uv -= .5;
  uv *= patternSize;
  uv = rotateAspect(uv, patternRotation, u_imageAspectRatio);

  float curve = 0.;
  float patternY = uv.y / u_imageAspectRatio;
  if (u_shape > 4.5) {
    // pattern
    curve = .5 + .5 * sin(.5 * PI * uv.x) * cos(.5 * PI * patternY);
  } else if (u_shape > 3.5) {
    // zigzag
    curve = 10. * abs(frac(.1 * patternY) - .5);
  } else if (u_shape > 2.5) {
    // wave
    curve = 4. * sin(.23 * patternY);
  } else if (u_shape > 1.5) {
    // lines irregular
    curve = .5 + .5 * sin(.5 * uv.x) * sin(1.7 * uv.x);
  } else {
    // lines
  }

  float2 UvToFract = uv + curve;
  float2 fractOrigUV = frac(uv);
  float2 floorOrigUV = floor(uv);

  float x = smoothFract(UvToFract.x);
  float xNonSmooth = frac(UvToFract.x) + .0001;

  float highlightsWidth = 2. * max(.001, fwidth(UvToFract.x));
  highlightsWidth += 2. * maskStrokeInner;
  float highlights = smoothstep(0., highlightsWidth, xNonSmooth);
  highlights *= smoothstep(1., 1. - highlightsWidth, xNonSmooth);
  highlights = 1. - highlights;
  highlights *= u_highlights;
  highlights = clamp(highlights, 0., 1.);
  highlights *= mask;

  float shadows = Gpow(x, 1.3);
  float distortion = 0.;
  float fadeX = 1.;
  float frameFade = 0.;

  float aa = fwidth(xNonSmooth);
  aa = max(aa, fwidth(uv.x));
  aa = max(aa, fwidth(UvToFract.x));
  aa = max(aa, .0001);

  if (u_distortionShape == 1.) {
    distortion = -Gpow(1.5 * x, 3.);
    distortion += (.5 - u_shift);

    frameFade = Gpow(1.5 * x, 3.);
    aa = max(.2, aa);
    aa += lerp(.2, 0., u_size);
    fadeX = smoothstep(0., aa, xNonSmooth) * smoothstep(1., 1. - aa, xNonSmooth);
    distortion = lerp(.5, distortion, fadeX);
  } else if (u_distortionShape == 2.) {
    distortion = 2. * Gpow(x, 2.);
    distortion -= (.5 + u_shift);

    frameFade = Gpow(abs(x - .5), 4.);
    aa = max(.2, aa);
    aa += lerp(.2, 0., u_size);
    fadeX = smoothstep(0., aa, xNonSmooth) * smoothstep(1., 1. - aa, xNonSmooth);
    distortion = lerp(.5, distortion, fadeX);
    frameFade = lerp(1., frameFade, .5 * fadeX);
  } else if (u_distortionShape == 3.) {
    distortion = Gpow(2. * (xNonSmooth - .5), 6.);
    distortion -= .25;
    distortion -= u_shift;

    frameFade = 1. - 2. * Gpow(abs(x - .4), 2.);
    aa = .15;
    aa += lerp(.1, 0., u_size);
    fadeX = smoothstep(0., aa, xNonSmooth) * smoothstep(1., 1. - aa, xNonSmooth);
    frameFade = lerp(1., frameFade, fadeX);

  } else if (u_distortionShape == 4.) {
    x = xNonSmooth;
    distortion = sin((x + .25) * TWO_PI);
    shadows = .5 + .5 * asin(distortion) / (.5 * PI);
    distortion *= .5;
    distortion -= u_shift;
    frameFade = .5 + .5 * sin(x * TWO_PI);
  } else if (u_distortionShape == 5.) {
    distortion -= Gpow(abs(x), .2) * x;
    distortion += .33;
    distortion -= 3. * u_shift;
    distortion *= .33;

    frameFade = .3 * (smoothstep(.0, 1., x));
    shadows = Gpow(x, 2.5);

    aa = max(.1, aa);
    aa += lerp(.1, 0., u_size);
    fadeX = smoothstep(0., aa, xNonSmooth) * smoothstep(1., 1. - aa, xNonSmooth);
    distortion *= fadeX;
  }

  float2 dudx = ddx(v_imageUV);
  float2 dudy = ddy(v_imageUV);
  float2 grainUV = v_imageUV - .5;
  grainUV *= (.8 / V2(length(dudx), length(dudy)));
  grainUV += .5;
  if (u_grainMixer > 0.) {
    float grain = valueNoise(grainUV);
    grain = smoothstep(.4, .7, grain);
    grain *= u_grainMixer;
    distortion = lerp(distortion, 0., grain);
  }

  shadows = min(shadows, 1.);
  shadows += maskStrokeInner;
  shadows *= mask;
  shadows = min(shadows, 1.);
  shadows *= Gpow(u_shadows, 2.);
  shadows = clamp(shadows, 0., 1.);

  distortion *= 3. * u_distortion;
  frameFade *= u_distortion;

  fractOrigUV.x += distortion;
  floorOrigUV = rotateAspect(floorOrigUV, -patternRotation, u_imageAspectRatio);
  fractOrigUV = rotateAspect(fractOrigUV, -patternRotation, u_imageAspectRatio);

  uv = (floorOrigUV + fractOrigUV) / patternSize;
  uv += Gpow(maskStroke, 4.);

  uv += V2(.5);

  uv = lerp(v_imageUV, uv, smoothstep(0., .7, mask));
  float blur = lerp(0., 50., u_blur);
  blur = lerp(0., blur, smoothstep(.5, 1., mask));

  float edgeDistortion = lerp(.0, .04, u_edges);
  edgeDistortion += .06 * frameFade * u_edges;
  edgeDistortion *= mask;
  float frame = getUvFrame(uv, edgeDistortion);

  float stretch = 1. - smoothstep(0., .5, xNonSmooth) * smoothstep(1., 1. - .5, xNonSmooth);
  stretch = Gpow(stretch, 2.);
  stretch *= mask;
  stretch *= getUvFrame(uv, .1 + .05 * mask * frameFade);
  uv.y = lerp(uv.y, .5, u_stretch * stretch);

  float4 image = getBlur(uv, 1. / u_resolution / u_pixelRatio, V2(0., 1.), blur);
  image.rgb *= image.a;
  float4 backColor = u_colorBack;
  backColor.rgb *= backColor.a;
  float4 highlightColor = u_colorHighlight;
  highlightColor.rgb *= highlightColor.a;
  float4 shadowColor = u_colorShadow;

  float3 color = highlightColor.rgb * highlights;
  float opacity = highlightColor.a * highlights;

  shadows = lerp(shadows * shadowColor.a, 0., highlights);
  color = lerp(color, shadowColor.rgb * shadowColor.a, .5 * shadows);
  color += .5 * Gpow(shadows, .5) * shadowColor.rgb;
  opacity += shadows;
  color = clamp(color, V3(0.), V3(1.));
  opacity = clamp(opacity, 0., 1.);

  color += image.rgb * (1. - opacity) * frame;
  opacity += image.a * (1. - opacity) * frame;

  color += backColor.rgb * (1. - opacity);
  opacity += backColor.a * (1. - opacity);

  if (u_grainOverlay > 0.) {
    float grainOverlay = valueNoise(rotate(grainUV, 1.) + V2(3.));
    grainOverlay = lerp(grainOverlay, valueNoise(rotate(grainUV, 2.) + V2(-1.)), .5);
    grainOverlay = Gpow(grainOverlay, 1.3);

    float grainOverlayV = grainOverlay * 2. - 1.;
    float3 grainOverlayColor = V3(step(0., grainOverlayV));
    float grainOverlayStrength = u_grainOverlay * abs(grainOverlayV);
    grainOverlayStrength = Gpow(grainOverlayStrength, .8);
    grainOverlayStrength *= mask;
    color = lerp(color, grainOverlayColor, .35 * grainOverlayStrength);

    opacity += .5 * grainOverlayStrength;
  }
  opacity = clamp(opacity, 0., 1.);

  fragColor = V4(color, opacity);
return fragColor;
}

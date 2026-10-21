// SPDX-License-Identifier: Apache-2.0
// Copyright Lost Coast Labs, Inc. (https://paper.design).
// Native HLSL adaptation by HTune; Paper 43cd68db79fa0b1759f72ffc941b3238e2a3954c.
cbuffer PaperConstants:register(b0){float4 nativeUniforms[64];float4 u_colors[10];};
Texture2D u_image:register(t0);
SamplerState linearClamp:register(s0);
#define u_imageAspectRatio nativeUniforms[0].x
#define u_spread nativeUniforms[1].x
#define u_bias nativeUniforms[2].x
#define u_angle nativeUniforms[3].x
#define u_perspective nativeUniforms[4].x
#define u_count nativeUniforms[5].x
#define u_dispersion nativeUniforms[6].x
#define u_dispersionShift nativeUniforms[7].x
#define u_dispersionColor nativeUniforms[8].x
#define u_focusCenter nativeUniforms[9].x
#define u_focusEdges nativeUniforms[10].x
#define u_swirl nativeUniforms[11].x
#define u_noise nativeUniforms[12].x
#define u_noiseFrequency nativeUniforms[13].x
#define u_noiseOffset nativeUniforms[14].x
#define u_lensBulge nativeUniforms[15].x
#define u_lensCircle nativeUniforms[16].x
#define u_grainMixer nativeUniforms[17].x
#define u_grainOverlay nativeUniforms[18].x
#define u_imageX nativeUniforms[19].x
#define u_imageY nativeUniforms[20].x
#define u_resolution nativeUniforms[21].xy

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


  float hash21(float2 p) {
    p = frac(p * V2(0.3183099, 0.3678794)) + 0.1;
    p += dot(p, p + 19.19);
    return frac(p.x * p.y);
  }


float2 rotate(float2 uv, float th) {
  return mul(uv,float2x2(cos(th), sin(th), -sin(th), cos(th)));
}


float valueNoise(float2 st) {
  float2 i = floor(st);
  float2 f = frac(st);
  float a = hash21(i);
  float b = hash21(i + V2(1., 0.));
  float c = hash21(i + V2(0., 1.));
  float d = hash21(i + V2(1., 1.));
  float2 u = f * f * (3. - 2. * f);
  return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float getUvFrame(float2 uv) {
  float2 invAA = 1. / clamp(fwidth(uv), 1e-5, .02);
  float2 lo = clamp(uv * invAA + .5, 0., 1.);
  float2 hi = clamp((1. - uv) * invAA + .5, 0., 1.);
  return lo.x * hi.x * lo.y * hi.y;
}

float4 sampleOverWhite(float2 uv) {
  float4 img = samplePaper(u_image, uv);
  float cover = img.a * getUvFrame(uv);
  float3 colorOverWhite = lerp(V3(1.), img.rgb, cover);
  return V4(colorOverWhite, cover);
}

float3 hueColor(float hue) {
  float3 rgb = clamp(abs(Gmod(hue * 6. + V3(0., 4., 2.), 6.) - 3.) - 1., 0., 1.);
  rgb = rgb * rgb * (3. - 2. * rgb);
  return rgb;
}
                                            
float boxInradius() {
  return .5 * min(u_imageAspectRatio, 1.);
}

float boxOutradius() {
  return .5 * length(V2(u_imageAspectRatio, 1.));
}

float spreadReach() {
  return .7 * Gpow(u_spread, 1.3 + 2.7 * u_spread);
}

float innerCircleMask(float radius, float inradius) {
  return 1. - smoothstep(.5 * inradius, 1.1 * inradius, radius);
}

float dispersionCurve(float t, float biasPow) {
  float mirroredT = u_bias < 0. ? 1. - t : t;
  float curved = Gpow(mirroredT, biasPow);
  return u_bias < 0. ? 1. - curved : curved;
}

float2 getSpread(float2 fromCenter, float radius, float2 warpedUV, float edgeAA, float inradius, float outradius, float reach, out float outStrength) {
  float angleRad = radians(u_angle);
  float2 uniformDir = V2(cos(angleRad), sin(angleRad));

  float invInradius = 1. / inradius;
  float2 radialDir = fromCenter * invInradius;
  float maxLen = (outradius + reach) * invInradius;
  float radialLen = radius * invInradius;
  if (radialLen > maxLen) radialDir *= maxLen / radialLen;
  float2 spreadDir = lerp(uniformDir, radialDir, u_perspective);

  float bandProximity = smoothstep(inradius * .8, inradius, radius);
  float lensCircleMaxing = bandProximity * u_lensCircle * u_lensCircle * u_lensCircle;
  spreadDir = lerp(spreadDir, radialDir, lensCircleMaxing);

  float2 warpedAbs = abs(warpedUV - .5);
  float inner = lerp(1., smoothstep(0., lerp(inradius, outradius, u_focusCenter), radius), u_focusCenter);
  float boxDist = max(warpedAbs.x, warpedAbs.y) * 2.;
  float outer = lerp(1., 1. - min(boxDist, 1.), u_focusEdges);
  float strength = inner * outer;

  strength *= lerp(1., lerp(.15, .03, max(-u_lensBulge, 0.)), lensCircleMaxing);

  float2 outside = max(warpedAbs - .5, 0.);
  float margin = max(reach * length(spreadDir) * (1. - u_lensCircle), edgeAA);
  strength *= 1. - smoothstep(0., margin, length(outside));

  outStrength = strength;
  float2 axis = spreadDir * (reach * strength);

  if (u_noise > 0.) {
    float turn = (valueNoise(fromCenter * u_noiseFrequency * 18. + u_noiseOffset * 30.) - .5) * 2. * u_noise;
    float cs = cos(turn), sn = sin(turn);
    axis = mul(axis,float2x2(cs, -sn, sn, cs));
  }

  axis.x /= u_imageAspectRatio;
  return axis;
}

float2 lensWarp(float2 fromCenter, float radius, float inradius, out float bulgeFade) {
  bulgeFade = 1.;
  if (u_lensBulge == 0. && u_lensCircle <= 0.) return fromCenter;
  if (radius < 1e-5) return fromCenter;
  
  float r = radius;

  if (u_lensBulge != 0.) {
    float rn = radius / inradius;
    float bulge = abs(u_lensBulge) * (u_lensBulge > 0. ? 1.4 : 1.2);
    if (u_lensBulge > 0.) bulgeFade = 1. - smoothstep(1.45, 1.53, rn * bulge);
    float map = u_lensBulge > 0.
      ? tan(min(rn * bulge, 1.53)) / tan(bulge)
      : Gatan(rn * tan(bulge)) / bulge;
    float bulgeScale = map / rn;
    fromCenter *= bulgeScale;
    r *= bulgeScale;
  }

  if (u_lensCircle > 0.) {
    float2 dir = fromCenter / max(r, 1e-5);
    float2 halfBox = V2(u_imageAspectRatio, 1.) * .5;
    float rBox = min(halfBox.x / max(abs(dir.x), 1e-4), halfBox.y / max(abs(dir.y), 1e-4));
    float band = inradius * lerp(.03, .2, .5 * (u_lensBulge + 1.));
    float innerEdge = inradius - band;
    float over = smoothstep(0., 1., (r - innerEdge) / band);
    float g = r + (rBox - inradius) * Gpow(over, 14.);
    fromCenter = dir * lerp(r, g, u_lensCircle);
  }

  return fromCenter;
}

float4 PaperPS6(PaperPixel input):SV_TARGET {
float4 fragColor=0;float2 v_objectUV=(input.uv-.5)*float2(u_resolution.x/u_resolution.y,1);float2 v_imageUV=(float2(input.uv.x,1-input.uv.y)-.5)*nativeUniforms[63].xy+.5;float4 gl_FragCoord=float4(input.position.x,u_resolution.y-input.position.y,0,1);
  float2 uv = v_imageUV;

  float invAspect = 1. / u_imageAspectRatio;
  float inradius = boxInradius();
  float outradius = boxOutradius();
  float reach = spreadReach();

  float2 fromCenter = uv - .5;
  fromCenter.x *= u_imageAspectRatio;
  float radius = length(fromCenter);

  float bulgeFade=0;
  float2 warpedFromCenter = lensWarp(fromCenter, radius, inradius, bulgeFade);
  float2 baseUV = V2(warpedFromCenter.x * invAspect, warpedFromCenter.y) + .5;

  float2 baseDerivative = fwidth(baseUV);
  float edgeAA = clamp(2. * max(baseDerivative.x, baseDerivative.y), .001, .02);

  float spreadStrength=0;
  float2 spreadAxis = getSpread(fromCenter, radius, baseUV, edgeAA, inradius, outradius, reach, spreadStrength);

  float2 grainUV = V2(0.);
  if (u_grainMixer > 0. || u_grainOverlay > 0.) {
    float2 dudx = ddx(v_imageUV);
    float2 dudy = ddy(v_imageUV);
    grainUV = (v_imageUV - .5) * (.8 / V2(length(dudx), length(dudy))) + .5;
  }

  if (u_grainMixer > 0.) {
    float grainSeed = valueNoise(grainUV);
    float2 jit = frac(grainSeed * V2(157.31, 113.57)) * 2. - 1.;
    spreadAxis += jit * .3 * u_grainMixer * length(spreadAxis);
  }

  float count = floor(u_count);
  int countInteger = int(count);

  float invCount = 1. / count;
  float invSpan = 1. / max(count - 1., 1.);
  float hueBase = u_dispersionColor + .5;
  float biasPower = 1. + 2. * abs(u_bias);
  float2 tapBias = .5 - V2(u_imageX, u_imageY);

  float centerAmount = clamp(1. - u_dispersionShift, 0., 1.);
  float edgesAmount = clamp(1. + u_dispersionShift, 0., 1.);
  float dispersion = u_dispersion * lerp(edgesAmount, centerAmount, innerCircleMask(radius, inradius));
  float dispersionPower = Gpow(dispersion, .8);

  float warpedRadius = length(warpedFromCenter);
  float swirlAngle = u_swirl * .4 * PI * spreadStrength * u_spread * min(1., inradius / max(warpedRadius, 1e-4));

  float3 colorSum = V3(0.);
  float3 weightSum = V3(0.);
  float coverSum = 0.;

  for (int i0 = 0; i0 < 50; i0++) {
    if (i0 >= countInteger) break;

    float layer = float(i0);
    float hue = hueBase + layer * invCount;

    float spread = layer * invSpan;
    if (u_bias != 0.) spread = dispersionCurve(spread, biasPower);

    // lerp(axis, -axis, spread) is axis scaled by the signed position along the fan
    float fanPos = 1. - 2. * spread;
    float2 tapUV = baseUV + spreadAxis * fanPos - .5;

    if (u_swirl != 0.) {
      tapUV.x *= u_imageAspectRatio;
      tapUV = rotate(tapUV, swirlAngle * fanPos);
      tapUV.x *= invAspect;
    }

    float4 tap = sampleOverWhite(tapUV + tapBias);
    float3 weight = 1. - dispersionPower * hueColor(hue);

    colorSum += tap.rgb * weight;
    weightSum += weight;
    coverSum += tap.a;
  }

  float3 color = colorSum / max(weightSum, 1e-4);
  float coverAvg = coverSum * invCount;

  float ground = min(color.r, min(color.g, color.b));
  float alpha = max(coverAvg, 1. - ground);
  float3 premult = max(color - (1. - alpha), 0.);
  fragColor = V4(premult, alpha) * bulgeFade;

  if (u_grainOverlay > 0.) {
    float grain = valueNoise(rotate(grainUV, 1.) + V2(3.));
    grain = lerp(grain, valueNoise(rotate(grainUV, 2.) + V2(-1.)), .5);
    grain = Gpow(grain, 1.3);
    float grainV = grain * 2. - 1.;
    float grainStrength = Gpow(u_grainOverlay * abs(grainV), .8) * fragColor.a;
    fragColor.rgb = lerp(fragColor.rgb, V3(step(0., grainV)) * fragColor.a, .35 * grainStrength);
  }
return fragColor;
}

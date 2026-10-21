// SPDX-License-Identifier: Apache-2.0
// Copyright Lost Coast Labs, Inc. (https://paper.design).
// Native HLSL adaptation by HTune; Paper 43cd68db79fa0b1759f72ffc941b3238e2a3954c.
cbuffer PaperConstants:register(b0){float4 nativeUniforms[64];float4 u_colors[10];};
Texture2D u_image:register(t0);
Texture2D u_noiseTexture:register(t1);
SamplerState linearClamp:register(s0);
#define u_isImage (nativeUniforms[0].x>.5)
#define u_imageAspectRatio nativeUniforms[1].x
#define u_colorBack nativeUniforms[2].xyzw
#define u_colorPaper nativeUniforms[3].xyzw
#define u_colorShadow nativeUniforms[4].xyzw
#define u_blending nativeUniforms[5].x
#define u_distortion nativeUniforms[6].x
#define u_clip (nativeUniforms[7].x>.5)
#define u_angle nativeUniforms[8].x
#define u_seed nativeUniforms[9].x
#define u_roughness nativeUniforms[10].x
#define u_roughnessSize nativeUniforms[11].x
#define u_roughnessRows nativeUniforms[12].x
#define u_fiber nativeUniforms[13].x
#define u_fiberSize nativeUniforms[14].x
#define u_folds nativeUniforms[15].x
#define u_foldSizeX nativeUniforms[16].x
#define u_foldSizeY nativeUniforms[17].x
#define u_foldOffsetX nativeUniforms[18].x
#define u_foldOffsetY nativeUniforms[19].x
#define u_wrinkles nativeUniforms[20].x
#define u_wrinkleSize nativeUniforms[21].x
#define u_crumples nativeUniforms[22].x
#define u_crumpleCount nativeUniforms[23].x
#define u_drops nativeUniforms[24].x
#define u_resolution nativeUniforms[25].xy

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







float getUvFrame(float2 uv) {
  float2 invAA = .5 / clamp(fwidth(uv), 1e-5, .02);
  float2 lo = clamp(uv * invAA + .5, 0., 1.);
  float2 hi = clamp((1. - uv) * invAA + .5, 0., 1.);
  return lo.x * hi.x * lo.y * hi.y;
}

float lst(float edge0, float edge1, float x) {
  return clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
}


#define TWO_PI 6.28318530718
#define PI 3.14159265358979323846


  float hash21(float2 p) {
    p = frac(p * V2(0.3183099, 0.3678794)) + 0.1;
    p += dot(p, p + 19.19);
    return frac(p.x * p.y);
  }


  float2 hash22(float2 p) {
    p = frac(p * V2(0.3183099, 0.3678794)) + 0.1;
    p += dot(p, p.yx + 19.19);
    return frac(V2(p.x * p.y, p.x + p.y));
  }


float getRoughness(float2 p, float2 lightDir, float2 seedShift, float basePixel) {
  float2 u = p / V2(2, 4);
  float w = 100. * basePixel;
  float eps = 4. * w;

  float size = lerp(3.2, .8, u_roughnessSize);
  float logLac = log2(2.1);
  float level = -log2(w + 1e-8) / logLac;
  float baseLevel = floor(level) - 2.;
  float fade = frac(level);

  float2 px = (u.x + V2(eps, -eps)) * .1;
  float py = u.y * .1;

  float2 sum = V2(0.);
  float norm = 0., amp = .5;
  float freq = exp2(baseLevel * logLac);
  for (int i0 = 0; i0 < 4; i0++) {
    float absIdx = baseLevel + float(i0);
    float2 qx = size * px * freq;
    float qy = size * py * freq;

    float wi = 1.;
    if (i0 == 0) wi = 1. - fade;
    if (i0 == 3) wi = fade;

    float2 fx = frac(qx);
    float fy = frac(qy);
    float2 shift = .5 + absIdx * .3 + seedShift;
    float uvY = floor(qy) / 50. + shift.y;
    float2 s0a = samplePaperLod(u_noiseTexture, frac(V2(floor(qx.x) / 50. + shift.x, uvY)), 0.).rg;
    float2 s1a = samplePaperLod(u_noiseTexture, frac(V2( ceil(qx.x) / 50. + shift.x, uvY)), 0.).rg;
    float2 s0b = samplePaperLod(u_noiseTexture, frac(V2(floor(qx.y) / 50. + shift.x, uvY)), 0.).rg;
    float2 s1b = samplePaperLod(u_noiseTexture, frac(V2( ceil(qx.y) / 50. + shift.x, uvY)), 0.).rg;
    float2 ny0 = lerp(V2(s0a.r, s0b.r), V2(s0a.g, s0b.g), fy);
    float2 ny1 = lerp(V2(s1a.r, s1b.r), V2(s1a.g, s1b.g), fy);
    float2 n = lerp(ny0, ny1, fx);

    sum += amp * wi * n;
    norm += amp * wi;
    amp *= .8;
    freq *= 2.1;
  }

  float2 r = sum / norm;
  float dx = .5 + r.x - r.y;
  float grain = 3. * dx * dx - .7;

  float rowBand = frac(dot(p, lightDir) * .05 * size);
  return grain + .6 * u_roughnessRows * (rowBand - .5);
}

float getFiber(float2 p, float2 seedShift, float basePixel) {
  float size = lerp(4., 1., u_fiberSize);
  float w = 50. * basePixel;
  float level = -log2(w + 1e-8);
  float baseLevel = floor(level) - 3.;
  float fade = frac(level);

  float2 grad = V2(0.);
  float scale = 1.;
  float amp = 1.;
  float freq = Gpow(1.7, baseLevel);
  for (int i1 = 0; i1 < 5; i1++) {
    float absIdx = baseLevel + float(i1);
    float2 q = size * p * freq;

    float an = absIdx * .8;
    float rc = cos(an), rs = sin(an);
    q = V2(rc * q.x - rs * q.y, rs * q.x + rc * q.y);

    float wi = 1.;
    if (i1 == 0) wi = 1. - fade;
    if (i1 == 4) wi = fade;

    float2 iq = floor(q);
    float2 fq = frac(q);
    float shift = absIdx * .3;
    float4 uv = frac(V4(iq, iq + 1.) / 50. + .5 + shift + seedShift.xyxy);
    float aF = samplePaperLod(u_noiseTexture, uv.xy, 0.).b;
    float bF = samplePaperLod(u_noiseTexture, uv.zy, 0.).b;
    float cF = samplePaperLod(u_noiseTexture, uv.xw, 0.).b;
    float dF = samplePaperLod(u_noiseTexture, uv.zw, 0.).b;
    float2 u = fq * fq * (3. - 2. * fq);
    float2 du = 8. * fq * (1. - fq);
    float dx = du.x * lerp(bF - aF, dF - cF, u.y);
    float dy = du.y * lerp(cF - aF, dF - bF, u.x);
    grad += wi * amp * scale * V2(rc * dx + rs * dy, -rs * dx + rc * dy);
    scale *= 1.7;
    amp *= .5;
    freq *= 1.7;
  }

  return clamp(.333 * length(grad), 0., 1.);
}

float2 smoothNoise(float2 p) {
  float2 t = p * 50. - .5;
  float2 i = floor(t);
  float2 f = frac(t);
  f = f * f * (3. - 2. * f);
  return samplePaperLod(u_noiseTexture, frac((i + f + .5) / V2(50.)), 0.).rg;
}

float getDrops(float2 uv, float2 seedShift) {
  float2 iDropsUV = floor(uv);
  float2 fDropsUV = frac(uv);
  float dropsMinDist = 1.;
  for (int y2 = -1; y2 < 2; y2 += 1) {
    for (int x3 = -1; x3 < 2; x3 += 1) {
      float2 neighbor = V2(float(y2), float(x3));
      float2 offset = hash22(iDropsUV + neighbor + 50. * seedShift);
      float2 pos = neighbor + offset - fDropsUV;
      dropsMinDist *= min(1., dot(pos, pos));
    }
  }
  return .5 * lst(.09, .08, sqrt(sqrt(dropsMinDist)));
}

float2 getCellTilt(float idx, float radius) {
  float2 rand = hash22(V2(idx + 31., idx * u_seed + 17.));
  float an = rand.x * TWO_PI;
  return V2(cos(an), sin(an)) * lerp(.3, 1.7, rand.y * rand.y) * lerp(.7, 1., radius);
}

float2 getCrumpleDetail(float2 uv, float freq, float shift, float basePixel, out float depth) {
  depth = 0.;
  float2 v = uv * freq;
  float pixel = .9 * freq * basePixel;
  float2 base = floor(v);
  float n1 = 9., n2 = 9.;
  float2 s1 = V2(0.), s2 = V2(0.), q1 = V2(0.), q2 = V2(0.);

  for (int y4 = -1; y4 <= 1; y4++) {
    for (int x5 = -1; x5 <= 1; x5++) {
      float2 cell = base + V2(float(x5), float(y4));
      float2 q = hash22(V2(cell.y * 1.9 + 3.1 + shift, cell.x * 1.3 + .11 * u_seed));
      float keep = .8;
      if (q.y > keep) continue;

      float2 r = hash22(V2(cell.x + .37 * u_seed + shift + 11.1, cell.y - .21 * u_seed + 5.7));
      float2 s = cell + .06 + .88 * r;

      float2 d = v - s;
      float dsq = dot(d, d);
      if (dsq < n1) {
        n2 = n1;
        s2 = s1;
        q2 = q1;
        n1 = dsq;
        s1 = s;
        q1 = q;
      } else if (dsq < n2) {
        n2 = dsq;
        s2 = s;
        q2 = q;
      }
    }
  }

  if (n1 > 8.) return V2(0.);

  float an1 = q1.x * TWO_PI, mag1 = q1.y / .8;
  float2 t1 = V2(cos(an1), sin(an1)) * lerp(.3, 1.7, mag1 * mag1);
  float an2 = q2.x * TWO_PI, mag2 = q2.y / .8;
  float2 t2 = V2(cos(an2), sin(an2)) * lerp(.3, 1.7, mag2 * mag2);

  float2 span = s1 - s2;
  float spanLen = max(length(span), 1e-4);
  float toEdge = (n2 - n1) / (2. * spanLen);

  float pair = hash21(s1 + s2 + 1.7 * abs(s1 - s2));
  float pairSoft = .05 + .1 * step(pair, .4);
  float b = clamp(toEdge / max(1.5 * pixel, .5 * pairSoft), 0., 1.);
  b = b * b * (3. - 2. * b);
  float d1 = max(sqrt(n1), 1e-4);
  depth = .2 * d1;
  float shoulderAmt = max(0., 1. - toEdge / .42) * b;
  float2 shoulder = V2(0.);
  if (shoulderAmt > 0.) {
    float d2 = max(sqrt(n2), 1e-4);
    shoulder = shoulderAmt * ((v - s2) / d2 - (v - s1) / d1);
  }

  float2 mid = .5 * (t1 + t2);
  return mid + (t1 - mid) * b + .9 * shoulder;
}

float4 getCrumples(float2 uv) {
  float crumpleN = max(2., floor(u_crumpleCount + .5));
  float near = 60000., nearB = 60000.;
  float idx = 0., rad = 0.;
  float2 nearP = V2(0.), nearPb = V2(0.);
  float4 seeds[15];
  for (int i6 = 0; i6 < 15; i6++) {
    if (float(i6) >= crumpleN) break;
    float2 rand = hash22(V2(float(i6), float(i6) * u_seed));
    float an = rand.x * TWO_PI;
    float2 p = V2(cos(an), sin(an)) * rand.y;
    seeds[i6] = V4(p, rand);

    float2 d = uv - p;
    float dsq = dot(d, d);
    if (dsq < near) {
      nearB = near;
      nearPb = nearP;
      near = dsq;
      idx = float(i6);
      rad = rand.y;
      nearP = p;
    } else if (dsq < nearB) {
      nearB = dsq;
      nearPb = p;
    }
  }
  float l = sqrt(near), lb = sqrt(nearB);
  float2 dir = (uv - nearP) / max(l, 1e-4);
  float edge = lst(0., .5, lb - l);

  float shoulderLimit = l + .5;

  float2 tilt = getCellTilt(idx, rad);
  float tiltSum = 1.;
  float2 wide = V2(0.);
  float wideSum = 0.;
  float toEdge = 9., edgeSoft = 0.;

  for (int i7 = 0; i7 < 15; i7++) {
    if (float(i7) >= crumpleN) break;
    if (float(i7) == idx) continue;
    float4 seed = seeds[i7];
    float2 p = seed.xy;
    float2 rand = seed.zw;

    float2 d = uv - p;
    float dsq = dot(d, d);

    if (dsq < shoulderLimit * shoulderLimit) {
      float di = sqrt(dsq);
      float shoulder = lst(.5, 0., di - l);
      wide += shoulder * (d / max(di, 1e-4) - dir);
      wideSum += shoulder;
    }

    float pairSoft = .01 + .05 * step(.2, rand.x) + .3 * rand.y;
    float toBisector = (dsq - near) / (.2 * max(length(p - nearP), 1e-4));
    if (toBisector < toEdge) {
      toEdge = toBisector;
      edgeSoft = pairSoft;
    }

    float w = 1. - toBisector / pairSoft;
    if (w > 0.) {
      w *= w;
      tilt += w * getCellTilt(float(i7), rand.y);
      tiltSum += w;
    }
  }

  float blend = clamp(toEdge / edgeSoft, 0., 1.);
  float2 sharp = (1. - edge) * ((uv - nearPb) / max(lb, 1e-4) - dir);
  float2 rounding = lerp(sharp, wide / max(wideSum, 1.), min(2. * edgeSoft, 1.));
  float radial = l / max(l + toEdge, 1e-4);

  return V4(tilt / tiltSum + rounding * blend * radial, .2 * l, edge);
}

void getFolds(float2 coord, float2 offset, float2 count, float2 noise, float2 baseFwidth, out float2 slope, out float2 dark, out float2 lift) {
  float2 g = coord * count + .5 * offset + noise;
  float2 dx = frac(g) - .5;
  float2 adx = abs(dx);
  float foldRadius = .3;
  float2 t = clamp(adx / foldRadius, 0., 1.);
  dark = t * t * (3. - 2. * t);
  lift = 1. - t;
  lift *= lift;

  float2 crease = clamp(dx / max(count * baseFwidth, 1e-5), -1., 1.);
  slope = crease * (1. - dark);

  float2 lineWidth = .02 * count * lerp(V2(2.5), V2(.8), dark.yx) + .1 * noise;
  slope -= 1. - smoothstep(V2(0.), lineWidth, adx);
}

float4 PaperPS5(PaperPixel input):SV_TARGET {
float4 fragColor=0;float2 v_objectUV=(input.uv-.5)*float2(u_resolution.x/u_resolution.y,1);float2 v_imageUV=(float2(input.uv.x,1-input.uv.y)-.5)*nativeUniforms[63].xy+.5;float4 gl_FragCoord=float4(input.position.x,u_resolution.y-input.position.y,0,1);

  float2 patternUV = (v_imageUV - .5) * V2(u_imageAspectRatio, 1.);
  float basePixel = max(length(ddx(patternUV)), length(ddy(patternUV)));
  float2 baseFwidth = fwidth(patternUV);

  float pattern = 0.;

  float crumpleDepth = 0.;
  float wrinkleDepth = 0.;
  float foldDepth = 0.;
  float roughness = 0.;
  float fiber = 0.;
  float drops = 0.;

  float grazing = 0.7;
  float lightRad = radians(u_angle);
  float2 lightDir = V2(sin(lightRad), -cos(lightRad));
  float foldAngleInSector = Gmod(u_angle, 90.);
  float foldLightRad = radians(u_angle - foldAngleInSector + lerp(12., 78., foldAngleInSector / 90.));
  float2 foldLightDir = V2(sin(foldLightRad), -cos(foldLightRad));
  float2 relief = V2(0.);
  float2 foldSlope = V2(0.);
  float reliefAmount = 0.;
  float foldInk = 1.;
  float2 crumpleFlow = V2(0.);

  float2 warpNoise = V2(0.);
  if (u_crumples > 0. || u_folds > 0. || u_wrinkles > 0.) {
    warpNoise = smoothNoise(patternUV * .1 + .2 + .6 * frac(.017 * u_seed)) - .5;
  }
  float2 crumplesUV = (patternUV * .9) + .012 * warpNoise;

  if (u_crumples > 0.) {
    float4 crumples = getCrumples(crumplesUV);

    float2 crumpleTilt = .5 * u_crumples * crumples.xy;
    crumpleTilt -= .1 * max(dot(crumpleTilt, lightDir), 0.) * lightDir;
    relief += crumpleTilt;
    reliefAmount += .6 * u_crumples;
    crumpleFlow = crumples.w * crumpleTilt;

    crumpleDepth = clamp(5. * crumples.z, 0., 1.);
  }

  if (u_wrinkles > 0.) {
    float detailFreq = lerp(10., 1., u_wrinkleSize);
    float detailAmp = .2;
    float2 detailGrad = V2(0.);
    float detailDepth = 0., depthSum = 0.;
    for (int i8 = 0; i8 < 3; i8++) {
      float layerDepth=0;
      detailGrad += detailAmp * getCrumpleDetail(crumplesUV, detailFreq, 31. * float(i8), basePixel, layerDepth);
      detailDepth += detailAmp * layerDepth;
      depthSum += detailAmp;
      detailAmp *= (.5 + .2 * detailGrad.x);
      detailFreq *= 2.1;
    }

    float2 detailTilt = 1.5 * u_wrinkles * detailGrad;
    detailTilt -= .65 * max(dot(detailTilt, lightDir), 0.) * lightDir;
    relief += detailTilt;
    reliefAmount += .6 * u_wrinkles;

    wrinkleDepth = clamp(5. * detailDepth / max(depthSum, 1e-4), 0., 1.);
  }

  if (u_folds > 0.) {
    float2 foldOffset = V2(1. - 2. * u_foldOffsetX, 1. - 2. * u_foldOffsetY);
    float2 foldCount = V2(lerp(3., .5, u_foldSizeX), lerp(3., .5, u_foldSizeY));
    float2 foldNoise = .005 * warpNoise * foldCount;
    float2 uv = patternUV + .03 * crumpleFlow;

    float2 slope, dark, lift;
    getFolds(uv, foldOffset, foldCount, foldNoise, baseFwidth, slope, dark, lift);

    foldSlope = u_folds * slope;
    reliefAmount += 1. * u_folds;
    float2 ink = lerp(V2(.96), V2(1.), dark);
    float flatness = dark.x * dark.y;
    foldInk *= lerp(1., ink.x * ink.y * lerp(.5, .3, flatness), u_folds);

    foldDepth = u_folds * (lift.y * foldLightDir.y - lift.x * foldLightDir.x);
  }

  float unlit = cos(grazing);
  float lit = unlit;

  if (reliefAmount > 0.) {
    float lightFalloff = clamp(.5 + dot(v_imageUV - .5, lightDir), 0., 1.);
    float lightPower = lerp(.5, 1., lightFalloff);
    float slope = clamp(dot(relief, lightDir) + dot(foldSlope, foldLightDir), -1.2, 1.2);
    lit = max(cos(slope + grazing), 0.);

    pattern += (clamp(reliefAmount, 0., 1.) * unlit + (lit - unlit)) * foldInk * lightPower;
  }

  patternUV += .04 * crumpleFlow;

  float2 seedShift = floor(frac(u_seed * V2(.7548776662, .5698402909)) * 50.) / 50.;

  if (u_roughness > 0.) {
    roughness = u_roughness * getRoughness(1000. * patternUV, lightDir, seedShift, basePixel);
    pattern += roughness;
  }

  if (u_fiber > 0.) {
    fiber = u_fiber * getFiber(50. * patternUV, seedShift, basePixel);
    pattern += fiber;
  }

  if (u_drops > 0.) {
    drops = u_drops * getDrops(patternUV * 10., seedShift);
  }

  pattern = clamp(pattern, 0., 1.);

  float3 backColor = u_colorBack.rgb * u_colorBack.a;
  float backOpacity = u_colorBack.a;
  float3 baseColor = u_colorPaper.rgb * u_colorPaper.a;
  float baseOpacity = u_colorPaper.a;
  float3 shadowColor = u_colorShadow.rgb * u_colorShadow.a;
  float shadowOpacity = u_colorShadow.a;

  float notClipped = u_clip ? .1 : 1.;
  float2 imageCenteredUV = v_imageUV - .5;
  float edgeDist = 2. * max(abs(imageCenteredUV.x), abs(imageCenteredUV.y));
  float wrinkleDistortion = lerp(.38, .0175, smoothstep(0., 1., edgeDist));
  float scaleDistortion = .15 * u_crumples * (crumpleDepth - .5) + wrinkleDistortion * u_wrinkles * (wrinkleDepth - .5) + .05 * foldDepth;
  float2 linearDistortion = notClipped * .002 * lightDir * drops;
  float radialDistortion = notClipped * .02 * (roughness + fiber);
  float2 centeredUV = imageCenteredUV * (1. - u_distortion * scaleDistortion);
  centeredUV -= u_distortion * linearDistortion;
  float2 imageUV = .5 + centeredUV * (1. - abs(u_distortion) * radialDistortion * dot(centeredUV, centeredUV));

  float3 color = shadowColor * pattern;
  float opacity = shadowOpacity * pattern;
  color += baseColor * (1. - opacity);
  opacity += baseOpacity * (1. - opacity);

  if (u_isImage) {
    float frame = getUvFrame(imageUV);
    float4 image = samplePaper(u_image, imageUV);
    frame *= image.a;

    float maxC = max(max(image.r, image.g), image.b);
    float minC = min(min(image.r, image.g), image.b);
    float sat = maxC > 0. ? (maxC - minC) / maxC : 0.;
    float midC = image.r + image.g + image.b - maxC - minC;
    float secondaryness = maxC > minC ? (midC - minC) / (maxC - minC) : 0.;
    float satDampen = sat * (1. - .5 * secondaryness);
    float darkDampen = 1. - dot(V3(.2126, .7152, .0722), image.rgb);
    float dampen = lerp(0., .7, u_blending) * max(satDampen, darkDampen);

    float3 paper = V3(1.) - opacity + color;
    float3 pic = image.rgb * paper * u_blending + image.rgb * (1. - u_blending);
    pic = lerp(pic, V3(1.), .6 * Gpow(dampen, 2. + 3. * pattern));

    color = lerp(color, pic, frame);
    opacity = frame + opacity * (1. - frame);

    if (u_clip) {
      color *= frame;
      opacity *= frame;
    }
  }

  color *= lerp(V3(1.), .5 * u_colorShadow.rgb, drops * shadowOpacity);

  color += backColor * (1. - opacity);
  opacity += backOpacity * (1. - opacity);

  fragColor = V4(color, opacity);
return fragColor;
}

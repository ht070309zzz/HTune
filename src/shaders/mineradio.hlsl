// SPDX-License-Identifier: GPL-3.0-only
// Copyright Mineradio contributors. Native port by HTune contributors.
// XxHuberrr/Mineradio-paused d43de565acabfdc1a9c9820a27e81a98ccbebcef
// 02-visual/00-pointer-cover-particles.js. Original particle formulas retained.
// Changed I/O, source textures, camera and point sprites for D3D11 instancing.
cbuffer MineradioConstants:register(b0){float4 frame,audio,shape,control,cameraEye,cameraTarget,pointer,tint;};
#define uTime frame.z
#define uPixel frame.w
#define uBass audio.x
#define uMid audio.y
#define uTreble audio.z
#define uBeat audio.w
#define uEnergy ((uBass+uMid+uTreble)/3)
#define uBurstAmt audio.w
#define uPreset shape.x
#define uIntensity shape.y
#define uPointScale shape.z
#define uSpeed shape.w
#define uDepth 1.0
#define uTwist control.x
#define uVinylSpin (uTime*.26)
#define uColorBoost 1.1
#define uScatter control.y
#define uCoverRes 1.0
#define uBgFade 0.0
#define uHasCover control.z
#define uHasDepth 0.0
#define uEdgeEnabled control.z
#define uAiBoost 0.0
#define uMouseActive pointer.z
#define uMouseXY pointer.xy
#define uColorMixT control.w
#define uLoading 0.0
#define uRippleCount ((int)cameraTarget.w)
#define uHandActive 0.0
#define uHandXY float2(0,0)
#define uGestureGrip 0.0
#define uTintColor tint.rgb
#define uTintStrength tint.a
#define uAlpha 1.0
#define uParticleDim 1.0
#define uBackdropAdapt 0.0
Texture2D uCoverTex:register(t0),uPrevCoverTex:register(t1),uEdgeTex:register(t2),uRippleTex:register(t3);
SamplerState linearClamp:register(s0);
float2 V2(float a){return a.xx;} float2 V2(float2 a){return a;} float2 V2(float a,float b){return float2(a,b);}
float3 V3(float a){return a.xxx;} float3 V3(float3 a){return a;} float3 V3(float a,float b,float c){return float3(a,b,c);} float3 V3(float2 a,float b){return float3(a,b);} float3 V3(float a,float2 b){return float3(a,b);}
float4 V4(float a){return a.xxxx;} float4 V4(float4 a){return a;} float4 V4(float a,float b,float c,float d){return float4(a,b,c,d);} float4 V4(float3 a,float b){return float4(a,b);} float4 V4(float2 a,float2 b){return float4(a,b);}
float GlslMod(float a,float b){return a-b*floor(a/b);}
struct Particle {float3 pos,col;float bright,ripple,edge,alpha,lum,size;};
struct Pixel {float4 position:SV_POSITION;float2 uv:TEXCOORD0;float3 color:COLOR0;float4 values:TEXCOORD1;float lum:TEXCOORD2;};
float3 ViewPoint(float3 pos){float3 z=normalize(cameraEye.xyz-cameraTarget.xyz),x=normalize(cross(float3(0,1,0),z)),y=cross(z,x);float3 p=pos-cameraEye.xyz;return float3(dot(p,x),dot(p,y),dot(p,z));}


#define PI 3.14159265359

float3 mod289(float3 x){return x-floor(x*(1.0/289.0))*289.0;}
float4 mod289v(float4 x){return x-floor(x*(1.0/289.0))*289.0;}
float4 perm(float4 x){return mod289v(((x*34.0)+1.0)*x);}
float snoise(float3 v){
  const float2 C=V2(1.0/6.0,1.0/3.0);
  const float4 D=V4(0.0,0.5,1.0,2.0);
  float3 i=floor(v+dot(v,C.yyy));
  float3 x0=v-i+dot(i,C.xxx);
  float3 g=step(x0.yzx,x0.xyz); float3 l=1.0-g;
  float3 i1=min(g.xyz,l.zxy); float3 i2=max(g.xyz,l.zxy);
  float3 x1=x0-i1+C.xxx;
  float3 x2=x0-i2+C.yyy;
  float3 x3=x0-D.yyy;
  i=mod289(i);
  float4 p=perm(perm(perm(i.z+V4(0.0,i1.z,i2.z,1.0))+i.y+V4(0.0,i1.y,i2.y,1.0))+i.x+V4(0.0,i1.x,i2.x,1.0));
  float n_=0.142857142857;
  float3 ns=n_*D.wyz-D.xzx;
  float4 j=p-49.0*floor(p*ns.z*ns.z);
  float4 x_=floor(j*ns.z); float4 y_=floor(j-7.0*x_);
  float4 x=x_*ns.x+ns.yyyy; float4 y=y_*ns.x+ns.yyyy;
  float4 h=1.0-abs(x)-abs(y);
  float4 b0=V4(x.xy,y.xy); float4 b1=V4(x.zw,y.zw);
  float4 s0=floor(b0)*2.0+1.0; float4 s1=floor(b1)*2.0+1.0;
  float4 sh=-step(h,V4(0.0));
  float4 a0=b0.xzyw+s0.xzyw*sh.xxyy; float4 a1=b1.xzyw+s1.xzyw*sh.zzww;
  float3 p0=V3(a0.xy,h.x); float3 p1=V3(a0.zw,h.y); float3 p2=V3(a1.xy,h.z); float3 p3=V3(a1.zw,h.w);
  float4 norm=rsqrt(V4(dot(p0,p0),dot(p1,p1),dot(p2,p2),dot(p3,p3)));
  p0*=norm.x; p1*=norm.y; p2*=norm.z; p3*=norm.w;
  float4 m=max(0.6-V4(dot(x0,x0),dot(x1,x1),dot(x2,x2),dot(x3,x3)),0.0);
  m=m*m;
  return 42.0*dot(m*m,V4(dot(p0,x0),dot(p1,x1),dot(p2,x2),dot(p3,x3)));
}

float hash11(float p) {
  return frac(sin(p * 127.1) * 43758.5453123);
}

float2 safeCoverUv(float2 uv) {
  // Formula coordinates are bottom-up world Y; decoded D3D image rows are
  // top-down. Apply the conversion once, equally to current/previous/edges.
  return clamp(float2(uv.x,1-uv.y), V2(0.0012), V2(0.9988));
}

float3 sampleNewCoverColor(float2 uv) {
  return uCoverTex.SampleLevel(linearClamp, safeCoverUv(uv),0).rgb;
}

float3 samplePrevCoverColor(float2 uv) {
  return uPrevCoverTex.SampleLevel(linearClamp, safeCoverUv(uv),0).rgb;
}

float4 sampleEdgeColor(float2 uv) {
  return uEdgeTex.SampleLevel(linearClamp, safeCoverUv(uv),0);
}

float rippleSumAt(float2 p, out float maxAmp) {
  float sum = 0.0; maxAmp = 0.0;
  for (int ri = 0; ri < 12; ri++) {
if (ri >= uRippleCount) break;
float vCoord = (float(ri) + 0.5) / 12.0;
float4 rd = uRippleTex.SampleLevel(linearClamp, V2(0.5, vCoord),0);
float age = rd.z; float str = rd.w;
if (str < 0.005 || age < 0.0 || age > 2.0) continue;
float dx = p.x - rd.x, dy = p.y - rd.y;
float dist = sqrt(dx*dx + dy*dy);
float lifeN = age / 2.0;
float fadeIn  = smoothstep(0.0, 0.06, age);
float fadeOut = 1.0 - smoothstep(0.7, 1.0, lifeN);
float env = fadeIn * fadeOut;
// v7.1: 把幅度放大 — 中心凸起更高更宽
float bulgeW = 0.55 + age * 0.80;
float bulge  = exp(-dist*dist / (2.0 * bulgeW * bulgeW)) * (1.0 - smoothstep(0.0, 0.55, lifeN));
float waveR  = age * 2.10;
float ringW  = 0.40 + age * 0.22;
float ring   = exp(-pow((dist - waveR) / ringW, 2.0));
// v7.1: 提升整体幅度 ×2
float local  = (bulge * 2.4 + ring * 1.30) * env * str;
sum += local;
maxAmp = max(maxAmp, abs(local));
  }
  return sum;
}

Particle SourceParticle(float2 aUv,float aRand,float3 position){
 Particle o=(Particle)0;float3 vColor;float vBright,vRipple,vEdgeBoost,vAlpha,vSourceLum;
  float t = uTime * uSpeed;
  float3 pos;
  float2 sampleUv = aUv;
  // 切歌颜色渐变: 在新旧封面间 mix
  float3 newCol = sampleNewCoverColor(sampleUv);
  float3 prevCol = samplePrevCoverColor(sampleUv);
  float3 coverColor = lerp(prevCol, newCol, clamp(uColorMixT, 0.0, 1.0));
  float4 edge = sampleEdgeColor(sampleUv);
  float depthVal = edge.r;
  float edgeVal  = edge.g;
  float fgMask   = edge.b;
  float lumVal   = edge.a;
  float maxRippleAmp = 0.0;
  float rippleZ = 0.0;

  float3 defaultColor = lerp(
V3(0.36, 0.28, 0.72),
lerp(V3(0.85, 0.55, 0.95), V3(0.45, 0.78, 0.95), aUv.x),
aUv.y
  );
  vColor = lerp(defaultColor, coverColor, uHasCover);
  vAlpha = 1.0;

  // 律动强度的真实倍数 (放大 intensity 滑块的影响)
  float K = uIntensity * 1.6;   // 滑块 1.0 → K=1.6, 滑块 1.6 → K=2.56

  // ====================================================
  //  Preset 0: SILK — 丝绸 (xy 平面, z 涟漪)
  //  v7.1: 全部位移 ×2.5
  // ====================================================
  if (uPreset < 0.5) {
pos = position;
rippleZ = rippleSumAt(pos.xy, maxRippleAmp);

float midN = snoise(V3(pos.x*1.4, pos.y*1.4, t*0.55)) * 0.6
           + snoise(V3(pos.x*2.8+5.0, pos.y*2.8-3.0, t*0.85)) * 0.4;
float midMask = 0.55 + 0.45 * snoise(V3(pos.x*0.4, pos.y*0.4, t*0.18));
float midDisp = midN * uMid * 0.55 * midMask * K;       // 0.20 → 0.55

float trebleJ = snoise(V3(pos.x*6.5, pos.y*6.5, t*3.5 + aRand*4.0)) * uTreble * 0.18 * K;  // 0.06→0.18
float bassBreath = snoise(V3(pos.x*0.35, pos.y*0.35, t*0.4)) * uBass * 0.42 * K;          // 0.14→0.42

// AI 深度: 显著强化 (0.85 → 1.4)
float depthZ = (depthVal - 0.5) * uAiBoost * uDepth * 1.40 * uHasDepth;

pos.z = rippleZ * 1.30 + midDisp + trebleJ + bassBreath + depthZ;
  }

  // ====================================================
  //  Preset 1: TUNNEL — 隧道 + 自旋
  // ====================================================
  else if (uPreset < 1.5) {
// v7.1: 整体自旋 — 整管缓慢绕 Z 轴
float spin = t * 0.12;
float angle = aUv.x * 2.0 * PI + spin;
float flow = aUv.y - t * 0.08 * (1.0 + uBass * 0.55);
flow = frac(flow);
float zPos = (flow - 0.5) * 9.0;
float baseR = 2.0 - uBass * 0.28 * K;                  // bass 收缩更明显
float ripG  = sin(angle * 5.0 + zPos * 1.4 + t * 2.2) * 0.10 * (uMid + uTreble) * K;   // 0.04→0.10
float r = baseR + ripG;
pos.x = cos(angle) * r;
pos.y = sin(angle) * r;
pos.z = zPos;

sampleUv = V2(aUv.x, flow);
sampleUv = clamp(sampleUv, V2(0.0012), V2(0.9988));
newCol = sampleNewCoverColor(sampleUv);
prevCol = samplePrevCoverColor(sampleUv);
coverColor = lerp(prevCol, newCol, clamp(uColorMixT, 0.0, 1.0));
vColor = lerp(defaultColor, coverColor, uHasCover);

float depthFade = smoothstep(-4.5, 4.5, zPos);
vColor *= 0.4 + depthFade * 0.7;
  }

  // ====================================================
  //  Preset 2: ORBIT — 星球 (保留自转)
  //  v7.1: 律动幅度加大
  // ====================================================


  // ====================================================
  //  Preset 3: VOID — 虚空 (无粒子, 适合自定义背景)
  // ====================================================
  else if (uPreset < 3.5) {
pos = V3((aUv.x - 0.5) * 0.01, (aUv.y - 0.5) * 0.01, -90.0);
vAlpha = 0.0;
vColor = V3(0.0);
maxRippleAmp = 0.0;
  }

  // ====================================================
  //  Preset 4: VINYL RECORD
  //  A real record layout: circular album cover in the center, black vinyl
  //  grooves outside, and a complete white particle rim.
  // ====================================================
  else if (uPreset < 4.5) {
float bassDrive = smoothstep(0.08, 0.78, uBass + uBeat * 0.82);
float highDrive = smoothstep(0.05, 0.46, uTreble);
float hiResGuard = smoothstep(1.08, 1.55, uCoverRes);
float edgeGuard = lerp(1.0, 0.38, hiResGuard);
float depthGuard = lerp(1.0, 0.44, hiResGuard);
float grooveGuard = lerp(1.0, 0.48, hiResGuard);
float beatGuard = lerp(1.0, 0.36, hiResGuard);

float2 p = (aUv - 0.5) * 5.12;
float spin = uVinylSpin;
float cs = cos(spin), sn = sin(spin);
float2 rp = mul(p,float2x2(cs,-sn,sn,cs));
float d = length(p);
float angle0 = atan2(p.y,p.x);
float recordR = 2.46;
float coverR = 1.18;
float recordAlpha = 1.0 - smoothstep(recordR - 0.02, recordR + 0.05, d);
float coverMask = 1.0 - smoothstep(coverR - 0.012, coverR + 0.018, d);
float border = exp(-pow((d - coverR) / 0.064, 2.0)) * edgeGuard;
float outerRim = exp(-pow((d - (recordR - 0.050)) / 0.055, 2.0)) * edgeGuard;
float vinylN = clamp((d - coverR) / max(0.001, recordR - coverR), 0.0, 1.0);

pos = V3(rp * (1.0 + bassDrive * 0.012 * beatGuard + uBeat * 0.026 * beatGuard), 0.0);
vAlpha = recordAlpha;

if (coverMask > 0.02) {
  float2 coverUv = p / (coverR * 2.0) + 0.5;
  newCol = sampleNewCoverColor(coverUv);
  prevCol = samplePrevCoverColor(coverUv);
  coverColor = lerp(prevCol, newCol, clamp(uColorMixT, 0.0, 1.0));
  if (hiResGuard > 0.001) {
    float2 sx = V2(0.0026, 0.0);
    float2 sy = V2(0.0, 0.0026);
    float3 softNew = (sampleNewCoverColor(coverUv + sx) + sampleNewCoverColor(coverUv - sx) + sampleNewCoverColor(coverUv + sy) + sampleNewCoverColor(coverUv - sy)) * 0.25;
    float3 softPrev = (samplePrevCoverColor(coverUv + sx) + samplePrevCoverColor(coverUv - sx) + samplePrevCoverColor(coverUv + sy) + samplePrevCoverColor(coverUv - sy)) * 0.25;
    coverColor = lerp(coverColor, lerp(softPrev, softNew, clamp(uColorMixT, 0.0, 1.0)), hiResGuard * 0.42);
  }
  vColor = lerp(defaultColor, coverColor, uHasCover);
  float coverShade = 1.02 + 0.10 * (1.0 - smoothstep(0.0, coverR, d));
  vColor *= coverShade;
  vColor = lerp(vColor, V3(1.0), border * 0.54);
  pos.z = 0.040 + border * 0.026 * depthGuard + uBeat * 0.018 * beatGuard;
  maxRippleAmp = max(maxRippleAmp, border * 0.30 + bassDrive * 0.075 * beatGuard + uBeat * 0.075 * beatGuard);
} else {
  float groove = 0.5 + 0.5 * sin((d - coverR) * lerp(98.0, 58.0, hiResGuard));
  float fineGroove = 0.5 + 0.5 * sin((d - coverR) * lerp(170.0, 92.0, hiResGuard) + aRand * 3.0);
  float tick = smoothstep(0.82, 0.995, hash11(floor((angle0 + PI) * 38.0) + floor(d * 72.0) * 2.1));
  float3 vinyl = V3(0.052, 0.054, 0.058) + V3(0.052 * grooveGuard) * groove + V3(0.026 * grooveGuard) * fineGroove;
  vinyl = lerp(vinyl, coverColor * 0.32, 0.18 * (1.0 - vinylN));
  float whiteRing = max(border * 0.92, outerRim * 0.26);
  vColor = lerp(vinyl, V3(0.92, 0.94, 0.94), whiteRing);
  vColor = lerp(vColor, V3(1.0), tick * highDrive * (0.06 + border * 0.12) * grooveGuard);
  pos.z = groove * 0.010 * grooveGuard + border * 0.024 * depthGuard + bassDrive * vinylN * 0.016 * K * beatGuard + tick * highDrive * 0.010 * grooveGuard;
  maxRippleAmp = max(maxRippleAmp, border * 0.32 + outerRim * 0.12 + bassDrive * vinylN * 0.11 * beatGuard + tick * highDrive * 0.10 * grooveGuard + uBeat * vinylN * 0.08 * beatGuard);
}
  }

  // ====================================================
  //  Preset 5: WALLPAPER PULSE
  //  Layered music-particle wallpaper: aurora ribbons, depth sparks,
  //  and cover-colored audio flow.
  // ====================================================
  else {
float bassGlow = smoothstep(0.07, 0.78, uBass) * 0.34 + uBeat * 0.014;
float midGlow = smoothstep(0.07, 0.62, uMid) * 0.42;
float highGlow = smoothstep(0.04, 0.46, uTreble) * 0.46;
float lane = aUv.y;
float transition = clamp(uBurstAmt, 0.0, 1.0);

if (lane < 0.80) {
  float laneWarp = snoise(V3(aUv.x * 0.42, lane * 1.7, t * 0.026)) * 0.11 + (hash11(aRand * 73.1) - 0.5) * 0.045;
  float warpedLane = clamp(lane + laneWarp, 0.0, 0.80);
  float bandCoord = warpedLane / 0.80 * 5.65 + snoise(V3(aUv.x * 0.82, lane * 2.25, t * 0.032)) * 0.62;
  float band = floor(bandCoord);
  float local = frac(bandCoord + hash11(band * 9.13 + aRand * 2.4) * 0.18);
  float bandN = clamp((band + 0.5) / 5.65, 0.0, 1.0);
  float seed = hash11(band * 19.17 + aRand * 31.0);
  float flow = frac(aUv.x + t * (0.0034 + bandN * 0.0038 + seed * 0.0022) + seed * 0.53);
  float arc = (flow - 0.5) * PI * (1.35 + bandN * 0.72 + seed * 0.24);
  float armCurve = sin(arc + bandN * 2.2 + seed * 5.3);
  float spiralRadius = 9.2 + bandN * 11.8 + seed * 6.0 + local * 2.9;
  float x = cos(arc * 0.72 + bandN * 0.92 + seed * 1.3) * spiralRadius + (flow - 0.5) * (13.5 + bandN * 9.5);
  float ribbonPhase = flow * PI * 2.0 * (0.55 + bandN * 0.24 + seed * 0.10) + t * (0.010 + bandN * 0.007) + seed * 5.7;
  float broadWave = sin(ribbonPhase) * 0.92;
  float fineWave = sin(ribbonPhase * (1.36 + seed * 0.62) - t * 0.044 + seed * 5.0) * 0.045;
  float yBase = (bandN - 0.5) * 13.2 + armCurve * (2.3 + bandN * 1.6) + (seed - 0.5) * 1.85 + snoise(V3(bandN * 2.0, flow * 0.62, seed)) * 0.92;
  float ridgeCenter = 0.43 + (seed - 0.5) * 0.18;
  float ridge = exp(-pow((local - ridgeCenter) / (0.25 + seed * 0.04), 2.0));
  float softMask = smoothstep(0.010, 0.12, lane) * (1.0 - smoothstep(0.72, 0.81, lane));
  float ribbonNoise = snoise(V3(flow * 1.18 + seed, bandN * 2.0, t * 0.018)) * 0.74;
  float zLayer = lerp(-23.5, 15.5, bandN) + (seed - 0.5) * 6.0;

  pos.x = x + ribbonNoise * 1.40 + sin(t * 0.012 + seed * 8.0) * 0.22;
  pos.y = yBase + broadWave + fineWave + (local - 0.5) * (0.58 + ridge * 0.14);
  pos.z = zLayer + broadWave * 1.35 + ribbonNoise * 1.85;

  float pulseLine = 0.5 + 0.5 * sin(ribbonPhase * (1.7 + seed * 0.9) - t * 0.32 + seed * 6.0);
  float3 aurora = lerp(V3(0.52, 0.86, 1.0), V3(0.70, 0.58, 1.0), bandN);
  aurora = lerp(aurora, V3(0.96, 0.98, 0.92), bassGlow * 0.05);
  vAlpha = (0.18 + ridge * 0.78 + pulseLine * highGlow * 0.035 + bassGlow * 0.025) * softMask * (0.96 + transition * 0.02);
  vColor = lerp(coverColor, aurora, 0.62 + ridge * 0.22) * (0.76 + ridge * 0.86 + pulseLine * highGlow * 0.05 + bassGlow * 0.04);
  maxRippleAmp = max(maxRippleAmp, ridge * (0.12 + midGlow * 0.05) + pulseLine * highGlow * 0.045 + bassGlow * 0.030);
} else {
  float q = (lane - 0.80) / 0.20;
  float seed = hash11(aRand * 917.0 + floor(q * 130.0));
  float depth = lerp(-32.0, 18.0, seed);
  float drift = frac(aUv.x + t * (0.0014 + seed * 0.0048) + seed * 0.63);
  float cluster = snoise(V3(seed * 2.0, q * 3.2, t * 0.007));
  float x = (drift - 0.5) * (45.0 + seed * 22.0) + cluster * 3.4;
  float y = (hash11(aRand * 331.0 + seed * 5.0) - 0.5) * 22.0 + sin(t * (0.018 + seed * 0.028) + seed * 7.0) * 0.86;
  float z = depth + sin(t * (0.020 + seed * 0.032) + aRand * 8.0) * 1.05;
  float twinkle = pow(0.5 + 0.5 * sin(t * (0.24 + seed * 0.42) + aRand * 17.0), 5.0);
  float dust = smoothstep(0.22, 0.98, hash11(aRand * 661.0 + floor(q * 160.0)));

  pos = V3(x, y, z);
  vAlpha = dust * (0.16 + twinkle * 0.46 + highGlow * 0.025 + bassGlow * 0.018) * (1.0 - q * 0.06);
  vColor = lerp(coverColor, V3(0.92, 0.97, 1.0), 0.62 + twinkle * 0.14) * (0.72 + twinkle * 0.62 + bassGlow * 0.025);
  maxRippleAmp = max(maxRippleAmp, twinkle * highGlow * 0.055 + dust * bassGlow * 0.030);
}

if (transition > 0.001) {
  float bloom = smoothstep(0.0, 1.0, transition);
  float2 burstVec = pos.xy + V2(hash11(aRand * 31.0) - 0.5, hash11(aRand * 47.0) - 0.5) * 0.75;
  float2 burstDir = burstVec / max(length(burstVec), 0.001);
  pos.xy += burstDir * bloom * 0.026;
  pos.xy += V2(snoise(V3(aRand, t * 0.014, 1.0)), snoise(V3(aRand, t * 0.014, 5.0))) * bloom * 0.06;
  pos.xy *= 1.0 + bloom * 0.014;
  pos.z += (hash11(aRand * 123.0) - 0.5) * bloom * 0.18;
  vAlpha *= 0.86 + bloom * 0.22;
  maxRippleAmp = max(maxRippleAmp, bloom * 0.10);
}
  }

  // ====================================================
  //  鼠标交互 (仅 SILK)
  // ====================================================
  if (uMouseActive > 0.5 && uPreset < 0.5) {
float mdx = pos.x - uMouseXY.x;
float mdy = pos.y - uMouseXY.y;
float md = sqrt(mdx*mdx + mdy*mdy);
if (md < 1.0) {
  float push = (1.0 - md) * (1.0 - md);
  pos.z += push * 0.55;
}
  }

  // ====================================================
  //  v8 手势遮挡 — uHandActive 是 0..1 平滑过渡, 大半径推开
  // ====================================================
  if (uHandActive > 0.01) {
float hdx = pos.x - uHandXY.x;
float hdy = pos.y - uHandXY.y;
float hd = sqrt(hdx*hdx + hdy*hdy);
float rad = 1.55;
if (hd < rad) {
  float push = (rad - hd) / rad;
  push = push * push * uHandActive;
  pos.z += push * 1.10;
  float2 outDir = V2(hdx, hdy) / max(0.001, hd);
  pos.xy += outDir * push * 0.28;
}
  }
  if (uGestureGrip > 0.001) {
float grip = clamp(uGestureGrip, 0.0, 1.0);
float gripWave = 0.5 + 0.5 * sin(uTime * 2.2 + aRand * 6.2831);
pos.xy *= lerp(1.0, 0.66 + gripWave * 0.035, grip);
pos.z += grip * (0.18 + uBass * 0.22 + gripWave * 0.10);
  }

  // ====================================================
  //  通用: 离散感 / 扭曲
  // ====================================================
  if (uScatter > 0.001) {
float2 jdir = V2(cos(aRand * 6.2831), sin(aRand * 6.2831));
pos.xy += jdir * uScatter * (0.05 + uTreble * 0.10);
  }
  if (uTwist > 0.001 && uPreset < 0.5) {
float ta = uTwist * pos.z * 0.6;
float cs = cos(ta), sn = sin(ta);
pos.xy = mul(pos.xy,float2x2(cs,-sn,sn,cs));
  }

  // 颜色
  float vinylHiResGuard = smoothstep(1.08, 1.55, uCoverRes) * step(3.5, uPreset) * (1.0 - step(4.5, uPreset));
  float edgeBoost = uEdgeEnabled * edgeVal * lerp(1.0, 0.42, vinylHiResGuard);
  vSourceLum = dot(max(vColor, V3(0.0)), V3(0.299, 0.587, 0.114));
  float blackParticleGuard = 1.0 - smoothstep(0.025, 0.115, vSourceLum);
  vEdgeBoost = edgeBoost * (uPreset > 3.5 ? 0.22 : 1.0) * (1.0 - blackParticleGuard);
  vColor = pow(max(vColor, V3(0.0)), V3(1.0 / max(0.35, uColorBoost)));
  float edgeColorMix = edgeBoost * (uPreset > 3.5 ? 0.20 : 0.50) * (1.0 - blackParticleGuard);
  vColor = lerp(vColor, vColor + V3(0.20), edgeColorMix);
  float tintLum = max(max(vColor.r, vColor.g), vColor.b);
  float3 tintedColor = uTintColor * max(0.24, tintLum * 1.12);
  vColor = lerp(vColor, tintedColor, clamp(uTintStrength, 0.0, 1.0) * (1.0 - blackParticleGuard));

  vBright = 0.82 + maxRippleAmp * 0.55 + uBass * 0.10 + edgeBoost * 0.30 + uEnergy * 0.05 + uBurstAmt * 0.40;
  if (uPreset > 4.5) {
vBright = 0.94 + maxRippleAmp * 0.34 + uBass * 0.020 + uEnergy * 0.026 + uBurstAmt * 0.025;
if (uPreset > 8.5) vBright = 0.86 + maxRippleAmp * 0.52 + uEnergy * 0.045 + uBeat * 0.055;
  } else if (uPreset > 3.5) {
vBright = 0.94 + maxRippleAmp * 0.64 + uBass * 0.08 + edgeBoost * 0.12 + uEnergy * 0.05 + uBeat * 0.16 + uBurstAmt * 0.16;
  }
  vRipple = clamp(maxRippleAmp * 1.5, 0.0, 1.0);

  if (uHasDepth > 0.5 && uPreset < 0.5) {
float bgMul = lerp(1.0, 0.55, uBgFade * (1.0 - fgMask));
vBright *= bgMul;
  }
  vBright += uGestureGrip * 0.22;
  float loadingMistSize = 1.0;

  // 加载形态: 雾状微尘流，避免廉价旋转圆环
  if (uLoading > 0.001) {
float mistSeed = hash11(aRand * 931.7);
float mistLayer = floor(mistSeed * 4.0);
float layerN = (mistLayer + 0.5) / 4.0;
float mistAngle = aRand * 6.2831 + uTime * (0.16 + mistSeed * 0.18) + snoise(V3(aRand * 2.1, uTime * 0.24, 2.0)) * 1.85;
float mistR = lerp(1.35, 3.15, sqrt(hash11(aRand * 127.3))) * (1.0 + sin(uTime * 0.42 + aRand * 7.0) * 0.13);
float2 mistCurl = V2(
  snoise(V3(aRand * 4.1, uTime * 0.32, 3.0)),
  snoise(V3(aRand * 4.7, uTime * 0.30, 8.0))
);
float mistBreath = 0.5 + 0.5 * sin(uTime * (0.82 + mistSeed * 0.55) + aRand * 17.0);
float mistRibbon = sin(mistAngle * (1.35 + layerN * 0.55) + uTime * 0.34 + mistSeed * 4.0);
float glowPick = smoothstep(0.88, 0.997, hash11(aRand * 1501.0 + mistLayer * 17.0));
float dustPick = 0.34 + glowPick * 0.66;
float3 mistPos = V3(
  cos(mistAngle) * mistR * (1.24 + mistCurl.x * 0.16) + mistCurl.x * 0.72,
  sin(mistAngle * 0.82 + mistRibbon * 0.25) * mistR * (0.56 + layerN * 0.10) + mistCurl.y * 0.62,
  (layerN - 0.5) * 4.85 + mistCurl.x * 0.56 + mistBreath * 0.36 + mistRibbon * 0.24
);
float3 mistCol = lerp(V3(0.62, 0.86, 0.84), V3(0.36, 0.46, 0.78), mistSeed);
mistCol = lerp(mistCol, V3(0.94, 1.0, 0.97), glowPick * (0.45 + mistBreath * 0.35));
vColor = lerp(vColor, mistCol, uLoading * 0.78);
vBright = lerp(vBright, 0.20 + mistBreath * 0.18 + abs(mistCurl.x) * 0.06 + glowPick * (0.72 + abs(mistRibbon) * 0.24), uLoading);
vAlpha = lerp(vAlpha, 0.08 + mistBreath * 0.11 + dustPick * 0.11 + glowPick * 0.30, uLoading);
pos = lerp(pos, mistPos, uLoading);
loadingMistSize = 1.26 + mistBreath * 0.24 + abs(mistRibbon) * 0.14 + glowPick * 0.78;
  }

  float4 mvPos = V4(ViewPoint(pos),1);
  float depthSize = 36.0 / max(0.5, -mvPos.z);
  float audioBoost = 1.0 + maxRippleAmp * 0.7 + edgeBoost * 0.55 + uBeat * 0.30 + uBurstAmt * 0.5;
  float sz = clamp(depthSize * audioBoost, 1.05, 4.95);
  if (uPreset > 4.5) {
float flowDrive = uBass * 0.070 + uMid * 0.046 + uTreble * 0.060 + uBurstAmt * 0.090 + uBeat * 0.055;
sz = clamp(depthSize * (1.05 + flowDrive), 1.00, 5.45);
if (uPreset > 8.5) {
  float authoredDrive = uBass * 0.10 + uMid * 0.08 + uTreble * 0.12 + uBeat * 0.10;
  sz = clamp(depthSize * (0.98 + authoredDrive), 1.00, 4.85);
}
  } else if (uPreset > 3.5) {
float ringDrive = uBass * 0.30 + uMid * 0.18 + uTreble * 0.22 + uBeat * 0.30;
sz = clamp(depthSize * (0.90 + ringDrive * 0.62), 1.05, 3.90);
  }
  // 加载态下粒子稍大
  sz = lerp(sz, sz * loadingMistSize, uLoading);
  o.size=sz*uPixel*uPointScale;
  o.pos=pos;o.col=vColor;o.bright=vBright;o.ripple=vRipple;o.edge=vEdgeBoost;o.alpha=vAlpha;o.lum=vSourceLum;return o;
}

Pixel MineradioVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID){
    uint grid=(uint)pointer.w;float2 uv=(float2(instance%grid,instance/grid)+.5)/grid;
    float rnd=hash11(instance+1);Particle src=SourceParticle(uv,rnd,float3((uv-.5)*5.12,0));
    float3 vp=ViewPoint(src.pos);float z=max(.05,-vp.z);float f=1/tan(cameraEye.w*.5);
    float2 corners[6]={float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)};
    float2 corner=corners[vertex];float2 ndc=float2(vp.x*f/(frame.x/frame.y),vp.y*f)/z;
    Pixel p;p.position=float4(ndc+corner*src.size/frame.xy,clamp((z-.05)/150,0,1),1);
    if(vp.z>-.05)p.position=float4(-10,-10,0,1);
    p.uv=corner*.5+.5;p.color=src.col;p.values=float4(src.bright,src.ripple,src.edge,src.alpha);p.lum=src.lum;return p;
}


float4 MineradioPS(Pixel p):SV_TARGET{
 float3 vColor=p.color;float vBright=p.values.x,vRipple=p.values.y,vEdgeBoost=p.values.z,vAlpha=p.values.w,vSourceLum=p.lum;float2 gl_PointCoord=p.uv;
  float dist=length(p.uv-.5)*2;float a=exp(-dist*dist*3.8)*saturate((1-dist)*6);float4 tex=float4(1,1,1,a);
  if (tex.a < 0.02) discard;
  float3 col = vColor * vBright;
  col = lerp(col, col * 1.3 + V3(0.05), vEdgeBoost * 0.35);
  col = lerp(col, col * 1.2, vRipple * 0.4);
  float keepBlack = 1.0 - smoothstep(0.025, 0.115, vSourceLum);
  float nonBlack = 1.0 - keepBlack;
  float dotDist = length(gl_PointCoord - V2(0.5)) * 2.0;
  float readableRim = smoothstep(0.44, 0.94, dotDist) * (1.0 - smoothstep(0.94, 1.08, dotDist)) * tex.a;
  float outLum = dot(col, V3(0.299, 0.587, 0.114));
  float lightParticle = smoothstep(0.50, 0.82, outLum) * nonBlack;
  float darkParticle = (1.0 - smoothstep(0.20, 0.50, outLum)) * nonBlack;
  float backdropAdapt = clamp(uBackdropAdapt, 0.0, 1.0);
  if (backdropAdapt > 0.001) {
    col = lerp(col, V3(0.0), readableRim * lightParticle * (0.38 + backdropAdapt * 0.30));
    col = lerp(col, V3(1.0), readableRim * darkParticle * (0.20 + backdropAdapt * 0.12));
  }
  col = clamp(col, V3(0.0), V3(1.6));
  float alpha=saturate(tex.a*uAlpha*uParticleDim*vAlpha);return float4(col*alpha,alpha);
}

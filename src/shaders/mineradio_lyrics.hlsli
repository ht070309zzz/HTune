// SPDX-License-Identifier: GPL-3.0-only
// Mineradio contributors, d43de565acabfdc1a9c9820a27e81a98ccbebcef.
// 02-visual/11-lyrics-shaders.js, native atlas I/O and six upstream motion profiles.
float2 V2(float a,float b){return float2(a,b);} float3 V3(float a){return a.xxx;} float3 V3(float a,float b,float c){return float3(a,b,c);}float hash(float n){ return frac(sin(n) * 43758.5453123); }
float hash2(float2 p){ return frac(sin(dot(p, V2(127.1,311.7))) * 43758.5453123); }
float4 MineradioLyrics(Pixel input){
  int style=(int)input.effect.x-4;float uTime=lyricMotion.x,uProgress=input.effect.y,uTextMin=0,uTextMax=1,uOpacity=input.color.a,uFeather=.03;
  float uSolar=lyricMotion.y;
  float uGlitch=style==5?lyricMotion.z:0,uGlitchSlice=1,uGlitchChroma=1,uGlitchRate=1,uGlitchSeed=lyricMotion.w,uGlitchBurst=lyricMotion.y;
  float uActiveMix=1;
  float3 uBaseColor=input.color.rgb*.48,uHiColor=input.karaoke.x>=0?lyricAccent.rgb:input.color.rgb,uSolarColor=float3(1,.88,.64);
  float2 vUv=input.local;float2 dx=ddx(input.local),dy=ddy(input.local),ax=ddx(input.uv),ay=ddy(input.uv);float det=dx.x*dy.y-dx.y*dy.x;
  float2 du=(ax*dy.y-ay*dx.y)/det,dv=(ay*dx.x-ax*dy.x)/det;
  float2 uv = vUv;
  float sliceRows = lerp(16.0, 38.0, clamp(uGlitchSlice / 1.4, 0.0, 1.0));
  float row = floor((uv.y + hash(uGlitchSeed) * 0.035) * sliceRows);
  float timeSlot = floor(uTime * lerp(7.0, 24.0, clamp(uGlitchRate / 2.2, 0.0, 1.0)) + hash(uGlitchSeed * 1.37) * 5.0);
  float rowRnd = hash2(V2(row + uGlitchSeed, timeSlot));
  float phaseRnd = hash2(V2(timeSlot + uGlitchSeed * 0.71, row * 3.17));
  float glitchGate = smoothstep(0.74, 0.99, rowRnd + uGlitchBurst * 0.28) * step(0.001, uGlitch);
  float glitchDir = hash2(V2(row * 5.11, timeSlot + uGlitchSeed)) < 0.5 ? -1.0 : 1.0;
  float micro = hash2(V2(floor(uv.x * 19.0) + row, timeSlot * 1.31 + uGlitchSeed));
  float glitchWave = (phaseRnd * 2.0 - 1.0) * (0.55 + micro * 0.95);
  float glitchWidth = (0.0020 + rowRnd * rowRnd * 0.0085) * (0.55 + uGlitchBurst * 1.85);
  float2 sampleUv = uv + V2(glitchGate * glitchDir * glitchWave * uGlitch * uGlitchSlice * glitchWidth, 0.0);
  float mask = glyphAtlas.Sample(linearClamp,input.uv+(sampleUv-input.local).x*du+(sampleUv-input.local).y*dv).a;
  if(mask < 0.01) discard;
  float activeMix = clamp(uActiveMix, 0.0, 1.0);
  float denom = max(0.001, uTextMax - uTextMin);
  float p = clamp((uv.x - uTextMin) / denom, 0.0, 1.0);
  float filled = (1.0 - smoothstep(uProgress, uProgress + uFeather, p)) * activeMix;
  if(input.karaoke.x>=0)filled=KaraokeFill(input);
  float chromaOffset = (0.0028 + phaseRnd * 0.0048 + uGlitchBurst * 0.0038) * uGlitch * uGlitchChroma;
  float chromaR = glyphAtlas.Sample(linearClamp,input.uv+(sampleUv + V2(chromaOffset * glitchDir, 0.0)-input.local).x*du+(sampleUv + V2(chromaOffset * glitchDir, 0.0)-input.local).y*dv).a;
  float chromaB = glyphAtlas.Sample(linearClamp,input.uv+(sampleUv - V2(chromaOffset * glitchDir, 0.0)-input.local).x*du+(sampleUv - V2(chromaOffset * glitchDir, 0.0)-input.local).y*dv).a;
  float3 color = lerp(uBaseColor, uHiColor, filled * 0.88);
  // Native adaptation: colour fill remains; decorative scans/bright bands in
  // the strokes are removed. External glyph particles use a separate shader.
  color += V3(chromaR, mask * 0.18, chromaB) * glitchGate * uGlitch * uGlitchChroma * activeMix * (0.20 + uGlitchBurst * 0.22);
  float3 solar = uSolarColor;
  color = lerp(color, color + solar * 0.34, uSolar * activeMix * (0.25 + filled * 0.45));
  float lum = dot(color, V3(0.299, 0.587, 0.114));
  color += V3(max(0.0, 0.30 - lum));
  float alpha = max(mask, max(chromaR, chromaB) * glitchGate * uGlitch * (0.30 + uGlitchBurst * 0.32));
  alpha=saturate(alpha*uOpacity);return float4(color*alpha,alpha);
}

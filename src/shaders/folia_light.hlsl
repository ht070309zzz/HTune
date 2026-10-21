// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 chthollyphile. Native HLSL adaptation by HTune contributors.
// Folia 26c57f9b1b26b402a09e8943171a332215418ce6 / lumiere/light/lightFieldShader.ts.
// Changes: HLSL I/O/constant layout, constructor splats and GLSL matrix orientation.
cbuffer LumiereConstants:register(b0){
    float4 uBeamA[6];
    float4 uBeamB[6];
    float4 uBeamC[6];
    float4 uBeamD[6];
    float4 uBeamE[6];
    float4 uBeamF[6];
    float4 uFrame;
    float4 uFogA;
    float4 uFogB;
    float3 uFogColor;
    float4 uGlare;
    float4 uGlareB;
    float3 uGlareColor;
    float4 uDark;
    float4 uCaustic;
    float4 uCausticFloor;
    float4 uCausticB;
    float4 uWave;
    float4 uWaveB;
    float4 uShield;
    float4 uShieldB;
    float4 uMotion;
};

struct LumierePixel {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
LumierePixel LumiereVS(uint id:SV_VertexID){LumierePixel p;p.uv=float2((id<<1)&2,id&2);p.position=float4(p.uv*float2(2,-2)+float2(-1,1),0,1);return p;}

float2 V2(float a){return a.xx;} float2 V2(float a,float b){return float2(a,b);} float2 V2(float2 a){return a;}
float3 V3(float a){return a.xxx;} float3 V3(float3 a){return a;} float3 V3(float a,float b,float c){return float3(a,b,c);} float3 V3(float2 a,float b){return float3(a,b);} float3 V3(float a,float2 b){return float3(a,b);}
float4 V4(float a){return a.xxxx;} float4 V4(float4 a){return a;} float4 V4(float a,float b,float c,float d){return float4(a,b,c,d);} float4 V4(float3 a,float b){return float4(a,b);} float4 V4(float2 a,float2 b){return float4(a,b);}


#define MAX_BEAMS 6


float hash11(float x) {
    float p = frac(x * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return frac(p);
}

float hash12(float2 p) {
    float3 p3 = frac(V3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float valueNoise1(float x) {
    float i = floor(x);
    float f = x - i;
    float u = f * f * (3.0 - 2.0 * f);
    return lerp(hash11(i), hash11(i + 1.0), u);
}

float valueNoise2(float2 p) {
    float2 i = floor(p);
    float2 f = p - i;
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + V2(1.0, 0.0));
    float c = hash12(i + V2(0.0, 1.0));
    float d = hash12(i + V2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm(float2 p, float octaves) {
    float sum = 0.0;
    float amp = 0.5;
    float norm = 0.0;
    float2x2 rot = float2x2(0.8, -0.6, 0.6, 0.8);
    for (int i = 0; i < 6; i++) {
        if (float(i) >= octaves) break;
        sum += amp * valueNoise2(p);
        norm += amp;
        p = mul(p,rot) * 2.03 + V2(17.1, 9.2);
        amp *= 0.5;
    }
    return sum / max(norm, 1e-4);
}

float fog(float2 p, float t) {
    float2 drift = uFogB.xy * t;
    float2 q = (p - drift) * uFogA.z;
    float octaves = uFogB.w;
    float2 w = V2(
        fbm(q + V2(0.0, t * 0.07), 3.0),
        fbm(q + V2(5.2, 1.3) - V2(t * 0.05, 0.0), 3.0)
    );
    float n = fbm(q + uFogA.w * (w - 0.5) * 2.0, octaves);
    // 拉开对比：烟是一缕一缕的，不是均匀的灰。
    return smoothstep(0.28, 0.82, n);
}

// 透光条：frac(x) 落在 [0, duty) 里为 1，边缘柔化（与 rig.ts 的 band 相同）。
float band(float x, float duty) {
    float f = frac(x);
    return min(1.0, 1.0 - smoothstep(duty - 0.06, duty + 0.06, f) + smoothstep(0.94, 1.0, f));
}

// 窗影图样：u 为截面横向位置 -1..1，v 为沿光束的距离。
float goboPattern(float4 E, float u, float v) {
    float pattern = E.x;
    if (pattern < 1.5) return band(u * E.y * 0.5 + E.w, E.z);
    if (pattern < 2.5) return smoothstep(E.z * 0.5, E.z * 0.5 + 0.08, abs(u));
    if (pattern < 3.5) return band(u * E.y * 0.5 + v * E.y * 0.8 + E.w, E.z) * band(u * E.y * 0.5 - v * E.y * 0.8 - E.w, E.z);
    return smoothstep(E.z - 0.12, E.z + 0.12, valueNoise2(V2(u * E.y, v * E.y * 0.6 + E.w)));
}

// 窗影（与 rig.ts 的 goboMask 相同）：影子里仍透过 12% 的散射光。
float goboMask(float4 E, float u, float v) {
    if (E.x < 0.5) return 1.0;
    return 0.12 + 0.88 * goboPattern(E, u, v);
}

// 数组下标只能用循环变量（GLSL ES 1.0 的限制），所以按分量传进来。返回 (强度, 截面横向位置 -1..1)。
float2 beamMask(float4 A, float4 B, float4 C, float coreAmount, float4 E, float reach, float2 p) {
    float2 d = p - A.xy;
    float along = dot(d, A.zw);
    if (along <= 0.0) return V2(0.0);
    float signedPerp = d.x * -A.w + d.y * A.z;
    // 取绝对值：会聚的光束过了焦点再散开。
    float half_ = abs(B.x + along * B.y) + 1e-4;
    float s = abs(signedPerp) / half_;
    float edge = smoothstep(1.0, 1.0 - max(B.z, 0.02), s);
    if (edge <= 0.0) return V2(0.0);
    float signedS = s * sign(signedPerp);
    float streak = 1.0 - C.y + C.y * valueNoise1(signedS * C.z + C.w);
    float core = 1.0 - coreAmount + coreAmount * (1.0 - s * s);
    float falloff = exp(-along / max(B.w, 0.001));
    float reachMask = reach > 0.0 ? 1.0 - smoothstep(reach - max(0.04, half_ * 2.5), reach, along) : 1.0;
    float start = smoothstep(0.0, abs(B.x) * 2.0 + 0.01, along);
    return V2(edge * streak * core * falloff * goboMask(E, signedS, along) * reachMask * start * C.x, signedS);
}

// 色散：截面横向位置 -> 光谱色（红在一侧、紫在另一侧）。
float3 spectrumColor(float u) {
    float h = clamp(0.5 + 0.5 * u, 0.0, 1.0) * 0.8;
    float3 k = clamp(abs(frac(V3(h) + V3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return lerp(V3(1.0), k, 0.85);
}

// 焦散（水面焦散的经典迭代，去掉取模，连续不接缝）。
float causticField(float2 p, float t) {
    float2 q = p * uCaustic.x * 6.2831853 - 250.0;
    float2 i = q;
    float c = 1.0;
    float inten = 0.005;
    for (int n = 0; n < 4; n++) {
        float tt = t * (1.0 - (3.5 / float(n + 1)));
        i = q + V2(cos(tt - i.x) + sin(tt + i.y), sin(tt - i.y) + cos(tt + i.x));
        c += 1.0 / length(V2(q.x / (sin(i.x + tt) / inten), q.y / (cos(i.y + tt) / inten)));
    }
    c /= 4.0;
    c = 1.17 - pow(c, 1.4);
    return clamp(pow(abs(c), 8.0), 0.0, 3.0);
}

// 干涉与衍射：在一块圆形区域里发光的条纹。
float wavePattern(float2 p, float t) {
    float2 d = p - uWaveB.xy;
    float radius = max(uWaveB.z, 1e-3);
    float r = length(d) / radius;
    float mask = smoothstep(1.0, 0.55, r);
    float mode = uWave.x;
    float f = uWave.y;
    float phase = t * uWave.z;
    float v;
    if (mode < 1.5) {
        v = 0.5 + 0.5 * cos(r * r * f * 6.0 - phase);
    } else if (mode < 2.5) {
        // 双缝：条纹离中心越远越弯（双曲线），包络是椭圆，不是矩形。
        float x = d.x / radius;
        float y = d.y / radius;
        float xc = x * (1.0 + 0.35 * y * y);
        v = pow(cos(xc * f * 3.0 - phase * 0.3), 2.0) * exp(-xc * xc * 2.5);
        mask = smoothstep(1.0, 0.25, length(V2(x * 0.85, y * 1.5)));
    } else if (mode < 3.5) {
        float2 s1 = V2(-uWaveB.w * 0.5, 0.0);
        float d1 = length(d - s1);
        float d2 = length(d + s1);
        v = (0.5 + 0.5 * cos((d1 - d2) * f * 20.0)) * (0.5 + 0.5 * cos((d1 + d2) * f * 6.0 - phase));
    } else {
        float x = r * f * 4.0 + 1e-3;
        float sc = sin(x) / x;
        v = min(sc * sc * 4.0, 1.5);
    }
    // 文字区里压暗，字压在条纹上还读得清。
    float2 q = (p - uShield.xy) / max(uShield.zw, V2(1e-3));
    float shield = 1.0 - uShieldB.x * smoothstep(1.0, 0.35, length(q));
    return v * mask * shield * uWave.w;
}

float4 LumierePS(LumierePixel input):SV_TARGET {
    float2 vPos=(input.uv*uFrame.xy-uFrame.xy*.5-uMotion.yz)/uMotion.x+uFrame.xy*.5;
    float vAlpha=1;
    float H = uFrame.y;
    float t = uFrame.z;
    float2 p = vPos / H;

    float density = fog(p, t);
    float tyndall = uFogA.y + (1.0 - uFogA.y) * density * uFogA.x;

    float caustic = uCaustic.w > 0.5 ? causticField(p, t * uCaustic.y + 23.0) : 0.0;
    float causticLift = lerp(1.0, 0.25 + 1.5 * caustic, uCaustic.w > 0.5 ? uCaustic.z : 0.0);

    float3 light = V3(0.0);
    int count = int(uFrame.w + 0.5);
    for (int i = 0; i < MAX_BEAMS; i++) {
        if (i >= count) break;
        float4 D = uBeamD[i];
        float2 m = beamMask(uBeamA[i], uBeamB[i], uBeamC[i], D.w, uBeamE[i], uBeamF[i].y, p);
        float3 color = D.rgb;
        float spectrum = uBeamF[i].x;
        if (spectrum > 0.0) color = lerp(color, spectrumColor(m.y) * max(max(color.r, color.g), color.b), spectrum);
        light += color * m.x;
    }
    light *= tyndall * causticLift;

    // 池底的焦散光斑（不靠烟，直接亮）。
    if (uCaustic.w > 0.5 && uCausticB.x > 0.0) {
        float2 e = (p - uCausticFloor.xy) / max(uCausticFloor.zw, V2(1e-3));
        light += uFogColor * caustic * uCausticB.x * smoothstep(1.0, 0.6, length(e));
    }
    // 干涉条纹。
    if (uWave.x > 0.5) light += uFogColor * wavePattern(p, t);

    // 光束之外的烟：很淡，只给暗场一点空间感。
    light += uFogColor * density * uFogB.z;

    // 光源眩光：亮核 + 大半径柔光 + 横向拉丝。
    if (uGlare.w > 0.0) {
        float2 g = p - uGlare.xy;
        float r = max(uGlare.z, 1e-3);
        float dist = length(g);
        float glare = exp(-dist / r) * 0.55 + exp(-(dist * dist) / (r * r * 0.03));
        float streak = exp(-abs(g.y) / (r * 0.035)) * exp(-abs(g.x) / (r * 7.0)) * uGlareB.x;
        light += uGlareColor * (glare + streak) * uGlare.w;
    }

    // 保持色相的压缩：按最大通道压到 0..1，只在极亮处微微发白（逐通道压缩会把金色压成白色）。
    float peak = max(max(light.r, light.g), light.b);
    float mapped = 1.0 - exp(-peak);
    float3 c = peak > 1e-5 ? light / peak * mapped : V3(0.0);
    c = lerp(c, V3(mapped), 0.3 * mapped * mapped * mapped);
    // 抖动，免得暗部渐变在 8 位视频里出色带（按像素坐标，确定性）。
    c += (hash12(input.position.xy) - 0.5) / 255.0;
    c = clamp(c, 0.0, 1.0);
    float a = max(max(c.r, c.g), c.b);

    // 叠在暗场底上（预乘的 over）。
    float3 rgb = c + uDark.rgb * (1.0 - a);
    float alpha = a + uDark.a * (1.0 - a);
    return V4(rgb, alpha) * vAlpha;
}

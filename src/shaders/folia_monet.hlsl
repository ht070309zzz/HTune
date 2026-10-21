// SPDX-License-Identifier: AGPL-3.0-only
// Native adaptation of Folia monetBackgroundPaint.ts and monetBackgroundDrift.ts.
cbuffer MonetConstants:register(b0){float4 frame;float4 drift;float4 base;float4 ink;float4 accent;};
Texture2D picture:register(t0);SamplerState linearClamp:register(s0);
struct P {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
float4 MonetPS(P input):SV_TARGET {
    float2 uv=(input.uv-.5-drift.xy)/drift.z+.5;
    float aspect=frame.x/frame.y;if(frame.z>aspect)uv.x=(uv.x-.5)*aspect/frame.z+.5;else uv.y=(uv.y-.5)*frame.z/aspect+.5;
    float2 blur=6/frame.xy;float3 c=picture.Sample(linearClamp,uv).rgb*4;
    c+=picture.Sample(linearClamp,uv+float2(blur.x,0)).rgb;c+=picture.Sample(linearClamp,uv-float2(blur.x,0)).rgb;
    c+=picture.Sample(linearClamp,uv+float2(0,blur.y)).rgb;c+=picture.Sample(linearClamp,uv-float2(0,blur.y)).rgb;c/=8;
    if(frame.w<.5)c=base.rgb;float lum=dot(c,float3(.2126,.7152,.0722));c=saturate(lum+(c-lum)*1.05);
    bool dark=dot(base.rgb,float3(.2126,.7152,.0722))<=dot(ink.rgb,float3(.2126,.7152,.0722));float3 shadow=dark?base.rgb:ink.rgb,highlight=dark?ink.rgb:base.rgb;
    float3 wash=lum<=.55?lerp(shadow,accent.rgb,lum/.55):lerp(accent.rgb,highlight,(lum-.55)/.45);c=lerp(c,wash,.34);
    float diagonal=(input.uv.x+input.uv.y)*.5,overlay=.74;float4 gradient=diagonal<.44?lerp(float4(accent.rgb,.08+overlay*.62),float4(base.rgb,.16+overlay*.58),diagonal/.44):lerp(float4(base.rgb,.16+overlay*.58),float4(ink.rgb,.06+overlay*.36),(diagonal-.44)/.56);
    c=lerp(c,gradient.rgb,gradient.a);float bloom=saturate(1-length((input.uv-float2(.18,.34))*float2(1,frame.y/frame.x))/.55)*overlay*.42;c=lerp(c,accent.rgb,bloom);
    c=lerp(c,base.rgb,saturate((input.uv.x-.45)/.55)*(.18+overlay*.38));float2 screen=input.uv*float2(1920,1080);
    [unroll]for(int i=0;i<18;++i){float x=(i*127)%1920,y=(i*211)%1080-40;if(abs(screen.x-x)<.5&&screen.y>=y&&screen.y<y+1080*.28)c=lerp(c,base.rgb,.1+overlay*.08);}
    return float4(saturate(c),1);
}


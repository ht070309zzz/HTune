// Original HTune: transparent lyric masks, cut light and outline echoes.
cbuffer RiftConstants:register(b0) {float2 viewport;float2 atlasTexel;float4 lyricMotion;float4 lyricAccent;};
Texture2D glyphAtlas:register(t0);SamplerState linearClamp:register(s0);
struct Vertex {float2 position:POSITION;float2 uv:TEXCOORD0;float4 color:COLOR0;float4 effect:TEXCOORD1;float3 karaoke:TEXCOORD2;};
struct Pixel {float4 position:SV_POSITION;float2 uv:TEXCOORD0;float4 color:COLOR0;float4 effect:TEXCOORD1;float2 screen:TEXCOORD2;float2 local:TEXCOORD3;float3 karaoke:TEXCOORD4;};
Pixel RiftVS(Vertex v,uint id:SV_VertexID){Pixel o;o.position=float4(v.position.x/viewport.x*2-1,1-v.position.y/viewport.y*2,0,1);o.uv=v.uv;o.color=v.color;o.effect=v.effect;o.karaoke=v.karaoke;o.screen=v.position/viewport;float2 corners[6]={float2(0,0),float2(1,0),float2(1,1),float2(0,0),float2(1,1),float2(0,1)};o.local=corners[id%6];return o;}
float KaraokeFill(Pixel p){
    float progress=saturate(p.karaoke.x);
    float boundary=p.karaoke.y+progress*p.karaoke.z;
    float feather=max(.003,fwidth(p.local.x)*.75);
    return progress<=0?0:progress>=1?1:1-smoothstep(boundary-feather,boundary+feather,p.local.x);
}
float3 KaraokeInk(Pixel p){
    float fill=KaraokeFill(p);
    // Keep timing-driven colour fill without an independent white scan in ink.
    return lerp(p.color.rgb*.43,lyricAccent.rgb,fill);
}
#include "mineradio_lyrics.hlsli"
// Separate emissive mask: identical glyph geometry and KTV boundary. Blur this
// screen-space image, never neighbouring atlas cells or the scenery itself.
float4 RiftGlowPS(Pixel p):SV_TARGET {
    if(p.karaoke.x<=0) return 0;
    float mask=glyphAtlas.Sample(linearClamp,p.uv).a;
    if(p.effect.x<3.5)mask*=1-smoothstep(p.effect.z-p.effect.w,p.effect.z+p.effect.w,p.uv.x);
    float active=p.effect.y<0?max(.48,-p.effect.y):.56+.44*sin(saturate(p.karaoke.x)*3.14159265);
    float a=mask*p.color.a*KaraokeFill(p)*active;
    float luma=dot(lyricAccent.rgb,float3(.2126,.7152,.0722));
    float3 light=lerp(lyricAccent.rgb,1,clamp((.68-luma)/max(.01,1-luma),0,.64));
    return float4(light*a,a);
}
float4 RiftPS(Pixel p):SV_TARGET {
    if(p.effect.x>=10){float a=p.color.a;
        if(p.effect.x>10.5){float d=length((p.local-.5)*2);a*=exp(-d*d*3)*saturate((1-d)*5);}
        return float4(p.color.rgb*a,a);}
    if(p.effect.x>3.5)return MineradioLyrics(p);
    float mask=1;
    if(p.effect.x<2||p.effect.x>2.5){
        float a=glyphAtlas.Sample(linearClamp,p.uv).a;
        if(p.effect.x>.5&&p.effect.x<1.5){
            float2 d=atlasTexel*p.effect.y;
            float edge=max(max(glyphAtlas.Sample(linearClamp,p.uv+float2(d.x,0)).a,glyphAtlas.Sample(linearClamp,p.uv-float2(d.x,0)).a),
                max(glyphAtlas.Sample(linearClamp,p.uv+float2(0,d.y)).a,glyphAtlas.Sample(linearClamp,p.uv-float2(0,d.y)).a));
            a=saturate(edge-a);
        }
        mask=a*(1-smoothstep(p.effect.z-p.effect.w,p.effect.z+p.effect.w,p.uv.x));
    }
    float3 ink=p.karaoke.x>=0?KaraokeInk(p):p.color.rgb;
    float alpha=saturate(p.color.a*mask);
    // Photographic/halftone lyrics use the same clean mask, without a dark rim.
    if(p.karaoke.x>0&&p.effect.x<.5){
        float2 d=atlasTexel*1.25;
        float edge=max(max(glyphAtlas.Sample(linearClamp,p.uv+float2(d.x,0)).a,glyphAtlas.Sample(linearClamp,p.uv-float2(d.x,0)).a),
            max(glyphAtlas.Sample(linearClamp,p.uv+float2(0,d.y)).a,glyphAtlas.Sample(linearClamp,p.uv-float2(0,d.y)).a));
        float reveal=1-smoothstep(p.effect.z-p.effect.w,p.effect.z+p.effect.w,p.uv.x);
        float rim=saturate(edge-glyphAtlas.Sample(linearClamp,p.uv).a)*reveal*p.color.a*KaraokeFill(p)*.45;
        float3 rimInk=lerp(lyricAccent.rgb,1,.55);
        return float4(ink*alpha+rimInk*rim*(1-alpha),alpha+rim*(1-alpha));
    }
    return float4(ink*alpha,alpha);
}

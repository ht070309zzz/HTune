// SPDX-License-Identifier: AGPL-3.0-only
// Native full-scene adaptation of Folia sonnetTransitions.ts and temperaTransitions.ts.
// 26c57f9b1b26b402a09e8943171a332215418ce6, Folia contributors.
cbuffer TransitionConstants:register(b0){float4 frame;float4 timing;};
Texture2D outgoing:register(t0),incoming:register(t1);SamplerState linearClamp:register(s0);
struct P {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
P TransitionVS(uint id:SV_VertexID){P p;p.uv=float2((id<<1)&2,id&2);p.position=float4(p.uv*float2(2,-2)+float2(-1,1),0,1);return p;}
float cubic(float v){v=saturate(v);return v<.5?4*v*v*v:1-pow(-2*v+2,3)/2;}
float hash(float n){return frac(sin(n)*43758.5453);}
float4 LayerPS(P p):SV_TARGET{return incoming.Sample(linearClamp,p.uv);}
float4 clipped(Texture2D source,float2 uv){return source.Sample(linearClamp,uv)*step(0,uv.x)*step(uv.x,1)*step(0,uv.y)*step(uv.y,1);}
float4 blur(Texture2D source,float2 uv,float amount){float2 d=amount/frame.xy;return (clipped(source,uv)*4+clipped(source,uv+d)+clipped(source,uv-d)+clipped(source,uv+float2(d.x,-d.y))+clipped(source,uv+float2(-d.x,d.y)))/8;}
float4 TransitionPS(P p):SV_TARGET {
    float t=saturate(timing.x),e=cubic(t),kind=timing.y;float2 flow=float2(cos(timing.z),sin(timing.z));
    if(kind<.5){ // block-wipe: the whole stage is carried through the same moving cut.
        float diagonal=dot(p.uv-.5,flow)/max(abs(flow.x)+abs(flow.y),.001)+.5;
        return lerp(clipped(outgoing,p.uv),clipped(incoming,p.uv),1-smoothstep(e-.015,e+.015,diagonal));
    }
    if(kind<1.5){ // camera-pan: both words and graphics move as one parent scene.
        float2 a=(p.uv-.5)/(1+e*.02)+.5-flow*e*.025;
        float2 b=(p.uv-.5)/(1+(1-e)*.02)+.5+flow*(1-e)*.025;
        float4 old=clipped(outgoing,a)*(1-saturate((t-.72)/.28)),next=clipped(incoming,b)*saturate(t/.3);
        return next+old*(1-next.a);
    }
    if(kind<2.5){
        float2 a=(p.uv-.5)/(1+e*.03)+.5-flow*e*.025,b=(p.uv-.5)/(1+(1-e)*.03)+.5+flow*(1-e)*.025;
        float4 old=blur(outgoing,a,e*6)*(1-saturate((t-.55)/.45)),next=blur(incoming,b,(1-e)*6)*saturate(t/.45);
        return next+old*(1-next.a);
    }
    if(kind<3.5){float4 old=blur(outgoing,p.uv,e*14)*(1-e),next=blur(incoming,p.uv,(1-e)*14)*(1-(1-e)*.82);return next+old*(1-next.a);}
    if(kind<4.5){
        float slot=floor(t*14),row=floor(p.uv.y*31),seed=timing.w*.0001+slot*.173;
        float gate=step(.72,hash(row+seed*119));float2 d=float2((hash(row*13+seed*83)-.5)*.055,0)*gate*sin(t*3.141593);
        float4 old=clipped(outgoing,p.uv+d)*(1-saturate((t-.86)/.14)),next=clipped(incoming,p.uv-d)*saturate(t/.3);
        return next+old*(1-next.a);
    }
    // Folia camera-pull keeps source scale at 1 to avoid exposing an empty surface.
    float4 old=clipped(outgoing,p.uv)*(1-e),next=clipped(incoming,p.uv)*(1-(1-e)*.72);return next+old*(1-next.a);
}

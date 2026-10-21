// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 HTune contributors.
// Original native terrain implementation. No Sonic Topography source/formulas.
cbuffer TerrainConstants:register(b0){float4 frame,audio,shape,control,cameraEye,cameraTarget,pointer,tint;float4 bandsA,bandsB;float4 rings[10];};
struct TerrainPixel {float4 position:SV_POSITION;float3 normal:TEXCOORD0;float3 world:TEXCOORD1;float2 uv:TEXCOORD2;float4 values:TEXCOORD3;};
float hash(float2 p){uint x=asuint(p.x*1723.13)^asuint(p.y*371.73);x^=x>>16;x*=0x7feb352d;x^=x>>15;x*=0x846ca68b;x^=x>>16;return float(x)/4294967295.0;}
float noise(float2 p){float2 n=floor(p),f=frac(p);f=f*f*(3-2*f);return lerp(lerp(hash(n),hash(n+float2(1,0)),f.x),lerp(hash(n+float2(0,1)),hash(n+1),f.x),f.y);}
TerrainPixel TerrainVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID){
    uint grid=(uint)pointer.w;float spacing=16.0/grid;float2 xz=(float2(instance%grid,instance/grid)+.5)*spacing-8;
    bool wave=shape.x>7.5;float t=frame.z,r=length(xz),a=atan2(xz.y,xz.x);
    float flow=wave?sin(xz.x*1.3+sin(xz.y*.6+t*.3)*1.8-t*.7):cos(r*1.6-t*.55+sin(a*3)*.25);
    float terrain=noise(xz*.5+float2(t*.07,-t*.04));float center=exp(-r*r/17.0);
    float lift=.07+terrain*.14+(bandsA.x*.75+bandsA.y*.5)*center;
    lift+=max(0,flow)*(.08+bandsA.z*.42+bandsA.w*.35)*(1-smoothstep(5.5,9.5,r));
    lift+=pow(noise(xz*3+t*.14),6)*bandsB.x*.60;
    float ringLight=0;
    for(int i=0;i<10;++i){float age=t-rings[i].z,dist=length(xz-rings[i].xy);float waveRadius=age*2.8;
        float ring=exp(-pow((dist-waveRadius)/(.14+age*.12),2))*exp(-age*.7)*abs(rings[i].w)*step(0,age)*(1-smoothstep(3.5,4.5,age));lift+=ring*.6;ringLight=max(ringLight,ring);}
    uint face=vertex/6;float2 corners[6]={float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)};float2 c=corners[vertex%6];
    float3 normals[6]={float3(1,0,0),float3(-1,0,0),float3(0,1,0),float3(0,-1,0),float3(0,0,1),float3(0,0,-1)};float3 n=normals[face];
    float3 box=face<2?float3(n.x,c.x,c.y):face<4?float3(c.x,n.y,c.y):float3(c.x,c.y,n.z);box*=.5;
    float3 world=float3(xz.x+box.x*spacing*.8,(box.y+.5)*lift-1.5,xz.y+box.z*spacing*.8-4);
    if(instance>=grid*grid){float id=instance-grid*grid,s=hash(float2(id,17));float ang=id*.79+t*(.045+s*.02),radius=3+s*5;
        float size=.05+s*.11;world=float3(cos(ang)*radius,-.35+s*1.7+sin(t*.2+id)*.18,sin(ang)*radius-4)+box*size;
        lift=.4+s*.6;ringLight=audio.w*.2;}
    float angle=t*(wave?.018:.009),cs=cos(angle),sn=sin(angle);world.xz=float2(world.x*cs-world.z*sn,world.x*sn+world.z*cs);
    float3 z=normalize(cameraEye.xyz-cameraTarget.xyz),x=normalize(cross(float3(0,1,0),z)),y=cross(z,x),d=world-cameraEye.xyz;
    float depth=max(.05,-dot(d,z)),f=1/tan(cameraEye.w*.5);TerrainPixel p;
    p.position=float4(dot(d,x)*f/(frame.x/frame.y),dot(d,y)*f,(depth-.05)*150/(150-.05),depth);
    p.normal=n;p.world=world;p.uv=c*.5+.5;p.values=float4(lift,ringLight,hash(xz),instance>=grid*grid?1:0);return p;
}
float4 TerrainPS(TerrainPixel p):SV_TARGET {
    float energy=saturate(p.values.x*2),edge=1-smoothstep(.015,.065,min(min(p.uv.x,1-p.uv.x),min(p.uv.y,1-p.uv.y)));
    float3 cold=shape.x>7.5?float3(.16,.43,.72):float3(.06,.58,.66),warm=shape.x>7.5?float3(1,.39,.43):float3(.59,.44,.95);
    float zone=saturate(noise(p.world.xz*.14)*.6+bandsA.x*.4);float3 glow=lerp(cold,warm,zone);
    float top=step(.5,p.normal.y),shade=.18+.5*saturate(dot(p.normal,normalize(float3(-.4,.8,.3))));
    float3 color=lerp(float3(.008,.015,.025),glow,energy*(top*.7+edge*.4))*shade;
    color+=glow*p.values.y*(.7+edge)+glow*p.values.w*.35;float fog=1-smoothstep(7,16,length(p.world.xz+float2(0,4)));
    return float4(color*fog,fog);
}

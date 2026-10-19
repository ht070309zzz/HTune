// Original MusicStage concepts: woven acoustic curtain and echo contour scroll.
// Procedural, frame-continuous geometry. No reference image is used as scenery.
cbuffer OriginalConstants : register(b0) {
    float4 resolution; // px width/height, DPI, style
    float4 clock;      // animation time, track entrance, line entrance, motion
    float4 audio;
    float4 pointer;    // normalized position, presence, drawer
    float4 base;
    float4 accent;
    float4 ink;
    float4 drawing;    // segments, instances, shadow/fold pass, heard-history extent
    float4 history[128]; // level, validity. Only already observed audio.
};
float Hash(float2 p) {return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);}
struct Surface {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
Surface FullscreenVS(uint id:SV_VertexID) {
    Surface o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;
}
float4 BackgroundPS(Surface i):SV_TARGET {
    float2 p=i.uv;
    float grain=(Hash(floor(p*resolution.xy))-.5)*.010;
    float3 color=base.rgb;
    if(resolution.w==3){
        float edge=pow(abs(p.x-.5)*1.6,2)+pow(abs(p.y-.5)*1.3,2);
        color*=.62+.38*saturate(1-edge);
        color+=float3(.015,.013,.013)*sin(p.y*45+p.x*9)*.15;
    }
    if(resolution.w==5)color=lerp(base.rgb,float3(.955,.973,.966),saturate((p.y-.5)*.25));
    return float4(saturate(color+grain),1);
}
struct Thread {float4 position:SV_POSITION;float2 edge:TEXCOORD0;float4 color:COLOR0;};
float2 VertexCorner(uint vertex) {
    uint id=vertex%6;
    return float2((id==1||id==2||id==4)?1:0,(id==0||id==1||id==3)?-1:1);
}
float2 ToClip(float2 p) {return p*float2(2,-2)+float2(-1,1);}
float2 WeavePoint(float u,float v,float seed) {
    float breath=clock.x*.28;
    float center=.49+.024*sin(u*11)+.009*sin(seed*9);
    float aperture=exp(-pow(abs((v-center)/(.185+.013*sin(u*9))),6));
    float side=u<.5?-1:1;
    float open=.335*aperture;
    float x=.5+side*(open+abs(u-.5)*(1-1.90*open));
    float wave=sin(v*18+u*13+breath)+.35*sin(v*43-u*17-breath*.7);
    x+=wave*(.007+.014*audio.x)*clock.w*(.32+.68*(1-aperture));
    float y=v+sin(u*17+breath)*.012*clock.w;
    float2 d=float2(x,y)-pointer.xy;
    float pull=exp(-dot(d*float2(1.1,1.7),d*float2(1.1,1.7))/.009)*pointer.z;
    x+=side*.032*pull;y+=sin(u*9+v*7)*.017*pull;
    // A newly identified song parts the curtain; the center remains readable.
    x+=side*(1-clock.y)*.48*(.4+.6*aperture);
    x+=sin(v*170+seed*7)*.00055;
    return float2(x,y);
}
Thread WeaveVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    float2 corner=VertexCorner(vertex);float segment=floor(vertex/6);
    float v=(segment+corner.x)/drawing.x*1.04-.02;
    float u=(instance+.5)/drawing.y;float seed=Hash(float2(instance,73));
    float2 p=WeavePoint(u,v,seed),next=WeavePoint(u,v+.001,seed);
    float2 tangent=normalize((next-p)*resolution.xy+float2(0,.001));
    float2 normal=float2(-tangent.y,tangent.x)/resolution.xy;
    float width=(.55+.55*seed)*max(1,resolution.z*.75);
    p+=normal*corner.y*width;
    float shine=.25+.65*pow(.5+.5*sin(v*21+u*12-clock.x*.17),3);
    float intensity=(.18+.58*seed)*shine;
    float3 color=lerp(accent.rgb,float3(.98,.79,.70),shine*.28);
    Thread o;o.position=float4(ToClip(p),0,1);o.edge=float2(corner.y,1);o.color=float4(color,intensity);return o;
}
float2 EchoSample(float t) {
    float at=saturate(t)*127;uint a=(uint)at;uint b=min(a+1,127);
    // Filter only recorded neighboring values: rounded paper relief rather
    // than straight polygon corners at each feature sample.
    uint before=a>0?a-1:0,after=min(b+1,127);
    float aLevel=(history[before].x+2*history[a].x+history[b].x)*.25;
    float bLevel=(history[a].x+2*history[b].x+history[after].x)*.25;
    float f=frac(at);float level=lerp(aLevel,bLevel,f*f*(3-2*f));
    return float2(level,lerp(history[a].y,history[b].y,f));
}
float2 EchoPoint(float t,float band) {
    float2 h=EchoSample(saturate(t+sin(band*3.14159)*.012*(1-t)));
    float taper=pow(saturate(1-t),.63)*sqrt(drawing.w);
    float flow=sin(t*10.6+.3)*.050+sin(t*21-1.1)*.024;
    float relief=pow(saturate(h.x),1.65)*.55*(.75+.25*sin(t*8+.4))*audio.w;
    float x=(1-drawing.w+t*drawing.w)*.705+(band-.5)*.032*taper;
    float depth=pow(saturate(1-band),.72);
    float y=.65+flow*taper-relief*depth*taper+(band-.43)*.21*taper;
    y+=sin(band*4+t*9)*.018*taper;
    if(drawing.z>1.5&&drawing.z<2.5){
        float turn=clock.y;float angle=t*15.5+turn*4;
        float2 rolled=float2(.72+cos(angle)*(.025+.045*t),.78+sin(angle)*(.025+.045*t));
        float fold=smoothstep(0,.8,turn);
        return lerp(float2(x,y),rolled,fold)+float2(0,turn*turn*.28);
    }
    return float2(x,y);
}
Thread EchoVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    float2 corner=VertexCorner(vertex);float segment=floor(vertex/6);
    float t=(segment+corner.x)/drawing.x,band=(instance+.5)/drawing.y;
    bool paper=drawing.z>2.5;
    if(paper)band=(instance+(corner.y+1)*.5)/drawing.y;
    float2 p=EchoPoint(t,band),next=EchoPoint(min(1,t+.001),band);
    float2 tangent=normalize((next-p)*resolution.xy+float2(.001,0));
    float2 normal=float2(-tangent.y,tangent.x)/resolution.xy;
    bool shadow=drawing.z>.5&&drawing.z<1.5;
    float width=shadow?5.4:1.10;
    if(shadow)p+=float2(.001,.018)*(1-t);
    if(!paper)p+=normal*corner.y*width*max(1,resolution.z*.75);
    float valid=EchoSample(t).y;
    float alpha=(shadow?.045:.82)*valid;
    if(drawing.z>1.5&&drawing.z<2.5)alpha*=1-clock.y;
    float3 green=lerp(ink.rgb,float3(.74,.84,.78),band*.67);
    if(paper){green=base.rgb;alpha=valid;}
    Thread o;o.position=float4(ToClip(p),0,1);o.edge=float2(corner.y,paper?3:shadow?2:1);o.color=float4(green,alpha);return o;
}
float4 ThreadPS(Thread i):SV_TARGET {
    if(i.edge.y>2.5){
        float crease=.045+.16*pow(saturate((1-i.edge.x)*.5),9);
        float3 face=lerp(i.color.rgb,ink.rgb,crease);
        return float4(face*i.color.a,i.color.a);
    }
    float coverage=pow(saturate(1-abs(i.edge.x)),i.edge.y);
    float a=i.color.a*coverage;return float4(i.color.rgb*a,a);
}

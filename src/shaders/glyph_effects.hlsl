// Wingless lyric comets. The spine follows sampled flight history; dense dust
// retains its birth identity while drifting and fading away from that path.
struct Emitter { float4 q01; float4 q23; float4 color; float4 life; float4 singing; };
cbuffer GlyphEffects : register(b0) {
    float4 view;
    float4 recipe;
    float4 stageClip;
    float4 budget; // local dust, glyph count, lanes per path point, comet pass
    Emitter emitters[64];
    float4 fairies[291]; // 3 * (head + 96 path points)
    float4 fairyColor[3];
    float4 fairyMotion[3];
};
float Hash(float n){return frac(sin(n*12.9898+78.233)*43758.5453);}
float2 Project(Emitter e,float2 p){return lerp(lerp(e.q01.xy,e.q01.zw,p.x),lerp(e.q23.zw,e.q23.xy,p.x),p.y);}
struct Output {
    float4 position:SV_Position;
    float2 uv:TEXCOORD0;
    float4 color:COLOR0;
    float2 shape:TEXCOORD1; // 0 dust, 1 head, 2 continuous spine; comet mask
    float2 dip:TEXCOORD2;
    float2 glyph:TEXCOORD3;
};
Output GlyphEffectVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID){
    const float2 corners[6]={float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)};
    uint quad=vertex/6;
    float2 uv=corners[vertex%6],center=0,local=float2(-1,-1);
    float2 axes=float2(1,0),extent=1;
    float alpha=0,kind=0;float3 tint=1;
    if(budget.w>.5){
        uint base=instance*97;float4 head=fairies[base];
        float seed=instance*31.71+recipe.x*13.3;
        tint=lerp(fairyColor[instance].rgb,1,.20);
        if(quad==0){
            center=head.xy;axes=float2(cos(head.z),sin(head.z));
            extent=float2(head.w*2.5,head.w*1.9);
            alpha=fairyColor[instance].a;kind=1;
        }else{
            uint lanes=(uint)budget.z,stride=lanes==2?2:1;uint sample=(quad-1)/lanes*stride+1,lane=(quad-1)%lanes;
            float4 past=fairies[base+sample];float age=max(0,past.z);
            float4 newer=sample==1?float4(head.xy,0,1):fairies[base+sample-stride];
            float2 delta=newer.xy-past.xy;float length_=length(delta);
            axes=length_>.001?delta/length_:float2(cos(head.z),sin(head.z));
            float fade=pow(saturate(1-age/1.48),1.25);
            if(lane==0){
                center=(past.xy+newer.xy)*.5;
                extent=float2(length_*.5+.65,1.75+4.75*fade);
                alpha=fade*fairyColor[instance].a*.76*step(.08,length_);kind=2;
            }else{
                float birth=round((view.z-age)*60);
                float s=seed+birth*.711+lane*17.3;
                float2 normal=float2(-axes.y,axes.x);
                float drift=(Hash(s+2)-.5)*(4+age*42);
                center=lerp(past.xy,newer.xy,Hash(s))+normal*drift-axes*age*(2+Hash(s+1)*9);
                center.y+=age*age*(Hash(s+4)-.25)*15;
                float size=(2.125+Hash(s+3)*2.375)*(1-age*.35);
                extent=max(.35,size)*float2(1.2,1);
                alpha=pow(saturate(1-age/1.48),.95)*fairyColor[instance].a*.94;
                if(recipe.x==2)tint=lerp(tint,float3(1,.81,.48),.24);
                if(recipe.x==4)tint=lerp(tint,tint.bgr,.2*Hash(s));
            }
            if(past.z>1.6)alpha=0;
        }
    }else{
        uint per=(uint)budget.x,index=instance/per,lane=instance%per;Emitter e=emitters[index];
        float seed=Hash(e.life.z+lane*17.17),age=max(0,e.life.x),active=saturate(e.life.y);
        float cycle=view.z*(.42+.16*seed)*recipe.y+seed*7,life=frac(cycle),generation=floor(cycle);
        float chance=Hash(e.life.z+lane*7+generation*19),side=lane%2?1:-1;
        local=float2(.5+(chance-.5)*.62+sin(life*3+seed*9)*.045,.5+side*(.54+life*(.12+.06*recipe.z)));
        if(lane%3==2)local=local.yx;center=Project(e,local);
        float em=clamp(length(e.q23.zw-e.q01.xy),14,210);
        extent=(.75+em*.004)*float2(1,recipe.x==1?1.5:1);
        float burst=exp(-age*5.8)*smoothstep(0,.055,age);
        alpha=sin(life*3.14159265)*(.10+.46*active+.5*burst)*e.color.a*recipe.w;
        if(e.life.w!=0)alpha=0;
        tint=lerp(e.color.rgb,1,.2+.16*active);
        if(recipe.x==2)tint=lerp(tint,float3(.95,.78,.44),.3);
        if(recipe.x==4)tint=lerp(tint,tint.bgr,.18*seed);
    }
    float2 normal=float2(-axes.y,axes.x),dip=center+axes*uv.x*extent.x+normal*uv.y*extent.y;
    Output o;o.position=float4(dip.x/view.x*2-1,1-dip.y/view.y*2,0,1);
    o.color=float4(tint,saturate(alpha));o.uv=uv;o.shape=float2(kind,budget.w);
    o.dip=dip;o.glyph=local;return o;
}
float InkMask(float2 p){
    float mask=1;
    [loop]for(uint n=0;n<(uint)budget.y;++n){Emitter e=emitters[n];if(e.life.w!=0)continue;
        float2 lo=min(min(e.q01.xy,e.q01.zw),min(e.q23.xy,e.q23.zw));
        float2 hi=max(max(e.q01.xy,e.q01.zw),max(e.q23.xy,e.q23.zw));
        if(any(p<lo)||any(p>hi))continue;
        // Inverse bilinear coordinates, per pixel: never hide the whole comet
        // because only the edge of its halo crosses a projected letter.
        float2 uv=.5;
        [unroll]for(int k=0;k<3;++k){
            float2 q=Project(e,uv)-p;
            float2 du=lerp(e.q01.zw-e.q01.xy,e.q23.xy-e.q23.zw,uv.y);
            float2 dv=lerp(e.q23.zw-e.q01.xy,e.q23.xy-e.q01.zw,uv.x);
            float det=du.x*dv.y-du.y*dv.x;
            if(abs(det)>.001)uv-=float2(q.x*dv.y-q.y*dv.x,du.x*q.y-du.y*q.x)/det;
        }
        float edge=min(min(uv.x,1-uv.x),min(uv.y,1-uv.y));
        mask=min(mask,1-smoothstep(.045,.13,edge));
    }
    return mask;
}
float4 GlyphEffectPS(Output i):SV_Target{
    clip(min(min(i.dip.x-stageClip.x,stageClip.z-i.dip.x),min(i.dip.y-stageClip.y,stageClip.w-i.dip.y)));
    if(all(i.glyph>.13)&&all(i.glyph<.87))discard;
    float2 p=i.uv;float alpha;
    if(i.shape.x>1.5)alpha=exp(-p.y*p.y*6)*.76+exp(-p.y*p.y*2)*.13;
    else if(i.shape.x>.5){
        alpha=exp(-dot(p,p)*28)+exp(-dot(p,p)*7)*.48;
        i.color.rgb=lerp(i.color.rgb,1,exp(-dot(p,p)*35)*.65);
    }else alpha=exp(-dot(p,p)*4.5)*.86+exp(-dot(p,p)*2)*.10;
    if(i.shape.y>.5)alpha*=InkMask(i.dip);
    alpha=saturate(alpha*i.color.a);return float4(i.color.rgb*alpha,alpha);
}

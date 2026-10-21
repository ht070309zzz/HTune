// HTune original finishing layer. Existing theme art stays underneath.
// Four spatial scales: broad illumination, material contours, fine rims and
// travelling highlights. Quiet central band protects lyric readability.
float DetailHash(float2 p){return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);}
float DetailNoise(float2 p){float2 i=floor(p),f=frac(p);f=f*f*(3-2*f);return lerp(lerp(DetailHash(i),DetailHash(i+float2(1,0)),f.x),lerp(DetailHash(i+float2(0,1)),DetailHash(i+1),f.x),f.y);}
float2 ArtRotate(float2 p,float a){float c=cos(a),s=sin(a);return float2(c*p.x-s*p.y,s*p.x+c*p.y);}
float3 ArtPalette(int style,float3 accent){
    if(style==1||style==9||style==13||style==17)return float3(.20,.35,.43);
    if(style==2||style==5||style==8||style==16)return float3(.48,.31,.23);
    if(style==6||style==7||style==14||style==19)return float3(.20,.38,.72);
    if(style==11||style==15||style==22||style==25)return float3(.42,.26,.56);
    if(style==24||style==26)return float3(.32,.22,.56);
    return float3(.28,.42,.46);
}
// Large, deliberately composed forms establish an art direction before the
// small particles. Two colour temperatures and unequal focal planes preserve
// depth, with a dark reading pocket and a distinct silhouette for each family.
float4 ThemeArt(float2 uv,float aspect,float t,float low,float high,float3 accent,float3 base,int style){
    float2 p=(uv-.5)*float2(aspect,1);
    bool paper=style==2||style==5||style==8||style==16;
    bool glass=style==4||style==6||style==7||style==14||style==19;
    bool ring=style==11||style==15||style==17||style==22||style==25;
    bool sky=style==24||style==26;
    float side=(style==1||style==12||style==17||style==19||style==25)?-1:1;
    float2 focus=float2(side*aspect*.30,-.19);
    float2 q=p-focus;
    float3 cold=ArtPalette(style,accent),warm=lerp(accent,float3(1,.89,.70),paper?.23:.15);
    float back=exp(-dot(q*float2(.85,1.2),q*float2(.85,1.2))*3.2);
    float counter=exp(-dot(p+focus*.85,p+focus*.85)*5.5);
    float3 rgb=cold*counter*.15+warm*back*(.12+.05*low);
    float alpha=back*.14+counter*.15;
    float form=0,rim=0,shadow=0,grain=0;
    float breath=.018*sin(t*.12+style);
    if(paper){
        // Translucent pigment pools, fibrous contour lines and metallic edges.
        float2 v=ArtRotate(q,style==5?-.4:.25);
        float n=DetailNoise(v*5+float2(t*.018,style));
        float edge=length(v*float2(.72,1.42))-(.35+.10*n+breath);
        form=(1-smoothstep(-.035,.04,edge))*.34;
        rim=exp(-edge*edge*6500)*.45;
        shadow=exp(-pow((edge+.026)*26,2))*.13;
        grain=(DetailNoise(p*145)-.5)*.022*form;
        float e2=length((p+focus*.65)*float2(1.15,.8))-(.28+.07*DetailNoise(p*8));
        rgb+=cold*(1-smoothstep(-.08,.08,e2))*.10;alpha+=(1-smoothstep(-.08,.08,e2))*.10;
    }else if(glass){
        // Asymmetric folded glass, broad illuminated faces with thin caustics.
        [unroll]for(int k=0;k<3;++k){float j=(float)k;
            float2 v=ArtRotate(p-float2(side*(.24+j*.20),-.05+j*.1),-.42+.11*j+.025*sin(t*.09+j));
            float edge=abs(v.x)-(.055+.025*j);
            float taper=1-smoothstep(.35,.85,abs(v.y));
            form+=exp(-edge*edge*140)*taper*(.12+.04*j);
            rim+=exp(-edge*edge*26000)*taper*(.38+.10*low);
            shadow+=(1-smoothstep(-.02,.02,edge))*taper*.055;
        }
    }else if(ring){
        // Sculptural tilted rings, a soft off-axis glow, and lit engraved arcs.
        float2 v=ArtRotate(q,-.40+style*.037)*float2(.78,1.30);
        float radius=length(v),angle=atan2(v.y,v.x);
        float r=.33+breath,edge=radius-r;
        float lighting=.25+.75*pow(.5+.5*cos(angle-t*.12),3);
        form=exp(-edge*edge*280)*lighting*.48;
        rim=exp(-edge*edge*20000)*lighting*.72;
        shadow=exp(-pow((edge+.024)*30,2))*.12;
        float inner=radius-r*.72;
        rim+=exp(-inner*inner*32000)*(.2+.18*cos(angle*3-t*.18));
        float ticks=pow(saturate(cos(angle*(style==17?60:36))),24);
        rim+=exp(-pow((radius-r*1.15)*150,2))*ticks*.26;
    }else if(sky){
        // Veils around a quiet void and sparse luminous star lanes.
        float2 v=ArtRotate(q,.36);float r=length(v*float2(.72,1.25));
        float n=DetailNoise(v*6+float2(t*.018,-t*.012));
        float edge=r-(style==24?.33:.46)-n*.10;
        form=exp(-edge*edge*100)*(.2+.25*n);
        rim=exp(-edge*edge*6000)*(.18+.22*high);
        shadow=(1-smoothstep(.16,.30,r))*.22;
    }else{
        // Flow/silk/voice themes get broad translucent folds, not a uniform
        // sine-wave overlay. Two opposing contours frame the lyric pocket.
        [unroll]for(int k=0;k<3;++k){float j=(float)k;
            float x=p.x+side*.25;
            float curve=sin(x*(1.6+j*.6)+t*(.12+j*.035)+style*.37+j*1.7)*(.15+j*.025);
            float offset=(j-1)*.32;
            if(style==18)offset=(j-1)*.38; // conversation space in the middle
            float edge=p.y-curve-offset;
            float sculpt=.3+.7*pow(.5+.5*cos(x*2.5-t*.16+j),2);
            form+=exp(-edge*edge*(45+j*18))*sculpt*(.16+.04*low);
            rim+=exp(-edge*edge*(8000+j*2000))*sculpt*.32;
            shadow+=exp(-pow((edge+.04)*20,2))*.035;
        }
    }
    float pocket=.20+.80*smoothstep(.13,.44,abs(uv.y-.48));
    if(style==21) pocket*=.50; // preserve the recognisable album particle face
    if(style==20){form*=.55;rim*=.45;} // quiet theme retains a restrained scene
    float a=saturate((form+rim+shadow)*pocket);
    float3 material=lerp(cold,warm,saturate(.35+rim*1.3+back*.3));
    if(paper)material=lerp(material,base,.28);
    material=lerp(material,base*.4,saturate(shadow/max(.001,form+rim+shadow)));
    rgb=rgb*(1-a)+material*a+grain;alpha=alpha*(1-a)+a;
    return float4(min(max(0,rgb),saturate(alpha)),saturate(alpha));
}
float4 ThemeDetail(float2 uv,float aspect,float t,float low,float high,float pulse,float3 accent,float3 base,int style,float quality){
    float2 p=(uv-.5)*float2(aspect,1);
    float light=step(.45,dot(base,float3(.2126,.7152,.0722)));
    float protection=.28+.72*smoothstep(.12,.46,abs(uv.y-.47));
    float slow=t*.12,shine=0,body=0,shadow=0;
    bool paper=style==2||style==5||style==8||style==16;
    bool structure=style==4||style==6||style==7||style==14||style==19;
    bool orbital=style==13||style==15||style==17||style==22||style==25;
    if(paper){
        // Layered fibre relief and a moving grazing light, never neon on paper.
        float n=DetailNoise(p*7+float2(slow*.18,0));
        float contour=p.y*7+p.x*(style==5?1.7:.7)+n*.24+.20*sin(p.x*4+slow);
        float ridge=abs(frac(contour)-.5);
        shine=exp(-ridge*ridge*1200)*.12;shadow=exp(-pow((ridge-.025)*18,2))*.04;
        body=exp(-pow(p.x*.6+p.y-.35*sin(slow),2)*5)*(.035+.025*low);
    }else if(structure){
        // Separated glass planes: soft backlight plus a thin refractive edge.
        [unroll] for(int j=0;j<3;++j){float k=(float)j;
            float x=p.x+p.y*(.33+.09*k)-.50*sin(slow*.36+k*2.1+style*.2);
            float d=abs(x)-(.11+.075*k);
            float edge=exp(-d*d*24000),fog=exp(-d*d*120);
            float travel=pow(.5+.5*sin(p.y*5-slow*2+k*1.9),6);
            shine+=(edge*.18+fog*.055)*(0.6+travel*.9+low*.25);
            shadow+=exp(-pow((d+.025)*30,2))*.025;
        }
        body=exp(-dot(p-float2(.55,-.3),p-float2(.55,-.3))*2)*.035;
    }else if(orbital){
        // Eccentric orbital engravings, different radii and independent speeds.
        float2 center=float2(.30*sin(slow*.22+style),.22*cos(slow*.17));
        float2 q=(p-center)*float2(.75,1.35);float r=length(q),a=atan2(q.y,q.x);
        [unroll]for(int j=0;j<3;++j){float radius=.32+j*.19+.012*sin(slow+j);
            float arc=.35+.65*pow(.5+.5*cos(a*(j+1)-slow*(.7+j*.16)),5);
            shine+=exp(-pow((r-radius)*380,2))*.19*arc;
            body+=exp(-pow((r-radius)*18,2))*.025*arc;
        }
        shine*=.7+low*.4+pulse*.12;
    }else{
        // Silk / volumetric currents: broad layers move at unequal rates.
        [unroll]for(int j=0;j<4;++j){float k=(float)j;
            float wave=sin(p.x*(2.1+k*.65)+slow*(.7+k*.23)+k*1.9+style*.28)*(.10+.018*k);
            float distance=p.y-wave-(k-1.5)*.17;
            float flow=.55+.45*sin(p.x*2.5-slow+k*1.7);
            shine+=exp(-distance*distance*(1800+k*500))*.12*flow;
            body+=exp(-distance*distance*26)*.022*(.7+low*.45)*flow;
        }
    }
    // Independent depth planes: distant pin lights, middle-distance fragments,
    // and sparse soft foreground motes. Fixed cells have feathered lifetimes;
    // a media seek directly reconstructs them, with no simulation backlog.
    float dust=0,glints=0;
    int layers=quality>=2?1:quality>=1?2:3;
    [loop]for(int depth=0;depth<layers;++depth){
        float k=(float)depth,grid=lerp(29,9,k*.5);
        float2 drift=float2(t*(.014+k*.012),t*(-.022-k*.015));
        if(structure)drift=float2(t*(.03+k*.014),t*.008);
        if(paper)drift*=.55;
        float2 field=p*grid+drift+float2(style*3.17,k*17.7);
        float2 cell=floor(field),local=frac(field)-.5;
        float seed=DetailHash(cell+float2(k*19,style));
        float2 offset=float2(DetailHash(cell+7.3),DetailHash(cell+41.1))*.50-.25;
        float2 d=local-offset;
        float life=.25+.75*pow(.5+.5*sin(t*(.45+k*.13)+seed*40),2);
        float sparse=step(k>1.5?.87:.72,seed);
        float radius=.024+k*.018;
        float dotLight=exp(-dot(d,d)/(radius*radius));
        float haze=exp(-dot(d,d)/(radius*radius*9));
        dust+=(dotLight*.60+haze*.08)*sparse*life*(.55+high*.65);
        if(!paper){
            float cross=exp(-abs(d.x)*170-abs(d.y)*17)+exp(-abs(d.y)*170-abs(d.x)*17);
            glints+=cross*sparse*pow(life,4)*(.11+k*.04);
        }
    }
    // Large-scale motion is family-specific, separate from tiny particle noise.
    float theatre=0,lightEdge=0;
    if(structure){
        float beam=p.x+p.y*.46-.60*sin(t*.16+style*.7);
        float scan=.5+.5*sin(p.y*7-t*.85);
        theatre=exp(-beam*beam*19)*(.13+.13*low);
        lightEdge=exp(-beam*beam*18000)*(.36+.24*scan);
        float split=beam-.09;
        lightEdge+=exp(-split*split*8500)*.12;
    }else if(orbital){
        float2 q=(p-float2(.26*sin(t*.035+style),-.12))*float2(.72,1.18);
        float r=length(q),angle=atan2(q.y,q.x);
        [unroll]for(int j=0;j<2;++j){
            float life=frac(t*.09+j*.5),radius=.18+life*.95;
            float envelope=sin(life*3.14159265);
            float d=r-radius;
            float crest=.35+.65*pow(.5+.5*sin(angle*3-t*.31+j*2),3);
            theatre+=exp(-d*d*85)*envelope*crest*(.08+.10*low);
            lightEdge+=exp(-d*d*9500)*envelope*crest*(.28+.18*pulse);
        }
    }else{
        [unroll]for(int j=0;j<2;++j){float k=(float)j;
            float bend=.21*sin(p.x*(1.7+k*.4)+t*(.14+k*.05)+style*.31+k*2.7);
            float d=p.y-bend-(k-.5)*.48;
            if(paper)d+=p.x*.18;
            float travel=.45+.55*pow(.5+.5*sin(p.x*3.1-t*.51+k*2.5),3);
            theatre+=exp(-d*d*(paper?70:35))*travel*(.10+.12*low);
            lightEdge+=exp(-d*d*(paper?16000:8500))*travel*(paper?.25:.34);
        }
    }
    float lens=exp(-dot(p-float2(-aspect*.32,.31),p-float2(-aspect*.32,.31))*4)*(.055+.075*low);
    float fine=(DetailNoise(p*65)-.5)*.006;
    float strength=(shine*1.7+body*1.8+lens+theatre+lightEdge+dust+glints)*protection*(1+high*.18+pulse*.16);
    float3 color=lerp(accent,1,.30+.16*saturate(glints+lightEdge));
    if(light)color=lerp(accent,base,.48);
    float alpha=min(.78,saturate(strength+shadow+abs(fine)));
    color=lerp(color,base*.6,saturate(shadow/max(.0001,alpha)));
    float4 art=ThemeArt(uv,aspect,t,low,high,accent,base,style);
    // Fine detail remains subordinate to the large composition.
    alpha*=.64;
    return float4(color*alpha+art.rgb*(1-alpha),alpha+art.a*(1-alpha));
}

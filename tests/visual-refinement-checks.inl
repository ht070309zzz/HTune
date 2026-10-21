// Native product paths, synthetic fixtures, no media export or user data.
auto lines=std::make_shared<LyricTimeline>();lines->state=LyricLoadState::Ready;lines->wordTimed=true;
LyricLine line;line.text=L"幸福常在";line.durationMs=8000;
for(size_t i=0;i<line.text.size();++i)line.words.push_back({int64_t(i)*2000,2000,line.text.substr(i,1)});lines->lines={line};
for(int style:{6,7,13,14,19}){
    RiftStage layer;layer.Create(device.Get(),write.Get());RiftFrame f;f.lyrics=lines;f.kind=MotionKind(style);f.width=800;f.height=450;f.identity=L"small-syllables";f.revision=1;
    f.following=f.precise=f.playing=f.phraseEntry=true;f.motionLines=1;
    for(int count=1;count<=4;++count){f.positionMs=(count-1)*2000+280;f.nowMs=100000+f.positionMs;
        const auto pixels=BitmapPixels(dc.Get(),layer.Render(f,dc.Get()));const auto state=layer.Reading();
        expect(state.glyphCount==count,"Syllable mode did not reveal exactly the due characters");
        for(size_t i=0;i<state.glyphCount;++i){expect(state.glyphs[i].fill==1.f,"Syllable mode still uses partial karaoke filling");expect(state.glyphs[i].text==i,"Syllable mode changed source character order");}
        expect(!pixels.empty(),"Syllable output missing");
    }
}
// Quad-exterior pixels must come from blurred glyph light, not atlas padding,
// decorations or a blurred background. Reduced motion suppresses the motes.
{
    auto one=std::make_shared<LyricTimeline>();one->state=LyricLoadState::Ready;one->wordTimed=true;
    line.text=L"光";line.startMs=0;line.durationMs=6000;line.words={{0,6000,L"光"}};one->lines={line};
    RiftFrame f;f.kind=LyricMotion::Rift;f.width=800;f.height=450;f.following=f.precise=f.reduced=true;f.positionMs=100;f.nowMs=100100;f.identity=L"glow-extent";f.accent={1,.7f,.3f,1};f.motionLines=1;
    RiftStage empty,lit;empty.Create(device.Get(),write.Get());lit.Create(device.Get(),write.Get());const auto ground=BitmapPixels(dc.Get(),empty.Render(f,dc.Get()));f.lyrics=one;
    const auto pixels=BitmapPixels(dc.Get(),lit.Render(f,dc.Get()));const auto r=lit.Reading();expect(r.glyphCount==1,"Glow fixture glyph absent");size_t outside=0;
    for(int y=0;y<450;++y)for(int x=0;x<800;++x){if(x>=r.glyphLeft-3&&x<=r.glyphRight+3&&y>=r.glyphTop-3&&y<=r.glyphBottom+3)continue;
        const size_t k=(size_t(y)*800+x)*4;if(int(pixels[k+2])-int(ground[k+2])>2)++outside;}
    std::cout<<"Diffuse glow outside glyph quad: "<<outside<<" pixels\n";expect(outside>50,"Soft glow is clipped to the glyph quad");
}
// An asymmetric top-red/bottom-blue source exercises the actual cover sampler.
{
    auto cover=std::make_shared<BackgroundFrame>();cover->width=cover->height=64;cover->stride=256;cover->sequence=1;cover->bgra.resize(64*64*4);
    for(int y=0;y<64;++y)for(int x=0;x<64;++x){auto* p=cover->bgra.data()+(y*64+x)*4;p[0]=y>=32?240:8;p[1]=8;p[2]=y<32?240:8;p[3]=255;}
    RiftStage layer;layer.Create(device.Get(),write.Get());RiftFrame f;f.kind=LyricMotion::Mineradio;f.width=640;f.height=640;f.cover=cover;f.reduced=true;f.camera=0;f.response=0;f.identity=L"upright-cover";f.nowMs=1000;
    const auto pixels=BitmapPixels(dc.Get(),layer.Render(f,dc.Get()));double redY=0,blueY=0;size_t red=0,blue=0;
    for(int y=50;y<590;++y)for(int x=50;x<590;++x){const auto* p=pixels.data()+(y*640+x)*4;if(p[2]>p[0]+45&&p[2]>90){redY+=y;++red;}if(p[0]>p[2]+45&&p[0]>90){blueY+=y;++blue;}}
    expect(red>100&&blue>100,"Directional cover colors missing");std::cout<<"Cover redY="<<redY/red<<" blueY="<<blueY/blue<<"\n";
    expect(redY/red+80<blueY/blue,"Album particle cover remains upside down");
}
// Opening metadata uses the same renderer on every retained theme, then yields
// at the original first sung timestamp. All duplicate-zero-time credits survive.
auto credits=std::make_shared<LyricTimeline>();credits->state=LyricLoadState::Ready;credits->songId=L"100001";
for(auto text:{L"作词：甲",L"作曲：乙",L"编曲：丙",L"制作人：丁"}){LyricLine c;c.text=text;c.credit=true;c.durationMs=5000;credits->lines.push_back(c);}
line.text=L"正文开始";line.startMs=5000;line.durationMs=5000;line.words.clear();credits->lines.push_back(line);
for(int style:AvailableBuiltInStyles){SceneRenderer renderer;renderer.CreateOffscreen(800,450,1,true);
    for(auto pos:{1000,6000}){const int64_t now=200000+pos;auto s=Scene(credits,now,pos,{});s.view.theme=style;s.view.reducedMotion=true;renderer.SetOffscreenTimeMs(now);
        std::optional<PresentedLyricPosition> selected;expect(renderer.Draw(s,false,&selected),"Credit scene draw failed");
        expect(selected&&selected->lineIndex==(pos<5000?3u:4u),"Opening credits displaced the source lyric timeline");}
}
// Subtitles live in the same native layer and follow the whole phrase, including
// characters that have not entered yet. Changing visibility must not move ink.
{
    auto translated=std::make_shared<LyricTimeline>();translated->state=LyricLoadState::Ready;translated->wordTimed=true;
    LyricLine t;t.text=L"幸福常在";t.durationMs=8000;t.translation=L"May happiness stay with you";t.romanization=L"xing fu chang zai";
    for(size_t i=0;i<t.text.size();++i)t.words.push_back({int64_t(i)*2000,2000,t.text.substr(i,1)});translated->lines={t};
    for(int style:AvailableBuiltInStyles){if(style<6)continue;
        RiftStage layer;layer.Create(device.Get(),write.Get());RiftFrame f;f.lyrics=translated;f.kind=MotionKind(style);f.particlePreset=IsMineradioStyle(style)?style-FirstMineradioStyle:0;
        f.width=800;f.height=450;f.identity=L"attached-translation";f.revision=1;f.following=f.precise=f.playing=f.phraseEntry=true;f.motionLines=1;
        f.showTranslation=f.showRomanization=true;
        f.positionMs=1800;f.nowMs=301800;layer.Render(f,dc.Get());
        f.positionMs=1999;f.nowMs=301999;layer.Render(f,dc.Get());const auto before=layer.Reading();
        f.positionMs=2001;f.nowMs=302001;layer.Render(f,dc.Get());const auto after=layer.Reading();
        expect(before.auxiliaryVisible&&after.auxiliaryVisible,"Attached translation missing");
        expect(std::hypot(before.auxiliaryTransform[4]-after.auxiliaryTransform[4],before.auxiliaryTransform[5]-after.auxiliaryTransform[5])<10.f,"Subtitle jumps when a new glyph appears");
        f.positionMs=5000;f.nowMs=305000;const auto withText=BitmapPixels(dc.Get(),layer.Render(f,dc.Get()));const auto pose=layer.Reading();
        f.paused=true;f.playing=false;f.nowMs+=100;layer.Render(f,dc.Get());const auto paused=layer.Reading();
        for(size_t i=0;i<6;++i)expect(std::abs(pose.auxiliaryTransform[i]-paused.auxiliaryTransform[i])<.1f,"Paused translation moved");
        f.showTranslation=f.showRomanization=false;const auto noText=BitmapPixels(dc.Get(),layer.Render(f,dc.Get()));const auto hidden=layer.Reading();
        expect(!hidden.auxiliaryVisible&&hidden.glyphCount==paused.glyphCount,"Subtitle toggle changed lyric visibility");
        size_t changed=0;for(size_t i=0;i<withText.size();i+=4)if(std::abs(int(withText[i])-int(noText[i]))+std::abs(int(withText[i+1])-int(noText[i+1]))+std::abs(int(withText[i+2])-int(noText[i+2]))>30)++changed;
        if(changed<=30)std::cerr<<"Subtitle pixel failure style "<<style<<" at "<<pose.auxiliaryTransform[4]<<","<<pose.auxiliaryTransform[5]<<" scale="<<pose.auxiliaryTransform[0]<<"\n";
        expect(changed>30,"Translation layer has no visible pixels");
        for(auto dimensions:{std::pair{480.f,300.f},std::pair{1280.f,720.f},std::pair{1920.f,1080.f}}){
            f.width=dimensions.first;f.height=dimensions.second;f.showTranslation=f.showRomanization=true;f.reduced=true;f.nowMs+=100;
            layer.Render(f,dc.Get());const auto resized=layer.Reading();
            expect(resized.auxiliaryVisible,"Translation lost after resize");
            const auto& m=resized.auxiliaryTransform;float bottom=-INFINITY,top=INFINITY,left=INFINITY,right=-INFINITY;
            for(float x:{-resized.auxiliaryWidth*.5f,resized.auxiliaryWidth*.5f})for(float y:{0.f,resized.auxiliaryHeight}){const float py=x*m[1]+y*m[3]+m[5],px=x*m[0]+y*m[2]+m[4];bottom=std::max(bottom,py);top=std::min(top,py);left=std::min(left,px);right=std::max(right,px);}
            if(top<0||bottom>f.height*.96f)std::cerr<<"Subtitle bounds style="<<style<<" size="<<f.width<<"x"<<f.height<<" y="<<top<<".."<<bottom<<"\n";
            expect(top>=0&&bottom<=f.height*.96f,"Translation extends outside the stage after resize");
            if(left<0||right>f.width)std::cerr<<"Subtitle horizontal bounds style="<<style<<" x="<<left<<".."<<right<<" width="<<f.width<<"\n";
            expect(left>=0&&right<=f.width,"Translation exceeds horizontal stage bounds");
        }
    }
    {
        auto delayed=std::make_shared<LyricTimeline>(*translated);delayed->lines[0].translation.clear();delayed->lines[0].romanization.clear();
        RiftStage layer;layer.Create(device.Get(),write.Get());RiftFrame f;f.lyrics=delayed;f.kind=LyricMotion::Lumiere;f.width=480;f.height=300;f.identity=L"late-translation";f.revision=1;f.following=f.precise=f.reduced=true;f.showTranslation=f.showRomanization=true;f.positionMs=5000;f.nowMs=400000;
        layer.Render(f,dc.Get());expect(!layer.Reading().auxiliaryVisible,"Empty auxiliary fields created text");
        f.lyrics=translated;f.nowMs+=17;layer.Render(f,dc.Get());const auto enriched=layer.Reading();
        expect(enriched.auxiliaryVisible&&enriched.line==0,"Late translation enrichment lost current lyric");
        expect(enriched.glyphBottom<300*.69f,"Late translation did not reserve group space");
    }
}
#include "art-light-checks.inl"
std::cout<<"Visual refinement checks passed: "<<passed<<"; synthetic native pixels only, no exports\n";

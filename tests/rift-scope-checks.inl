// Included inside the development-only WARP checks. No product I/O.
auto phrase=std::make_shared<LyricTimeline>();phrase->state=LyricLoadState::Ready;phrase->wordTimed=true;
LyricLine lyric;lyric.text=L"幸福吧";lyric.startMs=0;lyric.durationMs=5000;
lyric.words={{0,1800,L"幸"},{1800,500,L"福"},{2300,1800,L"吧"}};phrase->lines={lyric};
for(int style:AvailableBuiltInStyles){if(style<6)continue;
    RiftStage quiet,loud;quiet.Create(device.Get(),write.Get());loud.Create(device.Get(),write.Get());
    RiftFrame a;a.lyrics=phrase;a.identity=L"audio-independent-ink";a.revision=1;a.width=640;a.height=360;
    a.kind=MotionKind(style);a.particlePreset=style>=21?style-21:0;a.following=a.precise=a.playing=true;
    a.camera=1.35f;a.motionLines=1;a.quality=2;
    bool moved=false;RiftReading earlier;
    for(int frame=0;frame<150;++frame){a.positionMs=frame*20;a.nowMs=100000+a.positionMs;
        auto b=a;b.low=.7f;b.mid=.5f;b.high=.3f;b.rms=.4f;b.pulse=frame%8<4?1.f:0.f;b.spectrum.fill(.7f);
        quiet.Render(a,dc.Get());loud.Render(b,dc.Get());const auto q=quiet.Reading(),r=loud.Reading();
        expect(q.glyphCount==r.glyphCount,"Audio changed the visible character sequence");
        for(size_t i=0;i<q.glyphCount;++i){const auto& x=q.glyphs[i];const auto& y=r.glyphs[i];
            if(!(x.text==y.text&&std::abs(x.left-y.left)<.001f&&std::abs(x.right-y.right)<.001f&&std::abs(x.top-y.top)<.001f&&std::abs(x.bottom-y.bottom)<.001f))
                std::cerr<<"GEOMETRY style="<<style<<" frame="<<frame<<" text="<<x.text<<" q="<<x.left<<","<<x.top<<","<<x.right<<","<<x.bottom<<" loud="<<y.left<<","<<y.top<<","<<y.right<<","<<y.bottom<<" anchor="<<x.anchorX<<","<<y.anchorX<<" camera="<<q.cameraX<<","<<r.cameraX<<"\n";
            expect(x.text==y.text&&std::abs(x.left-y.left)<.001f&&std::abs(x.right-y.right)<.001f&&std::abs(x.top-y.top)<.001f&&std::abs(x.bottom-y.bottom)<.001f,"Audio still changes lyric geometry");
            expect(std::abs(x.fill-y.fill)<.0001f,"Audio controls KTV timing");
            if(i)expect(q.glyphs[i-1].text<x.text,"Character order changed during entrance");
        }
        if(frame==4)earlier=q;
        if(frame==10&&earlier.glyphCount&&q.glyphCount)moved=std::abs(q.glyphs[0].top-earlier.glyphs[0].top)+std::abs(q.glyphs[0].left-earlier.glyphs[0].left)+std::abs(q.glyphs[0].right-earlier.glyphs[0].right)>.1f;
    }
    if(style!=20)expect(moved,"Original one-shot entrance/motion was removed");else expect(!moved,"Still theme geometry moved");
    std::cout<<"SCOPE style="<<style<<" independentAudioGeometry=1 entrance="<<moved<<"\n";
}
for(int style=0;style<6;++style){
    SceneRenderer quiet,loud;quiet.CreateOffscreen(640,360,1,true);loud.CreateOffscreen(640,360,1,true);
    bool moved=false;uint64_t first=0;
    for(int frame=0;frame<40;++frame){const auto now=200000+frame*20;auto a=Scene(Demo(),now,(style<3?600:4200)+frame*20,{});
        a.view.theme=style;auto b=a;b.audio.rms=.4f;b.audio.peak=.9f;b.audio.spectrum.fill(.7f);
        quiet.SetOffscreenTimeMs(now);loud.SetOffscreenTimeMs(now);quiet.Draw(a,false);loud.Draw(b,false);
        const auto q=quiet.OffscreenLyricGeometry(),r=loud.OffscreenLyricGeometry();
        if(q!=r)std::cerr<<"ORIGINAL GEOMETRY style="<<style<<" frame="<<frame<<"\n";
        expect(q!=0&&q==r,"Audio still moves the first-six-theme lyric geometry");
        if(frame==1)first=q;if(frame>1&&q!=first)moved=true;
    }
    if(style<3)expect(moved,"Original kinetic word/line transition was removed");
    std::cout<<"SCOPE style="<<style<<" independentAudioGeometry=1 fullScene=1\n";
}
// All composition variants must retain source reading order, including the
// former longest-note extraction that displayed Happiness as Fu Ba Xing.
RiftStage order;order.Create(device.Get(),write.Get());
RiftFrame o;o.lyrics=phrase;o.identity=L"source-order";o.revision=2;o.following=o.precise=true;o.reduced=true;
o.positionMs=3000;o.nowMs=200000;o.motionLines=1;
for(const auto size:{std::pair{960.f,540.f},std::pair{540.f,960.f}}){o.width=size.first;o.height=size.second;
    for(int style:AvailableBuiltInStyles){if(style<6)continue;o.kind=MotionKind(style);o.particlePreset=style>=21?style-21:0;
        const int variants=std::max(1,MotionVariantCount(style));
        for(int variant=0;variant<variants;++variant){o.variant=variants==1?-1:variant;o.nowMs+=10;order.Render(o,dc.Get());const auto r=order.Reading();
            expect(r.glyphCount==3,"Short original phrase lost a character");
            for(size_t i=0;i<r.glyphCount;++i){expect(r.glyphs[i].text==i,"Native cluster mapping reordered source text");
                if(i){const auto& a=r.glyphs[i-1];const auto& b=r.glyphs[i];
                    if(!(a.row<b.row||(a.row==b.row&&a.anchorX<b.anchorX)))std::cerr<<"ORDER style="<<style<<" variant="<<variant<<" glyph="<<i<<"\n";
                    expect(a.row<b.row||(a.row==b.row&&a.anchorX<b.anchorX),"Layout changed source reading order");}
            }
        }
    }
}
// Actual GPU ink: KTV fraction changes pixels across the native glyph mask.
RiftStage ktv;ktv.Create(device.Get(),write.Get());
auto one=std::make_shared<LyricTimeline>();one->state=LyricLoadState::Ready;one->wordTimed=true;
lyric.text=L"唱";lyric.startMs=0;lyric.durationMs=5000;lyric.words={{0,4000,L"唱"}};one->lines={lyric};
RiftFrame k;k.lyrics=one;k.identity=L"ktv-pixels";k.revision=1;k.kind=LyricMotion::Still;k.width=640;k.height=360;
k.following=k.precise=true;k.ink={1,1,1,1};k.accent={1,.10f,.05f,1};k.motionLines=1;
size_t previousRed=0;
for(int pos:{0,1000,2000,3000,4000}){k.positionMs=pos;k.nowMs=300000+pos;
    const auto pixels=BitmapPixels(dc.Get(),ktv.Render(k,dc.Get()));const auto r=ktv.Reading();
    expect(r.glyphCount==1&&std::abs(r.glyphs[0].fill-float(pos)/4000.f)<.001f,"Real word interval lost its fill fraction");
    size_t red=0,gray=0;for(size_t p=0;p<pixels.size();p+=4)if(pixels[p+3]>245){
        if(pixels[p+2]>190&&pixels[p+1]<70)++red;
        if(pixels[p+2]>90&&pixels[p+2]<150&&std::abs(int(pixels[p+2])-int(pixels[p+1]))<5)++gray;}
    expect(pos==0?red==0:red>previousRed,"KTV sweep did not advance in rendered ink");
    if(pos==2000)expect(red>100&&gray>100,"KTV has no visible sung/unsung contrast");
    previousRed=red;
}
one->wordTimed=false;one->lines[0].words.clear();k.revision=2;
for(int pos:{0,1000,3000}){k.positionMs=pos;k.nowMs=400000+pos;ktv.Render(k,dc.Get());const auto r=ktv.Reading();
    expect(!r.wordTimed&&r.glyphCount==1&&r.glyphs[0].fill==1.f,"Line-only lyric fabricated a word sweep");}
// Multiple native clusters within one provider word share a measured sweep.
one->wordTimed=true;one->lines[0].text=L"幸福吧";one->lines[0].words={{0,3000,L"幸福吧"}};k.revision=3;k.positionMs=1500;k.nowMs=500000;
ktv.Render(k,dc.Get());const auto middle=ktv.Reading();
expect(middle.glyphCount==3&&middle.glyphs[0].fill==1&&middle.glyphs[1].fill>0&&middle.glyphs[1].fill<1&&middle.glyphs[2].fill==0,"Provider word swept every character at once");
std::cout<<"PASS "<<passed<<" targeted native checks; real word timing, source order, audio-independent ink, original entrances\n";

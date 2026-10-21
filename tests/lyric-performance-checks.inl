// Automated data/pixel assertions only: no image or video exports, no user I/O.
ComPtr<IDWriteTextFormat> format;
Hr(write->CreateTextFormat(L"Microsoft YaHei",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,64,L"zh-cn",&format));
auto measure=[&](const std::wstring& text){
    ComPtr<IDWriteTextLayout> layout;Hr(write->CreateTextLayout(text.data(),static_cast<UINT32>(text.size()),format.Get(),65536,1024,&layout));
    UINT32 count=0;layout->GetClusterMetrics(nullptr,0,&count);std::vector<DWRITE_CLUSTER_METRICS> metrics(count);
    Hr(layout->GetClusterMetrics(metrics.data(),count,&count));std::vector<LyricClusterMeasure> cells;size_t at=0;
    for(const auto& m:metrics){cells.push_back({at,at+m.length,m.width});at+=m.length;}return cells;
};
LyricTimeline source;source.state=LyricLoadState::Ready;LyricLine line;
line.text=L"窗外的天气 就像是你多变的表情";line.startMs=1000;line.durationMs=12000;source.lines={line};
auto grouped=BuildLyricPerformance(source,measure);std::wstring joined;
expect(grouped.timeline->lines.size()==2,"Long line did not become a phrase relay");
for(size_t i=0;i<grouped.timeline->lines.size();++i){const auto& g=grouped.timeline->lines[i];joined+=g.text;
    expect(g.text.size()>=6&&g.text.size()<=10,"CJK phrase outside requested short range");
    expect(grouped.sourceLines[i]==0&&grouped.estimated[i],"Estimated phrase lost source mapping");
    if(i)expect(g.startMs>grouped.timeline->lines[i-1].startMs,"Relay phrase clock reversed");
    for(size_t w=1;w<g.words.size();++w)expect(g.words[w].startMs>=g.words[w-1].startMs,"Word estimate reversed");
}
expect(joined==line.text&&source.lines[0].words.empty()&&!source.wordTimed,"Presentation changed provider text or timing claims");
source.lines[0].durationMs=0;LyricLine next;next.startMs=10000;next.text=L"后续歌词";next.durationMs=3000;source.lines.push_back(next);
grouped=BuildLyricPerformance(source,measure);expect(grouped.timeline->lines.front().words.size()>0,"Next line did not supply missing duration");
source.lines[0].durationMs=30000;grouped=BuildLyricPerformance(source,measure);
for(size_t i=0;i<grouped.sourceLines.size();++i)if(grouped.sourceLines[i]==0){const auto& g=grouped.timeline->lines[i];expect(g.startMs+g.durationMs<=next.startMs,"Estimated phrase crossed next original line");}
source.lines.resize(1);auto& real=source.lines[0];real.text=L"幸福吧";real.durationMs=5000;real.words={{1000,3000,L"幸"},{4000,400,L"福"},{4400,1600,L"吧"}};source.wordTimed=true;
grouped=BuildLyricPerformance(source,measure);const auto& words=grouped.timeline->lines[0].words;
expect(words.size()==3&&words[0].startMs==1000&&words[0].durationMs==3000&&words[1].startMs==4000&&words[1].durationMs==400,"Real singing intervals were replaced by estimates");
real.text=L"e\u0301 幸福 music \U0001F3B5 向前向前向前";real.words.clear();grouped=BuildLyricPerformance(source,measure);joined.clear();
for(const auto& g:grouped.timeline->lines){joined+=g.text;for(const auto& w:g.words){expect(w.text!=L"\u0301","Combining mark split from glyph cluster");expect(w.text.empty()||w.text.front()<0xdc00||w.text.front()>0xdfff,"Surrogate pair split");}}
expect(joined==real.text,"Mixed-script relay lost source text");

// Exercise the actual product SceneRenderer, including its DirectWrite render
// view, and the sample-only native clock seen in the user's current log.
auto lyrics=std::make_shared<LyricTimeline>();lyrics->state=LyricLoadState::Ready;lyrics->songId=L"100001";
line.text=L"唱唱唱唱";line.startMs=0;line.durationMs=8000;line.words.clear();lyrics->lines={line};
for(int style:AvailableBuiltInStyles){
    if(style>=6&&UsesSyllableEntrance(MotionKind(style),style>=21?style-21:0))continue; // separately checked as direct syllable appearance
    auto theme=std::make_shared<ThemeDefinition>(BuiltInTheme(style));theme->background=theme->panel=ThemeRgb(0x08090c);
    theme->ink=ThemeRgb(0xffffff);theme->muted=ThemeRgb(0x777777);theme->accent=ThemeRgb(0xff190c);theme->glow=theme->accent;
    std::array<size_t,3> counts{};std::array<size_t,3> neutral{},neutralCore{};
    std::array<std::array<size_t,3>,3> solid{};
    for(size_t sample=0;sample<3;++sample){
        const auto position=1000+int64_t(sample)*3000;const auto now=700000+position;
        SceneRenderer renderer;renderer.CreateOffscreen(960,540,1,true);
        auto s=Scene(lyrics,now,position,{});s.view.theme=style;s.view.customTheme=theme;s.view.reducedMotion=true;s.view.motionLines=1;s.view.lyricSize=48;
        s.playback.state=PlayState::Unknown;s.playback.nativeClockAdvanceAllowed=false;s.playback.durationMs=8000;
        // Let the existing scene-opacity/row-role transition finish. The source
        // position stays fixed, so this cannot advance the KTV sweep itself.
        for(int warm=0;warm<24;++warm){const auto tick=now+warm*40;s.frameAtMs=tick;s.playback.observedAtMs=s.playback.sampledAtMs=tick;
            renderer.SetOffscreenTimeMs(tick);expect(renderer.Draw(s,false),"Product scene failed to render");}
        std::vector<uint8_t> pixels;UINT width=0,height=0;expect(renderer.CaptureBgra(pixels,width,height),"Product pixels unavailable");
        for(UINT y=height/5;y<height*4/5;++y)for(UINT x=0;x<width;++x){const size_t p=(size_t(y)*width+x)*4;
            const int red=pixels[p+2],green=pixels[p+1],blue=pixels[p];
            if(red>100&&red-green>55&&red-blue>55)++counts[sample];
            for(size_t band=0;band<3;++band)if(red>150+int(band)*35&&red-green>90&&red-blue>90)++solid[band][sample];
            if(red>60&&std::abs(red-green)<12&&std::abs(red-blue)<12)++neutral[sample];
            // Diffuse light can itself be nearly grey. Look for the flat,
            // opaque unsung stroke tone, rather than counting its halo as ink.
            // Ordinary DWrite: mixed neutral ink; atlas: 0.43 white;
            // particle typography: 0.48 white. Do not test the latter against
            // the atlas shader's different tone (thin serif cores expose it).
            const int tone=style<6?157:IsMineradioStyle(style)?122:110;
            if(x+1<width&&y+1<height){bool core=true;
                for(UINT dy=0;dy<2;++dy)for(UINT dx=0;dx<2;++dx)for(size_t c=0;c<3;++c)
                    core&=std::abs(int(pixels[((size_t(y+dy)*width+x+dx)*4)+c])-tone)<9;
                if(core)++neutralCore[sample];}
        }
    }
    std::cout<<"KTV style="<<style<<" red="<<counts[0]<<","<<counts[1]<<","<<counts[2]<<" neutral="<<neutral[0]<<","<<neutral[1]<<","<<neutral[2]<<"\n";
    for(const auto& band:solid)std::cout<<"Solid ink="<<band[0]<<","<<band[1]<<","<<band[2]<<"\n";
    std::cout<<"Neutral stroke cores="<<neutralCore[0]<<","<<neutralCore[1]<<","<<neutralCore[2]<<"\n";
    // Broad coloured area includes the active halo, which legitimately fades
    // after a syllable. Require either that area or brighter stroke interiors
    // to advance, AND independently require neutral unsung ink to disappear.
    // Thin strokes may have too few flat 2x2 cores; broad glow may contribute
    // near-neutral pixels. Accept either neutral metric only when BOTH steps
    // decrease, with a visible unsung region at the middle sample.
    bool advances=counts[2]>counts[1]+100;
    for(const auto& band:solid)advances|=band[1]>band[0]+20&&band[2]>band[1]+20;
    expect(counts[1]>counts[0]+100&&advances,"Rendered KTV ink did not progressively fill");
    const bool neutralArea=neutral[0]>neutral[1]+40&&neutral[1]>neutral[2]+40&&neutral[1]>100;
    const bool neutralStroke=neutralCore[0]>neutralCore[1]+20&&neutralCore[1]>neutralCore[2]+20&&neutralCore[1]>50;
    expect(neutralArea||neutralStroke,"Unsung neutral characters did not follow karaoke timing");
}
// Per-character entrance and colour use independent clocks: the phrase is
// readable early, while its later characters remain unsung neutral ink.
auto renderView=BuildLyricPerformance(*lyrics,measure);
for(int style:AvailableBuiltInStyles){if(style<6||UsesSyllableEntrance(MotionKind(style),style>=21?style-21:0))continue;
    RiftStage stage;stage.Create(device.Get(),write.Get());RiftFrame f;f.lyrics=renderView.timeline;f.identity=L"phrase-entry";f.revision=1;
    f.width=960;f.height=540;f.following=f.precise=f.playing=f.phraseEntry=true;f.kind=MotionKind(style);f.particlePreset=style>=21?style-21:0;f.motionLines=1;
    f.positionMs=50;f.nowMs=800050;stage.Render(f,dc.Get());const auto early=stage.Reading();
    f.positionMs=700;f.nowMs=800700;stage.Render(f,dc.Get());const auto ready=stage.Reading();
    expect(ready.glyphCount==4,"Phrase entrance failed to expose neutral upcoming characters");
    expect(ready.glyphs[ready.glyphCount-1].fill==0,"Entrance marked unsung words as sung");
    if(style!=20)expect(early.glyphCount<ready.glyphCount,"Theme still has only a whole-line entrance");
}
// Changing the future main phrase must not affect pre-boundary main pixels.
for(auto kind:{LyricMotion::Rift,LyricMotion::Fume}){
    auto a=std::make_shared<LyricTimeline>(*renderView.timeline),b=std::make_shared<LyricTimeline>(*renderView.timeline);
    next.text=L"未来句甲";next.startMs=8000;next.durationMs=4000;next.words={{8000,4000,next.text}};a->lines.push_back(next);
    next.text=L"完全不同的未来句乙";next.words={{8000,4000,next.text}};b->lines.push_back(next);
    RiftFrame f;f.width=640;f.height=360;f.following=f.precise=true;f.reduced=true;f.kind=kind;f.motionLines=1;f.positionMs=7000;f.nowMs=907000;f.revision=1;f.identity=L"future-no-flash";
    RiftStage first,second;first.Create(device.Get(),write.Get());second.Create(device.Get(),write.Get());f.lyrics=a;const auto p=BitmapPixels(dc.Get(),first.Render(f,dc.Get()));f.lyrics=b;
    expect(p==BitmapPixels(dc.Get(),second.Render(f,dc.Get())),"Future main phrase leaked before its boundary");
}
// Product navigation keeps the original line index after phrase splitting,
// including a pause, a backwards jump and a rapid song/timeline replacement.
for(int style:{0,4,6,9,13,17,21}){
    auto original=std::make_shared<LyricTimeline>();original->state=LyricLoadState::Ready;original->songId=L"100001";
    line.text=L"窗外的天气就像是你多变的表情";line.startMs=0;line.durationMs=12000;line.words.clear();
    next.text=L"幸福吧";next.startMs=12000;next.durationMs=4000;next.words.clear();original->lines={line,next};
    SceneRenderer renderer;renderer.CreateOffscreen(640,360,1,true);int tick=0;
    for(auto position:{1000,9000,13000,3000}){const auto now=950000+(++tick)*1000;auto s=Scene(original,now,position,{});
        s.view.theme=style;s.view.reducedMotion=true;s.playback.state=PlayState::Paused;std::optional<PresentedLyricPosition> shown;
        renderer.SetOffscreenTimeMs(now);expect(renderer.Draw(s,false,&shown),"Navigation render failed");
        expect(shown&&shown->lineIndex==(position>=12000?1u:0u),"Phrase rendering exposed a derived index to product navigation");
    }
    auto replacement=std::make_shared<LyricTimeline>();replacement->state=LyricLoadState::Ready;replacement->lines={next};replacement->lines[0].startMs=0;
    auto s=Scene(replacement,960000,1000,{});s.view.theme=style;s.view.reducedMotion=true;s.lyricRevision=s.lyricReadingRevision=2;s.playback.songId=L"100002";
    std::optional<PresentedLyricPosition> shown;renderer.SetOffscreenTimeMs(960000);renderer.Draw(s,false,&shown);
    expect(shown&&shown->lineIndex==0,"Song change retained a prior phrase index");
}
std::cout<<"PASS "<<passed<<" phrase, source mapping, real/estimated KTV, all-theme product pixels and entrance checks\n";

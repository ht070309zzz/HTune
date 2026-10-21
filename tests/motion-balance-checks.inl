// Bounded native checks; all inputs and files belong to the synthetic fixture.
// No normal profile, media player, window, image or video export.
const auto fontPath=fs::current_path()/L"assets/fonts/ZCOOLKuaiLe-Regular.ttf";
for(const auto key:{L"@wenkai",L"@huangyou",L"@kuaile",L"Microsoft YaHei UI"}){
    OptionalLyricFont font;expect(font.Select(write.Get(),key,{}),"First font selection not resolved");
    expect(font.Active()&&!font.Family().empty()&&font.Collection(),"Optional native font unavailable");
    expect(!font.Select(write.Get(),key,{}),"Unchanged font rebuilt its collection");
    expect(font.Select(write.Get(),L"",{})&&!font.Active(),"Original theme font cannot be restored");
}
{
    OptionalLyricFont font;font.Select(write.Get(),L"",fontPath.wstring());expect(font.Active(),"Local TTF failed validation");
    font.Select(write.Get(),L"",L"C:/Windows/Fonts/Noto Sans SC (TrueType).otf");expect(font.Active(),"Local OTF failed validation");
    for(const auto path:{fixture/L"missing.ttf",fixture/L"broken.otf"}){
        if(path.extension()==L".otf"){std::ofstream out(path,std::ios::binary);out<<"this is not a valid font file";}
        font.Select(write.Get(),L"",path.wstring());expect(!font.Active(),"Missing/corrupt font did not fall back");
    }
    auto theme=BuiltInTheme(18);theme.lyricFont=L"@wenkai";
    const auto saved=SaveThemeNewFile(fixture/L"font-preset.json",theme);
    expect(saved.ok&&LoadTheme(fixture/L"font-preset.json").definition.lyricFont==L"@wenkai","Built-in font not saved in theme");
    fs::copy_file(fontPath,fixture/L"custom.ttf");theme.lyricFont=L"ZCOOL KuaiLe";theme.lyricFontPath=fixture/L"custom.ttf";
    expect(SaveThemeNewFile(fixture/L"font-file.json",theme).ok,"Local font theme export failed");
    const auto loadedFont=LoadTheme(fixture/L"font-file.json");
    expect(loadedFont.ok&&loadedFont.definition.lyricFont==theme.lyricFont&&loadedFont.definition.lyricFontPath==theme.lyricFontPath,"Relative font theme lost file reference");
    expect(!ParseThemeJson(R"({"schema":1,"baseStyle":18,"lyricFont":42})").ok,"Invalid font JSON accepted");
    expect(!ParseThemeJson(R"({"schema":1,"baseStyle":18,"lyricFont":"a\u0000b"})").ok,"Font NUL accepted");
}
auto lyrics=std::make_shared<LyricTimeline>();lyrics->state=LyricLoadState::Ready;lyrics->wordTimed=true;
for(int n=0;n<4;++n){LyricLine line;line.text=n%2?L"幸福吧一路向前":L"当我需要独自站在";line.startMs=n*1600;line.durationMs=1600;
    line.translation=L"Stay with the words as they move";line.romanization=L"dang wo xu yao du zi zhan zai";
    for(size_t i=0;i<line.text.size();++i)line.words.push_back({line.startMs+int64_t(i)*1600/int64_t(line.text.size()),1600/int64_t(line.text.size()),line.text.substr(i,1)});
    lyrics->lines.push_back(line);
}
auto makeFrame=[&](int style){RiftFrame f;f.lyrics=lyrics;f.identity=L"motion-balance";f.revision=1;f.width=640;f.height=360;
    f.kind=MotionKind(style);f.particlePreset=IsMineradioStyle(style)?style-FirstMineradioStyle:0;
    f.following=f.precise=f.playing=f.phraseEntry=true;f.motionLines=1;return f;};
// The screenshot regression: the bubble must enclose the actual projected glyphs
// and subtitles at entry, settled, next-line, portrait and custom-font sizes.
for(const auto font:{L"",L"@wenkai",L"@huangyou",L"@kuaile",L"Microsoft YaHei UI"}){
    for(const auto size:{std::pair{480.f,300.f},std::pair{360.f,640.f},std::pair{1540.f,752.f}}){
        RiftStage layer;layer.Create(device.Get(),write.Get());auto f=makeFrame(18);f.width=size.first;f.height=size.second;f.lyricFont=font;f.showTranslation=f.showRomanization=true;
        for(int64_t pos:{80,220,700,1590,1680,1900}){f.positionMs=pos;f.nowMs=100000+pos;layer.Render(f,dc.Get());const auto r=layer.Reading();
            expect(r.glyphCount>0&&r.missingGlyphs==0,"Cappella glyphs missing with selected font");
            expect(r.bubbleVisible,"Cappella bubble missing");const auto b=r.bubbleBounds;
            if(!(b.left<=r.glyphLeft&&b.right>=r.glyphRight&&b.top<=r.glyphTop&&b.bottom>=r.glyphBottom))
                std::cerr<<"Bubble mismatch "<<f.width<<"x"<<f.height<<" at "<<pos<<"\n";
            expect(b.left<=r.glyphLeft&&b.right>=r.glyphRight&&b.top<=r.glyphTop&&b.bottom>=r.glyphBottom,"Cappella bubble not attached to visible lyric bounds");
            expect(b.left>=0&&b.right<=f.width&&b.top>=0&&b.bottom<=f.height,"Cappella bubble outside stage");
            expect(r.auxiliaryVisible,"Cappella subtitles missing");const auto& m=r.auxiliaryTransform;
            for(float x:{-r.auxiliaryWidth*.5f,r.auxiliaryWidth*.5f})for(float y:{0.f,r.auxiliaryHeight}){
                const float px=x*m[0]+y*m[2]+m[4],py=x*m[1]+y*m[3]+m[5];
                expect(py>=b.bottom+5.f&&px>=0&&px<=f.width&&py<=f.height,"Cappella subtitle must follow outside below the bubble");}
        }
    }
}
// Optional fonts must reach both native renderer families; keep original theme
// typefaces when no override is chosen (also checked by art-light checks).
for(int style:AvailableBuiltInStyles){
    if(style<6){SceneRenderer renderer;renderer.CreateOffscreen(640,360,1,true);
        for(const auto font:{L"@wenkai",L"@huangyou",L"@kuaile"}){auto s=Scene(lyrics,100500,500,{});s.view.theme=style;s.view.lyricFont=font;renderer.SetOffscreenTimeMs(100500);expect(renderer.Draw(s,false),"Optional font failed ordinary lyric renderer");
            OptionalLyricFont expected;expected.Select(write.Get(),font,{});expect(renderer.OffscreenLyricFontFamily(style)==expected.Family(),"Optional font not selected in ordinary renderer");}
    }else{RiftStage layer;layer.Create(device.Get(),write.Get());auto f=makeFrame(style);f.positionMs=500;f.nowMs=100500;
        for(const auto font:{L"@wenkai",L"@huangyou",L"@kuaile"}){OptionalLyricFont expected;expected.Select(write.Get(),font,{});f.lyricFont=font;layer.Render(f,dc.Get());const auto r=layer.Reading();
            expect(r.fontFamily==expected.Family()&&r.missingGlyphs==0,"Font override did not reach theme glyph shaping");}
        f.lyricFont=L"";f.lyricFontPath=fontPath.wstring();layer.Render(f,dc.Get());expect(layer.Reading().fontFamily==L"ZCOOL KuaiLe","Imported TTF not used by theme");
    }
}
for(int style:AvailableBuiltInStyles){if(style<6)continue;
    const bool wantsFirefly=style==9||style==10||style==12||style==26;
    const bool wantsAssembly=style==6||style==8||style==19||style==28;
    RiftStage layer;layer.Create(device.Get(),write.Get());auto f=makeFrame(style);bool fireflySeen=false,assemblySeen=false;
    D2D1_POINT_2F firstFirefly{};float distance=0;RiftReading heldFrame;
    for(int64_t pos=60;pos<=1940;pos+=40){f.positionMs=pos;f.nowMs=100000+pos;layer.Render(f,dc.Get());const auto r=layer.Reading();
        expect(r.line==size_t(pos/1600),"Dense phrases lagged behind current line");
        expect(!r.fireflyVisible||wantsFirefly,"Firefly leaked to another theme");
        expect(!r.reassemblyFragments||wantsAssembly,"Reassembly leaked to another theme");
        expect(r.reassemblyFragments<=24*16*2,"Unbounded fragment count");
        if(r.fireflyVisible){if(!fireflySeen)firstFirefly=r.fireflyPosition;else distance=std::max(distance,std::hypot(r.fireflyPosition.x-firstFirefly.x,r.fireflyPosition.y-firstFirefly.y));fireflySeen=true;}
        assemblySeen|=r.reassemblyFragments>0;heldFrame=r;
    }
    if(wantsFirefly){expect(fireflySeen&&distance>15,"Firefly failed to follow the KTV frontier");expect(heldFrame.fireflyTrailSegments>0&&heldFrame.fireflyTrailSegments<=17,"Actual KTV-front trail missing or unbounded");}
    if(wantsAssembly){if(!assemblySeen)std::cerr<<"No fragments in style "<<style<<"\n";expect(assemblySeen,"Adjacent phrase never disassembled into the next phrase");}
    expect(heldFrame.reassemblyFragments==0,"Expired fragments remain on the stage");
    f.paused=true;f.playing=false;f.nowMs+=400;const auto pausedPixels=Pixels(dc.Get(),layer.Render(f,dc.Get()));const auto paused=layer.Reading();
    f.nowMs+=400;expect(Pixels(dc.Get(),layer.Render(f,dc.Get()))==pausedPixels,"Paused effect continued moving");
    if(wantsFirefly)expect(paused.fireflyPosition.x==heldFrame.fireflyPosition.x&&paused.fireflyPosition.y==heldFrame.fireflyPosition.y,"Paused firefly center moved");
    f.paused=false;f.playing=true;f.discontinuity=true;f.positionMs=3250;f.nowMs+=17;layer.Render(f,dc.Get());expect(layer.Reading().reassemblyFragments==0&&layer.Reading().fireflyTrailSegments==0,"Seek replayed old fragments/trail");
    f.discontinuity=false;f.identity=L"different-track";f.positionMs=200;f.nowMs+=17;layer.Render(f,dc.Get());expect(layer.Reading().reassemblyFragments==0,"Track change retained old fragments");
    f.reduced=true;f.positionMs=1660;f.nowMs+=1460;layer.Render(f,dc.Get());expect(!layer.Reading().fireflyVisible&&layer.Reading().reassemblyFragments==0,"Reduced motion retained extra animated handoff");
}
// Change tracks in the middle of a transition, while the new position happens
// to overlap the old handoff's timestamp. No old composite may survive.
for(int style:{6,7,8,19,28}){
    RiftStage old,fresh;old.Create(device.Get(),write.Get());fresh.Create(device.Get(),write.Get());auto f=makeFrame(style);
    for(int64_t pos:{1500,1590,1620,1660}){f.positionMs=pos;f.nowMs=100000+pos;old.Render(f,dc.Get());}
    f.identity=L"changed-during-handoff";f.positionMs=1700;f.nowMs+=40;
    const auto switched=Pixels(dc.Get(),old.Render(f,dc.Get())),clean=Pixels(dc.Get(),fresh.Render(f,dc.Get()));
    expect(switched==clean,"Track change retained an old transition composite");
}
std::cout<<"Motion balance/font/bubble checks passed: "<<passed<<"; synthetic native checks, no exports\n";

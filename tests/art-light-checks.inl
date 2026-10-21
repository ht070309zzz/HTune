// Typography must reach the actual product path, including embedded/system
// collection switches on the same renderer. No exported images or media.
{
    ComPtr<IDWriteFontCollection> collection;Hr(write->GetSystemFontCollection(&collection));
    const auto serif=ResolveLyricTypeface(collection.Get(),LyricTypefaceKind::Serif);
    SceneRenderer renderer;renderer.CreateOffscreen(800,450,1,true);
    auto cinema=Scene(Demo(),599000,3000,{});cinema.view.theme=1;cinema.view.reducedMotion=true;
    renderer.SetOffscreenTimeMs(599000);expect(renderer.Draw(cinema,false),"Cinema reference render failed");
    const auto cinemaFace=renderer.OffscreenLyricFontFamily(1);
    expect(cinemaFace==serif.family,"Cinema reference lost its original serif face");
    for(int style:AvailableBuiltInStyles){
        const int64_t now=600000+style*1000;
        auto s=Scene(Demo(),now,3000,{});s.view.theme=style;s.view.reducedMotion=true;
        renderer.SetOffscreenTimeMs(now);expect(renderer.Draw(s,false),"Art font product render failed");
        const auto face=renderer.OffscreenLyricFontFamily(style);
        std::wcout<<L"Art font style="<<style<<L" family="<<face<<L"\n";
        expect(!face.empty(),"Product lyric has no selected art face");
        expect(face==cinemaFace,"Theme default differs from cinema's actual font");
    }
    auto mixed=std::make_shared<LyricTimeline>();mixed->state=LyricLoadState::Ready;
    LyricLine l;l.text=L"幸福吧 光影 e\u0301 office 音乐";l.durationMs=9000;mixed->lines={l};
    RiftStage layer;layer.Create(device.Get(),write.Get());
    for(int style:AvailableBuiltInStyles){if(style<6)continue;
        RiftFrame f;f.lyrics=mixed;f.identity=L"mixed-art-font";f.revision=1;f.width=800;f.height=450;f.positionMs=4000;f.nowMs=800000+style*1000;
        f.kind=MotionKind(style);f.particlePreset=style>=21?style-21:0;f.reduced=f.following=true;f.motionLines=1;
        layer.Render(f,dc.Get());const auto r=layer.Reading();
        expect(r.valid&&r.visibleClusters>0,"Mixed art text disappeared");expect(r.missingGlyphs==0,"Art font fallback produced a missing glyph");
        expect(r.layoutOverlaps==0,"Art face produced overlapping settled glyphs");
        expect(!r.embeddedFont&&r.fontFamily==cinemaFace,"Default glyph path kept the previous embedded face");
    }
}
// Compare actual native background pixels with equal clocks and different
// audio, then different clocks and equal audio. Letter geometry is checked by
// the separate scope suite. Still intentionally retains its static scenery.
{
    for(int style:AvailableBuiltInStyles){if(style<6||style==20)continue;
        RiftFrame a;a.width=480;a.height=300;a.kind=MotionKind(style);a.particlePreset=style>=21?style-21:0;
        a.identity=L"background-art";a.following=a.playing=true;a.positionMs=2000;a.nowMs=902000;a.quality=2;
        const auto theme=BuiltInTheme(style);a.background={theme.background.r,theme.background.g,theme.background.b,1};
        a.accent={.85f,.65f,.3f,1};a.response=1;
        RiftStage quiet,loud;quiet.Create(device.Get(),write.Get());loud.Create(device.Get(),write.Get());
        const auto low=BitmapPixels(dc.Get(),quiet.Render(a,dc.Get()));
        auto b=a;b.low=.8f;b.high=.6f;b.rms=.5f;b.pulse=.65f;b.spectrum.fill(.5f);
        const auto high=BitmapPixels(dc.Get(),loud.Render(b,dc.Get()));
        const auto audioPixels=ChangedPixels(low,high);std::cout<<"Background style="<<style<<" audioPixels="<<audioPixels<<"\n";
        expect(audioPixels>100,"Enhanced background has no audio feedback");
        a.positionMs+=1700;a.nowMs+=1700;
        expect(ChangedPixels(low,BitmapPixels(dc.Get(),quiet.Render(a,dc.Get())))>100,"Enhanced background is static while advancing");
    }
}

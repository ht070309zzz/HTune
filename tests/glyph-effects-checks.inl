// Actual compiled HLSL and both product integration paths. Internal checks only;
// no export, user profile, window, audio capture or media control.
#include "lyric-fairies-checks.inl"
GlyphEffects effects;effects.Create(device.Get());
expect(GlyphSingingLight(0)==0&&GlyphSingingLight(1)>.20f&&GlyphSingingLight(1)<.5f&&GlyphSingingLight(.5f)>2*GlyphSingingLight(1),"Sung-light residue is missing or overpowers the active syllable");
expect(std::abs(GlyphSingingLight(.99999f)-GlyphSingingLight(1))<.01f,"Sung light drops abruptly at word completion");
D3D11_TEXTURE2D_DESC desc{};desc.Width=512;desc.Height=288;desc.ArraySize=desc.MipLevels=1;
desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> output;ComPtr<IDXGISurface> surface;ComPtr<ID2D1Bitmap1> bitmap;
Hr(device->CreateTexture2D(&desc,nullptr,&texture));Hr(device->CreateRenderTargetView(texture.Get(),nullptr,&output));Hr(texture.As(&surface));
const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(desc.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
Hr(dc->CreateBitmapFromDxgiSurface(surface.Get(),&properties,&bitmap));
GlyphEffectFrame frame;frame.width=512;frame.height=288;frame.seconds=3.25f;frame.clip={40,40,472,250};frame.advancing=true;frame.accent={.4f,.75f,1,1};
GlyphEmitter emitter;emitter.corners={D2D1::Point2F(216,104),D2D1::Point2F(296,104),D2D1::Point2F(296,184),D2D1::Point2F(216,184)};
emitter.color={.4f,.75f,1,1};emitter.age=.24f;emitter.active=1;emitter.progress=.4f;emitter.seed=173;
expect(frame.Add(emitter),"Valid letter emitter rejected");
auto render=[&](const GlyphEffectFrame& f){const float clear[4]{};ctx->ClearRenderTargetView(output.Get(),clear);effects.Draw(ctx.Get(),output.Get(),512,288,f);return BitmapPixels(dc.Get(),bitmap.Get());};
std::vector<std::vector<uint8_t>> signatures;
for(int style:AvailableBuiltInStyles){
    effects.Reset();frame.style=style;frame.presentationMs=3250;frame.seconds=3.25f;frame.reduced=false;frame.quality=0;
    const auto pixels=render(frame);size_t visible=0,invalid=0,interior=0,outside=0;
    for(int y=0;y<288;++y)for(int x=0;x<512;++x){const auto* p=pixels.data()+(y*512+x)*4;
        if(p[3]>5)++visible;if(p[0]>p[3]+1||p[1]>p[3]+1||p[2]>p[3]+1)++invalid;
        if(x>234&&x<278&&y>122&&y<166&&p[3]>0)++interior;
        if((x<40||x>=472||y<40||y>=250)&&p[3]>0)++outside;
    }
    std::cout<<"Particle style "<<style<<" pixels="<<visible<<"\n";
    expect(visible>45,"Theme particle material has no visible pixels");expect(!invalid,"Particle alpha is not premultiplied");
    expect(!interior,"Particle material obscures the glyph interior");expect(!outside,"Particle escaped stage clipping");
    expect(pixels==render(frame),"Frozen particle time changed between identical draws");
    frame.presentationMs+=250;frame.seconds+=.25f;expect(ChangedPixels(pixels,render(frame))>20,"Particle path has no continuous motion");
    effects.Reset();frame.presentationMs-=250;frame.seconds-=.25f;expect(pixels==render(frame),"Seeking back did not deterministically reconstruct particles");
    frame.reduced=true;auto still=render(frame);expect(std::all_of(still.begin(),still.end(),[](auto v){return v==0;}),"Reduced motion still emits particles");
    frame.reduced=false;frame.quality=3;auto low=render(frame);expect(ChangedPixels(low,pixels)>5,"Quality budget does not reduce particle detail");
    for(const auto& earlier:signatures)expect(ChangedPixels(pixels,earlier)>5,"Two themes share an identical particle field");
    signatures.push_back(pixels);
}
// Sustained native pixels with discrete media updates and a continuous display
// clock. The first frame alone cannot establish whether a trail is visible.
{
    effects.Reset();auto f=frame;f.count=1;f.quality=0;f.style=9;f.reduced=false;f.advancing=true;
    size_t tailPixels=0,minChanges=SIZE_MAX;std::vector<uint8_t> previous;
    for(int n=0;n<=120;++n){f.presentationMs=10000+std::lround(n*1000./60);f.seconds=std::floor(n/12.f)*.2f;
        const auto pixels=render(f);
        if(n>30){size_t lit=0;for(size_t i=3;i<pixels.size();i+=4)if(pixels[i]>12)++lit;
            tailPixels=std::max(tailPixels,lit);minChanges=std::min(minChanges,ChangedPixels(previous,pixels));
            expect(effects.TrailSamples()>30,"GPU trail keeps resetting between packets");
            expect(lit>110,"Continuous comet tail is too sparse to register");}
        previous=pixels;
    }
    std::cout<<"Comet sustained pixels="<<tailPixels<<" minimum changing pixels="<<minChanges<<"\n";
    expect(minChanges>5,"Native comet pixels freeze between media packets");
    f.paused=true;const auto heldPixels=render(f);f.presentationMs+=200;
    expect(heldPixels==render(f),"Paused native comet changes despite unchanged singing position");
}
// Capacity, invalid geometry, early-word and empty-frame safety.
{
    auto trail=frame;trail.count=1;trail.quality=0;trail.style=9;
    auto& e=trail.emitters[0];e.padding=3;e.active=1;e.age=.3f;
    e.corners={D2D1_POINT_2F{120,72},D2D1_POINT_2F{280,72},D2D1_POINT_2F{282,74},D2D1_POINT_2F{118,70}};
    // Legacy rigid ribbon markers must no longer produce a horizontal beam.
    const auto visible=render(trail);
    expect(std::all_of(visible.begin(),visible.end(),[](auto value){return value==0;}),"Legacy ribbon still produces a rigid light bar");
    trail.emitters[0].active=0;const auto faded=render(trail);
    expect(std::all_of(faded.begin(),faded.end(),[](auto value){return value==0;}),"Expired history trail leaves a visible ghost");
}
{
    auto firefly=frame;firefly.count=1;firefly.quality=0;firefly.style=9;firefly.emitters[0].padding=2;
    const auto first=render(firefly);size_t lit=0;for(size_t i=3;i<first.size();i+=4)if(first[i]>5)++lit;
    expect(lit>30,"Firefly HLSL produced no visible light");
    firefly.presentationMs+=250;firefly.seconds+=.25f;expect(ChangedPixels(first,render(firefly))>30,"Firefly HLSL path did not move");
    effects.Reset();firefly.presentationMs-=250;firefly.seconds-=.25f;expect(first==render(firefly),"Firefly HLSL time is not deterministic");
}
frame.count=0;for(size_t i=0;i<MaxGlyphEmitters;++i)expect(frame.Add(emitter),"Emitter budget filled prematurely");
expect(!frame.Add(emitter),"Emitter count exceeded the bounded GPU buffer");
frame.count=0;auto bad=emitter;bad.age=-.01f;expect(!frame.Add(bad),"Future glyph can emit before its entrance");
bad=emitter;bad.corners[0].x=std::numeric_limits<float>::infinity();expect(!frame.Add(bad),"Invalid projected position accepted");
auto empty=render(frame);expect(std::all_of(empty.begin(),empty.end(),[](auto v){return v==0;}),"Empty frame retained old particles");
// First six D2D-backed themes and later GPU themes must both submit actual
// current lyric anchors, including window, desktop and Backstage composition.
for(int style:AvailableBuiltInStyles){
    SceneRenderer renderer;renderer.CreateOffscreen(800,450,1,true);size_t peak=0;
    for(int n=0;n<25;++n){const auto now=400000+n*16;auto s=Scene(Demo(),now,1400+n*16,{});s.view.theme=style;s.view.panelVisible=false;
        s.view.desktop=n>=12;s.view.backstage=n>=20;s.view.glass=n>=20;
        renderer.SetOffscreenTimeMs(now);expect(renderer.Draw(s,false),"Particle integration frame failed");peak=std::max(peak,renderer.OffscreenGlyphEmitterCount());}
    std::cout<<"Product style "<<style<<" emitters="<<peak<<"\n";
    expect(peak>0&&peak<=MaxGlyphEmitters,"Product theme did not submit bounded glyph particles");
    auto s=Scene(Demo(),401000,2400,{});s.view.theme=style;s.view.reducedMotion=true;renderer.SetOffscreenTimeMs(401000);renderer.Draw(s,false);
    expect(renderer.OffscreenGlyphEmitterCount()==0,"Product reduced motion left glyph particles active");
    s.lyrics=std::make_shared<LyricTimeline>();s.lyricRevision=s.lyricReadingRevision=2;s.view.reducedMotion=false;renderer.SetOffscreenTimeMs(401020);renderer.Draw(s,false);
    expect(renderer.OffscreenGlyphEmitterCount()==0,"Product cleared lyric still has stale particle anchors");
}
// Product clocks in all 28 themes, not just the standalone simulation. The
// immutable timeline is retained, as it is in a normal scene between packets.
for(int style:AvailableBuiltInStyles){
    SceneRenderer renderer;renderer.CreateOffscreen(640,360,1,true);auto lyrics=Demo();
    double lastTime=0;size_t moved=0,peakTrail=0;
    for(int n=0;n<=85;++n){const int64_t now=600000+std::lround(n*1000./60),packetAt=600000+(n/12)*200;
        auto s=Scene(lyrics,packetAt,1600+(n/12)*200,{});s.frameAtMs=now;s.view.theme=style;s.view.desktop=true;
        s.playback.state=PlayState::Unknown;s.playback.nativeClockAdvanceAllowed=false;
        renderer.SetOffscreenTimeMs(now);expect(renderer.Draw(s,false),"Packetized product frame failed");
        const auto time=renderer.OffscreenCometFlightSeconds();peakTrail=std::max(peakTrail,renderer.OffscreenCometTrailSamples());
        if(n>24&&time>lastTime)++moved;lastTime=time;
    }
    std::cout<<"Product comet style "<<style<<" continuous frames="<<moved<<" peak history="<<peakTrail<<"\n";
    expect(moved>=55,"Product clock freezes comets between sampled progress packets");
    expect(peakTrail>40,"Product renderer does not retain a continuous comet tail");
    const bool checkedRelay=style==0||style==1||style==2||style==6||style==9||style==17||style==21;
    if(checkedRelay){
        for(int n=0;n<75;++n){const int64_t now=601450+n*33;auto next=Scene(lyrics,now,3050+n*33,{});next.view.theme=style;next.view.desktop=true;
            renderer.SetOffscreenTimeMs(now);expect(renderer.Draw(next,false),"Product relay frame failed");}
        std::cout<<"Product phrase relays style "<<style<<" count="<<renderer.OffscreenCometRelayCount()<<"\n";
        expect(renderer.OffscreenCometRelayCount()>0,"Actual product phrase change never triggers handoff");
    }
    auto s=Scene(lyrics,checkedRelay?603950:601450,checkedRelay?5550:3000,{});s.view.theme=style;s.playback.state=PlayState::Paused;
    renderer.SetOffscreenTimeMs(s.frameAtMs);renderer.Draw(s,false);const auto frozenTime=renderer.OffscreenCometFlightSeconds();
    s.frameAtMs+=200;renderer.SetOffscreenTimeMs(s.frameAtMs);renderer.Draw(s,false);
    expect(renderer.OffscreenCometFlightSeconds()==frozenTime,"Product pause does not freeze comet time");
}
std::cout<<"Glyph particle checks passed: "<<passed<<"; native synthetic GPU data, no exports\n";

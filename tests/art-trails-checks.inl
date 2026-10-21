// Compare actual raster output across DPI scales, not just shared geometry
// counters. No images/videos or normal-profile/window operations are exported.
{
    auto caption=std::make_shared<LyricTimeline>();caption->state=LyricLoadState::Ready;caption->wordTimed=true;
    LyricLine l;l.text=L"当我需要独自站在";l.startMs=0;l.durationMs=8000;l.translation=L"Stay close to the words";
    for(size_t i=0;i<l.text.size();++i)l.words.push_back({int64_t(i)*1000,1000,l.text.substr(i,1)});
    caption->lines={l};
    for(int bg:{0,1,2,3,4}){
        RiftFrame f;f.lyrics=caption;f.kind=LyricMotion::Cappella;f.identity=L"dpi-caption";f.width=640;f.height=360;
        f.following=f.reduced=true;f.positionMs=7500;f.nowMs=107500;f.showTranslation=true;f.motionLines=1;f.motionBackground=bg;
        RiftStage baseline,baseGround;baseGround.Create(device.Get(),write.Get());baseline.Create(device.Get(),write.Get());const auto base=BitmapPixels(dc.Get(),baseline.Render(f,dc.Get()));
        auto empty=f;empty.lyrics={};const auto baseBackground=BitmapPixels(dc.Get(),baseGround.Render(empty,dc.Get()));
        for(float scale:{1.f,1.25f,1.5f,2.f}){
            RiftStage layer;layer.Create(device.Get(),write.Get());f.dpi=scale;
            const auto pixels=BitmapPixels(dc.Get(),layer.Render(f,dc.Get()));const auto r=layer.Reading();
            RiftStage ground;ground.Create(device.Get(),write.Get());empty=f;empty.lyrics={};const auto background=BitmapPixels(dc.Get(),ground.Render(empty,dc.Get()));
            expect(r.bubbleVisible&&r.auxiliaryVisible,"Bubble missing with selected background");
            expect(r.auxiliaryTransform[5]>r.bubbleBounds.bottom+5,"Translation moved into the bubble");
            const int w=int(640*scale);double delta=0;size_t count=0;
            // Broad rectangular plate is measurable even when glyph pixels
            // differ slightly due to raster hints at different DPI scales.
            for(int y=30;y<310;++y)for(int x=20;x<620;++x){
                const size_t a=(size_t(y)*640+x)*4,b=(size_t(int((y+.5f)*scale))*w+int((x+.5f)*scale))*4;
                // The selected background can contain DPI-dependent halftone
                // samples. Subtract it to compare the caption layer itself.
                for(int c=0;c<4;++c){delta+=std::abs(int(base[a+c])-int(baseBackground[a+c])-int(pixels[b+c])+int(background[b+c]));++count;}
            }
            std::cout<<"Cappella DPI="<<scale<<" background="<<bg<<" mean pixel error="<<delta/count<<"\n";
            expect(delta/count<8,"D2D bubble/program layer does not align with GPU lyrics at high DPI");
            double plateError=0,plateLight=0;size_t plateSamples=0;
            const auto b=r.bubbleBounds;
            for(int y=int(b.top+3);y<int(b.top+9);++y)for(int x=int(b.left+20);x<int(b.right-20);++x){
                if(x<0||x>=640||y<0||y>=360)continue;
                // Measure the plate edge, subtracting a matching sample just
                // outside it. Correctly attached glow now reaches both sides;
                // its smooth field must not be mistaken for a displaced plate.
                const int outsideY=int(2*b.top-y-1);
                if(outsideY<0)continue;
                const size_t a=(size_t(y)*640+x)*4,z=(size_t(int((y+.5f)*scale))*w+int((x+.5f)*scale))*4;
                const size_t ao=(size_t(outsideY)*640+x)*4,zo=(size_t(int((outsideY+.5f)*scale))*w+int((x+.5f)*scale))*4;
                for(int c=0;c<3;++c){const int expected=int(base[a+c])-int(baseBackground[a+c])-int(base[ao+c])+int(baseBackground[ao+c]);
                    const int actual=int(pixels[z+c])-int(background[z+c])-int(pixels[zo+c])+int(background[zo+c]);
                    plateError+=std::abs(expected-actual);plateLight+=std::abs(actual);++plateSamples;}
            }
            expect(plateSamples>0&&plateLight/plateSamples>3,"Speech plate has no pixels at its reported lyric-relative location");
            std::cout<<"  isolated plate error="<<plateError/plateSamples<<" light="<<plateLight/plateSamples<<"\n";
            // Locate the sharp top edge instead of assuming identical glow
            // energy from differently hinted serif masks at every DPI.
            float edgeY=0;double edgeStrength=-1;
            // Stay at the plate lip: the glyph's top stroke is a stronger
            // edge roughly ten DIPs below it and is not the feature under test.
            for(int y=int(std::ceil(b.top))-2;y<=int(std::floor(b.top))+2;++y){if(y<3||y+3>=360)continue;
                double contrast=0;size_t samples=0;
                for(int x=int(b.left+20);x<int(b.right-20);++x){if(x<0||x>=640)continue;
                    const size_t hi=(size_t(int((y-2.5f)*scale))*w+int((x+.5f)*scale))*4;
                    const size_t lo=(size_t(int((y+3.5f)*scale))*w+int((x+.5f)*scale))*4;
                    for(int c=0;c<3;++c){contrast+=int(pixels[lo+c])-int(background[lo+c])-int(pixels[hi+c])+int(background[hi+c]);++samples;}}
                if(samples&&contrast/samples>edgeStrength){edgeStrength=contrast/samples;edgeY=y+.5f;}}
            std::cout<<"  plate edge="<<edgeY<<" expected="<<b.top<<" strength="<<edgeStrength<<"\n";
            expect(edgeStrength>12&&std::abs(edgeY-b.top)<3,"Raster speech plate is displaced from the glyph projection");
        }
    }
}
{
    int pops=0;for(int style:AvailableBuiltInStyles)if(style>=6&&UsesSyllableEntrance(MotionKind(style),style>=21?style-21:0))++pops;
    expect(pops==11,"Theme split does not match 11 syllable-entry / 17 KTV");
    for(int style:{9,10,12,26})expect(!UsesSyllableEntrance(MotionKind(style),style>=21?style-21:0),"Firefly KTV theme lost karaoke");
    auto text=std::make_shared<LyricTimeline>();text->state=LyricLoadState::Ready;text->wordTimed=true;
    LyricLine l;l.text=L"幸福吧一路向前";l.durationMs=7000;
    for(size_t i=0;i<l.text.size();++i)l.words.push_back({int64_t(i)*1000,1000,l.text.substr(i,1)});text->lines={l};
    for(int style:{6,7,8,11,13,14,17,18,19,24,29}){
        RiftStage layer;layer.Create(device.Get(),write.Get());RiftFrame f;f.lyrics=text;f.identity=L"word-entrance";f.width=800;f.height=450;
        f.kind=MotionKind(style);f.particlePreset=style>=21?style-21:0;f.following=f.playing=f.precise=true;f.motionLines=1;
        size_t before=0;for(int64_t pos:{400,1400,2400}){f.positionMs=pos;f.nowMs=200000+pos;layer.Render(f,dc.Get());const auto r=layer.Reading();
            expect(r.glyphCount>before,"New syllable theme still displays the entire sentence at once");before=r.glyphCount;
            for(size_t i=0;i<r.glyphCount;++i){expect(r.glyphs[i].fill==1,"Syllable theme still sweeps karaoke color");expect(r.glyphs[i].text==i,"Syllable entrance reordered source text");}}
    }
}
std::cout<<"Art/trail/DPI checks passed: "<<passed<<"; internal synthetic pixels, not real playback acceptance\n";

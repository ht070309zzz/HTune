// Compare the real composited light with the actual glyph position. The former
// "some pixels outside the quad" check could pass with a displaced glow blob.
{
    auto lyric=std::make_shared<LyricTimeline>();lyric->state=LyricLoadState::Ready;lyric->wordTimed=true;
    LyricLine l;l.text=L"光";l.durationMs=6000;l.words={{0,6000,l.text}};lyric->lines={l};
    RiftStage ground,lit;ground.Create(device.Get(),write.Get());lit.Create(device.Get(),write.Get());
    struct Surface{float width,height,dpi;};
    for(const auto surface:{Surface{800,450,1},Surface{800,450,1.25f},Surface{800,450,2},
            Surface{1600,900,1},Surface{803,451,1.5f},Surface{800,450,1}})for(int quality:{0,2}){
        const float dpi=surface.dpi;
        RiftFrame f;f.width=surface.width;f.height=surface.height;f.dpi=dpi;f.quality=quality;f.kind=LyricMotion::Rift;
        f.following=f.precise=f.reduced=true;f.positionMs=3000;f.nowMs=103000;f.identity=L"attached-glow";
        f.motionLines=1;f.accent={.25f,.8f,1.f,1};
        const auto background=BitmapPixels(dc.Get(),ground.Render(f,dc.Get()));f.lyrics=lyric;
        const auto pixels=BitmapPixels(dc.Get(),lit.Render(f,dc.Get()));const auto r=lit.Reading();
        expect(r.glyphCount==1,"Glow alignment fixture lost its glyph");
        const int w=int(std::ceil(f.width*dpi)),h=int(std::ceil(f.height*dpi));size_t detached=0,halo=0;double distance=0,light=0;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){const size_t at=(size_t(y)*w+x)*4;
            int delta=0;for(int c=0;c<3;++c)delta+=std::abs(int(pixels[at+c])-int(background[at+c]));
            const float px=(x+.5f)/dpi,py=(y+.5f)/dpi;
            if(px>=r.glyphLeft-3&&px<=r.glyphRight+3&&py>=r.glyphTop-3&&py<=r.glyphBottom+3)continue;
            if(delta>12){++halo;const float dx=std::max({r.glyphLeft-px,0.f,px-r.glyphRight});
                const float dy=std::max({r.glyphTop-py,0.f,py-r.glyphBottom});
                if(dx>90||dy>90)++detached;distance+=std::hypot(dx,dy)*delta;light+=delta;}
        }
        std::cout<<"Glyph glow size="<<f.width<<"x"<<f.height<<" dpi="<<dpi<<" quality="<<quality<<" halo="<<halo<<" detached="<<detached<<" meanDistance="<<(light?distance/light:0)<<"\n";
        expect(halo>50,"Attached glyph has no diffuse light");
        expect(detached<8,"Glyph glow appears as a detached light away from its letter");
        expect(light>0&&distance/light<35,"Glyph light is concentrated away from the letter");
    }
}

// Actual product composition, no exported pictures or normal-profile access.
// Mint ink is deliberately below white: near-white additions are clipped light,
// not the intended glyph colour. Reduced motion isolates glow from particles.
{
    auto words=std::make_shared<LyricTimeline>();words->state=LyricLoadState::Ready;words->wordTimed=true;
    LyricLine row;row.text=L"宁愿笨也不想要悔恨";row.durationMs=12000;
    for(size_t i=0;i<row.text.size();++i)row.words.push_back({int64_t(i)*1000,1000,row.text.substr(i,1)});
    words->lines={row};size_t clippedTotal=0;int cases=0;
    for(const auto kind:{LyricMotion::Partita,LyricMotion::Fume,LyricMotion::Rift})
        for(const auto portrait:{false,true})for(const auto quality:{0,2}){
            RiftFrame f;f.width=portrait?576.f:960.f;f.height=portrait?768.f:540.f;f.dpi=portrait?1.25f:1.f;
            f.kind=kind;f.quality=quality;f.following=f.precise=f.reduced=true;f.positionMs=8500;f.nowMs=108500;
            f.identity=L"soft-halo-exposure";f.motionLines=1;f.accent={.35f,.78f,.65f,1};
            RiftStage empty,lyric;empty.Create(device.Get(),write.Get());lyric.Create(device.Get(),write.Get());
            const auto ground=BitmapPixels(dc.Get(),empty.Render(f,dc.Get()));f.lyrics=words;
            const auto pixels=BitmapPixels(dc.Get(),lyric.Render(f,dc.Get()));const auto r=lyric.Reading();
            expect(r.glyphCount>0,"Exposure fixture has no letters");
            size_t clipped=0,nearLight=0;double outerEnergy=0;const int w=int(std::ceil(f.width*f.dpi)),h=int(std::ceil(f.height*f.dpi));
            for(int y=0;y<h;++y)for(int x=0;x<w;++x){const size_t k=(size_t(y)*w+x)*4;
                const int delta=int(pixels[k])+pixels[k+1]+pixels[k+2]-ground[k]-ground[k+1]-ground[k+2];
                if(delta>240&&pixels[k]>246&&pixels[k+1]>246&&pixels[k+2]>246)++clipped;
                const float px=(x+.5f)/f.dpi,py=(y+.5f)/f.dpi;float distance=1e9f;
                for(size_t i=0;i<r.glyphCount;++i){const auto& g=r.glyphs[i];
                    const float dx=std::max({g.left-px,0.f,px-g.right}),dy=std::max({g.top-py,0.f,py-g.bottom});
                    distance=std::min(distance,std::hypot(dx,dy));}
                if(distance>3&&distance<25&&delta>3){++nearLight;outerEnergy+=delta;}
            }
            std::cout<<"Halo exposure kind="<<int(kind)<<" portrait="<<portrait<<" quality="<<quality
                <<" clipped="<<clipped<<" nearPixels="<<nearLight<<" outerEnergy="<<outerEnergy<<"\n";
            clippedTotal+=clipped;++cases;
            expect(nearLight>50,"Lower gain removed the visible soft edge");
        }
    std::cout<<"Halo exposure cases="<<cases<<" clippedTotal="<<clippedTotal<<"\n";
    expect(clippedTotal==0,"Glyph halo clips into solid white patches");
}
#include "cinema-glow-checks.inl"
std::cout<<"Soft halo checks passed: "<<passed<<"; synthetic native pixels only, no exports\n";

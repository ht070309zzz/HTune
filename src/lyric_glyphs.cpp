#include "lyric_glyphs.h"
#include <algorithm>

namespace stage::glyphs {
namespace {
// Stack-scoped text renderer used only while IDWriteTextLayout::Draw runs.
// DirectWrite does not retain it after Draw returns.
class Collector final : public IDWriteTextRenderer {
public:
    explicit Collector(Layout& target):target_(target){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,void** object) override {
        if(!object)return E_POINTER;
        if(riid==__uuidof(IUnknown)||riid==__uuidof(IDWritePixelSnapping)||riid==__uuidof(IDWriteTextRenderer)){
            *object=static_cast<IDWriteTextRenderer*>(this);AddRef();return S_OK;
        }
        *object=nullptr;return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {return references_>1?--references_:1;}
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* disabled) override {
        if(!disabled)return E_POINTER;*disabled=TRUE;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* transform) override {
        if(!transform)return E_POINTER;*transform={1,0,0,1,0,0};return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* pixels) override {
        if(!pixels)return E_POINTER;*pixels=1;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE mode,
        const DWRITE_GLYPH_RUN* glyphRun,const DWRITE_GLYPH_RUN_DESCRIPTION* description,IUnknown*) override {
        if(!glyphRun||!glyphRun->fontFace||!glyphRun->glyphCount||!glyphRun->glyphIndices)return S_OK;
        const UINT32 count=glyphRun->glyphCount;
        if(count>4096||!glyphRun->glyphAdvances){unsupported_=true;return S_OK;}
        Run run;
        run.face=glyphRun->fontFace;run.emSize=glyphRun->fontEmSize;run.bidiLevel=glyphRun->bidiLevel;
        run.sideways=glyphRun->isSideways;run.measuring=mode;run.origin={x,y};
        if(run.sideways){unsupported_=true;return S_OK;}
        run.indices.assign(glyphRun->glyphIndices,glyphRun->glyphIndices+count);
        run.advances.assign(glyphRun->glyphAdvances,glyphRun->glyphAdvances+count);
        if(glyphRun->glyphOffsets)run.offsets.assign(glyphRun->glyphOffsets,glyphRun->glyphOffsets+count);
        else run.offsets.assign(count,DWRITE_GLYPH_OFFSET{0,0});
        DWRITE_FONT_METRICS metrics{};glyphRun->fontFace->GetMetrics(&metrics);
        const float unit=metrics.designUnitsPerEm?run.emSize/metrics.designUnitsPerEm:0.f;
        run.ascent=metrics.ascent*unit;run.descent=metrics.descent*unit;
        run.pen.resize(count);
        float pen=0;
        for(UINT32 i=0;i<count;++i){run.pen[i]=pen;pen+=run.advances[i];}
        const UINT32 base=description?description->textPosition:0;
        run.textPositions.assign(count,base);run.textLengths.assign(count,0);
        if(description&&description->clusterMap&&description->stringLength){
            const UINT32 length=description->stringLength;
            UINT32 c=0;
            while(c<length){
                const UINT32 glyphStart=description->clusterMap[c];
                UINT32 next=c+1;
                while(next<length&&description->clusterMap[next]==glyphStart)++next;
                const UINT32 glyphEnd=next<length?description->clusterMap[next]:count;
                for(UINT32 g=glyphStart;g<std::min(glyphEnd,count);++g){
                    run.textPositions[g]=base+c;
                    run.textLengths[g]=g==glyphStart?next-c:0;
                }
                c=next;
            }
        }
        target_.runs.push_back(std::move(run));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*) override {
        unsupported_=true;return S_OK;
    }
    bool unsupported_=false;
private:
    Layout& target_;
    ULONG references_=1;
};
}

HRESULT Capture(IDWriteTextLayout* layout,Layout& out) {
    out=Layout{};
    if(!layout)return E_POINTER;
    Collector collector(out);
    const HRESULT result=layout->Draw(nullptr,&collector,0,0);
    if(FAILED(result)){out=Layout{};return result;}
    DWRITE_TEXT_METRICS metrics{};
    if(SUCCEEDED(layout->GetMetrics(&metrics))){out.width=metrics.widthIncludingTrailingWhitespace;out.height=metrics.height;}
    out.valid=!collector.unsupported_&&!out.runs.empty();
    if(!out.valid)out.runs.clear();
    return S_OK;
}

DWRITE_GLYPH_RUN Slice(const Run& run,size_t first,size_t last) {
    DWRITE_GLYPH_RUN result{};
    first=std::min(first,run.indices.size());
    last=std::clamp(last,first,run.indices.size());
    result.fontFace=run.face.Get();result.fontEmSize=run.emSize;
    result.glyphCount=static_cast<UINT32>(last-first);
    result.glyphIndices=run.indices.data()+first;
    result.glyphAdvances=run.advances.data()+first;
    result.glyphOffsets=run.offsets.data()+first;
    result.isSideways=run.sideways;result.bidiLevel=run.bidiLevel;
    return result;
}
}

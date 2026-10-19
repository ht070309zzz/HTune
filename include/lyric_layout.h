#pragma once
#include <dwrite.h>
#include <array>
#include <cmath>
#include <string_view>

namespace stage::detail {
// Only called on a fresh, untrimmed main-lyric layout before cache publication.
// Keep text/font/ranges intact; callers center the narrowed box in its column.
inline HRESULT BalanceCenteredLyricLayout(IDWriteTextLayout* layout,std::wstring_view text) noexcept {
    if(!layout||text.empty()||text.size()>512||layout->GetTextAlignment()!=DWRITE_TEXT_ALIGNMENT_CENTER)return S_OK;
    const float originalWidth=layout->GetMaxWidth();
    if(!std::isfinite(originalWidth)||originalWidth<2.f)return S_OK;
    DWRITE_TEXT_METRICS metrics{};
    auto hr=layout->GetMetrics(&metrics);if(FAILED(hr))return hr;
    if(metrics.lineCount!=2)return S_OK;
    std::array<DWRITE_LINE_METRICS,2> lines{};UINT32 count=0;
    hr=layout->GetLineMetrics(lines.data(),static_cast<UINT32>(lines.size()),&count);if(FAILED(hr))return hr;
    const auto visible=[](const DWRITE_LINE_METRICS& line){
        return line.length>=line.trailingWhitespaceLength?line.length-line.trailingWhitespaceLength:0;
    };
    const auto complete=[&]{return count==2&&!lines[0].newlineLength&&!lines[1].newlineLength&&
        !lines[0].isTrimmed&&!lines[1].isTrimmed&&
        static_cast<size_t>(lines[0].length)+lines[1].length==text.size();};
    if(!complete()||!visible(lines[1])||visible(lines[1])>2)return S_OK;
    float lower=originalWidth*.5f,upper=originalWidth,best=originalWidth;
    for(unsigned attempt=0;attempt<8;++attempt){
        const float candidate=(lower+upper)*.5f;
        hr=layout->SetMaxWidth(candidate);if(FAILED(hr))return hr;
        hr=layout->GetMetrics(&metrics);if(FAILED(hr))return hr;
        if(metrics.lineCount>2||!std::isfinite(metrics.widthIncludingTrailingWhitespace)||
                metrics.widthIncludingTrailingWhitespace>candidate+.5f){lower=candidate;continue;}
        upper=candidate;
        if(metrics.lineCount!=2)continue;
        hr=layout->GetLineMetrics(lines.data(),static_cast<UINT32>(lines.size()),&count);if(FAILED(hr))return hr;
        if(complete()&&visible(lines[0])>=3&&visible(lines[1])>=3)best=candidate;
    }
    return layout->SetMaxWidth(best);
}
} // namespace stage::detail

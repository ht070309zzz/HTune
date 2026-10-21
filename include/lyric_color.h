#pragma once
#include "model.h"
#include <algorithm>
#include <limits>
#include <vector>

namespace stage {
// Presentation-only colour intervals. Estimated intervals never enter the
// provider model, cache, or playback/source reliability.
struct LyricColorSpan {
    size_t first=0,last=0;
    int64_t start=0,duration=0;
    bool estimated=false;
};
inline int64_t LyricColorDuration(const std::vector<LyricLine>& lines,size_t index) {
    const auto& line=lines[index];int64_t duration=std::max<int64_t>(0,line.durationMs);
    if(index+1<lines.size()&&lines[index+1].startMs>line.startMs){
        const auto gap=lines[index+1].startMs-line.startMs;
        duration=duration>0?std::min(duration,gap):gap;
    }
    return duration;
}
inline std::vector<LyricColorSpan> LyricColorSpans(const LyricLine& line,int64_t duration) {
    std::vector<LyricColorSpan> spans;size_t cursor=0;int64_t previous=line.startMs;
    const bool usable=std::any_of(line.words.begin(),line.words.end(),[](const auto& word){return !word.text.empty()&&word.durationMs>0;});
    if(usable)for(const auto& word:line.words){
        if(word.text.empty())continue;
        const auto at=line.text.find(word.text,cursor);
        if(at==std::wstring::npos){spans.clear();break;}
        // Whitespace/punctuation omitted by a provider uses the adjacent real
        // boundary; it never delays or redistributes a real sung word.
        if(at>cursor)spans.push_back({cursor,at,previous,0,false});
        spans.push_back({at,at+word.text.size(),word.startMs,std::max<int64_t>(0,word.durationMs),false});
        cursor=at+word.text.size();previous=word.startMs;
        if(word.durationMs>0&&previous<=std::numeric_limits<int64_t>::max()-word.durationMs)previous+=word.durationMs;
    }
    if(!spans.empty()){
        if(cursor<line.text.size())spans.push_back({cursor,line.text.size(),previous,0,false});
    }else if(!line.text.empty())spans.push_back({0,line.text.size(),line.startMs,std::max<int64_t>(0,duration),true});
    return spans;
}
inline float LyricColorFraction(int64_t position,int64_t start,int64_t duration) {
    if(position<start)return 0;
    if(duration<=0)return 1;
    return static_cast<float>(std::clamp((double(position)-double(start))/double(duration),0.,1.));
}
inline size_t LyricColorSpanAt(const std::vector<LyricColorSpan>& spans,size_t text) {
    for(size_t i=0;i<spans.size();++i)if(text>=spans[i].first&&text<spans[i].last)return i;
    return SIZE_MAX;
}
inline float LyricMeasuredFill(float progress,float before,float width,float total) {
    if(progress<=0)return 0;if(progress>=1)return 1;
    return std::clamp((progress*total-before)/std::max(.001f,width),0.f,1.f);
}
}

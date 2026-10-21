#pragma once
#include "lyric_color.h"
#include "lyric_credits.h"
#include <cmath>
#include <functional>
#include <memory>
#include <windows.h>

namespace stage {
struct LyricClusterMeasure {size_t first=0,last=0;float advance=1;};
inline void NormalizeLyricClusters(const std::wstring& text,std::vector<LyricClusterMeasure>& cells){
    std::erase_if(cells,[&](const auto& c){return c.first>=text.size()||c.last<=c.first||!std::isfinite(c.advance);});
    std::sort(cells.begin(),cells.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    for(size_t i=1;i<cells.size();){WORD type=0;const auto at=cells[i].first;GetStringTypeW(CT_CTYPE3,text.data()+at,1,&type);
        if(cells[i-1].last>at||(text[at]>=0xdc00&&text[at]<=0xdfff)||(type&(C3_NONSPACING|C3_DIACRITIC|C3_VOWELMARK))||text[at]==0x200d||(at&&text[at-1]==0x200d)){
            cells[i-1].last=std::max(cells[i-1].last,cells[i].last);cells[i-1].advance+=cells[i].advance;cells.erase(cells.begin()+i);
        }else ++i;}
    for(size_t i=0;i+1<cells.size();++i)cells[i].last=cells[i+1].first;
    if(!cells.empty()){cells.front().first=0;cells.back().last=text.size();}
}
struct LyricPerformance {
    std::shared_ptr<LyricTimeline> timeline;
    std::vector<size_t> sourceLines;
    std::vector<bool> estimated;
};
// Reveal a phrase in a short stagger, independently of the much longer KTV
// sweep. This leaves unsung characters visible in neutral ink after arrival.
inline int64_t LyricEntryStart(int64_t start,int64_t duration,size_t count,size_t index,int64_t sungStart){
    const auto step=std::clamp<int64_t>(duration/std::max<int64_t>(1,static_cast<int64_t>(count)*3),16,65);
    return std::min(sungStart,start+static_cast<int64_t>(index)*step);
}
// A private render view. The provider timeline and persisted words are never
// modified. Real provider intervals take precedence over line-duration estimates.
inline LyricPerformance BuildLyricPerformance(const LyricTimeline& source,
    const std::function<std::vector<LyricClusterMeasure>(const std::wstring&)>& measure) {
    LyricPerformance out;out.timeline=std::make_shared<LyricTimeline>(source);out.timeline->lines.clear();
    for(size_t index=0;index<source.lines.size();++index){
        const auto& original=source.lines[index];const auto duration=LyricColorDuration(source.lines,index);
        if(IsLyricCredit(original)){auto copy=original;copy.credit=true;copy.words.clear();out.timeline->lines.push_back(std::move(copy));out.sourceLines.push_back(index);out.estimated.push_back(false);continue;}
        auto cells=measure(original.text);NormalizeLyricClusters(original.text,cells);
        if(cells.empty()||duration<=0){out.timeline->lines.push_back(original);out.sourceLines.push_back(index);out.estimated.push_back(false);continue;}
        const auto spans=LyricColorSpans(original,duration);std::vector<float> totals(spans.size()),offsets(spans.size());
        for(const auto& cell:cells){const auto s=LyricColorSpanAt(spans,cell.first);if(s<spans.size())totals[s]+=std::max(.001f,cell.advance);}
        std::vector<LyricWord> words;words.reserve(cells.size());
        for(const auto& cell:cells){const auto s=LyricColorSpanAt(spans,cell.first);int64_t start=original.startMs,span=0;
            if(s<spans.size()){const auto& timing=spans[s];const double total=std::max(.001f,totals[s]);
                const auto before=static_cast<int64_t>(std::llround(double(timing.duration)*offsets[s]/total));
                offsets[s]+=std::max(.001f,cell.advance);
                const auto after=static_cast<int64_t>(std::llround(double(timing.duration)*offsets[s]/total));
                start=std::clamp(timing.start+before,original.startMs,original.startMs+duration);
                span=std::clamp(after-before,int64_t{0},original.startMs+duration-start);}
            words.push_back({start,span,original.text.substr(cell.first,cell.last-cell.first)});
        }
        const bool estimated=!spans.empty()&&spans.front().estimated;
        for(size_t begin=0;begin<cells.size();){
            size_t end=std::min(begin+8,cells.size());
            if(cells.size()-begin<=10)end=cells.size();
            else for(size_t at=begin+6;at<std::min(begin+11,cells.size());++at){
                const auto last=original.text[cells[at-1].last-1];
                if(last==L' '||last==L'，'||last==L'。'||last==L'、'||last==L','||last==L';'){end=at;break;}}
            // Do not split a Latin word solely to satisfy a Chinese group size.
            const auto latin=[](wchar_t c){return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z');};
            while(end<cells.size()&&latin(original.text[cells[end-1].last-1])&&latin(original.text[cells[end].first])&&end-begin<14)++end;
            LyricLine line;line.text=original.text.substr(cells[begin].first,cells[end-1].last-cells[begin].first);
            line.startMs=begin?words[begin].startMs:original.startMs;
            const int64_t finish=end<cells.size()?words[end].startMs:original.startMs+duration;
            line.durationMs=std::max<int64_t>(1,finish-line.startMs);
            line.words.assign(words.begin()+begin,words.begin()+end);
            line.translation=original.translation;line.romanization=original.romanization;
            out.timeline->lines.push_back(std::move(line));out.sourceLines.push_back(index);out.estimated.push_back(estimated);begin=end;
        }
    }
    return out;
}
}

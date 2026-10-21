#pragma once
#include "model.h"
#include <algorithm>
#include <cwctype>
#include <string_view>
#include <unordered_set>

namespace stage {
inline bool IsLyricCreditText(std::wstring_view text){
    const auto colon=text.find_first_of(L":：");if(colon==std::wstring_view::npos||colon>40)return false;
    std::wstring label;for(wchar_t c:text.substr(0,colon))if(!iswspace(c)&&c!=0xfeff)label+=static_cast<wchar_t>(towlower(c));
    for(auto name:{L"作词",L"作詞",L"词",L"詞",L"作曲",L"曲",L"编曲",L"編曲",L"制作人",L"製作人",L"制作",L"製作",L"监制",L"監製",L"混音",L"混音师",L"母带",L"母帶",L"录音",L"錄音",L"和声",L"和聲",L"吉他",L"贝斯",L"貝斯",L"鼓",L"钢琴",L"鋼琴",L"出品",L"出品人",L"发行",L"發行",L"企划",L"企劃",L"音乐制作",L"音乐总监",L"制作统筹",L"lyrics",L"lyricist",L"composer",L"composedby",L"writtenby",L"arranger",L"arrangedby",L"producer",L"producedby",L"mixing",L"mastering"})if(label==name)return true;
    return false;
}
inline bool IsLyricCredit(const LyricLine& line){return line.credit||IsLyricCreditText(line.text);}
inline size_t LeadingCreditCount(const LyricTimeline& t){
    size_t count=0;while(count<t.lines.size()&&(IsLyricCredit(t.lines[count])||t.lines[count].text.empty()))++count;return count;
}
// Only supplement opening credits from this same provider response. Never
// merge alternate sung lines or shift their original clocks.
inline void MergeOpeningCredits(LyricTimeline& base,const LyricTimeline& source){
    const size_t start=LeadingCreditCount(base);if(start>=base.lines.size())return;
    const int64_t firstSong=base.lines[start].startMs;
    size_t units=0;for(const auto& row:base.lines){units+=row.text.size()+row.translation.size()+row.romanization.size();for(const auto& word:row.words)units+=word.text.size();}
    std::unordered_set<std::wstring> seen;for(const auto& row:base.lines)seen.insert(row.text);
    const size_t count=LeadingCreditCount(source);
    for(size_t i=0;i<count;++i){const auto& row=source.lines[i];
        if(!IsLyricCredit(row)||row.startMs>firstSong||base.lines.size()>=20000)continue;
        if(units>2*1024*1024||row.text.size()>2*1024*1024-units)break;
        if(!seen.insert(row.text).second)continue;
        auto copy=row;copy.credit=true;copy.words.clear();units+=copy.text.size();base.lines.push_back(std::move(copy));
    }
    std::stable_sort(base.lines.begin(),base.lines.end(),[](const auto& a,const auto& b){return a.startMs==b.startMs?IsLyricCredit(a)&&!IsLyricCredit(b):a.startMs<b.startMs;});
}
}

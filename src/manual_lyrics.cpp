#include "manual_lyrics.h"
#include "catalog_id.h"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <limits>

namespace stage {
namespace {
std::wstring_view TrimInput(std::wstring_view text) {
    const auto space=[](wchar_t c){return std::iswspace(c)||c==0x3000||c==0xa0;};
    while(!text.empty()&&space(text.front()))text.remove_prefix(1);
    while(!text.empty()&&space(text.back()))text.remove_suffix(1);
    return text;
}
std::wstring DecimalId(std::wstring_view text) {
    if(text.empty()||text.size()>19)return {};
    uint64_t value=0;
    for(const auto c:text){
        if(c<L'0'||c>L'9')return {};
        const auto digit=static_cast<unsigned>(c-L'0');
        if(value>(static_cast<uint64_t>(std::numeric_limits<int64_t>::max())-digit)/10)return {};
        value=value*10+digit;
    }
    return value?std::to_wstring(value):std::wstring{};
}
std::wstring LowerAscii(std::wstring_view text) {
    std::wstring result(text);for(auto& c:result)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';return result;
}
bool ValidText(std::wstring_view text) {
    for(size_t i=0;i<text.size();++i){
        const auto c=static_cast<uint16_t>(text[i]);if(c<0x20||c==0x7f)return false;
        if(c>=0xd800&&c<=0xdbff){if(++i>=text.size())return false;const auto low=static_cast<uint16_t>(text[i]);if(low<0xdc00||low>0xdfff)return false;}
        else if(c>=0xdc00&&c<=0xdfff)return false;
    }
    return true;
}
}
ManualLyricRequest ParseManualLyricRequest(std::wstring_view text) {
    ManualLyricRequest result;text=TrimInput(text);
    if(text.empty()){result.diagnostic=L"请输入歌名、歌曲 ID 或网易云歌曲链接";return result;}
    if(text.size()>512||!ValidText(text)){result.diagnostic=L"输入最多512个字符，且不能包含控制字符或不完整文字";return result;}
    if(text.starts_with(L"soda-")||text.starts_with(L"soda:")){
        result.songId=NormalizeCatalogId(L"soda-"+std::wstring(text.substr(5)));
        if(result.songId.empty()){result.diagnostic=L"汽水歌曲 ID 应为 soda- 后接正整数";return result;}
        result.kind=ManualLyricKind::SongId;return result;
    }
    if(text.starts_with(L"kg-")||text.starts_with(L"kg:")){
        result.songId=NormalizeCatalogId(L"kg-"+std::wstring(text.substr(3)));
        if(result.songId.empty()){result.diagnostic=L"酷狗歌曲标识应为 kg- 后接 32 位哈希";return result;}
        result.kind=ManualLyricKind::SongId;return result;
    }
    if(text.starts_with(L"qq-")||text.starts_with(L"qq:")){
        result.songId=NormalizeCatalogId(L"qq-"+std::wstring(text.substr(3)));
        if(result.songId.empty()){result.diagnostic=L"QQ 歌曲 ID 应为 qq- 后接正整数";return result;}
        result.kind=ManualLyricKind::SongId;return result;
    }
    const bool forceSearch=text.starts_with(L"歌名:")||text.starts_with(L"歌名：");
    if(forceSearch){text=TrimInput(text.substr(3));if(text.empty()){result.diagnostic=L"请在“歌名：”后输入搜索文字";return result;}}
    if(!forceSearch&&text.find_first_not_of(L"0123456789")==std::wstring_view::npos){
        result.songId=DecimalId(text);if(result.songId.empty()){result.diagnostic=L"歌曲 ID 应为有效的正整数";return result;}
        result.kind=ManualLyricKind::SongId;return result;
    }
    const auto lower=LowerAscii(text);
    if(lower.starts_with(L"music.163.com/")||lower.starts_with(L"y.music.163.com/"))return ParseManualLyricRequest(std::wstring(L"https://")+std::wstring(text));
    const bool looksLikeLink=lower.find(L"://")!=std::wstring::npos||lower.starts_with(L"http:")||lower.starts_with(L"https:");
    if(looksLikeLink){
        const auto scheme=lower.find(L"://");
        if(scheme==std::wstring::npos||(lower.substr(0,scheme)!=L"https"&&lower.substr(0,scheme)!=L"http")){
            result.diagnostic=L"请粘贴含 id 的网易云网页歌曲链接";return result;
        }
        const auto begin=scheme+3,slash=lower.find(L'/',begin);
        const auto host=lower.substr(begin,slash==std::wstring::npos?std::wstring::npos:slash-begin);
        if(slash==std::wstring::npos||(host!=L"music.163.com"&&host!=L"y.music.163.com")){
            result.diagnostic=L"仅解析 music.163.com 或 y.music.163.com 的歌曲链接，不打开其他地址";return result;
        }
        auto route=text.substr(slash);if(route.starts_with(L"/#/"))route.remove_prefix(2);
        const auto question=route.find(L'?');const auto path=route.substr(0,question);
        if(question==std::wstring_view::npos||(path!=L"/song"&&path!=L"/m/song")){
            result.diagnostic=L"请使用 /song?id= 或 /m/song?id= 形式的歌曲链接；不跟随短链接";return result;
        }
        auto query=route.substr(question+1);if(const auto hash=query.find(L'#');hash!=std::wstring_view::npos)query=query.substr(0,hash);
        while(!query.empty()){
            const auto amp=query.find(L'&');const auto part=query.substr(0,amp);const auto equals=part.find(L'=');
            if(equals!=std::wstring_view::npos&&part.substr(0,equals)==L"id"){
                const auto id=DecimalId(part.substr(equals+1));
                if(id.empty()||(!result.songId.empty()&&result.songId!=id)){result.songId.clear();result.diagnostic=L"歌曲链接的 id 无效或有冲突";return result;}
                result.songId=id;
            }
            if(amp==std::wstring_view::npos)break;query.remove_prefix(amp+1);
        }
        if(result.songId.empty()){result.diagnostic=L"链接中没有有效歌曲 id";return result;}
        result.kind=ManualLyricKind::SongId;return result;
    }
    if(text.size()>300){result.diagnostic=L"歌名或歌手搜索文字最多300个字符";return result;}
    result.kind=ManualLyricKind::Search;result.query=std::wstring(text);return result;
}
}

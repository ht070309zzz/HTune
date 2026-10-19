#include "kugou_lyrics.h"
#include "lyrics.h"
#include "native_http.h"
#include "third_party/json.hpp"
#define MINIZ_HAS_64BIT_REGISTERS 1
#include "third_party/miniz/miniz_tinfl.h"
#include <set>
#include <sstream>
#include <stdexcept>
#include <cwctype>
#include <functional>
#include <map>
#include <tuple>

namespace stage {
namespace {
using Json=nlohmann::json;
constexpr size_t Limit=2*1024*1024;
std::wstring Wide(std::string_view s){
    if(s.empty()||s.size()>Limit)return {};
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if(!n)return {};std::wstring out(n,0);
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),out.data(),n);
    if(out.find(L'\0')!=std::wstring::npos)return {};return out;
}
std::wstring Encode(std::wstring_view s){
    if(s.empty()||s.size()>1024)return {};
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    if(!n)return {};std::string bytes(n,0);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),bytes.data(),n,nullptr,nullptr);
    constexpr wchar_t hex[]=L"0123456789ABCDEF";std::wstring out;
    for(unsigned char c:bytes){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out+=wchar_t(c);
        else{out+=L'%';out+=hex[c>>4];out+=hex[c&15];}}
    return out;
}
const Json& At(const Json& j,const char* key){static const Json empty;const auto i=j.is_object()?j.find(key):j.end();return i==j.end()?empty:*i;}
std::string Text(const Json& j,const char* key){const auto& v=At(j,key);return v.is_string()&&v.get_ref<const std::string&>().size()<=Limit?v.get<std::string>():std::string{};}
int64_t Number(const Json& j,const char* key){const auto& v=At(j,key);if(v.is_number_unsigned())return v.get<uint64_t>()<=INT64_MAX?v.get<int64_t>():-1;return v.is_number_integer()?v.get<int64_t>():-1;}
bool Decimal(std::wstring_view s,int64_t& n,int64_t max=86400000){
    if(s.empty()||s.size()>12)return false;n=0;
    for(auto c:s){if(c<L'0'||c>L'9'||n>(max-(c-L'0'))/10)return false;n=n*10+(c-L'0');}return n<=max;
}
Json ReadJson(std::string_view bytes,std::wstring& error){
    if(bytes.empty()||bytes.size()>Limit){error=L"酷狗响应为空或超过上限";return {};}
    try{return Json::parse(bytes,[](int depth,Json::parse_event_t,Json&){if(depth>24)throw std::runtime_error("Kugou depth");return true;});}
    catch(...){error=L"酷狗响应格式无效";return {};}
}
Json Get(const wchar_t* host,const std::wstring& path,std::wstring& error,std::stop_token stop){
    NativeHttpRequest q;q.host=host;q.path=path;q.totalTimeoutMs=12000;q.maxBytes=Limit;
    const auto response=NativeHttps(q,stop);
    if(!response.ok()){error=response.cancelled()?L"歌词请求已取消":L"酷狗请求失败："+response.diagnostic;return {};}
    return ReadJson(std::string_view(reinterpret_cast<const char*>(response.bytes.data()),response.bytes.size()),error);
}
std::wstring CatalogText(const Json& value,const char* field){
    const auto raw=Text(value,field);if(raw.size()>8192)return {};
    auto text=Wide(raw);if(text.size()>2048)return {};
    // Search highlighting is transport markup, never part of a song title.
    for(const auto tag:{std::wstring_view(L"<em>"),std::wstring_view(L"</em>")}){
        size_t at=0;while((at=text.find(tag,at))!=std::wstring::npos)text.erase(at,tag.size());
    }
    return text;
}
std::vector<SongCandidate> Catalog(const Json& data,std::wstring& error){
    const auto& list=At(At(data,"data"),"lists");
    if(Number(data,"status")!=1||!list.is_array()||list.size()>100){error=L"酷狗歌曲候选结构无效";return {};}
    if(list.empty()&&Number(At(data,"data"),"total")>0){error=L"酷狗搜索结果不完整";return {};}
    std::vector<SongCandidate> result;std::map<std::wstring,int64_t> durations;
    std::set<std::tuple<std::wstring,std::wstring,std::wstring,std::wstring,int64_t>> records;size_t visited=0;
    std::function<bool(const Json&,unsigned)> visit=[&](const Json& item,unsigned depth){
        if(++visited>600||depth>4||!item.is_object()){error=L"酷狗歌曲分组结构无效或超过上限";return false;}
        SongCandidate c;c.id=NormalizeCatalogId(L"kg-"+Wide(Text(item,"FileHash")));
        c.title=CatalogText(item,"SongName");c.artist=CatalogText(item,"SingerName");c.album=CatalogText(item,"AlbumName");
        const auto suffix=CatalogText(item,"Suffix");
        if(!c.title.empty()&&!suffix.empty()&&!c.title.ends_with(suffix))c.title+=L" "+suffix;
        const auto seconds=Number(item,"Duration");
        if(!c.id.empty()&&!c.title.empty()&&c.title.size()<=512&&!c.artist.empty()&&c.artist.size()<=1024&&c.album.size()<=1024&&seconds>0&&seconds<=86400){
            c.durationMs=seconds*1000;
            if(const auto [it,added]=durations.emplace(c.id,c.durationMs);!added&&it->second!=c.durationMs){
                error=L"酷狗同一歌曲哈希的时长有冲突";return false;
            }
            // Catalog aliases and compilation names can differ for one hash.
            // Keep each complete description for strict matching; never merge
            // fields from separate rows or erase album constraints.
            if(records.emplace(c.id,c.title,c.artist,c.album,c.durationMs).second)result.push_back(std::move(c));
        }
        // Grp holds other recordings, not alternate bitrates of the parent.
        // Each child must supply its own identity/text/duration. Never inherit
        // these fields or silently truncate a group and infer uniqueness.
        const auto group=item.find("Grp");
        if(group!=item.end()&&!group->is_null()){
            if(!group->is_array()||group->size()>600){error=L"酷狗歌曲分组不完整";return false;}
            for(const auto& child:*group)if(!visit(child,depth+1))return false;
        }
        return true;
    };
    for(const auto& item:list)if(!visit(item,0))return {};
    if(result.empty())error=L"没有搜索候选";return result;
}
std::vector<uint8_t> Base64(std::string_view s){
    if(s.empty()||s.size()>Limit||s.size()%4)return {};
    auto digit=[](char c)->int{if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;return c=='+'?62:c=='/'?63:-1;};
    std::vector<uint8_t> out;out.reserve(s.size()/4*3);
    for(size_t i=0;i<s.size();i+=4){
        const int a=digit(s[i]),b=digit(s[i+1]),c=digit(s[i+2]),d=digit(s[i+3]);
        const bool p2=s[i+2]=='=',p3=s[i+3]=='=';
        if(a<0||b<0||(!p2&&c<0)||(!p3&&d<0)||(p2&&!p3)||((p2||p3)&&i+4!=s.size())||(p2&&(b&15))||(p3&&!p2&&(c&3)))return {};
        out.push_back(static_cast<uint8_t>((a<<2)|(b>>4)));
        if(!p2)out.push_back(static_cast<uint8_t>((b<<4)|(c>>2)));
        if(!p3)out.push_back(static_cast<uint8_t>((c<<6)|d));
    }
    return out;
}
std::wstring DecodeKrc(std::vector<uint8_t> b,std::stop_token stop){
    if(b.size()<5||std::string_view(reinterpret_cast<const char*>(b.data()),4)!="krc1")return {};
    constexpr uint8_t key[]={0x40,0x47,0x61,0x77,0x5e,0x32,0x74,0x47,0x51,0x36,0x31,0x2d,0xce,0xd2,0x6e,0x69};
    for(size_t i=4;i<b.size();++i)b[i]^=key[(i-4)%16];
    if(stop.stop_requested())return {};std::string out(Limit,'\0');
    const auto n=tinfl_decompress_mem_to_mem(out.data(),out.size(),b.data()+4,b.size()-4,TINFL_FLAG_PARSE_ZLIB_HEADER);
    if(n==TINFL_DECOMPRESS_MEM_TO_MEM_FAILED||stop.stop_requested())return {};out.resize(n);return Wide(out);
}
LyricTimeline Failed(const wchar_t* text){LyricTimeline out;out.state=LyricLoadState::Failed;out.source=L"酷狗音乐";out.diagnostic=text;return out;}
}

LyricTimeline ParseKrc(const std::wstring& text){
    if(text.empty()||text.size()>Limit||text.find(L'\0')!=std::wstring::npos)return Failed(L"KRC 文本无效或超过上限");
    LyricTimeline out;out.source=L"KRC";std::wistringstream input(text);std::wstring row;size_t rows=0,units=0;int64_t offset=0;
    while(std::getline(input,row)){
        if(++rows>20000||row.size()>65536)return Failed(L"KRC 行数或长度超过上限");
        if(!row.empty()&&row.back()==L'\r')row.pop_back();if(row.empty())continue;
        if(rows==1&&row.front()==0xfeff)row.erase(0,1);if(row.empty())continue;
        if(row.starts_with(L"[offset:")){
            if(row.back()!=L']')return Failed(L"KRC 偏移无效");auto v=std::wstring_view(row).substr(8,row.size()-9);const bool negative=v.starts_with(L"-");
            if(negative||v.starts_with(L"+"))v.remove_prefix(1);if(!Decimal(v,offset,600000))return Failed(L"KRC 偏移越界");if(negative)offset=-offset;continue;
        }
        if(row.size()>1&&row[0]==L'['&&((row[1]>=L'a'&&row[1]<=L'z')||(row[1]>=L'A'&&row[1]<=L'Z'))){
            if(row.back()!=L']'||row.find(L':')==std::wstring::npos)return Failed(L"KRC 元数据无效");continue;
        }
        const auto close=row.find(L']'),comma=row.find(L',');LyricLine line;
        if(row.front()!=L'['||close==std::wstring::npos||comma==std::wstring::npos||comma>close||
            !Decimal(std::wstring_view(row).substr(1,comma-1),line.startMs)||
            !Decimal(std::wstring_view(row).substr(comma+1,close-comma-1),line.durationMs,600000)||line.durationMs>86400000-line.startMs)return Failed(L"KRC 行时间无效");
        size_t pos=close+1;
        while(pos<row.size()){
            if(row[pos]!=L'<'||line.words.size()>=8192)return Failed(L"KRC 逐字标记无效");
            const auto end=row.find(L'>',pos+1);if(end==std::wstring::npos||end-pos>40)return Failed(L"KRC 逐字标记无效");
            const auto tuple=std::wstring_view(row).substr(pos+1,end-pos-1);const auto c1=tuple.find(L','),c2=c1==std::wstring_view::npos?c1:tuple.find(L',',c1+1);
            int64_t relative=0,flag=0;LyricWord word;
            if(c1==std::wstring_view::npos||c2==std::wstring_view::npos||!Decimal(tuple.substr(0,c1),relative,600000)||
                !Decimal(tuple.substr(c1+1,c2-c1-1),word.durationMs,600000)||!Decimal(tuple.substr(c2+1),flag)||
                relative>line.durationMs||word.durationMs>line.durationMs-relative)return Failed(L"KRC 逐字时间越界");
            word.startMs=line.startMs+relative;
            if(!line.words.empty()&&word.startMs<line.words.back().startMs)return Failed(L"KRC 逐字时间逆序");
            const auto next=row.find(L'<',end+1);const auto fragment=std::wstring_view(row).substr(end+1,next==std::wstring::npos?next:next-end-1);
            if(fragment.size()>2048||fragment.size()>8192-line.text.size())return Failed(L"KRC 文本超过上限");
            word.text=fragment;line.text+=word.text;line.words.push_back(std::move(word));pos=next==std::wstring::npos?row.size():next;
        }
        if(line.words.empty())continue;
        if(out.lines.size()>=20000||line.text.size()*2>Limit-units)return Failed(L"KRC 展开超过上限");
        units+=line.text.size()*2;out.lines.push_back(std::move(line));
    }
    bool readable=false,animated=false;
    for(auto& line:out.lines){
        line.startMs+=offset;if(line.startMs<0||line.startMs>86400000-line.durationMs)return Failed(L"KRC 偏移后越界");
        for(auto& word:line.words)word.startMs+=offset;
        const bool timed=std::any_of(line.words.begin(),line.words.end(),[](const auto& w){return w.durationMs>0&&std::any_of(w.text.begin(),w.text.end(),[](wchar_t c){return !iswspace(c);});});
        if(!timed)line.words.clear();else animated=true;
        readable=readable||std::any_of(line.text.begin(),line.text.end(),[](wchar_t c){return !iswspace(c);});
    }
    std::stable_sort(out.lines.begin(),out.lines.end(),[](const auto& a,const auto& b){return a.startMs<b.startMs;});
    out.wordTimed=animated;out.state=readable?LyricLoadState::Ready:LyricLoadState::NoLyrics;return out;
}

std::vector<SongCandidate> ParseKugouSearchResponse(std::string_view bytes,std::wstring& error){
    error.clear();auto value=ReadJson(bytes,error);return error.empty()?Catalog(value,error):std::vector<SongCandidate>{};
}
std::vector<SongCandidate> SearchKugouLyrics(const std::wstring& query,std::wstring& error,std::stop_token stop){
    error.clear();const auto encoded=Encode(query);
    if(query.empty()||query.size()>300||encoded.empty()||query.find(L'\0')!=std::wstring::npos){error=L"请输入有效歌名或歌手（最多 300 字符）";return {};}
    if(stop.stop_requested()){error=L"歌词查找已取消";return {};}
    // The abbreviated WebFilter request can report an empty catalog for a
    // known song. Use the complete desktop query; retain the former web route
    // only as a bounded empty-result fallback for other catalog entries.
    for(bool desktop:{true,false}){
        error.clear();const auto path=L"/song_search_v2?keyword="+encoded+(desktop?
            L"&page=1&pagesize=30&platform=PcFilter&filter=0&bitrate=0&isfuzzy=0&inputtype=2&userid=-1&clientver=20151&iscorrection=3":
            L"&page=1&pagesize=20&platform=WebFilter");
        const auto data=Get(L"songsearch.kugou.com",path,error,stop);
        if(!error.empty())return {};
        auto result=Catalog(data,error);
        if(stop.stop_requested()){error=L"歌词查找已取消";return {};}
        if(!result.empty()||error!=L"没有搜索候选")return result;
    }
    error=L"没有搜索候选";return {};
}
LyricTimeline FetchKugouLyrics(const std::wstring& id,std::stop_token stop){
    auto out=Failed(L"酷狗歌曲哈希无效");out.songId=NormalizeCatalogId(id);
    if(stop.stop_requested()){out.state=LyricLoadState::Cancelled;return out;}
    if(!IsKugouCatalogId(out.songId))return out;
    std::wstring error;
    const auto data=Get(L"krcs.kugou.com",L"/search?ver=1&man=yes&client=pc&hash="+out.songId.substr(3),error,stop);
    const auto& candidates=At(data,"candidates");
    if(!error.empty()||Number(data,"status")!=200||!candidates.is_array()||candidates.size()>100){
        out.diagnostic=error.empty()?L"酷狗歌词候选响应无效":error;if(stop.stop_requested())out.state=LyricLoadState::Cancelled;return out;
    }
    if(candidates.empty()){out.state=LyricLoadState::NoLyrics;out.diagnostic=L"酷狗暂未提供该歌曲歌词";return out;}
    const auto proposal=Text(data,"proposal");const Json* selected=nullptr;
    for(const auto& candidate:candidates)if(Text(candidate,"id")==proposal&&!proposal.empty()){
        if(selected){out.diagnostic=L"酷狗歌词候选冲突";return out;}selected=&candidate;
    }
    if(!selected&&candidates.size()==1)selected=&candidates.front();
    if(!selected){out.diagnostic=L"酷狗歌词候选不唯一";return out;}
    const auto lyricId=Wide(Text(*selected,"id")),key=Wide(Text(*selected,"accesskey"));int64_t number=0;
    if(!Decimal(lyricId,number,999999999999LL)||!number||key.empty()||key.size()>256){out.diagnostic=L"酷狗歌词下载参数无效";return out;}
    std::wstring primaryError;
    for(bool krc:{true,false}){
        error.clear();const auto response=Get(L"lyrics.kugou.com",L"/download?ver=1&client=pc&id="+lyricId+L"&accesskey="+Encode(key)+L"&fmt="+(krc?L"krc":L"lrc")+L"&charset=utf8",error,stop);
        if(stop.stop_requested()){out.state=LyricLoadState::Cancelled;return out;}
        if(error.empty()&&Number(response,"status")==200){
            auto bytes=Base64(Text(response,"content"));const auto decoded=krc?DecodeKrc(std::move(bytes),stop):Wide(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
            auto parsed=krc?ParseKrc(decoded):ParseLrc(decoded);
            if(parsed.state==LyricLoadState::Ready&&!parsed.lines.empty()){
                parsed.songId=out.songId;parsed.source=krc?(parsed.wordTimed?L"酷狗 KRC · 原生逐字歌词":L"酷狗 KRC · 逐句降级"):L"酷狗 LRC · 原生逐句歌词";
                parsed.retryWordTiming=!krc&&!primaryError.empty();parsed.diagnostic=parsed.wordTimed?L"酷狗逐字歌词已就绪":L"酷狗逐句歌词已就绪";
                if(stop.stop_requested()){parsed.lines.clear();parsed.wordTimed=false;parsed.state=LyricLoadState::Cancelled;}return parsed;
            }
            error=L"酷狗歌词内容或时间格式无效";
        }else if(error.empty())error=L"酷狗歌词下载失败";
        if(krc)primaryError=error;else out.diagnostic=error;
    }
    return out;
}
}

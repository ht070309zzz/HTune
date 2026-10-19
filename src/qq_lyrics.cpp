#include "qq_lyrics.h"
#include "catalog_id.h"
#include "lyrics.h"
#include "native_http.h"
#include "third_party/json.hpp"
#include "third_party/qqmusicdecoder/qrc_des.h"
#define MINIZ_HAS_64BIT_REGISTERS 1
#include "third_party/miniz/miniz_tinfl.h"
#include <algorithm>
#include <atomic>
#include <set>
#include <sstream>
#include <stdexcept>

namespace stage {
namespace {
using Json=nlohmann::json;
constexpr size_t Limit=2*1024*1024;
std::string Utf8(const std::wstring& s) {
    if(s.empty()||s.size()>Limit)return {};
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    if(!n)return {};
    std::string out(n,0);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),out.data(),n,nullptr,nullptr);return out;
}
std::wstring Wide(std::string_view s) {
    if(s.empty()||s.size()>Limit)return {};
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if(!n)return {};
    std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),out.data(),n);
    if(out.find(L'\0')!=std::wstring::npos)return {};
    return out;
}
const Json& At(const Json& value,const char* name) {
    static const Json empty;
    if(!value.is_object())return empty;
    const auto i=value.find(name);return i==value.end()?empty:*i;
}
int64_t Number(const Json& value,const char* name,int64_t fallback=-1) {
    const auto& field=At(value,name);
    if(field.is_number_unsigned())return field.get<uint64_t>()<=INT64_MAX?field.get<int64_t>():fallback;
    return field.is_number_integer()?field.get<int64_t>():fallback;
}
std::string Text(const Json& value,const char* name) {
    const auto& field=At(value,name);
    if(!field.is_string()||field.get_ref<const std::string&>().size()>Limit)return {};
    return field.get<std::string>();
}
Json Request(const char* module,const char* method,Json params,bool lyric,std::wstring& error,std::stop_token stop,int64_t* serviceCode=nullptr) {
    if(serviceCode)*serviceCode=-1;
    Json body{{"req",{{"module",module},{"method",method},{"param",std::move(params)}}}};
    // The search service uses its desktop defaults. The web lyric service
    // requires its own comm version; sharing it made search return filtered empties.
    if(lyric)body["comm"]={{"ct",24},{"cv",4747474}};
    NativeHttpRequest request;request.host=L"u.y.qq.com";request.path=L"/cgi-bin/musicu.fcg";
    request.headers=L"Content-Type: application/json; charset=utf-8\r\nReferer: https://y.qq.com/\r\n";
    request.body=body.dump();request.maxBytes=Limit;request.totalTimeoutMs=12000;
    auto response=NativeHttps(request,stop);
    if(!response.ok()){error=response.cancelled()?L"歌词请求已取消":L"QQ 音乐请求失败："+response.diagnostic;return {};}
    try{
        auto value=Json::parse(response.bytes,[](int depth,Json::parse_event_t,Json&){
            if(depth>24)throw std::runtime_error("QQ response depth");return true;
        });
        if(Number(value,"code")==0&&serviceCode)*serviceCode=Number(At(value,"req"),"code");
        if(Number(value,"code")!=0||Number(At(value,"req"),"code")!=0){
            error=L"QQ 音乐服务返回错误（"+std::to_wstring(Number(value,"code"))+L" / "+
                std::to_wstring(Number(At(value,"req"),"code"))+L"）";return {};
        }
        auto data=At(At(value,"req"),"data");
        if(!data.is_object()){error=L"QQ 音乐响应缺少数据";return {};}
        return data;
    }catch(...){error=L"QQ 音乐响应格式无效";return {};}
}
std::wstring Decode(std::string_view hex,std::stop_token stop) {
    if(hex.empty()||hex.size()>Limit||hex.size()%16)return {};
    auto digit=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    std::vector<uint8_t> bytes(hex.size()/2);
    for(size_t i=0;i<bytes.size();++i){const auto a=digit(hex[i*2]),b=digit(hex[i*2+1]);if(a<0||b<0)return {};bytes[i]=static_cast<uint8_t>((a<<4)|b);}
    static constexpr uint8_t key[]="!@#)(*$%123ZXC!@!@#)(NHL";
    uint8_t schedule[3][16][6]{};qqdes::TripleDESKeySetup(key,schedule,qqdes::DECRYPT);
    for(size_t i=0;i<bytes.size();i+=8){
        if((i%4096)==0&&stop.stop_requested())return {};
        qqdes::TripleDESCrypt(bytes.data()+i,bytes.data()+i,schedule);
    }
    std::string expanded(Limit,'\0');
    const auto size=tinfl_decompress_mem_to_mem(expanded.data(),expanded.size(),bytes.data(),bytes.size(),TINFL_FLAG_PARSE_ZLIB_HEADER);
    if(size==TINFL_DECOMPRESS_MEM_TO_MEM_FAILED||stop.stop_requested())return {};
    expanded.resize(size);return Wide(expanded);
}
bool Decimal(std::wstring_view text,int64_t& value,int64_t maximum=86400000) {
    if(text.empty()||text.size()>12)return false;value=0;
    for(auto c:text){if(c<L'0'||c>L'9'||value>(maximum-(c-L'0'))/10)return false;value=value*10+(c-L'0');}
    return value<=maximum;
}
// A bounded attribute decoder, not an XML engine: raw newlines in QQ's
// LyricContent must survive (XML attribute normalization would erase them).
std::wstring QrcContent(std::wstring_view xml) {
    if(xml.size()>Limit||xml.find(L"<!")!=std::wstring_view::npos)return {};
    const auto tag=xml.find(L"<Lyric_1 ");
    if(tag==std::wstring_view::npos)return {};
    const auto end=xml.find(L'>',tag);
    if(end==std::wstring_view::npos)return {};
    const auto attr=xml.find(L"LyricContent=",tag);
    if(attr==std::wstring_view::npos||attr>=end||attr+13>=xml.size())return {};
    const auto quote=xml[attr+13]; // LyricContent is 12 chars, followed by '='.
    if(quote!=L'"'&&quote!=L'\'')return {};
    const auto close=xml.find(quote,attr+14);
    if(close==std::wstring_view::npos||close>end)return {};
    auto value=xml.substr(attr+14,close-attr-14);std::wstring out;out.reserve(value.size());
    for(size_t i=0;i<value.size();++i){
        if(value[i]!=L'&'){if(value[i]==0||value[i]==L'<')return {};out+=value[i];continue;}
        const auto semi=value.find(L';',i+1);if(semi==std::wstring_view::npos||semi-i>12)return {};
        auto entity=value.substr(i+1,semi-i-1);uint32_t c=0;
        if(entity==L"amp")c=L'&';else if(entity==L"lt")c=L'<';else if(entity==L"gt")c=L'>';
        else if(entity==L"quot")c=L'"';else if(entity==L"apos")c=L'\'';
        else if(entity.starts_with(L"#")){
            entity.remove_prefix(1);const bool hex=entity.starts_with(L"x");if(hex)entity.remove_prefix(1);if(entity.empty())return {};
            for(auto d:entity){unsigned n;if(d>=L'0'&&d<=L'9')n=d-L'0';else if(hex&&d>=L'a'&&d<=L'f')n=d-L'a'+10;else if(hex&&d>=L'A'&&d<=L'F')n=d-L'A'+10;else return {};
                const auto base=hex?16u:10u;if(c>(0x10ffff-n)/base)return {};c=c*base+n;}
        }else return {};
        if(!c||c>0x10ffff||(c>=0xd800&&c<=0xdfff)||(c<32&&c!=9&&c!=10&&c!=13))return {};
        if(c>0xffff){c-=0x10000;out+=static_cast<wchar_t>(0xd800+(c>>10));out+=static_cast<wchar_t>(0xdc00+(c&1023));}
        else out+=static_cast<wchar_t>(c);
        i=semi;
    }
    return out;
}
LyricTimeline Failed(const wchar_t* message) {LyricTimeline result;result.state=LyricLoadState::Failed;result.source=L"QQ 音乐";result.diagnostic=message;return result;}
}

LyricTimeline ParseQrc(const std::wstring& text) {
    if(text.empty()||text.size()>Limit)return Failed(L"QRC 文本无效或超过上限");
    auto content=text.find(L"<QrcInfos")!=std::wstring::npos?QrcContent(text):text;
    if(content.empty())return Failed(L"QRC 歌词内容无法解析");
    std::wistringstream input(content);std::wstring row,converted;size_t rows=0;int64_t offset=0;
    while(std::getline(input,row)){
        if(++rows>20000||row.size()>65536)return Failed(L"QRC 行数或行长度超过上限");
        if(!row.empty()&&row.back()==L'\r')row.pop_back();
        if(row.empty())continue;
        if(row.starts_with(L"[offset:")){
            if(row.back()!=L']')return Failed(L"QRC 偏移标记无效");
            auto value=std::wstring_view(row).substr(8,row.size()-9);bool negative=value.starts_with(L"-");
            if(negative||value.starts_with(L"+"))value.remove_prefix(1);
            if(!Decimal(value,offset,600000))return Failed(L"QRC 偏移标记无效");
            if(negative)offset=-offset;continue;
        }
        if(row.starts_with(L"[ti:")||row.starts_with(L"[ar:")||row.starts_with(L"[al:")||row.starts_with(L"[by:"))continue;
        const auto close=row.find(L']'),comma=row.find(L',');int64_t start=0,duration=0;
        if(row.front()!=L'['||close==std::wstring::npos||comma==std::wstring::npos||comma>close||
            !Decimal(std::wstring_view(row).substr(1,comma-1),start)||!Decimal(std::wstring_view(row).substr(comma+1,close-comma-1),duration,600000)||duration>86400000-start)
            return Failed(L"QRC 行时间标记无效");
        std::wstring convertedRow=row.substr(0,close+1);size_t position=close+1;bool timed=false;
        while(position<row.size()){
            auto open=row.find(L'(',position);size_t end=std::wstring::npos;int64_t wordStart=0,wordDuration=0;
            while(open!=std::wstring::npos){
                // A timing tuple has at most 25 characters. Restrict the
                // scan so many literal '(' cannot make parsing quadratic.
                const auto probe=std::wstring_view(row).substr(open+1,26);
                const auto relativeEnd=probe.find(L')');
                if(relativeEnd==std::wstring_view::npos){open=row.find(L'(',open+1);continue;}
                end=open+1+relativeEnd;
                const auto tuple=std::wstring_view(row).substr(open+1,end-open-1);const auto sep=tuple.find(L',');
                if(sep!=std::wstring_view::npos&&Decimal(tuple.substr(0,sep),wordStart)&&Decimal(tuple.substr(sep+1),wordDuration,600000))break;
                open=row.find(L'(',open+1);
            }
            if(open==std::wstring::npos||end==std::wstring::npos||open==position||wordStart<start||wordDuration>86400000-wordStart)
                return Failed(L"QRC 逐字时间标记无效");
            convertedRow+=L"("+std::to_wstring(wordStart)+L","+std::to_wstring(wordDuration)+L",0)"+row.substr(position,open-position);
            position=end+1;timed=true;
        }
        if(!timed)continue;
        if(convertedRow.size()+1>Limit-converted.size())return Failed(L"QRC 展开后超过上限");
        converted+=convertedRow+L"\n";
    }
    auto result=ParseYrc(converted);result.source=L"QRC";
    if(offset)for(auto& line:result.lines){
        const auto shifted=line.startMs+offset;
        if(shifted<0||shifted>86400000)return Failed(L"QRC 偏移后的行时间越界");
        line.startMs=shifted;
        for(auto& word:line.words){
            word.startMs+=offset;
            if(word.startMs<0||word.startMs>86400000-word.durationMs)return Failed(L"QRC 偏移后的逐字时间越界");
        }
    }
    return result;
}

std::vector<SongCandidate> SearchQqLyrics(const std::wstring& query,std::wstring& error,std::stop_token stop) {
    error.clear();std::vector<SongCandidate> result;
    if(stop.stop_requested()){error=L"歌词查找已取消";return {};}
    static std::atomic<int64_t> filteredUntil{0};
    if(MonotonicMs()<filteredUntil.load()){error=L"QQ 音乐搜索暂时受限，稍后自动重试";return {};}
    const auto bytes=Utf8(query);
    if(query.empty()||query.size()>300||bytes.empty()||query.find(L'\0')!=std::wstring::npos){error=L"请输入有效的歌名或歌手（最多 300 字符）";return {};}
    const auto data=Request("music.search.SearchCgiService","DoSearchForQQMusicDesktop",
        {{"query",bytes},{"num_per_page",20},{"page_num",1},{"search_type",0}},false,error,stop);
    if(!error.empty())return {};
    const auto& meta=At(data,"meta");
    if(Number(meta,"ret")!=0||Number(meta,"is_filter")!=0){
        filteredUntil.store(MonotonicMs()+60000);error=L"QQ 音乐搜索暂时被过滤，请稍后重试";return {};
    }
    const auto& songs=At(At(At(data,"body"),"song"),"list");
    if(!songs.is_array()||songs.size()>100){error=L"QQ 音乐候选结构无效";return {};}
    std::set<std::wstring> ids;
    for(const auto& song:songs){
        const auto id=Number(song,"id"),seconds=Number(song,"interval");
        if(id<=0||seconds<=0||seconds>86400)continue;
        SongCandidate candidate;candidate.id=L"qq-"+std::to_wstring(id);
        candidate.title=Wide(Text(song,"title"));candidate.album=Wide(Text(At(song,"album"),"name"));candidate.durationMs=seconds*1000;
        const auto& singers=At(song,"singer");if(!singers.is_array()||singers.size()>32)continue;
        for(const auto& singer:singers){auto name=Wide(Text(singer,"name"));if(name.empty())continue;if(!candidate.artist.empty())candidate.artist+=L" / ";candidate.artist+=name;}
        if(candidate.title.empty()||candidate.title.size()>512||candidate.artist.empty()||candidate.artist.size()>1024||candidate.album.size()>512)continue;
        if(ids.insert(candidate.id).second)result.push_back(std::move(candidate));
    }
    if(stop.stop_requested()){error=L"歌词查找已取消";return {};}
    if(result.empty())error=L"没有搜索候选";
    return result;
}

LyricTimeline FetchQqLyrics(const std::wstring& catalogId,std::stop_token stop) {
    auto output=Failed(L"QQ 歌曲 ID 无效");const auto key=NormalizeCatalogId(catalogId);
    if(!IsQqCatalogId(key))return output;
    output.songId=key;const auto id=std::stoll(key.substr(3));std::wstring primaryError;bool primaryAbsent=false;
    for(int qrc=1;qrc>=0;--qrc){
        if(stop.stop_requested()){output.lines.clear();output.state=LyricLoadState::Cancelled;return output;}
        std::wstring error;int64_t code=-1;
        auto data=Request("music.musichallSong.PlayLyricInfo","GetPlayLyricInfo",
            {{"songId",id},{"crypt",1},{"qrc",qrc},{"qrc_t",0},{"lrc_t",0},{"trans",1},{"trans_t",0},{"roma",1},{"roma_t",0},{"type",1}},true,error,stop,&code);
        if(code==24001){
            if(qrc){primaryAbsent=true;continue;}
            if(primaryAbsent){output.state=LyricLoadState::NoLyrics;output.diagnostic=L"QQ 音乐未提供该歌曲歌词，可导入本地歌词";return output;}
        }
        if(error.empty()&&Number(data,"songID")!=id)error=L"QQ 歌词响应的歌曲 ID 不一致";
        if(error.empty()&&!At(data,"lyric").is_string())error=L"QQ 歌词原文结构无效";
        if(error.empty()){
            const auto encoded=Text(data,"lyric");const auto decoded=Decode(encoded,stop);
            if(!encoded.empty()&&decoded.empty())error=L"QQ 歌词解码失败";
            else{
                auto lyric=decoded.find(L"<QrcInfos")!=std::wstring::npos?ParseQrc(decoded):ParseLrc(decoded);
                lyric.songId=key;lyric.source=lyric.wordTimed?L"QQ 音乐 QRC":L"QQ 音乐 LRC";
                if(lyric.state==LyricLoadState::Ready&&!lyric.lines.empty()){
                    for(const auto field:{"trans","roma"}){
                        auto supplementary=Decode(Text(data,field),stop);
                        if(!supplementary.empty())AttachLyricSupplement(lyric,supplementary.find(L"<QrcInfos")!=std::wstring::npos?ParseQrc(supplementary):ParseLrc(supplementary),std::string_view(field)=="roma");
                    }
                    if(stop.stop_requested()){lyric.lines.clear();lyric.wordTimed=false;lyric.state=LyricLoadState::Cancelled;return lyric;}
                    lyric.retryWordTiming=!lyric.wordTimed&&!primaryError.empty();
                    lyric.diagnostic=lyric.wordTimed?L"QQ 原生逐字歌词已就绪":L"QQ 原生逐句歌词已就绪";
                    if(!primaryError.empty())lyric.diagnostic+=L"；逐字请求："+primaryError;
                    return lyric;
                }
                if(lyric.state==LyricLoadState::Failed||(lyric.lines.empty()&&!decoded.empty()))error=L"QQ 歌词时间格式无效";
                else if(!qrc&&primaryError.empty()){
                    lyric.state=LyricLoadState::NoLyrics;lyric.diagnostic=L"QQ 音乐未提供该歌曲歌词，可导入本地歌词";return lyric;
                }
            }
        }
        if(qrc)primaryError=error;
        else output.diagnostic=error.empty()?L"QQ 逐字请求异常，逐句暂未取得":error;
    }
    if(stop.stop_requested())output.state=LyricLoadState::Cancelled;
    return output;
}
}

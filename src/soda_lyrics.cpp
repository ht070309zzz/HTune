#include "soda_lyrics.h"
#include "lyrics.h"
#include "kugou_lyrics.h"
#include "native_http.h"
#include "third_party/json.hpp"
#define MINIZ_HAS_64BIT_REGISTERS 1
#include "third_party/miniz/miniz_tinfl.h"
#include <shlobj.h>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>

namespace stage {
namespace {
using Json=nlohmann::json;
constexpr size_t CompressedLimit=4*1024*1024,ExpandedLimit=8*1024*1024,ResponseLimit=2*1024*1024;
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
const Json& At(const Json& j,const char* key){static const Json empty;const auto it=j.is_object()?j.find(key):j.end();return it==j.end()?empty:*it;}
std::string Text(const Json& j,const char* key,size_t limit=8192){const auto& v=At(j,key);return v.is_string()&&v.get_ref<const std::string&>().size()<=limit?v.get<std::string>():std::string{};}
std::wstring Wide(std::string_view s){
    if(s.empty()||s.size()>ResponseLimit)return {};
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if(!n)return {};std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),out.data(),n);
    return out.find(L'\0')==std::wstring::npos?out:std::wstring{};
}
Json Parse(std::string_view s,size_t limit){
    if(s.empty()||s.size()>limit)throw std::runtime_error("Soda size");
    return Json::parse(s,[](int depth,Json::parse_event_t,Json&){if(depth>32)throw std::runtime_error("Soda depth");return true;});
}
uint32_t U32(const uint8_t* b){return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);}
uint32_t Crc(std::span<const uint8_t> bytes){
    static const auto table=[] {std::array<uint32_t,256> t{};for(uint32_t i=0;i<256;++i){auto n=i;for(int j=0;j<8;++j)n=(n>>1)^((n&1)?0xedb88320u:0u);t[i]=n;}return t;}();
    uint32_t crc=0xffffffffu;for(auto b:bytes)crc=table[(crc^b)&255]^(crc>>8);return ~crc;
}
std::vector<uint8_t> InflateQueue(std::span<const uint8_t> bytes){
    if(bytes.size()<24||bytes.size()>CompressedLimit||memcmp(bytes.data(),"LUNA",4)!=0)throw std::runtime_error("Soda header");
    bytes=bytes.subspan(4);
    if(bytes[0]!=0x1f||bytes[1]!=0x8b||bytes[2]!=8||(bytes[3]&0xe0))throw std::runtime_error("gzip header");
    const auto flags=bytes[3];size_t begin=10;const auto end=bytes.size()-8;
    if(flags&4){if(begin+2>end)throw std::runtime_error("gzip extra");const size_t size=bytes[begin]|(bytes[begin+1]<<8);begin+=2;if(size>end-begin)throw std::runtime_error("gzip extra");begin+=size;}
    for(const int bit:{8,16})if(flags&bit){while(begin<end&&bytes[begin])++begin;if(begin>=end)throw std::runtime_error("gzip name");++begin;}
    if(flags&2){if(begin+2>end||(Crc(bytes.first(begin))&0xffff)!=(uint32_t(bytes[begin])|(uint32_t(bytes[begin+1])<<8)))throw std::runtime_error("gzip checksum");begin+=2;}
    if(begin>=end)throw std::runtime_error("gzip empty");
    const auto length=U32(bytes.data()+end+4);if(!length||length>ExpandedLimit)throw std::runtime_error("gzip size");
    std::vector<uint8_t> out(length);tinfl_decompressor decoder{};tinfl_init(&decoder);
    size_t inSize=end-begin,outSize=out.size();
    const auto status=tinfl_decompress(&decoder,bytes.data()+begin,&inSize,out.data(),out.data(),&outSize,TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if(status!=TINFL_STATUS_DONE||inSize!=end-begin||outSize!=length||Crc(out)!=U32(bytes.data()+end))throw std::runtime_error("gzip content");
    return out;
}
SongCandidate Track(const Json& t){
    SongCandidate c;const auto raw=Wide(Text(t,"id",19));
    // Decimal IDs must remain strings, with no double conversion or rounding.
    if(raw.empty()||raw.find_first_not_of(L"0123456789")!=std::wstring::npos)return c;
    c.id=NormalizeCatalogId(L"soda-"+raw);c.title=Wide(Text(t,"name",2048));c.album=Wide(Text(At(t,"album"),"name",4096));
    const auto& duration=At(t,"duration");
    if(duration.is_number_integer()&&duration>=0&&duration<=86400000)c.durationMs=duration.get<int64_t>();
    const auto& artists=At(t,"artists");
    if(artists.is_array()&&artists.size()<=32)for(const auto& a:artists){auto name=Wide(Text(a,"name",2048));if(name.empty())name=Wide(Text(At(a,"user_info"),"nickname",2048));if(name.empty())continue;if(!c.artist.empty())c.artist+=L" / ";c.artist+=name;}
    if(c.id.empty()||c.title.empty()||c.title.size()>512||c.artist.empty()||c.artist.size()>512||c.album.size()>1024||c.durationMs<=0)return {};
    return c;
}
LyricTimeline Failure(const std::wstring& id,const std::wstring& message){LyricTimeline out;out.songId=NormalizeCatalogId(id);out.state=LyricLoadState::Failed;out.diagnostic=message;return out;}
}

std::vector<SongCandidate> ParseSodaQueue(std::span<const uint8_t> bytes,std::wstring& error){
    error.clear();std::vector<SongCandidate> out;
    try{
        const auto expanded=InflateQueue(bytes);const auto doc=Parse({reinterpret_cast<const char*>(expanded.data()),expanded.size()},ExpandedLimit);
        if(!doc.is_object()||doc.size()>128)throw std::runtime_error("queue structure");
        size_t rows=0;
        for(const auto& queue:doc){
            const auto& tracks=At(queue,"playables");if(!tracks.is_array())continue;
            for(const auto& t:tracks){
                if(++rows>4096)throw std::runtime_error("queue count");
                if(Text(t,"media_type",32)!="track")continue;
                auto c=Track(t);if(c.id.empty())throw std::runtime_error("incomplete track");
                const auto same=[&](const auto& x){return x.id==c.id&&x.title==c.title&&x.artist==c.artist&&x.album==c.album&&x.durationMs==c.durationMs;};
                if(std::none_of(out.begin(),out.end(),same))out.push_back(std::move(c));
            }
        }
        if(out.empty())error=L"没有搜索候选";
    }catch(...){out.clear();error=L"汽水本地歌曲目录暂不可用，等待更新";}
    return out;
}
std::vector<SongCandidate> ReadSodaCandidates(std::wstring& error,std::stop_token stop){
    error.clear();if(stop.stop_requested()){error=L"歌词查找已取消";return {};}
    try{
        // Use the actual Windows profile even when a development tool is MSIX
        // packaged. Never read another application's LocalCache/profile copy.
        PWSTR profile=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_Profile,0,nullptr,&profile))||!profile)throw std::runtime_error("profile");
        const std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> ownedProfile(profile,CoTaskMemFree);
        std::filesystem::path path(profile);
        path/=L"AppData/Roaming/SodaMusic/LunaStorage/QueueCache";
        Handle file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
        BY_HANDLE_FILE_INFORMATION before{},after{};
        if(file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.value,&before)||before.nFileSizeHigh||!before.nFileSizeLow||before.nFileSizeLow>CompressedLimit)throw std::runtime_error("queue open");
        std::vector<uint8_t> bytes(before.nFileSizeLow);DWORD read=0;
        if(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=bytes.size()||!GetFileInformationByHandle(file.value,&after)||
            before.nFileSizeLow!=after.nFileSizeLow||before.nFileSizeHigh!=after.nFileSizeHigh||CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime)!=0)throw std::runtime_error("queue changed");
        if(stop.stop_requested()){error=L"歌词查找已取消";return {};}
        return ParseSodaQueue(bytes,error);
    }catch(...){error=L"汽水本地歌曲目录暂不可用，等待更新";return {};}
}
std::vector<SongCandidate> SearchSodaLyrics(const std::wstring& query,std::wstring& error,std::stop_token stop){
    auto out=ReadSodaCandidates(error,stop);if(!error.empty())return {};
    std::wistringstream parts(query);std::wstring term;std::vector<std::wstring> terms;while(parts>>term)terms.push_back(term);
    std::erase_if(out,[&](const auto& c){const auto text=c.title+L" "+c.artist+L" "+c.album;return std::any_of(terms.begin(),terms.end(),[&](const auto& t){return text.find(t)==std::wstring::npos;});});
    if(out.empty())error=L"没有搜索候选";return out;
}
LyricTimeline ParseSodaLyricsResponse(std::string_view bytes,const std::wstring& id){
    if(!IsSodaCatalogId(id))return Failure(id,L"汽水歌曲 ID 无效");
    try{
        const auto j=Parse(bytes,ResponseLimit);const auto track=Track(At(At(j,"seo_track"),"track"));
        if(!SameCatalogId(track.id,id))return Failure(id,L"汽水返回歌曲身份不一致");
        const auto& lyric=At(j,"lyric");const auto type=Text(lyric,"type",32);const auto content=Wide(Text(lyric,"content",ResponseLimit));
        if(content.empty()){auto out=Failure(id,L"汽水当前歌曲没有可用歌词");out.state=LyricLoadState::NoLyrics;return out;}
        LyricTimeline out;
        if(type=="krc"||type=="KRC")out=ParseKrc(content);
        else if(type=="lrc"||type=="LRC")out=ParseLrc(content);
        else return Failure(id,L"汽水歌词格式尚不支持");
        out.songId=NormalizeCatalogId(id);out.source=out.wordTimed?L"汽水音乐 · 原生逐字歌词":L"汽水音乐 · 原生逐句歌词";
        if(out.state==LyricLoadState::Ready)out.diagnostic=L"汽水歌词已就绪";
        return out;
    }catch(...){return Failure(id,L"汽水歌词响应格式无效");}
}
LyricTimeline FetchSodaLyrics(const std::wstring& id,std::stop_token stop){
    if(!IsSodaCatalogId(id))return Failure(id,L"汽水歌曲 ID 无效");
    NativeHttpRequest request;request.host=L"api.qishui.com";request.path=L"/luna/h5/seo_track?track_id="+std::wstring(CatalogDigits(id))+L"&device_platform=web";
    request.headers=L"User-Agent: Mozilla/5.0\r\n";request.maxBytes=ResponseLimit;request.totalTimeoutMs=12000;
    const auto response=NativeHttps(request,stop);
    if(stop.stop_requested()||response.cancelled()){auto out=Failure(id,L"歌词请求已取消");out.state=LyricLoadState::Cancelled;return out;}
    if(!response.ok())return Failure(id,L"汽水歌词请求失败："+response.diagnostic);
    auto out=ParseSodaLyricsResponse({reinterpret_cast<const char*>(response.bytes.data()),response.bytes.size()},id);
    if(stop.stop_requested()){out.lines.clear();out.wordTimed=false;out.state=LyricLoadState::Cancelled;}
    return out;
}
}

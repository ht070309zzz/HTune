#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "song_metadata_cache.h"
#include "file_transaction.h"
#include "third_party/json.hpp"
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <array>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace stage {
namespace {
using MetadataJson=nlohmann::json;
constexpr size_t MetadataMaxBytes=32768;
constexpr uint64_t MetadataCacheQuotaBytes=32ull*1024*1024;
constexpr size_t MetadataCacheRecordLimit=2048;
constexpr size_t MetadataMaxScanEntries=4096;
constexpr size_t MetadataMaxEvictions=64;
constexpr size_t MetadataSlotCount=9;
constexpr char MetadataOwner[]="MusicStageNativePrototype.SongMetadata";
std::mutex MetadataSaveMutex;
struct MetadataCacheHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit MetadataCacheHandle(HANDLE handle=INVALID_HANDLE_VALUE):value(handle){}
    ~MetadataCacheHandle(){Close();}
    MetadataCacheHandle(const MetadataCacheHandle&)=delete;
    MetadataCacheHandle& operator=(const MetadataCacheHandle&)=delete;
    MetadataCacheHandle(MetadataCacheHandle&& other) noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
    MetadataCacheHandle& operator=(MetadataCacheHandle&& other) noexcept {
        if(this!=&other){Close();value=std::exchange(other.value,INVALID_HANDLE_VALUE);}return *this;
    }
    void Close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
};
struct MetadataFindHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit MetadataFindHandle(HANDLE handle):value(handle){}
    ~MetadataFindHandle(){if(value!=INVALID_HANDLE_VALUE)FindClose(value);}
    MetadataFindHandle(const MetadataFindHandle&)=delete;
    MetadataFindHandle& operator=(const MetadataFindHandle&)=delete;
};
std::wstring MetadataCanonicalId(std::wstring_view id) {
    if(id.empty()||id.size()>19)return {};
    uint64_t number=0;
    for(const auto c:id){
        if(c<L'0'||c>L'9')return {};
        const auto digit=static_cast<unsigned>(c-L'0');
        if(number>(static_cast<uint64_t>(INT64_MAX)-digit)/10)return {};
        number=number*10+digit;
    }
    return number?std::to_wstring(number):std::wstring{};
}
std::string MetadataUtf8(const std::wstring& text) {
    if(text.empty())return {};
    const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(count<=0)throw std::runtime_error("Metadata UTF-16");
    std::string result(static_cast<size_t>(count),'\0');
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count,nullptr,nullptr)!=count)
        throw std::runtime_error("Metadata UTF-8 encode");
    return result;
}
std::wstring MetadataWide(const MetadataJson& data,const char* field) {
    const auto& item=data.at(field);
    if(!item.is_string())throw std::runtime_error("Metadata text type");
    const auto& text=item.get_ref<const std::string&>();
    if(text.size()>MetadataMaxBytes)throw std::runtime_error("Metadata text size");
    if(text.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(count<=0)throw std::runtime_error("Metadata UTF-8");
    std::wstring result(static_cast<size_t>(count),L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count)!=count)
        throw std::runtime_error("Metadata UTF-8 decode");
    return result;
}
bool MetadataTextValid(const std::wstring& text,size_t maximum,bool required) {
    if(text.size()>maximum||(required&&text.find_first_not_of(L" \t\r\n")==std::wstring::npos))return false;
    return std::none_of(text.begin(),text.end(),[](wchar_t c){return c<32||c==127;});
}
bool MetadataValid(const SongMetadata& value) {
    return !value.id.empty()&&MetadataCanonicalId(value.id)==value.id&&
        MetadataTextValid(value.title,1024,true)&&MetadataTextValid(value.artist,2048,true)&&
        MetadataTextValid(value.album,2048,false)&&SongMetadataDurationMatches(value.durationMs,value.durationMs);
}
bool MetadataSame(const SongMetadata& a,const SongMetadata& b) {
    return a.id==b.id&&a.title==b.title&&a.artist==b.artist&&a.album==b.album&&a.durationMs==b.durationMs;
}
std::optional<std::filesystem::path> MetadataDirectory(bool create,
    detail::DirectoryTreeLock& directoryLock,std::wstring& diagnostic) {
    PWSTR local=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))||!local){diagnostic=L"无法取得本机歌曲资料缓存目录";return {};}
    struct Free {PWSTR value;~Free(){CoTaskMemFree(value);}} free{local};
    const auto directory=std::filesystem::path(local)/L"MusicStageNativePrototype"/L"cache"/L"song-metadata";
    DWORD directoryError=ERROR_SUCCESS;
    if(!directoryLock.Acquire(directory,directoryError,create)){
        diagnostic=L"本机歌曲资料缓存目录缺失、不可用或为重解析路径（Windows "+
            std::to_wstring(directoryError)+L"）";return {};
    }
    return directory;
}
std::wstring MetadataSlotName(const std::wstring& id,unsigned slot) {
    return id+(slot?L".recovered-"+std::to_wstring(slot-1):std::wstring{})+L".json";
}
std::filesystem::path MetadataSlot(const std::filesystem::path& directory,const std::wstring& id,unsigned slot) {
    return directory/MetadataSlotName(id,slot);
}
enum class MetadataRead { Invalid,Ready };
struct MetadataFileFacts {
    uint64_t bytes=0;
    uint64_t modifiedAt=0;
    DWORD attributes=0;
};
MetadataRead MetadataReadHandle(HANDLE file,SongMetadata& value,MetadataFileFacts* facts=nullptr) {
    BY_HANDLE_FILE_INFORMATION before{},after{};LARGE_INTEGER size{},afterSize{},zero{};
    // Atomic cache creation produces one ordinary directory entry. Refuse
    // hard-linked aliases so eviction cannot affect a separately named object.
    if(file==INVALID_HANDLE_VALUE||GetFileType(file)!=FILE_TYPE_DISK||
        !GetFileInformationByHandle(file,&before)||before.nNumberOfLinks!=1||
        (before.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
        !GetFileSizeEx(file,&size)||size.QuadPart<=0||size.QuadPart>static_cast<int64_t>(MetadataMaxBytes)||
        !SetFilePointerEx(file,zero,nullptr,FILE_BEGIN))return MetadataRead::Invalid;
    std::string bytes(static_cast<size_t>(size.QuadPart),'\0');DWORD read=0;
    if(!ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||
        read!=static_cast<DWORD>(bytes.size())||!GetFileInformationByHandle(file,&after)||
        !GetFileSizeEx(file,&afterSize)||afterSize.QuadPart!=size.QuadPart||after.nNumberOfLinks!=1||
        after.dwFileAttributes!=before.dwFileAttributes||
        CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime)!=0)return MetadataRead::Invalid;
    try{
        const auto data=MetadataJson::parse(bytes,[](int depth,MetadataJson::parse_event_t,MetadataJson&){
            if(depth>6)throw std::runtime_error("Metadata depth");return true;
        });
        if(!data.is_object()||data.size()!=7||data.at("owner")!=MetadataOwner||!data.at("schema").is_number_integer()||
            data.at("schema")!=1||!data.at("durationMs").is_number_integer())return MetadataRead::Invalid;
        SongMetadata parsed;
        parsed.id=MetadataWide(data,"id");parsed.title=MetadataWide(data,"title");
        parsed.artist=MetadataWide(data,"artist");parsed.album=MetadataWide(data,"album");
        const auto& duration=data.at("durationMs");
        if(duration.is_number_unsigned()&&duration.get<uint64_t>()>24*3600*1000ULL)return MetadataRead::Invalid;
        parsed.durationMs=duration.get<int64_t>();
        if(!MetadataValid(parsed))return MetadataRead::Invalid;
        if(facts){
            facts->bytes=static_cast<uint64_t>(size.QuadPart);
            facts->modifiedAt=(static_cast<uint64_t>(before.ftLastWriteTime.dwHighDateTime)<<32)|
                before.ftLastWriteTime.dwLowDateTime;
            facts->attributes=before.dwFileAttributes;
        }
        value=std::move(parsed);return MetadataRead::Ready;
    }catch(...){return MetadataRead::Invalid;}
}
struct MetadataLoadCandidate {
    unsigned slot=0;
    MetadataCacheHandle handle;
    SongMetadata metadata;
    MetadataFileFacts facts;
};
struct MetadataCacheName {
    std::wstring id;
    unsigned slot=0;
};
std::optional<MetadataCacheName> MetadataParseCacheName(std::wstring_view name) {
    constexpr std::wstring_view extension=L".json";
    constexpr std::wstring_view recovery=L".recovered-";
    if(name.size()<=extension.size()||!name.ends_with(extension))return {};
    const auto stem=name.substr(0,name.size()-extension.size());
    const auto marker=stem.find(recovery);
    std::wstring id;unsigned slot=0;
    if(marker==std::wstring_view::npos)id=std::wstring(stem);
    else{
        id=std::wstring(stem.substr(0,marker));const auto suffix=stem.substr(marker+recovery.size());
        if(suffix.size()!=1||suffix.front()<L'0'||suffix.front()>L'7')return {};
        slot=static_cast<unsigned>(suffix.front()-L'0')+1;
    }
    if(MetadataCanonicalId(id)!=id||MetadataSlotName(id,slot)!=name)return {};
    return MetadataCacheName{std::move(id),slot};
}
bool MetadataEnumerateNames(const std::filesystem::path& directory,std::vector<std::wstring>& names) {
    names.clear();WIN32_FIND_DATAW data{};
    MetadataFindHandle search(FindFirstFileW((directory/L"*").c_str(),&data));
    if(search.value==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    bool complete=true;
    do{
        const std::wstring name=data.cFileName;
        if(name==L"."||name==L"..")continue;
        if(names.size()>=MetadataMaxScanEntries){complete=false;break;}
        names.push_back(name);
    }while(FindNextFileW(search.value,&data));
    if(complete&&GetLastError()!=ERROR_NO_MORE_FILES)complete=false;
    if(!complete){names.clear();return false;}
    std::sort(names.begin(),names.end());return true;
}
struct MetadataCacheEntry {
    std::filesystem::path path;
    MetadataCacheName name;
    SongMetadata metadata;
    MetadataCacheHandle handle;
    uint64_t bytes=0;
    uint64_t modifiedAt=0;
    bool deletable=false;
};
struct MetadataCacheScan {
    std::vector<MetadataCacheEntry> entries;
    uint64_t totalBytes=0;
};
bool MetadataScanDirectory(const std::filesystem::path& directory,const std::wstring& protectedId,
    MetadataCacheScan& result) {
    std::vector<std::wstring> names,verifiedNames;
    if(!MetadataEnumerateNames(directory,names))return false;
    MetadataCacheScan scan;scan.entries.reserve(names.size());
    for(const auto& name:names){
        auto parsed=MetadataParseCacheName(name);if(!parsed)return false;
        MetadataCacheEntry entry;entry.path=directory/name;entry.name=std::move(*parsed);
        const DWORD sharing=entry.name.id==protectedId?FILE_SHARE_READ:FILE_SHARE_READ|FILE_SHARE_DELETE;
        entry.handle=MetadataCacheHandle(CreateFileW(entry.path.c_str(),GENERIC_READ,sharing,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        MetadataFileFacts facts;
        if(entry.handle.value==INVALID_HANDLE_VALUE||
            MetadataReadHandle(entry.handle.value,entry.metadata,&facts)!=MetadataRead::Ready||
            entry.metadata.id!=entry.name.id||facts.bytes>std::numeric_limits<uint64_t>::max()-scan.totalBytes)return false;
        scan.totalBytes+=facts.bytes;entry.bytes=facts.bytes;entry.modifiedAt=facts.modifiedAt;
        entry.deletable=entry.name.id!=protectedId&&(facts.attributes&FILE_ATTRIBUTE_READONLY)==0;
        scan.entries.push_back(std::move(entry));
    }
    if(!MetadataEnumerateNames(directory,verifiedNames)||verifiedNames!=names)return false;
    for(const auto& entry:scan.entries){
        const DWORD sharing=entry.name.id==protectedId?FILE_SHARE_READ:FILE_SHARE_READ|FILE_SHARE_DELETE;
        MetadataCacheHandle verification(CreateFileW(entry.path.c_str(),GENERIC_READ,sharing,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(verification.value==INVALID_HANDLE_VALUE||
            detail::CompareFileObjects(entry.handle.value,verification.value)!=detail::FileObjectComparison::Same)return false;
    }
    result=std::move(scan);return true;
}
bool MetadataHasRoom(const MetadataCacheScan& scan,size_t incomingBytes) {
    return incomingBytes<=MetadataCacheQuotaBytes&&scan.entries.size()<MetadataCacheRecordLimit&&
        scan.totalBytes<=MetadataCacheQuotaBytes-incomingBytes;
}
enum class MetadataDestination { Ready,AlreadyStored,Full };
MetadataDestination MetadataChooseDestination(const MetadataCacheScan& scan,const SongMetadata& value,
    const std::filesystem::path& directory,std::filesystem::path& destination) {
    std::array<bool,MetadataSlotCount> occupied{};
    for(const auto& entry:scan.entries){
        if(entry.name.id!=value.id)continue;
        if(entry.name.slot>=occupied.size()||occupied[entry.name.slot])return MetadataDestination::Full;
        occupied[entry.name.slot]=true;
        if(MetadataSame(entry.metadata,value))return MetadataDestination::AlreadyStored;
    }
    for(size_t slot=0;slot<occupied.size();++slot)if(!occupied[slot]){
        destination=MetadataSlot(directory,value.id,static_cast<unsigned>(slot));return MetadataDestination::Ready;
    }
    return MetadataDestination::Full;
}
bool MetadataSelectEvictions(const MetadataCacheScan& scan,size_t incomingBytes,std::vector<size_t>& selected) {
    selected.clear();if(incomingBytes>MetadataCacheQuotaBytes)return false;
    std::vector<size_t> candidates;
    for(size_t index=0;index<scan.entries.size();++index)if(scan.entries[index].deletable)candidates.push_back(index);
    std::sort(candidates.begin(),candidates.end(),[&](size_t left,size_t right){
        const auto& a=scan.entries[left];const auto& b=scan.entries[right];
        if(a.modifiedAt!=b.modifiedAt)return a.modifiedAt<b.modifiedAt;
        return a.path.filename().wstring()<b.path.filename().wstring();
    });
    size_t projectedEntries=scan.entries.size();uint64_t projectedBytes=scan.totalBytes;
    for(const auto index:candidates){
        if(projectedEntries<MetadataCacheRecordLimit&&projectedBytes<=MetadataCacheQuotaBytes-incomingBytes)break;
        if(selected.size()>=MetadataMaxEvictions)return false;
        selected.push_back(index);--projectedEntries;projectedBytes-=scan.entries[index].bytes;
    }
    return projectedEntries<MetadataCacheRecordLimit&&projectedBytes<=MetadataCacheQuotaBytes-incomingBytes;
}
struct MetadataPendingDelete {
    size_t index=0;
    MetadataCacheHandle handle;
};
bool MetadataDeleteSelected(MetadataCacheScan& scan,const std::vector<size_t>& selected) {
    std::vector<MetadataPendingDelete> pending;pending.reserve(selected.size());
    for(const auto index:selected){
        if(index>=scan.entries.size())return false;
        const auto& entry=scan.entries[index];
        const auto parsedName=MetadataParseCacheName(entry.path.filename().wstring());
        if(!entry.deletable||!parsedName||parsedName->id!=entry.name.id||parsedName->slot!=entry.name.slot)return false;
        MetadataCacheHandle deletion(CreateFileW(entry.path.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        SongMetadata reopened;MetadataFileFacts facts;
        if(deletion.value==INVALID_HANDLE_VALUE||
            detail::CompareFileObjects(entry.handle.value,deletion.value)!=detail::FileObjectComparison::Same||
            MetadataReadHandle(deletion.value,reopened,&facts)!=MetadataRead::Ready||
            reopened.id!=entry.name.id||!MetadataSame(reopened,entry.metadata)||facts.bytes!=entry.bytes||
            facts.modifiedAt!=entry.modifiedAt||(facts.attributes&FILE_ATTRIBUTE_READONLY))return false;
        pending.push_back({index,std::move(deletion)});
    }
    bool removed=true;
    for(auto& deletion:pending)if(!detail::DeleteOpenFile(deletion.handle.value))removed=false;
    return removed;
}
bool MetadataMakeRoom(const std::filesystem::path& directory,const std::wstring& protectedId,
    size_t incomingBytes,MetadataCacheScan& scan,std::wstring& diagnostic) {
    if(MetadataHasRoom(scan,incomingBytes))return true;
    std::vector<size_t> selected;
    if(!MetadataSelectEvictions(scan,incomingBytes,selected)||selected.empty()){
        diagnostic=L"歌曲资料缓存已达全局上限；当前歌曲及不可安全淘汰的记录已保留，本次未保存";return false;
    }
    const bool deleted=MetadataDeleteSelected(scan,selected);
    // Let selected delete-pending objects close without releasing the handles
    // that continuously protect every candidate for the current song ID.
    for(const auto index:selected)if(index<scan.entries.size())scan.entries[index].handle.Close();
    if(!deleted){
        diagnostic=L"歌曲资料缓存淘汰对象正在读取、已变化或无法安全删除，本次未写入新记录";return false;
    }
    MetadataCacheScan refreshed;
    if(!MetadataScanDirectory(directory,protectedId,refreshed)){
        diagnostic=L"淘汰后无法完整复核歌曲资料缓存；当前歌曲候选仍受保护，本次未写入新记录";return false;
    }
    scan=std::move(refreshed);
    if(!MetadataHasRoom(scan,incomingBytes)){
        diagnostic=L"歌曲资料缓存复核时出现竞争或仍超出全局上限，本次未写入新记录";return false;
    }
    return true;
}
bool MetadataAtomicCreate(const std::filesystem::path& path,const std::string& bytes) {
    GUID guid{};wchar_t token[40]{};
    if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,token,40))return false;
    auto temporary=path;temporary+=std::wstring(L".")+token+L".tmp";
    MetadataCacheHandle file(CreateFileW(temporary.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,
        CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)return false;
    try{
        DWORD written=0;
        const bool complete=WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&
            written==bytes.size()&&FlushFileBuffers(file.value);
        DWORD commitError=ERROR_SUCCESS;
        // Rename and cleanup operate on the exact open object. The absent-target
        // commit never replaces a cache entry created concurrently.
        if(complete&&detail::RenameOpenFile(file.value,path,false,commitError)){
            FlushFileBuffers(file.value);file.Close();return true;
        }
    }catch(...){
        detail::DeleteOpenFile(file.value);return false;
    }
    detail::DeleteOpenFile(file.value);return false;
}
}
std::optional<SongMetadata> LoadSongMetadataCache(const std::wstring& id,int64_t nativeDuration,
    std::stop_token stop,std::wstring& diagnostic) noexcept {
    try{
        diagnostic.clear();const auto canonical=MetadataCanonicalId(id);
        if(canonical.empty()||!SongMetadataDurationMatches(nativeDuration,nativeDuration)||stop.stop_requested())return {};
        detail::DirectoryTreeLock directoryLock;
        const auto directory=MetadataDirectory(false,directoryLock,diagnostic);if(!directory)return {};
        std::optional<MetadataLoadCandidate> best;
        for(unsigned slot=0;slot<MetadataSlotCount;++slot){
            if(stop.stop_requested())return {};
            MetadataCacheHandle handle(CreateFileW(MetadataSlot(*directory,canonical,slot).c_str(),GENERIC_READ,
                FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(handle.value==INVALID_HANDLE_VALUE)continue;
            SongMetadata value;MetadataFileFacts facts;
            if(MetadataReadHandle(handle.value,value,&facts)!=MetadataRead::Ready||value.id!=canonical||
                !SongMetadataDurationMatches(value.durationMs,nativeDuration))continue;
            MetadataLoadCandidate candidate{slot,std::move(handle),std::move(value),facts};
            if(!best||candidate.facts.modifiedAt>best->facts.modifiedAt||
                (candidate.facts.modifiedAt==best->facts.modifiedAt&&candidate.slot>best->slot))best=std::move(candidate);
        }
        if(stop.stop_requested())return {};
        if(best){
            SongMetadata verified;MetadataFileFacts facts;
            if(MetadataReadHandle(best->handle.value,verified,&facts)==MetadataRead::Ready&&
                MetadataSame(verified,best->metadata)&&verified.id==canonical&&facts.bytes==best->facts.bytes&&
                facts.modifiedAt==best->facts.modifiedAt&&facts.attributes==best->facts.attributes&&
                SongMetadataDurationMatches(verified.durationMs,nativeDuration)&&!stop.stop_requested()){
                diagnostic=L"本机歌曲资料缓存：已选最后写入的同ID及正时长记录；仅为静态资料，仍须新原生样本确认";
                return verified;
            }
        }
        diagnostic=L"本机歌曲资料缓存未命中或字段/ID/时长未通过校验，原文件保留";
    }catch(...){try{diagnostic=L"本机歌曲资料缓存暂不可读，继续既有资料来源";}catch(...){}}
    return {};
}
bool SaveSongMetadataCache(const SongMetadata& value,std::wstring& diagnostic) noexcept {
    try{
        diagnostic.clear();if(!MetadataValid(value)){diagnostic=L"歌曲资料不符合缓存字段限制，未保存";return false;}
        std::lock_guard saveLock(MetadataSaveMutex);
        const MetadataJson data{{"owner",MetadataOwner},{"schema",1},{"id",MetadataUtf8(value.id)},
            {"title",MetadataUtf8(value.title)},{"artist",MetadataUtf8(value.artist)},
            {"album",MetadataUtf8(value.album)},{"durationMs",value.durationMs}};
        const auto bytes=data.dump();
        if(bytes.empty()||bytes.size()>MetadataMaxBytes){diagnostic=L"歌曲资料超过缓存单条限制，未保存";return false;}
        detail::DirectoryTreeLock directoryLock;
        const auto directory=MetadataDirectory(true,directoryLock,diagnostic);if(!directory)return false;
        MetadataCacheScan scan;
        if(!MetadataScanDirectory(*directory,value.id,scan)){
            diagnostic=L"无法完整核对歌曲资料缓存；未知、损坏、重解析或竞争对象已保留，本次未保存";return false;
        }
        std::filesystem::path destination;
        auto destinationState=MetadataChooseDestination(scan,value,*directory,destination);
        if(destinationState==MetadataDestination::AlreadyStored)return true;
        if(destinationState==MetadataDestination::Full){
            diagnostic=L"歌曲资料缓存及八个恢复位置已占用，当前歌曲旧文件保留，本次未保存";return false;
        }
        if(!MetadataMakeRoom(*directory,value.id,bytes.size(),scan,diagnostic))return false;
        destinationState=MetadataChooseDestination(scan,value,*directory,destination);
        if(destinationState==MetadataDestination::AlreadyStored)return true;
        if(destinationState!=MetadataDestination::Ready){
            diagnostic=L"歌曲资料缓存复核时当前歌曲槽位发生变化，旧文件保留，本次未保存";return false;
        }
        if(!MetadataAtomicCreate(destination,bytes)){
            diagnostic=L"歌曲资料缓存写入失败，旧文件保留，实时跟随继续";return false;
        }
        return true;
    }catch(...){try{diagnostic=L"歌曲资料缓存未保存，实时跟随继续";}catch(...){}}
    return false;
}
}

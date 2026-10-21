#include "settings.h"
#include "catalog_id.h"
#include "themes.h"
#include "file_transaction.h"
#include "local_file_limits.h"
#include "third_party/json.hpp"
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <limits>
#include <string_view>
#include <vector>

namespace stage {
namespace {
using Json=nlohmann::json;
constexpr size_t SettingsMaxBytes=256*1024;
constexpr size_t TrackPreferencesMaxBytes=1024*1024;
constexpr size_t CandidateMatchKeyMaxUtf8Bytes=MaxConfirmedCandidateKeyChars*4;
std::mutex settingsSaveMutex,trackPreferencesSaveMutex;
std::string Utf8(const std::wstring& text) {
    if(text.empty())return {};
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),(int)text.size(),nullptr,0,nullptr,nullptr);
    if(n<=0)return {};std::string value(n,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),(int)text.size(),value.data(),n,nullptr,nullptr);return value;
}
std::wstring Wide(const std::string& text) {
    if(text.empty())return {};const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),(int)text.size(),nullptr,0);
    if(n<=0)return {};std::wstring value(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),(int)text.size(),value.data(),n);return value;
}
bool PositiveId(const std::wstring& id) {return !id.empty()&&id.size()<=19&&id.find_first_not_of(L"0123456789")==std::wstring::npos&&id.find_first_not_of(L'0')!=std::wstring::npos;}
bool ValidCandidateMatchKey(const std::wstring& key) {
    return !key.empty()&&key.size()<=MaxConfirmedCandidateKeyChars&&key.find(L'\0')==std::wstring::npos&&!Utf8(key).empty();
}
bool ValidCustomThemePath(const std::wstring& path) {
    return path.size()<=MaxLocalFilePathChars && path.find(L'\0')==std::wstring::npos &&
        (path.empty()||!Utf8(path).empty());
}
bool ReadCustomThemePath(const Json& data,std::wstring& path,std::wstring& diagnostic) {
    path.clear();
    const auto field=data.find("customThemePath");
    if(field==data.end())return true;
    if(field->is_string()) {
        const auto& encoded=field->get_ref<const std::string&>();
        const auto decoded=Wide(encoded);
        if((encoded.empty()||!decoded.empty())&&ValidCustomThemePath(decoded)){path=decoded;return true;}
    }
    if(!diagnostic.empty())diagnostic+=L"；";
    diagnostic+=L"自定义主题路径无效或超过32700字符，已忽略该路径";
    return false;
}
bool SettingsPayloadAllowed(const std::string& serialized,std::wstring& diagnostic) {
    if(serialized.size()<=SettingsMaxBytes)return true;
    diagnostic=L"设置序列化后超过256 KiB，旧设置文件已保留";
    return false;
}
void Note(std::wstring& diagnostic,const std::wstring& message) {
    if(!diagnostic.empty())diagnostic+=L"；";
    diagnostic+=message;
}
std::optional<int64_t> IntegerField(const Json& object,const char* name) {
    const auto field=object.find(name);
    if(field==object.end()||!field->is_number_integer())return std::nullopt;
    try{
        if(field->is_number_unsigned()){
            const auto value=field->get<uint64_t>();
            if(value>uint64_t(std::numeric_limits<int64_t>::max()))return std::nullopt;
            return (int64_t)value;
        }
        return field->get<int64_t>();
    }catch(...){return std::nullopt;}
}
std::optional<double> NumberField(const Json& object,const char* name) {
    const auto field=object.find(name);
    if(field==object.end()||!field->is_number())return std::nullopt;
    try{const auto value=field->get<double>();return std::isfinite(value)?std::optional<double>(value):std::nullopt;}catch(...){return std::nullopt;}
}
std::optional<bool> BooleanField(const Json& object,const char* name) {
    const auto field=object.find(name);
    if(field==object.end()||!field->is_boolean())return std::nullopt;
    return field->get<bool>();
}
std::optional<std::string> StringField(const Json& object,const char* name) {
    const auto field=object.find(name);
    if(field==object.end()||!field->is_string())return std::nullopt;
    return field->get<std::string>();
}
bool ContainsOnlyFields(const Json& object,std::initializer_list<std::string_view> allowed) {
    if(!object.is_object())return false;
    for(auto field=object.begin();field!=object.end();++field){
        const auto name=std::string_view(field.key());
        if(std::find(allowed.begin(),allowed.end(),name)==allowed.end())return false;
    }
    return true;
}

enum class BoundedFileReadResult { Missing, Read };

BoundedFileReadResult ReadBoundedSettingsFile(const std::filesystem::path& file,size_t maximumBytes,
    std::string& contents) {
    contents.clear();
    const auto directory=file.parent_path().lexically_normal();
    if(directory!=(UserDataDirectory()).lexically_normal())throw std::runtime_error("read-boundary");
    detail::DirectoryTreeLock directoryLock;DWORD directoryError=ERROR_SUCCESS;
    if(!directoryLock.Acquire(directory,directoryError)){
        if(directoryError==ERROR_FILE_NOT_FOUND||directoryError==ERROR_PATH_NOT_FOUND)
            return BoundedFileReadResult::Missing;
        throw std::runtime_error("read-directory");
    }
    HANDLE input=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(input==INVALID_HANDLE_VALUE){
        const auto openError=GetLastError();
        if(openError==ERROR_FILE_NOT_FOUND||openError==ERROR_PATH_NOT_FOUND)
            return BoundedFileReadResult::Missing;
        throw std::runtime_error("read-open");
    }
    try{
        BY_HANDLE_FILE_INFORMATION information{};LARGE_INTEGER size{};
        if(GetFileType(input)!=FILE_TYPE_DISK||!GetFileInformationByHandle(input,&information)||
           (information.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
           !GetFileSizeEx(input,&size)||size.QuadPart<0||
           static_cast<unsigned long long>(size.QuadPart)>maximumBytes)
            throw std::runtime_error("read-size");
        contents.resize(static_cast<size_t>(size.QuadPart));
        size_t offset=0;
        while(offset<contents.size()){
            const auto remaining=contents.size()-offset;
            const auto request=static_cast<DWORD>(std::min<size_t>(remaining,std::numeric_limits<DWORD>::max()));
            DWORD read=0;
            if(!ReadFile(input,contents.data()+offset,request,&read,nullptr)||read==0)
                throw std::runtime_error("read-data");
            offset+=read;
        }
        LARGE_INTEGER finalSize{};
        if(!GetFileSizeEx(input,&finalSize)||finalSize.QuadPart!=size.QuadPart)
            throw std::runtime_error("read-changed");
    }catch(...){
        CloseHandle(input);
        throw;
    }
    CloseHandle(input);
    return BoundedFileReadResult::Read;
}
}
std::filesystem::path UserDataDirectory() {
    PWSTR path=nullptr;std::filesystem::path base;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&path))){base=path;CoTaskMemFree(path);}else base=std::filesystem::temp_directory_path();
    return base/L"MusicStageNativePrototype";
}
std::wstring TrackMatchKey(const std::wstring& title,const std::wstring& artist) {return title+L"\x1f"+artist;}
std::wstring TrackLyricKey(const std::wstring& verifiedId,const std::wstring& title,const std::wstring& artist) {
    if(PositiveId(verifiedId))return L"song:"+verifiedId.substr(verifiedId.find_first_not_of(L'0'));
    if(title.empty()||title.size()+artist.size()>1024||title.find_first_of(L"\x1f")!=std::wstring::npos||
       artist.find_first_of(L"\x1f")!=std::wstring::npos||title.find(L'\0')!=std::wstring::npos||artist.find(L'\0')!=std::wstring::npos)return {};
    const auto key=L"meta:"+TrackMatchKey(title,artist);
    return Utf8(key).empty()?std::wstring{}:key;
}
namespace {
bool ValidTrackKey(const std::wstring& key) {
    if(key.empty()||key.size()>1030||key.find(L'\0')!=std::wstring::npos||Utf8(key).empty())return false;
    if(key.starts_with(L"song:")){const auto id=key.substr(5);return PositiveId(id)&&id.front()!=L'0';}
    if(!key.starts_with(L"meta:"))return false;
    const auto split=key.find(L'\x1f',5);
    return split!=std::wstring::npos&&split>5&&key.find(L'\x1f',split+1)==std::wstring::npos;
}
std::optional<std::string> TrackPreferencesPayload(const TrackLyricPreferences& value,std::wstring& diagnostic) {
    if(value.entries.size()>512){diagnostic=L"歌词记忆超过512首，原文件已保留";return std::nullopt;}
    Json tracks=Json::object();
    for(const auto& [key,preference]:value.entries){
        if(!ValidTrackKey(key)||!ValidCustomThemePath(preference.importPath)||
           (preference.offsetMs&&(*preference.offsetMs < -30000||*preference.offsetMs > 30000))){
            diagnostic=L"歌词记忆含无效记录，原文件已保留";return std::nullopt;
        }
        Json row=Json::object();if(preference.offsetMs)row["offsetMs"]=*preference.offsetMs;
        if(!preference.importPath.empty())row["importPath"]=Utf8(preference.importPath);
        tracks[Utf8(key)]=std::move(row);
    }
    auto serialized=Json{{"schema",1},{"tracks",std::move(tracks)}}.dump(2)+"\n";
    if(serialized.size()>TrackPreferencesMaxBytes){diagnostic=L"本歌歌词记忆超过1 MiB，旧文件已保留";return std::nullopt;}
    return serialized;
}

bool CommitSerializedFile(const std::filesystem::path& file,const wchar_t* temporaryStem,
    const wchar_t* label,const std::string& serialized,std::wstring& diagnostic) {
    std::filesystem::path temporary;
    HANDLE output=INVALID_HANDLE_VALUE;
    bool committed=false;
    try{
        const auto directory=file.parent_path().lexically_normal();
        if(directory!=(UserDataDirectory()).lexically_normal()){
            diagnostic=std::wstring(label)+L"保存目标不在应用数据目录内，旧文件已保留";
            return false;
        }
        detail::DirectoryTreeLock directoryLock;DWORD directoryError=ERROR_SUCCESS;
        if(!directoryLock.Acquire(directory,directoryError,true)){
            diagnostic=std::wstring(label)+L"保存目录不是稳定的本地普通目录（Windows "+
                std::to_wstring(directoryError)+L"），旧文件已保留";
            return false;
        }
        DWORD createError=ERROR_SUCCESS;
        for(unsigned attempt=0;attempt<4&&output==INVALID_HANDLE_VALUE;++attempt){
            GUID guid{};wchar_t token[40]{};
            if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,token,40)){
                createError=ERROR_INVALID_DATA;break;
            }
            temporary=directory/(std::wstring(temporaryStem)+L"-"+token+L".tmp");
            output=CreateFileW(temporary.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
            if(output==INVALID_HANDLE_VALUE){
                createError=GetLastError();
                if(createError!=ERROR_FILE_EXISTS&&createError!=ERROR_ALREADY_EXISTS)break;
            }
        }
        if(output==INVALID_HANDLE_VALUE){
            diagnostic=std::wstring(label)+L"无法创建唯一临时文件（Windows "+std::to_wstring(createError)+L"），旧文件已保留";
            return false;
        }
        DWORD written=0;
        DWORD writeError=ERROR_SUCCESS;
        if(!WriteFile(output,serialized.data(),static_cast<DWORD>(serialized.size()),&written,nullptr))writeError=GetLastError();
        else if(written!=static_cast<DWORD>(serialized.size()))writeError=ERROR_WRITE_FAULT;
        else if(!FlushFileBuffers(output))writeError=GetLastError();
        if(writeError!=ERROR_SUCCESS){
            detail::DeleteOpenFile(output);CloseHandle(output);output=INVALID_HANDLE_VALUE;
            diagnostic=std::wstring(label)+L"未能完整写入（Windows "+std::to_wstring(writeError)+L"），旧文件已保留";
            return false;
        }
        const auto attributes=GetFileAttributesW(file.c_str());
        bool replaceExisting=false;
        if(attributes!=INVALID_FILE_ATTRIBUTES){
            if(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)){
                detail::DeleteOpenFile(output);CloseHandle(output);output=INVALID_HANDLE_VALUE;
                diagnostic=std::wstring(label)+L"目标不是可替换的普通文件，旧目标已保留";
                return false;
            }
            replaceExisting=true;
        }else{
            const auto statusError=GetLastError();
            if(statusError!=ERROR_FILE_NOT_FOUND&&statusError!=ERROR_PATH_NOT_FOUND){
                detail::DeleteOpenFile(output);CloseHandle(output);output=INVALID_HANDLE_VALUE;
                diagnostic=std::wstring(label)+L"无法检查目标文件（Windows "+std::to_wstring(statusError)+L"），旧目标已保留";
                return false;
            }
        }
        DWORD replaceError=ERROR_SUCCESS;
        if(!detail::RenameOpenFile(output,file,replaceExisting,replaceError)){
            detail::DeleteOpenFile(output);CloseHandle(output);output=INVALID_HANDLE_VALUE;
            diagnostic=std::wstring(label)+L"保存失败（Windows "+std::to_wstring(replaceError)+L"），旧文件未替换";
            return false;
        }
        committed=true;
        FlushFileBuffers(output);
        CloseHandle(output);output=INVALID_HANDLE_VALUE;
        return true;
    }catch(...){
        if(output!=INVALID_HANDLE_VALUE){if(!committed)detail::DeleteOpenFile(output);CloseHandle(output);}
        diagnostic=std::wstring(label)+L"无法保存，旧文件已保留";
        return false;
    }
}

enum class RecoveryMaterialState { None, Present, Unavailable };

RecoveryMaterialState FindRecoveryMaterial(const wchar_t* stem) noexcept {
    try{
        const auto directory=UserDataDirectory();
        bool unavailable=false;
        for(const auto suffix:{L"-recovery-*.tmp",L"-backup-*.json"}){
            const auto pattern=(directory/(std::wstring(stem)+suffix)).wstring();
            WIN32_FIND_DATAW found{};
            const auto search=FindFirstFileW(pattern.c_str(),&found);
            if(search!=INVALID_HANDLE_VALUE){FindClose(search);return RecoveryMaterialState::Present;}
            const auto error=GetLastError();
            if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND&&error!=ERROR_NO_MORE_FILES)
                unavailable=true;
        }
        return unavailable?RecoveryMaterialState::Unavailable:RecoveryMaterialState::None;
    }catch(...){return RecoveryMaterialState::Unavailable;}
}
}
TrackLyricPreferences LoadTrackLyricPreferences(std::wstring& diagnostic) {
    diagnostic.clear();TrackLyricPreferences value;
    try{
        const auto file=UserDataDirectory()/L"track-lyrics.json";
        std::string encoded;
        if(ReadBoundedSettingsFile(file,TrackPreferencesMaxBytes,encoded)==BoundedFileReadResult::Missing){
            const auto recovery=FindRecoveryMaterial(L"track-lyrics");
            if(recovery!=RecoveryMaterialState::None){
                value.writable=false;
                diagnostic=recovery==RecoveryMaterialState::Present?
                    L"正式歌词记忆文件缺失，但检测到备份或未完成的恢复材料；已暂停自动保存，请在歌词页确认恢复":
                    L"正式歌词记忆文件缺失，且无法核验恢复材料；已暂停自动保存以免覆盖可恢复内容";
            }
            return value;
        }
        const auto data=Json::parse(encoded,[](int depth,Json::parse_event_t,Json&){if(depth>8)throw std::runtime_error("depth");return true;},false);
        const auto schema=data.is_object()?IntegerField(data,"schema"):std::nullopt;
        if(!schema||*schema!=1||!data.contains("tracks")||!data["tracks"].is_object()||data["tracks"].size()>512)throw std::runtime_error("schema");
        bool preserveOriginal=!ContainsOnlyFields(data,{"schema","tracks"});
        for(auto it=data["tracks"].begin();it!=data["tracks"].end();++it){
            const auto key=Wide(it.key());const auto& row=it.value();
            if(!ValidTrackKey(key)||!row.is_object())throw std::runtime_error("entry");
            if(!ContainsOnlyFields(row,{"offsetMs","importPath"}))preserveOriginal=true;
            TrackLyricPreference preference;
            if(row.contains("offsetMs")){
                const auto offset=IntegerField(row,"offsetMs");
                if(!offset||*offset < -30000||*offset > 30000)throw std::runtime_error("offset");
                preference.offsetMs=(int)*offset;
            }
            if(row.contains("importPath")){
                const auto path=StringField(row,"importPath");if(!path)throw std::runtime_error("path-type");
                preference.importPath=Wide(*path);
                if((!path->empty()&&preference.importPath.empty())||!ValidCustomThemePath(preference.importPath))throw std::runtime_error("path");
            }
            value.entries.emplace(key,std::move(preference));
        }
        if(preserveOriginal){
            value.writable=false;
            diagnostic=L"本歌歌词记忆包含当前版本不识别的字段，已载入已知内容并保留原文件；本次调整暂不写入";
        }
    }catch(...){value.entries.clear();value.writable=false;diagnostic=L"本歌歌词记忆文件无法解析或读取，已保留原文件；本次调整暂不写入";}
    return value;
}
bool SaveTrackLyricPreferences(const TrackLyricPreferences& value,std::wstring& diagnostic) {
    std::lock_guard lock(trackPreferencesSaveMutex);diagnostic.clear();
    if(!value.writable){diagnostic=L"本歌歌词记忆原文件需恢复，本次调整只保留在当前运行中";return false;}
    try{
        const auto serialized=TrackPreferencesPayload(value,diagnostic);if(!serialized)return false;
        return CommitSerializedFile(UserDataDirectory()/L"track-lyrics.json",L"track-lyrics-save",L"本歌歌词记忆",*serialized,diagnostic);
    }catch(...){diagnostic=L"本歌歌词记忆无法保存，旧文件已保留";return false;}
}
AppSettings LoadSettings(std::wstring& diagnostic) {
    diagnostic.clear();AppSettings value;
    try {
        const auto file=UserDataDirectory()/L"settings.json";
        std::string encoded;
        if(ReadBoundedSettingsFile(file,SettingsMaxBytes,encoded)==BoundedFileReadResult::Missing){
            const auto recovery=FindRecoveryMaterial(L"settings");
            if(recovery!=RecoveryMaterialState::None){
                value.writable=false;
                diagnostic=recovery==RecoveryMaterialState::Present?
                    L"正式设置文件缺失，但检测到备份或未完成的恢复材料；已暂停自动保存，请在状态页确认恢复":
                    L"正式设置文件缺失，且无法核验恢复材料；已暂停自动保存以免覆盖可恢复内容";
            }
            return value;
        }
        if(encoded.empty())throw std::runtime_error("size");
        Json data=Json::parse(encoded,[](int depth,Json::parse_event_t,Json&){if(depth>16)throw std::runtime_error("depth");return true;},false);
        const auto schema=data.is_object()?IntegerField(data,"schema"):std::nullopt;
        if(data.is_discarded()||!data.is_object()||!schema||*schema!=1)throw std::runtime_error("schema");
        bool preserveOriginal=false;
        const auto preserve=[&](const std::wstring& message){preserveOriginal=true;Note(diagnostic,message);};
        if(!ContainsOnlyFields(data,{"schema","theme","offsetMs","response","lyricSize","lyricFont","lyricFontPath","reducedMotion",
            "panelVisible","nativeClockEnabled","showTranslation","showRomanization","lyricDisplayMode",
            "fullscreenAutoHide","backgroundShade","cameraMotion","presentationMode","backgroundKind",
            "backgroundPath","window","customThemePath","confirmedCandidates",
            "qualityMode","frameRateMode","pauseForFullscreenApps","motionVariant","motionFx","motionCamera","motionLines","motionBackground","motionFilter"}))
            preserve(L"设置包含当前版本不识别的字段，已保留原文件避免丢失扩展数据");
        if(const auto field=IntegerField(data,"theme")){
            value.theme=NormalizeBuiltInStyle((int)std::clamp<int64_t>(*field,0,BuiltInStyleCount-1));
            if(*field<0||*field>=BuiltInStyleCount)preserve(L"theme 越界，已限制到有效范围");
        }else if(data.contains("theme"))preserve(L"theme 无效，已使用默认值");
        if(const auto field=IntegerField(data,"motionVariant")){value.motionVariant=static_cast<int>(std::clamp<int64_t>(*field,-1,255));if(*field<-1||*field>255)preserve(L"motionVariant 越界，原文件已保留");}else if(data.contains("motionVariant"))preserve(L"motionVariant 无效，原文件已保留");
        if(MotionVariantCount(value.theme)>0&&value.motionVariant>=MotionVariantCount(value.theme)){value.motionVariant=-1;preserve(L"motionVariant 超出当前主题的子预设范围，已使用自动编排并保留原文件");}
        if(const auto field=IntegerField(data,"motionFx")){value.motionFx=static_cast<int>(std::clamp<int64_t>(*field,0,5));if(*field<0||*field>5)preserve(L"motionFx 越界，原文件已保留");}else if(data.contains("motionFx"))preserve(L"motionFx 无效，原文件已保留");
        if(const auto field=IntegerField(data,"motionCamera")){value.motionCamera=static_cast<int>(std::clamp<int64_t>(*field,0,2));if(*field<0||*field>2)preserve(L"motionCamera 越界，原文件已保留");}else if(data.contains("motionCamera"))preserve(L"motionCamera 无效，原文件已保留");
        if(const auto field=IntegerField(data,"motionLines")){value.motionLines=static_cast<int>(std::clamp<int64_t>(*field,1,10));if(*field<1||*field>10)preserve(L"motionLines 越界，原文件已保留");}else if(data.contains("motionLines"))preserve(L"motionLines 无效，原文件已保留");
        if(const auto field=IntegerField(data,"motionFilter")){value.motionFilter=static_cast<int>(std::clamp<int64_t>(*field,0,4));if(*field<0||*field>4)preserve(L"motionFilter 越界，原文件已保留");}else if(data.contains("motionFilter"))preserve(L"motionFilter 无效，原文件已保留");
        if(const auto field=IntegerField(data,"motionBackground")){value.motionBackground=static_cast<int>(std::clamp<int64_t>(*field,0,4));if(*field<0||*field>4)preserve(L"motionBackground 越界，原文件已保留");}else if(data.contains("motionBackground"))preserve(L"motionBackground 无效，原文件已保留");
        if(const auto field=IntegerField(data,"offsetMs")){
            value.offsetMs=(int)std::clamp<int64_t>(*field,-30000,30000);
            if(*field<-30000||*field>30000)preserve(L"offsetMs 越界，已限制到有效范围");
        }else if(data.contains("offsetMs"))preserve(L"offsetMs 无效，已使用默认值");
        if(const auto field=NumberField(data,"response")){
            value.response=(float)std::clamp(*field,0.,2.);
            if(*field<0.||*field>2.)preserve(L"response 越界，已限制到有效范围");
        }else if(data.contains("response"))preserve(L"response 无效，已使用默认值");
        for(const auto name:{"lyricFont","lyricFontPath"})if(data.contains(name)){
            const auto text=StringField(data,name);
            if(!text||text->size()>(std::string(name)=="lyricFont"?768u:98304u)){preserve(L"歌词字体字段无效，原设置已保留");continue;}
            auto valueText=Wide(*text);
            const size_t limit=std::string(name)=="lyricFont"?256:32760;
            if((!text->empty()&&valueText.empty())||valueText.size()>limit||valueText.find(L'\0')!=std::wstring::npos){preserve(L"歌词字体字段含无效字符或过长");continue;}
            if(std::string(name)=="lyricFont")value.lyricFont=std::move(valueText);else value.lyricFontPath=std::move(valueText);
        }
        if(const auto field=NumberField(data,"lyricSize")){
            value.lyricSize=(float)std::clamp(*field,26.,80.);
            if(*field<26.||*field>80.)preserve(L"lyricSize 越界，已限制到有效范围");
        }else if(data.contains("lyricSize"))preserve(L"lyricSize 无效，已使用默认值");
        if(const auto field=BooleanField(data,"reducedMotion"))value.reducedMotion=*field;else if(data.contains("reducedMotion"))preserve(L"reducedMotion 无效，已使用默认值");
        if(const auto field=BooleanField(data,"panelVisible"))value.panelVisible=*field;else if(data.contains("panelVisible"))preserve(L"panelVisible 无效，已使用默认值");
        if(const auto field=BooleanField(data,"nativeClockEnabled"))value.nativeClockEnabled=*field;else if(data.contains("nativeClockEnabled"))preserve(L"nativeClockEnabled 无效，已使用默认值");
        if(const auto field=BooleanField(data,"showTranslation"))value.showTranslation=*field;else if(data.contains("showTranslation"))preserve(L"showTranslation 无效，已使用默认值");
        if(const auto field=BooleanField(data,"showRomanization"))value.showRomanization=*field;else if(data.contains("showRomanization"))preserve(L"showRomanization 无效，已使用默认值");
        if(const auto field=IntegerField(data,"lyricDisplayMode")){
            value.lyricDisplayMode=(int)std::clamp<int64_t>(*field,0,2);
            if(*field<0||*field>2)preserve(L"lyricDisplayMode 越界，已限制到有效范围");
        }else if(data.contains("lyricDisplayMode"))preserve(L"lyricDisplayMode 无效，已使用流动歌词");
        if(const auto field=BooleanField(data,"fullscreenAutoHide"))value.fullscreenAutoHide=*field;else if(data.contains("fullscreenAutoHide"))preserve(L"fullscreenAutoHide 无效，已使用默认值");
        if(const auto field=IntegerField(data,"qualityMode")){
            value.qualityMode=(int)std::clamp<int64_t>(*field,0,3);
            if(*field<0||*field>3)preserve(L"qualityMode 越界，已限制到有效范围");
        }else if(data.contains("qualityMode"))preserve(L"qualityMode 无效，已使用自动画质");
        if(const auto field=IntegerField(data,"frameRateMode")){
            value.frameRateMode=(int)std::clamp<int64_t>(*field,0,2);
            if(*field<0||*field>2)preserve(L"frameRateMode 越界，已限制到有效范围");
        }else if(data.contains("frameRateMode"))preserve(L"frameRateMode 无效，已使用自动帧率");
        if(const auto field=BooleanField(data,"pauseForFullscreenApps"))value.pauseForFullscreenApps=*field;
        else if(data.contains("pauseForFullscreenApps"))preserve(L"pauseForFullscreenApps 无效，已使用默认值");
        if(const auto field=NumberField(data,"backgroundShade")){
            value.backgroundShade=(float)std::clamp(*field,0.,1.);
            if(*field<0.||*field>1.)preserve(L"backgroundShade 越界，已限制到有效范围");
        }else if(data.contains("backgroundShade"))preserve(L"backgroundShade 无效，已使用默认值");
        if(const auto field=NumberField(data,"cameraMotion")){
            value.cameraMotion=(float)std::clamp(*field,0.,2.);
            if(*field<0.||*field>2.)preserve(L"cameraMotion 越界，已限制到有效范围");
        }else if(data.contains("cameraMotion"))preserve(L"cameraMotion 无效，已使用默认值");
        if(const auto field=IntegerField(data,"presentationMode")){
            value.presentationMode=(int)std::clamp<int64_t>(*field,0,1);
            if(*field<0||*field>1)preserve(L"presentationMode 越界，已限制到有效范围");
        }else if(data.contains("presentationMode"))preserve(L"presentationMode 无效，已使用窗口模式");
        if(const auto field=IntegerField(data,"backgroundKind")){
            value.backgroundKind=(int)std::clamp<int64_t>(*field,0,2);
            if(*field<0||*field>2)preserve(L"backgroundKind 越界，已限制到有效范围");
        }else if(data.contains("backgroundKind"))preserve(L"backgroundKind 无效，已使用默认值");
        if(const auto field=StringField(data,"backgroundPath")){
            value.backgroundPath=Wide(*field);
            if((!field->empty()&&value.backgroundPath.empty())||!ValidCustomThemePath(value.backgroundPath)){
                value.backgroundPath.clear();value.backgroundKind=0;
                preserve(L"背景路径无效，已恢复封面背景");
            }
        }else if(data.contains("backgroundPath")){
            value.backgroundKind=0;
            preserve(L"backgroundPath 无效，已恢复封面背景");
        }
        if(value.backgroundKind!=0&&value.backgroundPath.empty()){
            value.backgroundKind=0;
            preserve(L"图片或视频背景缺少文件路径，已恢复封面背景");
        }
        const auto window=data.find("window");
        if(window!=data.end()&&window->is_object()){
            if(!ContainsOnlyFields(*window,{"x","y","width","height","maximized","dpi"}))
                preserve(L"窗口设置包含当前版本不识别的字段，已保留原文件避免丢失扩展数据");
            const auto x=IntegerField(*window,"x"),y=IntegerField(*window,"y"),width=IntegerField(*window,"width"),height=IntegerField(*window,"height");
            const auto maximized=BooleanField(*window,"maximized");
            if(x&&y&&width&&height&&*x>=-200000&&*x<=200000&&*y>=-200000&&*y<=200000&&*width>=320&&*width<=16384&&*height>=240&&*height<=16384){value.hasWindowPlacement=true;value.windowX=(int)*x;value.windowY=(int)*y;value.windowWidth=(int)*width;value.windowHeight=(int)*height;value.windowMaximized=maximized.value_or(false);
                if(window->contains("maximized")&&!maximized)preserve(L"窗口最大化状态无效，已使用普通窗口状态");
                if(const auto dpi=IntegerField(*window,"dpi");dpi&&*dpi>=48&&*dpi<=768)value.windowDpi=(int)*dpi;
                else if(window->contains("dpi"))preserve(L"窗口 DPI 无效，已按当前显示器调整");
            }
            else preserve(L"窗口位置无效，已由 Windows 自动选择");
        }else if(window!=data.end())preserve(L"window 无效，已由 Windows 自动选择");
        if(!ReadCustomThemePath(data,value.customThemePath,diagnostic))preserveOriginal=true;
        const auto matches=data.find("confirmedCandidates");
        if(matches!=data.end()&&matches->is_object()){
            bool invalid=false,overflow=false;
            for(auto it=matches->begin();it!=matches->end();++it){
                if(it.key().size()>CandidateMatchKeyMaxUtf8Bytes||!it.value().is_string()){invalid=true;continue;}
                auto key=Wide(it.key()),id=Wide(it.value().get<std::string>());
                if(!ValidCandidateMatchKey(key)||NormalizeCatalogId(id).empty()){invalid=true;continue;}
                if(value.confirmedCandidates.size()>=MaxConfirmedCandidates){overflow=true;continue;}
                value.confirmedCandidates.emplace(std::move(key),std::move(id));
            }
            if(invalid){
                value.writable=false;
                Note(diagnostic,L"已确认歌词版本含无效记录，已载入其余有效记录；原设置文件不作自动覆盖，可在状态页备份后恢复保存");
            }
            if(overflow){
                value.writable=false;
                Note(diagnostic,L"已确认歌词版本超过512首，已载入确定顺序内的前512首；原设置文件不作自动覆盖，可在状态页备份后恢复保存");
            }
        }else if(matches!=data.end()){
            value.writable=false;
            Note(diagnostic,L"已确认歌词版本字段无效，原设置文件不作自动覆盖，可在状态页备份后恢复保存");
        }
        if(preserveOriginal){
            value.writable=false;
            Note(diagnostic,L"设置含无效或越界字段，原文件不作自动覆盖；可在状态页备份后恢复保存");
        }
    } catch(...) {value=AppSettings{};value.writable=false;diagnostic=L"设置无法完整读取或版本不支持，原文件不作自动覆盖；本次使用默认值，调整暂不保存，可在状态页恢复设置保存";}
    return value;
}
namespace {
std::optional<std::string> SettingsPayload(const AppSettings& value,std::wstring& diagnostic) {
    if(value.lyricFont.size()>256||value.lyricFont.find(L'\0')!=std::wstring::npos||(!value.lyricFont.empty()&&Utf8(value.lyricFont).empty())||value.lyricFontPath.size()>32760||value.lyricFontPath.find(L'\0')!=std::wstring::npos||(!value.lyricFontPath.empty()&&Utf8(value.lyricFontPath).empty())){diagnostic=L"歌词字体名称或路径无效，旧设置文件已保留";return std::nullopt;}
    if(!ValidCustomThemePath(value.customThemePath)){diagnostic=L"自定义主题路径无效或超过32700字符，设置未写入";return std::nullopt;}
        if(value.backgroundKind<0||value.backgroundKind>2||!ValidCustomThemePath(value.backgroundPath)||
           (value.backgroundKind!=0&&value.backgroundPath.empty())){
            diagnostic=L"背景类型或路径无效，旧设置文件已保留";return std::nullopt;
        }
        std::vector<std::pair<std::string,std::string>> validMatches;validMatches.reserve(value.confirmedCandidates.size());
        for(const auto& [key,id]:value.confirmedCandidates){
            if(!ValidCandidateMatchKey(key)||NormalizeCatalogId(id).empty()){diagnostic=L"已确认歌词版本含无效记录，旧设置文件已保留";return std::nullopt;}
            validMatches.emplace_back(Utf8(key),Utf8(id));
        }
        if(validMatches.size()>MaxConfirmedCandidates){diagnostic=L"已确认歌词版本超过512首，旧设置文件已保留";return std::nullopt;}
        std::sort(validMatches.begin(),validMatches.end(),[](const auto& left,const auto& right){return left.first<right.first;});
        Json matches=Json::object();for(auto& [key,id]:validMatches)matches[std::move(key)]=std::move(id);
        Json data={{"schema",1},{"theme",NormalizeBuiltInStyle(value.theme)},{"offsetMs",std::clamp(value.offsetMs,-30000,30000)},
          {"response",std::isfinite(value.response)?std::clamp(value.response,0.f,2.f):1.f},{"lyricSize",std::isfinite(value.lyricSize)?std::clamp(value.lyricSize,26.f,80.f):44.f},
          {"reducedMotion",value.reducedMotion},{"panelVisible",value.panelVisible},{"nativeClockEnabled",value.nativeClockEnabled},{"showTranslation",value.showTranslation},{"showRomanization",value.showRomanization},{"presentationMode",std::clamp(value.presentationMode,0,1)},{"backgroundKind",std::clamp(value.backgroundKind,0,2)},
          {"backgroundPath",Utf8(value.backgroundPath)},{"confirmedCandidates",std::move(matches)}};
        if(value.hasWindowPlacement){data["window"]={{"x",std::clamp(value.windowX,-200000,200000)},{"y",std::clamp(value.windowY,-200000,200000)},{"width",std::clamp(value.windowWidth,320,16384)},{"height",std::clamp(value.windowHeight,240,16384)},{"maximized",value.windowMaximized}};
            if(value.windowDpi>=48&&value.windowDpi<=768)data["window"]["dpi"]=value.windowDpi;
        }
        data["lyricFont"]=Utf8(value.lyricFont);data["lyricFontPath"]=Utf8(value.lyricFontPath);
        data["motionVariant"]=std::clamp(value.motionVariant,-1,255);
        data["motionFx"]=std::clamp(value.motionFx,0,5);
        data["motionCamera"]=std::clamp(value.motionCamera,0,2);
        data["motionLines"]=std::clamp(value.motionLines,1,10);
        data["motionFilter"]=std::clamp(value.motionFilter,0,4);
        data["motionBackground"]=std::clamp(value.motionBackground,0,4);
        data["customThemePath"]=Utf8(value.customThemePath);
        data["lyricDisplayMode"]=std::clamp(value.lyricDisplayMode,0,2);
        data["fullscreenAutoHide"]=value.fullscreenAutoHide;
        data["backgroundShade"]=std::isfinite(value.backgroundShade)?std::clamp(value.backgroundShade,0.f,1.f):0.f;
        data["cameraMotion"]=std::isfinite(value.cameraMotion)?std::clamp(value.cameraMotion,0.f,2.f):1.f;
        data["qualityMode"]=std::clamp(value.qualityMode,0,3);
        data["frameRateMode"]=std::clamp(value.frameRateMode,0,2);
        data["pauseForFullscreenApps"]=value.pauseForFullscreenApps;
        const auto serialized=data.dump(2)+"\n";
        if(!SettingsPayloadAllowed(serialized,diagnostic))return std::nullopt;
        return serialized;
}
}
bool SaveSettings(const AppSettings& value,std::wstring& diagnostic) {
    std::lock_guard lock(settingsSaveMutex);diagnostic.clear();
    if(!value.writable){diagnostic=L"原设置文件不作自动覆盖，本次调整仅在当前运行中生效；可在状态页恢复设置保存";return false;}
    try {
        const auto serialized=SettingsPayload(value,diagnostic);if(!serialized)return false;
        return CommitSerializedFile(UserDataDirectory()/L"settings.json",L"settings-save",L"设置",*serialized,diagnostic);
    } catch(...) {diagnostic=L"设置目录或文件无法保存";return false;}
}
namespace {
// Caller owns this target file's save mutex and validated its bounded payload.
// Recovery deliberately retains every material. When an original exists,
// ReplaceFileW normally publishes the prepared file and creates its backup in
// one operation. Its documented partial-move failure is repaired below only
// with an exact, no-replace backup rename. A missing target likewise uses a
// no-replace rename of the exact open temporary.
bool RecoverSettingsFile(const wchar_t* fileName,const wchar_t* stem,const wchar_t* label,
    const std::string& serialized,std::filesystem::path& backupFile,std::wstring& diagnostic) {
    const std::wstring name=label;backupFile.clear();
    std::filesystem::path temporary,backup;
    std::wstring successDiagnostic;
    HANDLE output=INVALID_HANDLE_VALUE,original=INVALID_HANDLE_VALUE,recoveryBackup=INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION originalInformation{};
    FILE_ID_INFO originalFileId{};
    bool originalIdentityCaptured=false,originalFileIdCaptured=false;
    try{
        const auto directory=UserDataDirectory();
        detail::DirectoryTreeLock directoryLock;DWORD directoryError=ERROR_SUCCESS;
        if(!directoryLock.Acquire(directory,directoryError,true))throw std::runtime_error("directory-boundary");
        GUID guid{};wchar_t token[40]{};
        if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,token,40))throw std::runtime_error("token");
        const auto file=directory/fileName;
        temporary=directory/(std::wstring(stem)+L"-recovery-"+token+L".tmp");
        backup=directory/(std::wstring(stem)+L"-backup-"+token+L".json");
        output=CreateFileW(temporary.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
        if(output==INVALID_HANDLE_VALUE)throw std::runtime_error("create");
        DWORD written=0;
        DWORD writeError=ERROR_SUCCESS;
        if(!WriteFile(output,serialized.data(),static_cast<DWORD>(serialized.size()),&written,nullptr))writeError=GetLastError();
        else if(written!=serialized.size())writeError=ERROR_WRITE_FAULT;
        else if(!FlushFileBuffers(output))writeError=GetLastError();
        if(writeError!=ERROR_SUCCESS){
            CloseHandle(output);output=INVALID_HANDLE_VALUE;
            diagnostic=name+L"未能完整写入（Windows "+std::to_wstring(writeError)+L"），原文件未替换；临时材料已保留："+temporary.wstring();return false;
        }
        original=CreateFileW(file.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(original==INVALID_HANDLE_VALUE){
            const auto targetError=GetLastError();
            if(targetError!=ERROR_FILE_NOT_FOUND&&targetError!=ERROR_PATH_NOT_FOUND)
                throw std::runtime_error("target-open");
            successDiagnostic=name+L"保存已恢复；原文件已不存在，未覆盖其他文件";
        }else{
            if(GetFileType(original)!=FILE_TYPE_DISK||!GetFileInformationByHandle(original,&originalInformation)||
               (originalInformation.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
                throw std::runtime_error("target-kind");
            originalIdentityCaptured=true;
            originalFileIdCaptured=GetFileInformationByHandleEx(original,FileIdInfo,
                &originalFileId,sizeof(originalFileId))!=FALSE;
            successDiagnostic=name+L"保存已恢复；原文件备份："+backup.filename().wstring();
            CloseHandle(original);original=INVALID_HANDLE_VALUE;
            CloseHandle(output);output=INVALID_HANDLE_VALUE;
            if(!ReplaceFileW(file.c_str(),temporary.c_str(),backup.c_str(),0,nullptr,nullptr)){
                const auto replaceError=GetLastError();
                diagnostic=name+L"带备份恢复发布失败（Windows "+std::to_wstring(replaceError)+
                    L"），自动保存仍暂停；请保留设置目录中的现有文件及恢复材料";
                if(replaceError==ERROR_UNABLE_TO_MOVE_REPLACEMENT_2){
                    // ReplaceFileW documents this partial state explicitly: the
                    // old target has moved to lpBackupFileName while the new
                    // file remains at lpReplacementFileName. Restore only the
                    // now-empty formal name, from the exact verified backup
                    // object, and never replace a target that has reappeared.
                    const auto normalizedDirectory=directory.lexically_normal();
                    const auto normalizedBackup=backup.lexically_normal();
                    const auto targetAttributes=GetFileAttributesW(file.c_str());
                    const auto targetStatus=targetAttributes==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
                    if(normalizedBackup.parent_path()!=normalizedDirectory){
                        diagnostic+=L"；备份路径不在受锁设置目录内，未尝试恢复旧正式文件";
                    }else if(targetAttributes!=INVALID_FILE_ATTRIBUTES){
                        diagnostic+=L"；正式目标已重新出现，未覆盖该目标";
                    }else if(targetStatus!=ERROR_FILE_NOT_FOUND&&targetStatus!=ERROR_PATH_NOT_FOUND){
                        diagnostic+=L"；无法确认正式目标是否缺失（Windows "+std::to_wstring(targetStatus)+L"），未尝试覆盖";
                    }else{
                        recoveryBackup=CreateFileW(backup.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,
                            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                        if(recoveryBackup==INVALID_HANDLE_VALUE){
                            diagnostic+=L"；无法打开旧文件备份以恢复正式名（Windows "+std::to_wstring(GetLastError())+L"）";
                        }else{
                            BY_HANDLE_FILE_INFORMATION backupInformation{};
                            FILE_ID_INFO backupFileId{};
                            if(GetFileType(recoveryBackup)!=FILE_TYPE_DISK||
                               !GetFileInformationByHandle(recoveryBackup,&backupInformation)||
                               (backupInformation.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){
                                diagnostic+=L"；旧文件备份不是可恢复的本地普通文件，未移动该备份";
                            }else{
                                const bool backupFileIdCaptured=GetFileInformationByHandleEx(recoveryBackup,FileIdInfo,
                                    &backupFileId,sizeof(backupFileId))!=FALSE;
                                const bool extendedIdentityMatches=originalFileIdCaptured&&backupFileIdCaptured&&
                                    backupFileId.VolumeSerialNumber==originalFileId.VolumeSerialNumber&&
                                    std::memcmp(backupFileId.FileId.Identifier,originalFileId.FileId.Identifier,
                                        sizeof(originalFileId.FileId.Identifier))==0;
                                const bool legacyIdentityMatches=!originalFileIdCaptured&&!backupFileIdCaptured&&
                                    backupInformation.dwVolumeSerialNumber==originalInformation.dwVolumeSerialNumber&&
                                    backupInformation.nFileIndexHigh==originalInformation.nFileIndexHigh&&
                                    backupInformation.nFileIndexLow==originalInformation.nFileIndexLow;
                                if(!originalIdentityCaptured||(!extendedIdentityMatches&&!legacyIdentityMatches)){
                                    diagnostic+=L"；旧文件备份身份无法精确确认或与已验证的原文件不一致，未移动该备份";
                                }else{
                                    DWORD restoreError=ERROR_SUCCESS;
                                    if(detail::RenameOpenFile(recoveryBackup,file,false,restoreError))
                                        diagnostic+=L"；旧正式文件已从备份安全恢复，新恢复材料仍保留";
                                    else diagnostic+=L"；旧正式文件未能恢复（Windows "+std::to_wstring(restoreError)+
                                        L"），未覆盖任何已出现的目标";
                                }
                            }
                            CloseHandle(recoveryBackup);recoveryBackup=INVALID_HANDLE_VALUE;
                        }
                    }
                }
                if(GetFileAttributesW(temporary.c_str())!=INVALID_FILE_ATTRIBUTES)
                    diagnostic+=L"；恢复材料："+temporary.wstring();
                if(GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES){
                    backupFile=backup;diagnostic+=L"；原文件备份："+backup.wstring();
                }
                return false;
            }
            backupFile=backup;
            diagnostic=std::move(successDiagnostic);
            return true;
        }
        DWORD publishError=ERROR_SUCCESS;
        if(!detail::RenameOpenFile(output,file,false,publishError)){
            CloseHandle(output);output=INVALID_HANDLE_VALUE;
            diagnostic=name+L"恢复发布失败（Windows "+std::to_wstring(publishError)+
                L"），自动保存仍暂停；恢复材料已保留："+temporary.wstring();
            return false;
        }
        FlushFileBuffers(output);CloseHandle(output);output=INVALID_HANDLE_VALUE;
        diagnostic=std::move(successDiagnostic);
        return true;
    }catch(...){
        if(original!=INVALID_HANDLE_VALUE)CloseHandle(original);
        if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);
        if(recoveryBackup!=INVALID_HANDLE_VALUE)CloseHandle(recoveryBackup);
        diagnostic=name+L"暂时无法恢复，自动保存仍暂停；请保留设置目录中的原文件及恢复材料";
        if(!backup.empty()&&GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES){
            backupFile=backup;diagnostic+=L"；原文件备份："+backup.wstring();
        }
        if(!temporary.empty()&&GetFileAttributesW(temporary.c_str())!=INVALID_FILE_ATTRIBUTES)diagnostic+=L"："+temporary.wstring();
        return false;
    }
}
}
bool RecoverSettings(AppSettings& value,std::filesystem::path& backupFile,std::wstring& diagnostic) {
    std::lock_guard lock(settingsSaveMutex);diagnostic.clear();backupFile.clear();
    if(value.writable){diagnostic=L"设置当前允许保存，无需恢复";return true;}
    try{
        const auto serialized=SettingsPayload(value,diagnostic);if(!serialized)return false;
        if(!RecoverSettingsFile(L"settings.json",L"settings",L"设置",*serialized,backupFile,diagnostic))return false;
        value.writable=true;return true;
    }catch(...){diagnostic=L"设置无法准备，自动保存仍暂停，原文件未替换";return false;}
}
bool RecoverTrackLyricPreferences(TrackLyricPreferences& value,std::filesystem::path& backupFile,std::wstring& diagnostic) {
    std::lock_guard lock(trackPreferencesSaveMutex);diagnostic.clear();backupFile.clear();
    if(value.writable){diagnostic=L"歌词记忆当前允许保存，无需恢复";return true;}
    try{
        const auto serialized=TrackPreferencesPayload(value,diagnostic);if(!serialized)return false;
        if(!RecoverSettingsFile(L"track-lyrics.json",L"track-lyrics",L"歌词记忆",*serialized,backupFile,diagnostic))return false;
        value.writable=true;return true;
    }catch(...){diagnostic=L"歌词记忆无法准备，自动保存仍暂停，原文件未替换";return false;}
}
}

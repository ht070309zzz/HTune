#include "qq_identity.h"
#include <tlhelp32.h>
#include <bcrypt.h>
#include <filesystem>
#include <cstring>
#include <utility>

namespace stage {
namespace {
struct Handle {
    HANDLE value=nullptr;
    ~Handle(){Close();}
    void Close(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=nullptr;}
};
uint64_t Created(HANDLE process){
    FILETIME created{},exit{},kernel{},user{};
    return GetProcessTimes(process,&created,&exit,&kernel,&user)?
        (uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime:0;
}
bool ReadBytes(HANDLE process,uintptr_t address,void* out,size_t size){
    SIZE_T count=0;
    return address>=0x10000&&address<=UINT32_MAX&&size&&size<=UINT32_MAX-address&&
        ReadProcessMemory(process,reinterpret_cast<const void*>(address),out,size,&count)&&count==size;
}
template<class T> T At(const void* data,size_t offset){T value{};std::memcpy(&value,static_cast<const uint8_t*>(data)+offset,sizeof(value));return value;}
// Exact QQ 22.61 DLL observed on 2026-10-19. Unsupported binaries fail closed
// to SMTC/title matching; a future version needs its own layout evidence.
constexpr std::array<uint8_t,32> SupportedHash={0xd4,0x2a,0x80,0x0e,0x21,0x10,0xb2,0x7c,0x2d,0x94,0xdb,0xb1,0xd7,0x8a,0xb1,0xa9,
    0xdd,0xda,0x2b,0xbd,0xa3,0xe6,0x23,0xc5,0xee,0xbb,0x98,0x0a,0xf9,0x2f,0x9b,0x29};
bool SupportedFile(const std::filesystem::path& path,std::vector<uint8_t>& bytes){
    Handle file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{};
    if(!file.value||file.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(file.value,&info)||
       (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||
       !GetFileSizeEx(file.value,&length)||length.QuadPart<4096||length.QuadPart>64*1024*1024)return false;
    bytes.resize(static_cast<size_t>(length.QuadPart));DWORD count=0;
    if(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)||count!=bytes.size())return false;
    std::array<uint8_t,32> hash{};
    return BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,bytes.data(),static_cast<ULONG>(bytes.size()),hash.data(),static_cast<ULONG>(hash.size()))>=0&&hash==SupportedHash;
}
bool StringAt(HANDLE process,const std::array<uint8_t,116>& header,size_t offset,std::wstring& result){
    const auto length=At<uint32_t>(header.data(),offset+16),capacity=At<uint32_t>(header.data(),offset+20);
    if(length>2048||length>capacity||capacity>1024*1024||capacity<15)return false;
    std::vector<char> bytes(size_t(length)+1);
    if(capacity==15)std::memcpy(bytes.data(),header.data()+offset,length+1);
    else if(!ReadBytes(process,At<uint32_t>(header.data(),offset),bytes.data(),bytes.size()))return false;
    if(bytes[length]!=0||std::find(bytes.begin(),bytes.begin()+length,'\0')!=bytes.begin()+length)return false;
    if(!length){result.clear();return true;}
    const auto chars=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(length),nullptr,0);
    if(chars<=0||chars>1024)return false;
    result.resize(static_cast<size_t>(chars));
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(length),result.data(),chars)==chars&&
        std::none_of(result.begin(),result.end(),[](wchar_t c){return c<32||c==127;});
}
}
struct QqIdentityReader::Impl {
    Handle process;
    DWORD pid=0;uint64_t created=0;
    uintptr_t base=0,address=0;DWORD imageSize=0;
    int64_t retryAt=0;
    void Clear(){process.Close();pid=0;created=0;base=address=0;imageSize=0;retryAt=0;}
    bool Locate(){
        Handle modules{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid)};
        if(modules.value==INVALID_HANDLE_VALUE)return false;
        wchar_t image[32768]{};DWORD length=32768;
        if(!QueryFullProcessImageNameW(process.value,0,image,&length))return false;
        const std::filesystem::path executable(image);
        if(_wcsicmp(executable.filename().c_str(),L"QQMusic.exe")!=0)return false;
        MODULEENTRY32W module{};module.dwSize=sizeof(module);bool found=false;
        if(Module32FirstW(modules.value,&module))do{
            if(_wcsicmp(module.szModule,L"QQMusic.dll")==0){found=true;break;}
        }while(Module32NextW(modules.value,&module));
        if(!found||_wcsicmp(std::filesystem::path(module.szExePath).parent_path().c_str(),executable.parent_path().c_str())!=0)return false;
        std::vector<uint8_t> bytes;if(!SupportedFile(module.szExePath,bytes))return false;
        const auto pe=At<uint32_t>(bytes.data(),0x3c);
        if(pe>bytes.size()-sizeof(IMAGE_NT_HEADERS32))return false;
        const auto header=At<IMAGE_NT_HEADERS32>(bytes.data(),pe);
        if(header.Signature!=IMAGE_NT_SIGNATURE||header.FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||
           header.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||header.FileHeader.NumberOfSections>32||
           header.OptionalHeader.SizeOfImage!=module.modBaseSize)return false;
        const auto moduleBase=reinterpret_cast<uintptr_t>(module.modBaseAddr);
        if(moduleBase>UINT32_MAX||module.modBaseSize>UINT32_MAX-moduleBase)return false;
        IMAGE_NT_HEADERS32 loaded{};
        if(!ReadBytes(process.value,moduleBase+pe,&loaded,sizeof(loaded)))return false;
        // The Windows loader updates ImageBase after ASLR. Validate the actual
        // mapping, then compare all remaining header fields to the pinned file.
        if(loaded.OptionalHeader.ImageBase!=moduleBase&&loaded.OptionalHeader.ImageBase!=header.OptionalHeader.ImageBase)return false;
        loaded.OptionalHeader.ImageBase=header.OptionalHeader.ImageBase;
        if(std::memcmp(&loaded,&header,sizeof(header))!=0)return false;
        const auto sectionOffset=size_t(pe)+24+header.FileHeader.SizeOfOptionalHeader;
        const auto sectionCount=header.FileHeader.NumberOfSections;
        if(sectionOffset>bytes.size()||sectionCount>(bytes.size()-sectionOffset)/sizeof(IMAGE_SECTION_HEADER))return false;
        std::vector<IMAGE_SECTION_HEADER> sections(sectionCount);
        std::memcpy(sections.data(),bytes.data()+sectionOffset,sections.size()*sizeof(IMAGE_SECTION_HEADER));
        uintptr_t result=0;size_t matches=0;
        for(const auto& section:sections){
            if(!(section.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
            if(section.PointerToRawData>bytes.size()||section.SizeOfRawData>bytes.size()-section.PointerToRawData||
               section.VirtualAddress>module.modBaseSize||section.SizeOfRawData>module.modBaseSize-section.VirtualAddress)return false;
            for(size_t i=0;i+50<=section.SizeOfRawData;++i){
                const auto* code=bytes.data()+section.PointerToRawData+i;
                if(code[0]!=0xa2||code[5]!=0xa3||code[10]!=0xc7||code[11]!=0x05||
                   code[20]!=0xa2||code[25]!=0xa3||code[30]!=0xc7||code[31]!=0x05||code[40]!=0xa2||code[45]!=0xa3||
                   At<uint32_t>(code,16)!=15||At<uint32_t>(code,36)!=15)continue;
                std::array<uint8_t,50> live{};
                if(!ReadBytes(process.value,moduleBase+section.VirtualAddress+i,live.data(),live.size()))return false;
                // All non-address bytes must match the fingerprinted image.
                bool same=true;
                for(size_t n=0;n<live.size();++n){
                    const bool operand=(n>=1&&n<5)||(n>=6&&n<10)||(n>=12&&n<16)||(n>=21&&n<25)||
                        (n>=26&&n<30)||(n>=32&&n<36)||(n>=41&&n<45)||(n>=46&&n<50);
                    if(!operand&&code[n]!=live[n])same=false;
                }
                if(!same)return false;
                const auto target=uintptr_t(At<uint32_t>(live.data(),1));
                if(target<moduleBase||target+116>moduleBase+module.modBaseSize)continue;
                const auto fileTarget=At<uint32_t>(code,1);
                if(fileTarget<header.OptionalHeader.ImageBase||target-moduleBase!=fileTarget-header.OptionalHeader.ImageBase)continue;
                const std::array<size_t,7> offsets={6,12,21,26,32,41,46};
                const std::array<uint32_t,7> distances={40,44,24,64,68,48,88};
                for(size_t n=0;n<offsets.size();++n)if(At<uint32_t>(live.data(),offsets[n])!=target+distances[n])same=false;
                const auto dataSection=std::find_if(sections.begin(),sections.end(),[&](const auto& s){
                    const uint64_t begin=moduleBase+s.VirtualAddress,end=begin+s.Misc.VirtualSize;
                    return (s.Characteristics&IMAGE_SCN_MEM_WRITE)&&!(s.Characteristics&IMAGE_SCN_MEM_EXECUTE)&&target>=begin&&target+116<=end;
                });
                if(!same||dataSection==sections.end())continue;
                result=target;++matches;
            }
        }
        if(matches!=1)return false;
        base=moduleBase;imageSize=module.modBaseSize;address=result;return true;
    }
    std::optional<QqNativeSong> Read(DWORD requestedPid,uint64_t requestedCreated){
        if(!requestedPid||!requestedCreated){Clear();return {};}
        if(pid!=requestedPid||created!=requestedCreated){Clear();pid=requestedPid;created=requestedCreated;}
        if(MonotonicMs()<retryAt)return {};
        if(!process.value){
            process.value=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid);
            if(!process.value){retryAt=MonotonicMs()+30000;return {};}
        }
        if(WaitForSingleObject(process.value,0)!=WAIT_TIMEOUT||Created(process.value)!=created){Clear();return {};}
        if(!address&&!Locate()){retryAt=MonotonicMs()+30000;return {};}
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQueryEx(process.value,reinterpret_cast<void*>(address),&region,sizeof(region))||
           region.AllocationBase!=reinterpret_cast<void*>(base)||region.Type!=MEM_IMAGE||region.State!=MEM_COMMIT||
           (region.Protect&(PAGE_NOACCESS|PAGE_GUARD))){address=0;retryAt=MonotonicMs()+30000;return {};}
        std::array<uint8_t,116> first{},last{},final{};
        if(!ReadBytes(process.value,address,first.data(),first.size()))return {};
        QqNativeSong song;
        song.id=At<uint32_t>(first.data(),96);song.durationMs=At<uint32_t>(first.data(),104);song.positionMs=At<uint32_t>(first.data(),108);
        const auto state=At<uint32_t>(first.data(),112);
        if(!song.id||song.durationMs<=0||song.durationMs>86400000||song.positionMs>song.durationMs||state>3||state==2)return {};
        song.state=state==1?PlayState::Playing:state==0?PlayState::Paused:PlayState::Unknown;
        if(!StringAt(process.value,first,0,song.title)||!StringAt(process.value,first,24,song.artist)||
           !StringAt(process.value,first,48,song.album)||song.title.empty())return {};
        if(!ReadBytes(process.value,address,last.data(),last.size())||std::memcmp(first.data(),last.data(),108)!=0||
           At<uint32_t>(last.data(),112)!=state||std::abs(int64_t(At<uint32_t>(last.data(),108))-song.positionMs)>250)return {};
        // Long strings can mutate in place without changing their pointers.
        std::wstring title,artist,album;
        if(!StringAt(process.value,last,0,title)||!StringAt(process.value,last,24,artist)||!StringAt(process.value,last,48,album)||
           title!=song.title||artist!=song.artist||album!=song.album||
           !ReadBytes(process.value,address,final.data(),final.size())||std::memcmp(first.data(),final.data(),108)!=0||
           At<uint32_t>(final.data(),112)!=state||std::abs(int64_t(At<uint32_t>(final.data(),108))-song.positionMs)>250||
           WaitForSingleObject(process.value,0)!=WAIT_TIMEOUT||Created(process.value)!=created)return {};
        song.observedAtMs=MonotonicMs();return song;
    }
};
QqIdentityReader::QqIdentityReader():impl_(std::make_unique<Impl>()){}
QqIdentityReader::~QqIdentityReader()=default;
std::optional<QqNativeSong> QqIdentityReader::Read(DWORD pid,uint64_t created){try{return impl_->Read(pid,created);}catch(...){impl_->Clear();return {};}}
void QqIdentityReader::Reset(){impl_->Clear();}
}

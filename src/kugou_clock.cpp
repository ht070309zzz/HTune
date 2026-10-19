#include "kugou_clock.h"
#include <tlhelp32.h>
#include <bcrypt.h>
#include <filesystem>
#include <cstring>
#include <cmath>

namespace stage {
namespace {
struct Handle {HANDLE value=nullptr;~Handle(){Close();}void Close(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=nullptr;}};
uint64_t Created(HANDLE p){FILETIME c{},e{},k{},u{};return GetProcessTimes(p,&c,&e,&k,&u)?(uint64_t(c.dwHighDateTime)<<32)|c.dwLowDateTime:0;}
bool ReadBytes(HANDLE p,uintptr_t a,void* out,size_t n,uintptr_t image=0){
    if(a<0x10000||a>0x7fffffffffffULL||!n||n>0x7fffffffffffULL-a)return false;
    MEMORY_BASIC_INFORMATION m{};
    if(!VirtualQueryEx(p,reinterpret_cast<void*>(a),&m,sizeof(m))||m.State!=MEM_COMMIT||
        (m.Protect&(PAGE_GUARD|PAGE_NOACCESS))||a-reinterpret_cast<uintptr_t>(m.BaseAddress)>m.RegionSize||
        n>m.RegionSize-(a-reinterpret_cast<uintptr_t>(m.BaseAddress))||
        (image&&(m.Type!=MEM_IMAGE||reinterpret_cast<uintptr_t>(m.AllocationBase)!=image)))return false;
    SIZE_T read=0;return ReadProcessMemory(p,reinterpret_cast<void*>(a),out,n,&read)&&read==n;
}
template<class T> T At(const std::vector<uint8_t>& b,size_t o){T t{};std::memcpy(&t,b.data()+o,sizeof(t));return t;}
// Fixed layouts are used only after matching both installed binaries. Position
// layout research: AF-Media-Bar (MIT); see THIRD-PARTY-NOTICES.md. The duration
// root differs on this build and was independently located on 2026-10-19.
constexpr char KugouHash[]="070e1af169fd526374bfdf173ecd9747171861504285596eebeb5887bcf6af3e";
constexpr char PlayerHash[]="8fc4386de6ff55742dd2768c4373a0b521ad1b8056dac21154afdc63684a76ba";
bool Pin(HANDLE p,const MODULEENTRY32W& module,const char* expected,uintptr_t offset){
    Handle f{CreateFileW(module.szExePath,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{};
    if(!f.value||f.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(f.value,&info)||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||!GetFileSizeEx(f.value,&length)||
        length.QuadPart<4096||length.QuadPart>64*1024*1024)return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(length.QuadPart));DWORD count=0;
    if(!ReadFile(f.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)||count!=bytes.size())return false;
    std::array<uint8_t,32> hash{};
    if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,bytes.data(),static_cast<ULONG>(bytes.size()),hash.data(),32)<0)return false;
    constexpr char digits[]="0123456789abcdef";
    for(size_t i=0;i<hash.size();++i)if(digits[hash[i]>>4]!=expected[i*2]||digits[hash[i]&15]!=expected[i*2+1])return false;
    const auto pe=At<uint32_t>(bytes,0x3c);
    if(pe>bytes.size()-sizeof(IMAGE_NT_HEADERS64))return false;
    const auto h=At<IMAGE_NT_HEADERS64>(bytes,pe);const auto base=reinterpret_cast<uintptr_t>(module.modBaseAddr);
    if(h.Signature!=IMAGE_NT_SIGNATURE||h.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||
        h.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||h.OptionalHeader.SizeOfImage!=module.modBaseSize||
        offset>module.modBaseSize||8>module.modBaseSize-offset)return false;
    IMAGE_NT_HEADERS64 live{};
    if(!ReadBytes(p,base+pe,&live,sizeof(live),base))return false;
    if(live.OptionalHeader.ImageBase!=base&&live.OptionalHeader.ImageBase!=h.OptionalHeader.ImageBase)return false;
    live.OptionalHeader.ImageBase=h.OptionalHeader.ImageBase;
    return std::memcmp(&live,&h,sizeof(h))==0;
}
}
struct KugouClockReader::Impl {
    Handle process;DWORD pid=0;uint64_t created=0;uintptr_t kugou=0,player=0;int64_t retryAt=0;
    void Clear(){process.Close();pid=0;created=0;kugou=player=0;retryAt=0;}
    bool Locate(){
        Handle list{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid)};
        if(list.value==INVALID_HANDLE_VALUE)return false;
        wchar_t exe[32768]{};DWORD len=32768;
        if(!QueryFullProcessImageNameW(process.value,0,exe,&len))return false;
        const std::filesystem::path executable(exe);
        if(_wcsicmp(executable.filename().c_str(),L"KuGou.exe")!=0)return false;
        MODULEENTRY32W m{},a{},b{};m.dwSize=sizeof(m);unsigned na=0,nb=0;
        if(Module32FirstW(list.value,&m))do{
            if(_wcsicmp(m.szModule,L"kugou.dll")==0){a=m;++na;}
            if(_wcsicmp(m.szModule,L"kgplayer.dll")==0){b=m;++nb;}
        }while(Module32NextW(list.value,&m));
        if(na!=1||nb!=1)return false;
        const auto dir=std::filesystem::path(a.szExePath).parent_path();
        if(_wcsicmp(dir.c_str(),std::filesystem::path(b.szExePath).parent_path().c_str())!=0||
            (_wcsicmp(dir.c_str(),executable.parent_path().c_str())!=0&&
             _wcsicmp(dir.parent_path().c_str(),executable.parent_path().c_str())!=0))return false;
        if(!Pin(process.value,a,KugouHash,0x23c7b68)||!Pin(process.value,b,PlayerHash,0x3929d8))return false;
        kugou=reinterpret_cast<uintptr_t>(a.modBaseAddr);player=reinterpret_cast<uintptr_t>(b.modBaseAddr);return true;
    }
    bool Duration(uintptr_t& object,int32_t& seconds){
        uintptr_t address=kugou+0x23c7b68;
        constexpr uintptr_t offsets[]={0x70,0x30,0,0x24};
        for(size_t i=0;i<4;++i){
            uintptr_t ptr=0;if(!ReadBytes(process.value,address,&ptr,sizeof(ptr),i==0?kugou:0)||ptr<0x10000||ptr>0x7fffffffffffULL-offsets[i])return false;
            address=ptr+offsets[i];
        }
        object=address-0x24;return ReadBytes(process.value,address,&seconds,sizeof(seconds))&&seconds>0&&seconds<=86400;
    }
    std::optional<KugouClockSample> Read(DWORD id,uint64_t birth){
        if(!id||!birth){Clear();return {};}
        if(pid!=id||created!=birth){Clear();pid=id;created=birth;}
        if(MonotonicMs()<retryAt)return {};
        if(!process.value)process.value=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid);
        if(!process.value){retryAt=MonotonicMs()+30000;return {};}
        if(WaitForSingleObject(process.value,0)!=WAIT_TIMEOUT||Created(process.value)!=created){Clear();return {};}
        if((!kugou||!player)&&!Locate()){retryAt=MonotonicMs()+30000;return {};}
        const auto began=MonotonicMs();uintptr_t first=0,last=0;int32_t d1=0,d2=0;double p1=0,p2=0;
        if(!Duration(first,d1)||!ReadBytes(process.value,player+0x3929d8,&p1,sizeof(p1),player)||
            !Duration(last,d2)||!ReadBytes(process.value,player+0x3929d8,&p2,sizeof(p2),player)||
            first!=last||d1!=d2||!std::isfinite(p1)||!std::isfinite(p2)||p1<0||p2<0||
            p1>d1+2.||p2>d2+2.||std::abs(p2-p1)>.25||MonotonicMs()-began>250||
            WaitForSingleObject(process.value,0)!=WAIT_TIMEOUT||Created(process.value)!=created)return {};
        return KugouClockSample{std::min<int64_t>(std::llround(p2*1000),int64_t(d2)*1000),int64_t(d2)*1000,MonotonicMs(),last};
    }
};
KugouClockReader::KugouClockReader():impl_(std::make_unique<Impl>()){}
KugouClockReader::~KugouClockReader()=default;
std::optional<KugouClockSample> KugouClockReader::Read(DWORD pid,uint64_t created){try{return impl_->Read(pid,created);}catch(...){impl_->Clear();return {};}}
}

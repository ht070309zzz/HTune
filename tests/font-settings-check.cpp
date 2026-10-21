// Compile the product persistence with its known-folder dependency redirected
// to a fresh workspace fixture. No real profile is resolved or accessed.
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iostream>
static std::wstring fixtureBase;
HRESULT FixtureLocalAppData(REFKNOWNFOLDERID,DWORD,HANDLE,PWSTR* out){
    if(fixtureBase.empty())return E_FAIL;
    *out=static_cast<PWSTR>(CoTaskMemAlloc((fixtureBase.size()+1)*sizeof(wchar_t)));
    if(!*out)return E_OUTOFMEMORY;memcpy(*out,fixtureBase.c_str(),(fixtureBase.size()+1)*sizeof(wchar_t));return S_OK;
}
#define SHGetKnownFolderPath FixtureLocalAppData
#include "../src/settings.cpp"
#undef SHGetKnownFolderPath
int wmain(){try{
    GUID id{};if(FAILED(CoCreateGuid(&id)))return 2;wchar_t guid[40]{};StringFromGUID2(id,guid,40);
    const auto root=std::filesystem::current_path()/L"build/font-settings-checks"/guid;
    std::filesystem::create_directories(root);fixtureBase=root.wstring();
    int count=0;const auto check=[&](bool ok){if(!ok)throw std::runtime_error("Font settings persistence check failed");++count;};
    std::wstring diagnostic;stage::AppSettings s;s.theme=18;
    for(const auto key:{L"",L"@wenkai",L"@huangyou",L"@kuaile",L"Microsoft YaHei UI",L"本地字体"}){
        s.lyricFont=key;s.lyricFontPath=std::wstring(key)==L"本地字体"?(root/L"字体 空格.otf").wstring():L"";
        check(stage::SaveSettings(s,diagnostic));const auto loaded=stage::LoadSettings(diagnostic);
        check(loaded.writable&&loaded.theme==18&&loaded.lyricFont==s.lyricFont&&loaded.lyricFontPath==s.lyricFontPath);
    }
    const auto file=stage::UserDataDirectory()/L"settings.json";
    const auto content=[&](){std::ifstream in(file,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
    const auto good=content();s.lyricFont=std::wstring(257,L'x');check(!stage::SaveSettings(s,diagnostic)&&content()==good);
    for(auto body:{std::string(R"({"schema":1,"theme":18})"),std::string(R"({"schema":1,"lyricFont":42})"),std::string(R"({"schema":1,"lyricFont":"a\u0000b"})")}){
        {std::ofstream out(file,std::ios::binary|std::ios::trunc);out<<body;}
        const auto loaded=stage::LoadSettings(diagnostic);const bool legacy=body.find("theme")!=std::string::npos;
        check(legacy?(loaded.writable&&loaded.lyricFont.empty()):!loaded.writable);
        if(!legacy)check(!stage::SaveSettings(loaded,diagnostic)&&content()==body);
    }
    std::cout<<"Font settings checks passed: "<<count<<"; isolated workspace fixture only\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}

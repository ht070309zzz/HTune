#pragma once
#include "resource.h"
#include <dwrite_3.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cwctype>

namespace stage {
// Private DirectWrite collections: selecting a font never installs it globally.
// An empty choice deliberately leaves every theme's original typeface intact.
class OptionalLyricFont {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<IDWriteFactory5> factory;
    Ptr<IDWriteInMemoryFontFileLoader> loader;
    Ptr<IDWriteFontCollection1> custom;
    Ptr<IDWriteFontCollection> system;
    std::wstring key,path,family;
    DWRITE_FONT_WEIGHT weight=DWRITE_FONT_WEIGHT_NORMAL;
    bool resolved=false;
    static bool FamilyName(IDWriteFontCollection* collection,std::wstring& name,DWRITE_FONT_WEIGHT& weight) {
        Ptr<IDWriteFontFamily> f;Ptr<IDWriteLocalizedStrings> names;Ptr<IDWriteFont> font;
        if(FAILED(collection->GetFontFamily(0,&f))||FAILED(f->GetFamilyNames(&names)))return false;
        UINT32 n=0;if(FAILED(names->GetStringLength(0,&n))||n>255)return false;
        std::vector<wchar_t> text(n+1);if(FAILED(names->GetString(0,text.data(),n+1)))return false;name=text.data();
        if(SUCCEEDED(f->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STRETCH_NORMAL,DWRITE_FONT_STYLE_NORMAL,&font)))weight=font->GetWeight();
        return !name.empty();
    }
public:
    ~OptionalLyricFont(){custom.Reset();if(factory&&loader)factory->UnregisterFontFileLoader(loader.Get());}
    bool Select(IDWriteFactory* write,const std::wstring& choice,const std::wstring& file) {
        if(resolved&&key==choice&&path==file)return false;
        resolved=true;key=choice;path=file;family.clear();custom.Reset();weight=DWRITE_FONT_WEIGHT_NORMAL;
        if(choice.empty()&&file.empty())return true;
        if(!system)write->GetSystemFontCollection(&system);
        int resource=choice==L"@wenkai"?IDR_FONT_WENKAI:choice==L"@huangyou"?IDR_FONT_HUANGYOU:choice==L"@kuaile"?IDR_FONT_KUAILE:0;
        if(!resource&&file.empty()) {
            UINT32 index=0;BOOL exists=FALSE;
            if(system&&SUCCEEDED(system->FindFamilyName(choice.c_str(),&index,&exists))&&exists)family=choice;
            return true;
        }
        if(!factory&&FAILED(write->QueryInterface(IID_PPV_ARGS(&factory))))return true;
        Ptr<IDWriteFontFile> fontFile;
        if(!file.empty()) {
            // Reject network/device paths and oversized input before DirectWrite.
            const std::filesystem::path p(file);auto extension=p.extension().wstring();
            for(auto& c:extension)c=static_cast<wchar_t>(towlower(c));
            if(!p.is_absolute()||file.size()>32760||file.rfind(L"\\\\",0)==0||(extension!=L".ttf"&&extension!=L".otf"))return true;
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if(!GetFileAttributesExW(file.c_str(),GetFileExInfoStandard,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||info.nFileSizeHigh||info.nFileSizeLow>64*1024*1024||info.nFileSizeLow<12)return true;
            if(FAILED(write->CreateFontFileReference(file.c_str(),nullptr,&fontFile)))return true;
        }else{
            const auto entry=FindResourceW(nullptr,MAKEINTRESOURCEW(resource),RT_RCDATA);
            if(!entry)return true;const auto data=LockResource(LoadResource(nullptr,entry));const auto bytes=SizeofResource(nullptr,entry);
            if(!data||!bytes)return true;
            if(!loader){if(FAILED(factory->CreateInMemoryFontFileLoader(&loader))||FAILED(factory->RegisterFontFileLoader(loader.Get()))){loader.Reset();return true;}}
            if(FAILED(loader->CreateInMemoryFontFileReference(write,data,bytes,nullptr,&fontFile)))return true;
        }
        BOOL supported=FALSE;DWRITE_FONT_FILE_TYPE type{};DWRITE_FONT_FACE_TYPE face{};UINT32 faces=0;
        if(FAILED(fontFile->Analyze(&supported,&type,&face,&faces))||!supported||!faces||faces>16)return true;
        Ptr<IDWriteFontSetBuilder1> builder;Ptr<IDWriteFontSet> set;
        if(FAILED(factory->CreateFontSetBuilder(&builder))||FAILED(builder->AddFontFile(fontFile.Get()))||FAILED(builder->CreateFontSet(&set))||FAILED(factory->CreateFontCollectionFromFontSet(set.Get(),&custom)))return true;
        if(!FamilyName(custom.Get(),family,weight)){custom.Reset();family.clear();}
        return true;
    }
    bool Active()const{return !family.empty();}
    const std::wstring& Family()const{return family;}
    IDWriteFontCollection* Collection()const{return custom?custom.Get():system.Get();}
    DWRITE_FONT_WEIGHT Weight()const{return weight;}
};
inline std::wstring LyricFontLabel(const std::wstring& key,const std::wstring& file={}) {
    if(!file.empty())return key.empty()?std::filesystem::path(file).stem().wstring():key;
    return key==L"@wenkai"?L"霞鹜文楷":key==L"@huangyou"?L"站酷庆科黄油体":key==L"@kuaile"?L"站酷快乐体":key.empty()?L"主题原字体":key;
}
}

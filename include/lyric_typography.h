#pragma once
#include <dwrite.h>
#include <wrl/client.h>
#include <initializer_list>
#include <string>

namespace stage {
enum class LyricTypefaceKind { Serif, Book, Display };
inline LyricTypefaceKind ThemeTypeface(int /*style*/) {
    // Cinema is the default for every stage. Explicit DIY selections are
    // resolved before this fallback and retain their own family and weight.
    return LyricTypefaceKind::Serif;
}
struct LyricTypeface {
    std::wstring family=L"Microsoft YaHei UI";
    DWRITE_FONT_WEIGHT weight=DWRITE_FONT_WEIGHT_NORMAL;
};
// Use locally installed faces, never redistribute system font files. Resolve
// once per renderer; shaping, fallback and metrics remain DirectWrite's job.
inline LyricTypeface ResolveLyricTypeface(IDWriteFontCollection* collection,LyricTypefaceKind kind) {
    using Microsoft::WRL::ComPtr;
    LyricTypeface result;
    if(!collection)return result;
    const auto choose=[&](std::initializer_list<const wchar_t*> names,DWRITE_FONT_WEIGHT wanted) {
        for(const auto* name:names) {
            UINT32 index=0;BOOL found=FALSE;
            if(FAILED(collection->FindFamilyName(name,&index,&found))||!found)continue;
            ComPtr<IDWriteFontFamily> family;ComPtr<IDWriteFont> face;
            if(FAILED(collection->GetFontFamily(index,&family))||FAILED(family->GetFirstMatchingFont(wanted,DWRITE_FONT_STRETCH_NORMAL,DWRITE_FONT_STYLE_NORMAL,&face)))continue;
            result.family=name;result.weight=face->GetWeight();return;
        }
    };
    if(kind==LyricTypefaceKind::Book)
        choose({L"STKaiti",L"KaiTi",L"楷体",L"Noto Serif SC",L"SimSun",L"Microsoft YaHei UI"},DWRITE_FONT_WEIGHT_NORMAL);
    else if(kind==LyricTypefaceKind::Serif)
        choose({L"Noto Serif SC",L"Source Han Serif SC",L"Source Han Serif CN",L"思源宋体",L"Noto Serif CJK SC",L"SimSun",L"Microsoft YaHei UI"},DWRITE_FONT_WEIGHT_SEMI_BOLD);
    else
        choose({L"Noto Sans SC",L"Source Han Sans SC",L"Microsoft YaHei UI",L"Microsoft YaHei",L"Segoe UI"},DWRITE_FONT_WEIGHT_BOLD);
    return result;
}
}

#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace stage {
inline wchar_t CatalogLower(wchar_t c) noexcept {return c>=L'A'&&c<=L'F'?c+(L'a'-L'A'):c;}
inline bool IsKugouCatalogId(std::wstring_view key) noexcept {
    if(!key.starts_with(L"kg-")||key.size()!=35)return false;
    bool nonzero=false;
    for(auto c:key.substr(3)){
        c=CatalogLower(c);if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))return false;
        nonzero=nonzero||c!=L'0';
    }
    return nonzero;
}
// Catalog keys are not proof of the player's current identity. Existing bare
// decimal keys remain NetEase; QQ decimal IDs and Kugou hashes use distinct prefixes.
inline std::wstring_view CatalogDigits(std::wstring_view key) noexcept {
    if(key.starts_with(L"qq-"))key.remove_prefix(3);
    else if(key.starts_with(L"soda-"))key.remove_prefix(5);
    if(key.empty()||key.size()>19)return {};
    uint64_t number=0;
    for(const auto c:key){
        if(c<L'0'||c>L'9')return {};
        const auto digit=static_cast<unsigned>(c-L'0');
        if(number>(static_cast<uint64_t>(std::numeric_limits<int64_t>::max())-digit)/10)return {};
        number=number*10+digit;
    }
    if(!number)return {};
    return key.substr(key.find_first_not_of(L'0'));
}
inline std::wstring NormalizeCatalogId(std::wstring_view key) {
    if(IsKugouCatalogId(key)){std::wstring out(key);for(auto& c:out)c=CatalogLower(c);return out;}
    const auto digits=CatalogDigits(key);if(digits.empty())return {};
    return (key.starts_with(L"qq-")?L"qq-":key.starts_with(L"soda-")?L"soda-":L"")+std::wstring(digits);
}
inline bool SameCatalogId(std::wstring_view a,std::wstring_view b) noexcept {
    if(a.starts_with(L"kg-")||b.starts_with(L"kg-")){
        if(!IsKugouCatalogId(a)||!IsKugouCatalogId(b))return false;
        for(size_t i=3;i<a.size();++i)if(CatalogLower(a[i])!=CatalogLower(b[i]))return false;
        return true;
    }
    if(a.starts_with(L"qq-")!=b.starts_with(L"qq-")||a.starts_with(L"soda-")!=b.starts_with(L"soda-"))return false;
    const auto digits=CatalogDigits(a);return !digits.empty()&&digits==CatalogDigits(b);
}
inline bool IsSodaCatalogId(std::wstring_view key) noexcept {
    return key.starts_with(L"soda-")&&!CatalogDigits(key).empty();
}
inline bool IsQqCatalogId(std::wstring_view key) noexcept {
    return key.starts_with(L"qq-")&&!CatalogDigits(key).empty();
}
}

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "themes.h"
#include "file_transaction.h"
#include "local_file_limits.h"
#include "settings_writer.h"
#include "third_party/json.hpp"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "Ole32.lib")

namespace stage {
namespace {
using ThemeJson = nlohmann::json;
constexpr size_t ThemeMaxBytes = 128 * 1024;
constexpr size_t ThemeMaxName = 128;
constexpr size_t ThemeMaxPath = MaxLocalFilePathChars;
static_assert(MaxLocalFilePathUtf8Bytes + 8192 < ThemeMaxBytes,
    "A maximum-length local path plus the bounded theme fields must fit in a theme file");

void ThemeNote(std::wstring& text, const std::wstring& message) {
    if (!text.empty()) text += L"；";
    text += message;
}
std::wstring ThemeFromUtf8(const std::string& text) {
    if (text.empty() || text.size() > ThemeMaxBytes) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!length) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}
std::string ThemeToUtf8(const std::wstring& text) {
    if (text.empty() || text.size() > ThemeMaxPath) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!length) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}
bool ThemeReservedDevice(std::wstring_view component) {
    while (!component.empty() && (component.back() == L' ' || component.back() == L'.')) component.remove_suffix(1);
    component = component.substr(0, component.find(L'.'));
    while (!component.empty() && component.back() == L' ') component.remove_suffix(1);
    std::wstring name(component);
    for (auto& c : name) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
    if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL") return true;
    if (name.size() != 4 || (name.substr(0, 3) != L"COM" && name.substr(0, 3) != L"LPT")) return false;
    // Windows also recognizes superscript 1/2/3 as device-number suffixes.
    return (name[3] >= L'1' && name[3] <= L'9') || name[3] == L'\u00b9' || name[3] == L'\u00b2' || name[3] == L'\u00b3';
}
bool ThemePathSyntax(const std::filesystem::path& path, std::wstring& error) {
    const auto text = path.wstring();
    if (text.empty() || text.size() > ThemeMaxPath) { error = L"文件路径为空或超过 32700 字符"; return false; }
    if (text.starts_with(L"\\") || text.starts_with(L"/")) { error = L"不接受 UNC、设备路径或无盘符根路径"; return false; }
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (c < 32 || c == L'<' || c == L'>' || c == L'|' || c == L'"' || c == L'*' || c == L'?') {
            error = L"文件路径包含无效字符"; return false;
        }
        if (c == L':' && !(i == 1 && ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')))) {
            error = L"不接受 URL 或文件数据流路径"; return false;
        }
    }
    if (text.size() > 1 && text[1] == L':' && (text.size() < 3 || (text[2] != L'\\' && text[2] != L'/'))) {
        error = L"本地绝对路径必须包含盘符和根目录"; return false;
    }
    size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find_first_of(L"\\/", start);
        const auto component = std::wstring_view(text).substr(start, end == std::wstring::npos ? end : end - start);
        if (!component.empty() && component != L"." && component != L".." &&
            (component.back() == L' ' || component.back() == L'.')) {
            error = L"文件路径组件不能以空格或句点结尾"; return false;
        }
        if (ThemeReservedDevice(component)) { error = L"不接受 Windows 保留设备名（含扩展名或尾点空格变体）"; return false; }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (ThemeToUtf8(text).empty()) { error = L"路径不是有效 Unicode"; return false; }
    return true;
}
bool ThemeLocalPath(const std::filesystem::path& raw, const std::filesystem::path& parent,
    std::filesystem::path& resolved, std::wstring& error, bool* exists = nullptr) {
    if (!ThemePathSyntax(raw, error)) return false;
    std::error_code ec;
    resolved = raw.is_absolute() ? raw.lexically_normal() : std::filesystem::absolute(parent / raw, ec).lexically_normal();
    if (ec || !ThemePathSyntax(resolved, error)) return false;
    const auto root = resolved.root_path();
    const UINT type = GetDriveTypeW(root.c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK) {
        error = L"仅接受固定盘、可移动盘或 RAM 磁盘上的本地文件"; return false;
    }
    std::filesystem::path current;
    bool found = true;
    for (const auto& part : resolved) {
        current /= part;
        if (current == resolved.root_name()) continue;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) { found = false; break; }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { error = L"不接受链接、云占位或其他重解析材料路径"; return false; }
    }
    if (exists) *exists = found;
    return true;
}
struct ThemeFileHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~ThemeFileHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    ThemeFileHandle()=default;
    ThemeFileHandle(const ThemeFileHandle&)=delete;
    ThemeFileHandle& operator=(const ThemeFileHandle&)=delete;
};
bool ReadThemeFileBounded(const std::filesystem::path& file,std::string& text,std::wstring& error) {
    detail::DirectoryTreeLock directoryLock;DWORD directoryError=ERROR_SUCCESS;
    if(file.parent_path().empty()||!directoryLock.Acquire(file.parent_path(),directoryError)){
        error=L"主题目录不是稳定的本地普通目录（Windows "+std::to_wstring(directoryError)+L"）";return false;
    }
    ThemeFileHandle handle;
    handle.value=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(handle.value==INVALID_HANDLE_VALUE){error=L"主题文件不可读";return false;}
    BY_HANDLE_FILE_INFORMATION information{};
    if(GetFileType(handle.value)!=FILE_TYPE_DISK||!GetFileInformationByHandle(handle.value,&information)||
       (information.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){
        error=L"主题文件必须是本地普通文件";return false;
    }
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(handle.value,&size)||size.QuadPart<=0||
       size.QuadPart>static_cast<LONGLONG>(ThemeMaxBytes)){
        error=L"主题文件不可读、为空或超过 128 KiB";return false;
    }
    text.assign(static_cast<size_t>(size.QuadPart),'\0');
    size_t offset=0;
    while(offset<text.size()){
        DWORD read=0;
        const auto remaining=static_cast<DWORD>(text.size()-offset);
        if(!ReadFile(handle.value,text.data()+offset,remaining,&read,nullptr)||read==0){
            error=L"主题文件未完整读入";return false;
        }
        offset+=read;
    }
    return true;
}
const ThemeJson* ThemeField(const ThemeJson& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto item = value.find(key);
    return item == value.end() ? nullptr : &*item;
}
bool ThemeNumeric(const ThemeJson& value, float minimum, float maximum, const wchar_t* field,
    float& output, std::wstring& warnings, std::wstring& error) {
    if (!value.is_number()) { error = std::wstring(field) + L" 必须是数值"; return false; }
    const double raw = value.get<double>();
    if (!std::isfinite(raw)) { error = std::wstring(field) + L" 不接受 NaN 或无穷大"; return false; }
    const double bounded = std::clamp(raw, static_cast<double>(minimum), static_cast<double>(maximum));
    output = static_cast<float>(bounded);
    if (raw != bounded) ThemeNote(warnings, std::wstring(field) + L" 已限制到允许范围");
    return true;
}
bool ThemeColorValue(const ThemeJson& value, const wchar_t* field, ThemeColor& result,
    std::wstring& warnings, std::wstring& error) {
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text.size() != 7 || text[0] != '#') { error = std::wstring(field) + L" 必须是 #RRGGBB 或 RGB(A) 数组"; return false; }
        uint32_t color = 0;
        for (size_t i = 1; i < text.size(); ++i) {
            unsigned digit;
            if (text[i] >= '0' && text[i] <= '9') digit = text[i] - '0';
            else if (text[i] >= 'a' && text[i] <= 'f') digit = text[i] - 'a' + 10;
            else if (text[i] >= 'A' && text[i] <= 'F') digit = text[i] - 'A' + 10;
            else { error = std::wstring(field) + L" 的十六进制颜色无效"; return false; }
            color = (color << 4) | digit;
        }
        result = ThemeRgb(color);
        return true;
    }
    if (!value.is_array() || (value.size() != 3 && value.size() != 4)) {
        error = std::wstring(field) + L" 必须是 #RRGGBB 或 3/4 个分量的数组"; return false;
    }
    float components[4]{0, 0, 0, 1};
    for (size_t i = 0; i < value.size(); ++i)
        if (!ThemeNumeric(value[i], 0, 1, field, components[i], warnings, error)) return false;
    result = {components[0], components[1], components[2], components[3]};
    return true;
}
bool ThemeValidDefinition(ThemeDefinition& definition, std::wstring& warnings, std::wstring& error) {
    if (definition.baseStyle < 0 || definition.baseStyle >= BuiltInStyleCount) { error = L"baseStyle 必须是 0 到 5"; return false; }
    if(definition.lyricDisplayMode<0||definition.lyricDisplayMode>2){error=L"lyricDisplayMode 必须是 0、1 或 2";return false;}
    if (definition.name.empty() || definition.name.size() > ThemeMaxName || ThemeToUtf8(definition.name).empty()) {
        error = L"name 必须是有效且不超过 128 字符的名称"; return false;
    }
    for (wchar_t c : definition.name) if (c < 32) { error = L"name 不接受控制字符"; return false; }
    const std::pair<const wchar_t*, ThemeColor*> colors[] = {{L"background", &definition.background}, {L"panel", &definition.panel},
        {L"ink", &definition.ink}, {L"muted", &definition.muted}, {L"accent", &definition.accent}, {L"glow", &definition.glow}};
    for (auto [name, color] : colors) {
        float* components[]{&color->r, &color->g, &color->b, &color->a};
        for (auto component : components) {
            if (!std::isfinite(*component)) { error = std::wstring(name) + L" 颜色不接受 NaN 或无穷大"; return false; }
            const auto value = std::clamp(*component, 0.f, 1.f);
            if (value != *component) ThemeNote(warnings, std::wstring(name) + L" 颜色已限制到 0..1");
            *component = value;
        }
    }
    if (!std::isfinite(definition.response) || !std::isfinite(definition.lyricSize)) { error = L"response/lyricSize 必须是有限数值"; return false; }
    const float response = std::clamp(definition.response, 0.f, 2.f), size = std::clamp(definition.lyricSize, 26.f, 80.f);
    if (response != definition.response) ThemeNote(warnings, L"response 已限制到 0..2");
    if (size != definition.lyricSize) ThemeNote(warnings, L"lyricSize 已限制到 26..80");
    definition.response = response; definition.lyricSize = size;
    if(!std::isfinite(definition.backgroundShade)||!std::isfinite(definition.cameraMotion)){error=L"backgroundShade/cameraMotion 必须是有限数值";return false;}
    const float shade=std::clamp(definition.backgroundShade,0.f,1.f),camera=std::clamp(definition.cameraMotion,0.f,2.f);
    if(shade!=definition.backgroundShade)ThemeNote(warnings,L"backgroundShade 已限制到 0..1");
    if(camera!=definition.cameraMotion)ThemeNote(warnings,L"cameraMotion 已限制到 0..2");
    definition.backgroundShade=shade;definition.cameraMotion=camera;
    if (definition.backgroundKind != ThemeBackgroundKind::None && definition.backgroundKind != ThemeBackgroundKind::Image &&
        definition.backgroundKind != ThemeBackgroundKind::Video) { error = L"background.kind 无效"; return false; }
    if (definition.backgroundKind != ThemeBackgroundKind::None && !ThemePathSyntax(definition.backgroundPath, error)) {
        error = L"background.path：" + error; return false;
    }
    if (definition.backgroundKind == ThemeBackgroundKind::None && !definition.backgroundPath.empty()) {
        definition.backgroundPath.clear(); ThemeNote(warnings, L"background.kind=none，材料引用已忽略");
    }
    return true;
}
ThemeJson ThemeSerialize(const ThemeDefinition& definition) {
    auto color = [](const ThemeColor& c) { return ThemeJson::array({c.r, c.g, c.b, c.a}); };
    const char* kind = definition.backgroundKind == ThemeBackgroundKind::Image ? "image" :
        definition.backgroundKind == ThemeBackgroundKind::Video ? "video" : "none";
    return ThemeJson{{"schema", 1}, {"name", ThemeToUtf8(definition.name)}, {"baseStyle", definition.baseStyle},
        {"colors", {{"background", color(definition.background)}, {"panel", color(definition.panel)},
            {"ink", color(definition.ink)}, {"muted", color(definition.muted)},
            {"accent", color(definition.accent)}, {"glow", color(definition.glow)}}},
        {"response", definition.response}, {"lyricSize", definition.lyricSize}, {"reducedMotion", definition.reducedMotion},
        {"lyricDisplayMode",definition.lyricDisplayMode},
        {"backgroundShade",definition.backgroundShade},{"cameraMotion",definition.cameraMotion},
        {"background", {{"kind", kind}, {"path", ThemeToUtf8(definition.backgroundPath.wstring())}}}};
}
} // namespace

ThemeDefinition BuiltInTheme(int style) {
    ThemeDefinition result;
    if (style == 0) {
        result.name = L"轻光流动"; result.baseStyle = 0;
        result.background = ThemeRgb(0x111B24); result.panel = ThemeRgb(0x17232E); result.ink = ThemeRgb(0xF0F4F3);
        result.muted = ThemeRgb(0x9CAFB8); result.accent = ThemeRgb(0x90D4C7); result.glow = ThemeRgb(0x457C8C);
    } else if (style == 1) {
        result.name = L"电影剧场"; result.baseStyle = 1;
        result.background = ThemeRgb(0x151411); result.panel = ThemeRgb(0x201E19); result.ink = ThemeRgb(0xF4ECDF);
        result.muted = ThemeRgb(0xB3A68D); result.accent = ThemeRgb(0xDAB374); result.glow = ThemeRgb(0x755B35);
    } else if (style == 3) {
        result.name=L"声织";result.baseStyle=3;
        result.background=ThemeRgb(0x101112);result.panel=ThemeRgb(0x191819);result.ink=ThemeRgb(0xF6F0E8);
        result.muted=ThemeRgb(0x858181);result.accent=ThemeRgb(0xF7877E);result.glow=ThemeRgb(0x8F443D);
    } else if (style == 4) {
        result.name=L"节拍拼贴";result.baseStyle=4;
        result.background=ThemeRgb(0x073CDD);result.panel=ThemeRgb(0x101112);result.ink=ThemeRgb(0xFAFAF6);
        result.muted=ThemeRgb(0xCFDDFD);result.accent=ThemeRgb(0xDDFF00);result.glow=ThemeRgb(0x1234AB);
    } else if (style == 5) {
        result.name=L"回声长卷";result.baseStyle=5;
        result.background=ThemeRgb(0xFCFDFC);result.panel=ThemeRgb(0xEFF3F0);result.ink=ThemeRgb(0x153E30);
        result.muted=ThemeRgb(0x8DA699);result.accent=ThemeRgb(0x28664D);result.glow=ThemeRgb(0xCBDCD1);
    }
    return result;
}

ThemeResult ParseThemeJson(std::string_view utf8) {
    ThemeResult result;
    if (utf8.empty() || utf8.size() > ThemeMaxBytes) { result.diagnostic = L"主题为空或超过 128 KiB 上限"; return result; }
    try {
        auto check = [](int depth, ThemeJson::parse_event_t, ThemeJson&) {
            if (depth > 16) throw std::runtime_error("theme depth limit");
            return true;
        };
        const auto document = ThemeJson::parse(utf8, check);
        const auto* schema = ThemeField(document, "schema");
        if (!document.is_object() || !schema || !schema->is_number_integer() || schema->get<int64_t>() != 1) {
            result.diagnostic = L"主题必须是 schema=1 的 JSON 对象"; return result;
        }
        int style = 2;
        if (const auto* field = ThemeField(document, "baseStyle")) {
            if (!field->is_number_integer() || field->get<int64_t>() < 0 || field->get<int64_t>() >= BuiltInStyleCount) {
                result.diagnostic = L"baseStyle 必须是 0 到 5"; return result;
            }
            style = field->get<int>();
        }
        result.definition = BuiltInTheme(style);
        if (const auto* field = ThemeField(document, "name")) {
            if (!field->is_string()) { result.diagnostic = L"name 必须是字符串"; return result; }
            result.definition.name = ThemeFromUtf8(field->get<std::string>());
        }
        std::wstring warnings, error;
        if (const auto* colors = ThemeField(document, "colors")) {
            if (!colors->is_object()) { result.diagnostic = L"colors 必须是对象"; return result; }
            const std::pair<const char*, ThemeColor*> fields[] = {{"background", &result.definition.background}, {"panel", &result.definition.panel},
                {"ink", &result.definition.ink}, {"muted", &result.definition.muted}, {"accent", &result.definition.accent}, {"glow", &result.definition.glow}};
            for (auto [name, destination] : fields) if (const auto* field = ThemeField(*colors, name))
                if (!ThemeColorValue(*field, std::wstring(name, name + strlen(name)).c_str(), *destination, warnings, error)) {
                    result.diagnostic = error; return result;
                }
        }
        if (const auto* field = ThemeField(document, "response"))
            if (!ThemeNumeric(*field, 0, 2, L"response", result.definition.response, warnings, error)) { result.diagnostic = error; return result; }
        if (const auto* field = ThemeField(document, "lyricSize"))
            if (!ThemeNumeric(*field, 26, 80, L"lyricSize", result.definition.lyricSize, warnings, error)) { result.diagnostic = error; return result; }
        if (const auto* field = ThemeField(document, "reducedMotion")) {
            if (!field->is_boolean()) { result.diagnostic = L"reducedMotion 必须是布尔值"; return result; }
            result.definition.reducedMotion = field->get<bool>();
        }
        if(const auto* field=ThemeField(document,"lyricDisplayMode")){
            if(!field->is_number_integer()||(*field<0)||(*field>2)){result.diagnostic=L"lyricDisplayMode 必须是 0、1 或 2";return result;}
            result.definition.lyricDisplayMode=field->get<int>();
        }
        if(const auto* field=ThemeField(document,"backgroundShade"))
            if(!ThemeNumeric(*field,0,1,L"backgroundShade",result.definition.backgroundShade,warnings,error)){result.diagnostic=error;return result;}
        if(const auto* field=ThemeField(document,"cameraMotion"))
            if(!ThemeNumeric(*field,0,2,L"cameraMotion",result.definition.cameraMotion,warnings,error)){result.diagnostic=error;return result;}
        if (const auto* background = ThemeField(document, "background")) {
            if (!background->is_object()) { result.diagnostic = L"background 必须是对象"; return result; }
            if (const auto* field = ThemeField(*background, "kind")) {
                if (!field->is_string()) { result.diagnostic = L"background.kind 必须是字符串"; return result; }
                const auto value = field->get<std::string>();
                if (value == "none") result.definition.backgroundKind = ThemeBackgroundKind::None;
                else if (value == "image") result.definition.backgroundKind = ThemeBackgroundKind::Image;
                else if (value == "video") result.definition.backgroundKind = ThemeBackgroundKind::Video;
                else { result.diagnostic = L"background.kind 必须是 none、image 或 video"; return result; }
            }
            if (const auto* field = ThemeField(*background, "path")) {
                if (!field->is_string()) { result.diagnostic = L"background.path 必须是字符串"; return result; }
                result.definition.backgroundPath = ThemeFromUtf8(field->get<std::string>());
            }
        }
        if (!ThemeValidDefinition(result.definition, warnings, error)) { result.diagnostic = error; return result; }
        const std::set<std::string> known{"schema", "name", "baseStyle", "colors", "response", "lyricSize", "reducedMotion", "lyricDisplayMode", "backgroundShade", "cameraMotion", "background"};
        for (const auto& item : document.items()) if (!known.contains(item.key())) ThemeNote(warnings, L"忽略未知主题字段：" + ThemeFromUtf8(item.key()));
        result.diagnostic = warnings.empty() ? L"主题数据已解析" : warnings;
        result.ok = true;
    } catch (...) { result.diagnostic = L"主题 JSON 编码、数值或结构无效（深度上限 16）"; }
    return result;
}

ThemeResult LoadTheme(const std::filesystem::path& selectedFile) {
    ThemeResult result;
    try {
        std::filesystem::path file;
        std::wstring error;
        if (!ThemeLocalPath(selectedFile, std::filesystem::current_path(), file, error)) { result.diagnostic = error; return result; }
        std::string text;
        if(!ReadThemeFileBounded(file,text,error)){result.diagnostic=error;return result;}
        result = ParseThemeJson(text);
        if (!result.ok || result.definition.backgroundKind == ThemeBackgroundKind::None) return result;
        std::filesystem::path material;
        bool exists = false;
        if (!ThemeLocalPath(result.definition.backgroundPath, file.parent_path(), material, error, &exists)) {
            result.ok = false; result.diagnostic = L"background.path：" + error; return result;
        }
        result.definition.backgroundPath = material;
        if (!exists) ThemeNote(result.diagnostic, L"background.path 指定的本地材料不存在");
        return result;
    } catch (...) { result.ok = false; result.diagnostic = L"主题文件或材料路径无效"; return result; }
}

namespace {
ThemeResult SaveThemeFile(const std::filesystem::path& selectedFile, const ThemeDefinition& definition,
    bool replaceExisting) {
    ThemeResult result;
    result.definition = definition;
    std::filesystem::path temporary;
    HANDLE handle=INVALID_HANDLE_VALUE;
    bool committed=false;
    try {
        std::wstring warnings, error;
        std::filesystem::path file;
        if (!ThemeLocalPath(selectedFile, std::filesystem::current_path(), file, error) ||
            !ThemeValidDefinition(result.definition, warnings, error)) { result.diagnostic = error; return result; }
        if (file.filename().empty() || std::filesystem::is_directory(file)) { result.diagnostic = L"必须选择主题文件，不能选择目录"; return result; }
        if (!std::filesystem::is_directory(file.parent_path())) { result.diagnostic = L"所选主题文件的目录必须已经存在"; return result; }
        detail::DirectoryTreeLock directoryLock;DWORD directoryError=ERROR_SUCCESS;
        if(!directoryLock.Acquire(file.parent_path(),directoryError)){
            result.diagnostic=L"主题目录不是稳定的本地普通目录（Windows "+
                std::to_wstring(directoryError)+L"）";return result;
        }
        if(replaceExisting){
            detail::DirectoryTreeLock privateDirectoryLock;DWORD privateDirectoryError=ERROR_SUCCESS;
            const auto privateDirectory=(UserDataDirectory()/L"themes").lexically_normal();
            if(privateDirectoryLock.Acquire(privateDirectory,privateDirectoryError)){
                const auto comparison=detail::CompareFileObjects(directoryLock.FinalHandle(),privateDirectoryLock.FinalHandle());
                if(comparison==detail::FileObjectComparison::Same){
                    result.diagnostic=L"应用私有主题快照由设置事务管理，请选择其他目录导出";return result;
                }
                if(comparison==detail::FileObjectComparison::Unavailable){
                    result.diagnostic=L"暂时无法核验主题导出目录身份，请稍后重试";return result;
                }
            }else if(privateDirectoryError!=ERROR_FILE_NOT_FOUND&&privateDirectoryError!=ERROR_PATH_NOT_FOUND){
                result.diagnostic=L"暂时无法核验应用私有主题目录，请稍后重试（Windows "+
                    std::to_wstring(privateDirectoryError)+L"）";return result;
            }
        }
        if (result.definition.backgroundKind != ThemeBackgroundKind::None) {
            std::filesystem::path material;
            bool exists = false;
            if (!ThemeLocalPath(result.definition.backgroundPath, file.parent_path(), material, error, &exists)) {
                result.diagnostic = L"background.path：" + error; return result;
            }
            result.definition.backgroundPath = material;
            if (!exists) ThemeNote(warnings, L"background.path 指定的本地材料不存在");
        }
        auto exported = result.definition;
        if (!exported.backgroundPath.empty()) {
            const auto relative = exported.backgroundPath.lexically_relative(file.parent_path());
            if (!relative.empty() && !relative.is_absolute() && *relative.begin() != L"..") exported.backgroundPath = relative;
        }
        const auto json = ThemeSerialize(exported).dump(2) + "\n";
        if (json.size() > ThemeMaxBytes) { result.diagnostic = L"导出主题超过 128 KiB"; return result; }
        auto successDiagnostic = warnings.empty() ? L"主题已原子保存到所选文件" : warnings;
        GUID guid{}; wchar_t token[40]{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, token, 40)) { result.diagnostic = L"无法生成主题临时文件"; return result; }
        temporary = file;
        temporary += std::wstring(L".") + token + L".tmp";
        handle = CreateFileW(temporary.c_str(), GENERIC_WRITE|DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH, nullptr);
        if (handle == INVALID_HANDLE_VALUE) { result.diagnostic = L"不能创建所选目录中的主题临时文件"; return result; }
        DWORD written = 0;
        DWORD operationError = ERROR_SUCCESS;
        bool complete = false;
        if (!WriteFile(handle, json.data(), static_cast<DWORD>(json.size()), &written, nullptr)) operationError = GetLastError();
        else if (written != static_cast<DWORD>(json.size())) operationError = ERROR_WRITE_FAULT;
        else if (!FlushFileBuffers(handle)) operationError = GetLastError();
        else complete = true;
        if (complete) {
            const auto attributes = GetFileAttributesW(file.c_str());
            bool replaceTarget=false;
            if (attributes != INVALID_FILE_ATTRIBUTES) {
                if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
                    operationError = ERROR_INVALID_NAME; complete = false;
                } else if (!replaceExisting) { operationError = ERROR_FILE_EXISTS; complete = false; }
                else replaceTarget=true;
            } else {
                const auto statusError = GetLastError();
                if (statusError != ERROR_FILE_NOT_FOUND && statusError != ERROR_PATH_NOT_FOUND) {
                    operationError = statusError; complete = false;
                }
            }
            if(complete&&!detail::RenameOpenFile(handle,file,replaceTarget,operationError))complete=false;
        }
        if (!complete) {
            detail::DeleteOpenFile(handle);CloseHandle(handle);handle=INVALID_HANDLE_VALUE;
            result.diagnostic = L"主题原子写入失败（Windows " + std::to_wstring(operationError) + L"），未直接截断所选文件"; return result;
        }
        committed=true;FlushFileBuffers(handle);CloseHandle(handle);handle=INVALID_HANDLE_VALUE;
        result.ok = true;
        result.diagnostic = std::move(successDiagnostic);
        return result;
    } catch (...) {
        if(handle!=INVALID_HANDLE_VALUE){if(!committed)detail::DeleteOpenFile(handle);CloseHandle(handle);}
        result.diagnostic = L"主题写入失败，原始主题未被直接覆盖"; return result;
    }
}
}

ThemeResult SaveTheme(const std::filesystem::path& selectedFile, const ThemeDefinition& definition) {
    return SaveThemeFile(selectedFile, definition, true);
}

ThemeResult SaveThemeNewFile(const std::filesystem::path& selectedFile, const ThemeDefinition& definition) {
    return SaveThemeFile(selectedFile, definition, false);
}
} // namespace stage

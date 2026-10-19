#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "file_transaction.h"
#include "local_file_limits.h"
#include "lyrics.h"
#include "catalog_id.h"
#include "qq_lyrics.h"
#include "kugou_lyrics.h"
#include "soda_lyrics.h"
#include "native_http.h"
#include "settings.h"
#include "third_party/json.hpp"
#include <winhttp.h>
#include <bcrypt.h>
#include <objbase.h>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <limits>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Normaliz.lib")

namespace stage {
namespace {
using Json = nlohmann::json;
constexpr size_t MaxBytes = 2 * 1024 * 1024;
constexpr size_t MaxCacheBytes = 4 * 1024 * 1024;
constexpr uint64_t MaxTimelineCacheDirectoryBytes = 64ull * 1024 * 1024;
constexpr size_t MaxTimelineCacheEntries = 256;
constexpr size_t MaxTimelineCacheScanEntries = 1024;
constexpr size_t MaxTimelineCacheEvictions = 32;
constexpr size_t TimelineCacheVersionSlots = 8;
constexpr int64_t MaxTime = 24 * 60 * 60 * 1000;
constexpr size_t MaxLines = 20000;
constexpr size_t MaxLineText = 8192;
constexpr size_t MaxWordText = 2048;
constexpr size_t MaxWordsPerLine = 8192;
constexpr size_t MaxSupplementText = 4096;
constexpr size_t MaxTimestampsPerLine = 64;
constexpr size_t MaxYrcMetadataItems = 128;

bool AddTextUnits(size_t& total,size_t units) {
    if(total>MaxBytes||units>MaxBytes-total)return false;
    total+=units;
    return true;
}

std::wstring Trim(std::wstring_view value) {
    const size_t begin = value.find_first_not_of(L" \t\r\n\ufeff");
    if (begin == std::wstring_view::npos) return {};
    const size_t end = value.find_last_not_of(L" \t\r\n");
    return std::wstring(value.substr(begin, end - begin + 1));
}

bool Unsigned(std::wstring_view text, int64_t& value, int64_t maximum = MaxTime) {
    if (text.empty() || text.size() > 20) return false;
    value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return false;
        const int digit = character - L'0';
        if (value > (maximum - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return value <= maximum;
}

std::wstring NormalizeMatchText(const std::wstring& text) {
    if (text.empty() || text.size() > static_cast<size_t>(INT_MAX) ||
        text.find(L'\0') != std::wstring::npos) return {};
    const int length = static_cast<int>(text.size());
    int required = NormalizeString(NormalizationKC, text.data(), length, nullptr, 0);
    if (required <= 0) return {};
    std::wstring normalized;
    bool success = false;
    // NormalizeString's initial size is an estimate. If its real result needs
    // more room, the documented negative result gives the next size estimate.
    for (int attempt = 0; attempt < 3; ++attempt) {
        normalized.assign(static_cast<size_t>(required), L'\0');
        const int written = NormalizeString(NormalizationKC, text.data(), length, normalized.data(), required);
        if (written > 0) { normalized.resize(static_cast<size_t>(written)); success = true; break; }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || written >= 0 || written == INT_MIN) return {};
        required = -written;
    }
    if (!success) return {};
    const int mappedSize = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
        normalized.data(), static_cast<int>(normalized.size()), nullptr, 0, nullptr, nullptr, 0);
    if (mappedSize <= 0) return {};
    std::wstring mapped(static_cast<size_t>(mappedSize), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, normalized.data(),
        static_cast<int>(normalized.size()), mapped.data(), mappedSize, nullptr, nullptr, 0) != mappedSize) return {};
    std::vector<WORD> types(mapped.size());
    if (!GetStringTypeW(CT_CTYPE1, mapped.data(), mappedSize, types.data())) return {};
    std::wstring result;
    bool pendingSpace = false;
    for (size_t index = 0; index < mapped.size(); ++index) {
        if (types[index] & C1_SPACE) { pendingSpace = !result.empty(); continue; }
        if (pendingSpace) { result += L' '; pendingSpace = false; }
        result += mapped[index];
    }
    return result;
}

bool Timestamp(std::wstring_view text, int64_t& result) {
    const size_t colon = text.find(L':');
    if (colon == std::wstring_view::npos) return false;
    int64_t minutes = 0, seconds = 0, fraction = 0;
    if (!Unsigned(text.substr(0, colon), minutes, 1440)) return false;
    text.remove_prefix(colon + 1);
    const size_t point = text.find_first_of(L".:");
    if (!Unsigned(text.substr(0, point), seconds, 59)) return false;
    if (point != std::wstring_view::npos) {
        const auto digits = text.substr(point + 1);
        if (digits.empty() || digits.size() > 3 || !Unsigned(digits, fraction, 999)) return false;
        if (digits.size() == 1) fraction *= 100;
        if (digits.size() == 2) fraction *= 10;
    }
    result = minutes * 60000 + seconds * 1000 + fraction;
    return result <= MaxTime;
}

template<class Function> bool EachLine(std::wstring_view text, Function function) {
    size_t count = 0;
    while (!text.empty()) {
        if (count++ >= MaxLines) return false;
        const size_t end = text.find(L'\n');
        auto line = text.substr(0, end);
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        function(line);
        if (end == std::wstring_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return true;
}

bool HasReadableLyrics(const LyricTimeline& timeline) {
    return std::any_of(timeline.lines.begin(),timeline.lines.end(),[](const auto& line){
        return line.text.find_first_not_of(L" \t\r\n\ufeff")!=std::wstring::npos;
    });
}
bool IsEmptyLyricNotice(const LyricTimeline& timeline) {
    if(timeline.lines.size()!=1)return false;
    const auto& line=timeline.lines.front();
    return line.startMs==0&&line.words.empty()&&Trim(line.text)==L"暂无歌词";
}
bool HasReusableLyricText(const LyricTimeline& timeline) {
    // Old schema-1 files lost the response flags. Preserve such files, but
    // require another online observation rather than reviving the notice.
    return HasReadableLyrics(timeline)&&(!IsEmptyLyricNotice(timeline)||timeline.emptyNoticeValidated);
}
bool HasUsableWordTiming(const LyricLine& line) {
    bool animatedText=false;int64_t previous=line.startMs;
    for(const auto& word:line.words){
        if(word.startMs<previous||word.startMs<0||word.startMs>MaxTime||word.durationMs<0||
            word.durationMs>600000||word.durationMs>MaxTime-word.startMs)return false;
        previous=word.startMs;
        if(word.durationMs>0&&word.text.find_first_not_of(L" \t\r\n\ufeff")!=std::wstring::npos)animatedText=true;
    }
    return animatedText;
}

void Finalize(LyricTimeline& timeline) {
    bool lineFallback=false;
    std::stable_sort(timeline.lines.begin(), timeline.lines.end(), [](const auto& a, const auto& b) {
        return a.startMs < b.startMs;
    });
    // Multiple translations or repeated timestamps share the same next distinct
    // timestamp. The final line has an unknown end and remains visible for 6 s.
    for (size_t begin = 0; begin < timeline.lines.size();) {
        size_t next = begin + 1;
        while (next < timeline.lines.size() && timeline.lines[next].startMs == timeline.lines[begin].startMs) ++next;
        const int64_t end = next < timeline.lines.size() ? timeline.lines[next].startMs : timeline.lines[begin].startMs + 6000;
        for (size_t index = begin; index < next; ++index) {
            auto& line = timeline.lines[index];
            // Keep the original text and line clock. Zero-time punctuation in
            // an otherwise usable row is valid; a row with no animated text
            // must not advertise a continuous word highlight or invent time.
            if(!line.words.empty()&&!HasUsableWordTiming(line)){line.words.clear();lineFallback=true;}
            if (line.durationMs <= 0) line.durationMs = std::max<int64_t>(0, end - line.startMs);
        }
        begin = next;
    }
    timeline.wordTimed = std::any_of(timeline.lines.begin(), timeline.lines.end(), [](const auto& line) {
        return !line.words.empty();
    });
    timeline.diagnostic = timeline.lines.empty() ? L"没有可用的时间标记歌词" :
        (timeline.wordTimed ? L"逐字时间轴" : L"逐句时间轴");
    if(lineFallback&&HasReadableLyrics(timeline))timeline.diagnostic+=L" · 不可用词时标已按句显示";
    timeline.state = timeline.lines.empty() ? LyricLoadState::NoLyrics : LyricLoadState::Ready;
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (!size) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(std::string_view text) {
    if (text.empty() || text.size() > MaxBytes) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring DecodeFile(std::string_view bytes, std::wstring& error) {
    size_t start = 0;
    bool utf16 = false, bigEndian = false;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        utf16 = true; start = 2;
    } else if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF) {
        utf16 = true; bigEndian = true; start = 2;
    } else if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF") start = 3;
    else if (bytes.size() >= 4 && bytes[0] && bytes[1] == 0 && bytes[2] && bytes[3] == 0) utf16 = true;
    else if (bytes.size() >= 4 && bytes[0] == 0 && bytes[1] && bytes[2] == 0 && bytes[3]) { utf16 = true; bigEndian = true; }
    if (!utf16) {
        auto result = FromUtf8(bytes.substr(start));
        if (!bytes.substr(start).empty() && result.empty()) error = L"歌词文件不是有效的 UTF-8 或 UTF-16";
        if (result.find(L'\0') != std::wstring::npos) { result.clear(); error = L"歌词文件包含空字符"; }
        return result;
    }
    if ((bytes.size() - start) % 2) { error = L"UTF-16 文件长度不完整"; return {}; }
    std::wstring result;
    result.reserve((bytes.size() - start) / 2);
    for (size_t index = start; index < bytes.size(); index += 2) {
        const unsigned first = static_cast<unsigned char>(bytes[index]);
        const unsigned second = static_cast<unsigned char>(bytes[index + 1]);
        const wchar_t code = static_cast<wchar_t>(bigEndian ? first * 256 + second : second * 256 + first);
        if (!code) { error = L"歌词文件包含空字符"; return {}; }
        result += code;
    }
    for (size_t index = 0; index < result.size(); ++index) {
        if (result[index] >= 0xD800 && result[index] <= 0xDBFF) {
            if (++index == result.size() || result[index] < 0xDC00 || result[index] > 0xDFFF) {
                error = L"UTF-16 代理字符不完整"; return {};
            }
        } else if (result[index] >= 0xDC00 && result[index] <= 0xDFFF) { error = L"UTF-16 代理字符无效"; return {}; }
    }
    return result;
}

std::string Escape(std::string_view bytes) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char byte : bytes) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~') output += byte;
        else { output += '%'; output += hex[byte >> 4]; output += hex[byte & 15]; }
    }
    return output;
}

std::string Http(const wchar_t* host, const std::wstring& path, const std::string& body,
    std::wstring& error, std::stop_token stop) {
    constexpr wchar_t headers[] = L"Referer: https://music.163.com/\r\nAccept: application/json\r\nContent-Type: application/x-www-form-urlencoded\r\n";
    const auto result = NativeHttps({host, path, headers, body, MaxBytes, 15000}, stop);
    if (!result.ok()) { error = result.diagnostic; return {}; }
    std::string response(result.bytes.begin(), result.bytes.end());
    if (response.empty()) error = L"歌词服务返回空响应";
    return response;
}

Json ReadJson(const std::string& text, std::wstring& error) {
    if (text.empty()) return {};
    try {
        auto limitDepth = [](int depth, Json::parse_event_t, Json&) {
            if (depth > 32) throw std::runtime_error("JSON depth limit");
            return true;
        };
        Json result = Json::parse(text, limitDepth);
        if (!result.is_object()) { error = L"歌词服务返回非对象 JSON"; return {}; }
        return result;
    } catch (...) { error = L"歌词服务返回无效或过度嵌套的 JSON"; return {}; }
}

const Json* Field(const Json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

std::wstring Text(const Json& object, const char* key, size_t max = 256) {
    const auto* value = Field(object, key);
    if (!value || !value->is_string()) return {};
    const auto& bytes = value->get_ref<const std::string&>();
    if (bytes.size() > MaxBytes) return {};
    auto result = FromUtf8(bytes);
    if (result.size() > max) result.resize(max);
    return result;
}

int64_t Number(const Json& object, const char* key) {
    const auto* value = Field(object, key);
    if (!value) return 0;
    if (value->is_number_unsigned()) {
        const auto number = value->get<uint64_t>();
        return number <= INT64_MAX ? static_cast<int64_t>(number) : 0;
    }
    if (value->is_number_integer()) return value->get<int64_t>();
    if (value->is_string()) {
        const auto bytes = value->get_ref<const std::string&>();
        int64_t number = 0;
        if (Unsigned(FromUtf8(bytes), number, INT64_MAX)) return number;
    }
    return 0;
}

std::wstring LyricText(const Json& document, const char* key) {
    const auto* entry = Field(document, key);
    return entry ? Text(*entry, "lyric", MaxBytes) : std::wstring{};
}
struct MainLyricChannel {
    std::wstring text;
    bool recognized=false,invalid=false;
};
MainLyricChannel ReadMainLyricChannel(const Json& document,const char* key) {
    MainLyricChannel result;
    const auto* entry=Field(document,key);
    if(!entry||entry->is_null())return result;
    const auto* lyric=Field(*entry,"lyric");
    if(!entry->is_object()||!lyric||!lyric->is_string()){result.invalid=true;return result;}
    const auto& bytes=lyric->get_ref<const std::string&>();
    if(bytes.size()>MaxBytes||bytes.find('\0')!=std::string::npos){result.invalid=true;return result;}
    result.text=FromUtf8(bytes);
    if(!bytes.empty()&&result.text.empty()){result.invalid=true;return result;}
    result.recognized=true;return result;
}
bool ExplicitNoLyric(const Json& document) {
    for(const auto* key:{"nolyric","uncollected"}){
        const auto* flag=Field(document,key);
        if(flag&&flag->is_boolean()&&flag->get<bool>())return true;
    }
    return false;
}

std::string Hex(const BYTE* bytes, size_t size, bool uppercase) {
    const char* table = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string output;
    output.reserve(size * 2);
    for (size_t index = 0; index < size; ++index) { output += table[bytes[index] >> 4]; output += table[bytes[index] & 15]; }
    return output;
}

// Native Windows CNG implementation of the documented eapi wire protocol.
// No third-party cryptographic implementation or JavaScript runtime is used.
std::string Eapi(const std::string& path, const Json& data, std::wstring& error) {
    const std::string payload = data.dump();
    const std::string signInput = "nobody" + path + "use" + payload + "md5forencrypt";
    BCRYPT_ALG_HANDLE md5 = nullptr, aes = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    struct Cleanup {
        BCRYPT_ALG_HANDLE& md5; BCRYPT_ALG_HANDLE& aes; BCRYPT_HASH_HANDLE& hash; BCRYPT_KEY_HANDLE& key;
        ~Cleanup() {
            if (key) BCryptDestroyKey(key);
            if (hash) BCryptDestroyHash(hash);
            if (aes) BCryptCloseAlgorithmProvider(aes, 0);
            if (md5) BCryptCloseAlgorithmProvider(md5, 0);
        }
    } cleanup{md5, aes, hash, key};
    auto failed = [&](NTSTATUS status) {
        if (status >= 0) return false;
        error = L"Windows CNG 歌词请求签名失败";
        return true;
    };
    if (failed(BCryptOpenAlgorithmProvider(&md5, BCRYPT_MD5_ALGORITHM, nullptr, 0))) return {};
    if (failed(BCryptCreateHash(md5, &hash, nullptr, 0, nullptr, 0, 0))) return {};
    if (failed(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(signInput.data())), static_cast<ULONG>(signInput.size()), 0))) return {};
    BYTE digest[16]{};
    if (failed(BCryptFinishHash(hash, digest, sizeof(digest), 0))) return {};
    std::string plain = path + "-36cd479b6b5-" + payload + "-36cd479b6b5-" + Hex(digest, sizeof(digest), false);
    if (failed(BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0))) return {};
    if (failed(BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
        sizeof(BCRYPT_CHAIN_MODE_ECB), 0))) return {};
    BYTE protocolKey[] = {'e','8','2','c','k','e','n','h','8','d','i','c','h','e','n','8'};
    if (failed(BCryptGenerateSymmetricKey(aes, &key, nullptr, 0, protocolKey, sizeof(protocolKey), 0))) return {};
    ULONG length = 0;
    if (failed(BCryptEncrypt(key, reinterpret_cast<PUCHAR>(plain.data()), static_cast<ULONG>(plain.size()), nullptr,
        nullptr, 0, nullptr, 0, &length, BCRYPT_BLOCK_PADDING))) return {};
    std::vector<BYTE> encrypted(length);
    if (failed(BCryptEncrypt(key, reinterpret_cast<PUCHAR>(plain.data()), static_cast<ULONG>(plain.size()), nullptr,
        nullptr, 0, encrypted.data(), length, &length, BCRYPT_BLOCK_PADDING))) return {};
    return "params=" + Hex(encrypted.data(), length, true);
}

void AttachSupplement(LyricTimeline& base,const LyricTimeline& supplement,bool romanization) {
    if(base.lines.empty()||supplement.lines.empty())return;
    constexpr int64_t matchWindowMs=120;
    size_t totalUnits=0;
    for(const auto& line:base.lines){
        if(!AddTextUnits(totalUnits,line.text.size())||!AddTextUnits(totalUnits,line.translation.size())||
            !AddTextUnits(totalUnits,line.romanization.size()))return;
        for(const auto& word:line.words)if(!AddTextUnits(totalUnits,word.text.size()))return;
    }
    struct Match {size_t first=0,end=0;int64_t distance=INT64_MAX;bool ambiguous=false;};
    std::vector<Match> matches(base.lines.size());
    const auto before=[](const LyricLine& row,int64_t time){return row.startMs<time;};
    // First assign each auxiliary timestamp to one original timestamp group.
    // Reversing this relation per row could reuse one translation on two lines.
    for(size_t first=0;first<supplement.lines.size();){
        const auto groupFirst=first;
        const auto stamp=supplement.lines[first].startMs;
        size_t end=first+1;
        while(end<supplement.lines.size()&&supplement.lines[end].startMs==stamp)++end;
        first=end;
        const auto next=std::lower_bound(base.lines.begin(),base.lines.end(),stamp,before);
        auto owner=next;
        int64_t distance=owner==base.lines.end()?INT64_MAX:std::abs(owner->startMs-stamp);
        if(next!=base.lines.begin()){
            const auto previous=std::prev(next);
            const auto previousDistance=std::abs(previous->startMs-stamp);
            if(previousDistance<distance){owner=previous;distance=previousDistance;}
            else if(previousDistance==distance&&owner!=base.lines.end()&&previous->startMs!=owner->startMs)continue;
        }
        if(owner==base.lines.end()||distance>matchWindowMs)continue;
        const auto ownerFirst=std::lower_bound(base.lines.begin(),base.lines.end(),owner->startMs,before);
        auto& match=matches[static_cast<size_t>(ownerFirst-base.lines.begin())];
        if(distance<match.distance)match={groupFirst,end,distance,false};
        else if(distance==match.distance)match.ambiguous=true;
    }
    for(size_t first=0;first<base.lines.size();){
        const auto groupFirst=first;
        const auto stamp=base.lines[first].startMs;
        size_t end=first+1;
        while(end<base.lines.size()&&base.lines[end].startMs==stamp)++end;
        first=end;
        const auto& match=matches[groupFirst];
        if(match.ambiguous||match.distance>matchWindowMs)continue;
        std::optional<std::set<std::wstring>> uniqueTexts;
        std::vector<const std::wstring*> pieces;
        std::wstring singleText;
        const bool singleRow=match.end-match.first==1;
        bool prepared=false;
        for(size_t index=groupFirst;index<end;++index){
            auto& line=base.lines[index];
            auto& destination=romanization?line.romanization:line.translation;
            // Preferred nonempty text wins; the other channel only fills gaps.
            if(line.text.empty()||!destination.empty())continue;
            const size_t availableUnits=MaxBytes-totalUnits;
            if(!availableUnits)return;
            if(!prepared){
                if(singleRow)singleText=Trim(supplement.lines[match.first].text);
                else{
                    // Set nodes own the strings and stay at fixed addresses
                    // while inserting. Never erase or replace this owner.
                    // Single-row groups leave the ownership set unconstructed.
                    auto& owned=uniqueTexts.emplace();
                    for(size_t row=match.first;row<match.end;++row){
                        auto text=Trim(supplement.lines[row].text);
                        if(text.empty())continue;
                        const auto [entry,inserted]=owned.insert(std::move(text));
                        if(inserted)pieces.push_back(&*entry);
                    }
                }
                prepared=true;
            }
            const size_t pieceCount=singleRow?(singleText.empty()?size_t{0}:size_t{1}):pieces.size();
            if(!pieceCount)break;
            std::wstring value;
            bool fitsBudget=true;
            for(size_t piece=0;piece<pieceCount;++piece){
                const auto& text=singleRow?singleText:*pieces[piece];
                if(text==line.text||text==value)continue;
                const size_t separator=value.empty()?0:3;
                if(text.size()>MaxSupplementText||separator>MaxSupplementText-value.size()||
                    text.size()>MaxSupplementText-value.size()-separator)break;
                // Apply the per-line cap first: it can leave a valid prefix.
                // If the next allowed addition exceeds the total budget,
                // later appends cannot make this row fit, so abandon it now.
                if(value.size()>availableUnits||separator>availableUnits-value.size()||
                    text.size()>availableUnits-value.size()-separator){fitsBudget=false;break;}
                if(!value.empty())value+=L" / ";value+=text;
            }
            if(!fitsBudget||value.empty()||value.size()>MaxSupplementText||destination.size()>totalUnits||
                value.size()>MaxBytes-(totalUnits-destination.size()))continue;
            totalUnits=totalUnits-destination.size()+value.size();
            destination=std::move(value);
        }
    }
}
LyricTimeline ReadTimeline(const Json& document) {
    LyricTimeline timeline;
    std::wstring parseFailure;
    const auto yrc=ReadMainLyricChannel(document,"yrc"),lrc=ReadMainLyricChannel(document,"lrc");
    if (!yrc.invalid&&!yrc.text.empty()) {
        timeline = ParseYrc(yrc.text);
        if(timeline.state==LyricLoadState::Failed)parseFailure=timeline.diagnostic;
    }
    const bool yrcFailed=yrc.invalid||(!Trim(yrc.text).empty()&&(!timeline.wordTimed||!HasReadableLyrics(timeline)));
    if((!timeline.wordTimed||!HasReadableLyrics(timeline))&&!lrc.invalid){
        auto fallback=ParseLrc(lrc.text);
        if(fallback.state==LyricLoadState::Failed&&!fallback.diagnostic.empty())parseFailure=fallback.diagnostic;
        if(HasReadableLyrics(fallback)||!HasReadableLyrics(timeline))timeline=std::move(fallback);
    }
    if(!HasReadableLyrics(timeline)){
        timeline.lines.clear();timeline.wordTimed=false;
        const bool nonempty=!Trim(yrc.text).empty()||!Trim(lrc.text).empty();
        if(yrc.invalid||lrc.invalid){
            timeline.state=LyricLoadState::Failed;timeline.diagnostic=L"歌词原文通道类型、文本或长度无效";
        }else if(!parseFailure.empty()){
            timeline.state=LyricLoadState::Failed;timeline.diagnostic=std::move(parseFailure);
        }else if(nonempty){
            timeline.state=LyricLoadState::Failed;timeline.diagnostic=L"歌词响应包含内容，但未能解析出有效时间标记";
        }else if(yrc.recognized||lrc.recognized||ExplicitNoLyric(document)){
            timeline.state=LyricLoadState::NoLyrics;timeline.diagnostic=L"歌词服务返回空歌词结果";
        }else{
            timeline.state=LyricLoadState::Failed;timeline.diagnostic=L"歌词响应缺少可识别的原文通道或明确缺词标记";
        }
        return timeline;
    }
    if(IsEmptyLyricNotice(timeline)){
        if(ExplicitNoLyric(document)){
            timeline.lines.clear();timeline.wordTimed=false;timeline.retryWordTiming=false;
            // A notice is not readable fallback content for a broken original
            // channel. Keep malformed/failed YRC as a failure, not confirmed
            // missing lyrics, so existing recovery policy remains available.
            const bool brokenOriginal=yrc.invalid||lrc.invalid||yrcFailed||!parseFailure.empty();
            timeline.state=brokenOriginal?LyricLoadState::Failed:LyricLoadState::NoLyrics;
            timeline.diagnostic=brokenOriginal?L"缺词占位同时伴随无效原文通道，尚不能确认没有歌词":L"歌词服务明确标记暂无歌词";
            return timeline;
        }
        // The same words are allowed as genuine lyrics when the service does
        // not mark them as a notice. Local import parsing remains unchanged.
        timeline.emptyNoticeValidated=true;
    }
    timeline.retryWordTiming=!timeline.wordTimed&&yrcFailed;
    const auto attachChannel=[&](const char* preferred,const char* fallback,bool romanization){
        AttachSupplement(timeline,ParseLrc(LyricText(document,preferred)),romanization);
        const bool missing=std::any_of(timeline.lines.begin(),timeline.lines.end(),[romanization](const LyricLine& line){
            return !line.text.empty()&&(romanization?line.romanization.empty():line.translation.empty());
        });
        if(missing)AttachSupplement(timeline,ParseLrc(LyricText(document,fallback)),romanization);
    };
    attachChannel(timeline.wordTimed?"ytlrc":"tlyric",timeline.wordTimed?"tlyric":"ytlrc",false);
    attachChannel(timeline.wordTimed?"yromalrc":"romalrc",timeline.wordTimed?"romalrc":"yromalrc",true);
    return timeline;
}

std::optional<std::filesystem::path> TimelineCacheDirectory(bool create) {
    const auto root=UserDataDirectory();
    if(root.empty()||!root.is_absolute())return {};
    for(const auto& path:{root,root/L"lyrics"}){
        if(create){std::error_code ec;std::filesystem::create_directory(path,ec);}
        const auto attributes=GetFileAttributesW(path.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return {};
    }
    return root/L"lyrics";
}
std::filesystem::path TimelineCachePath(const std::filesystem::path& directory,const std::wstring& songId,
    bool words=false,size_t slot=0) {
    auto name=songId+(words?L".words":L"");
    if(slot)name+=L".cache-"+std::to_wstring(slot);
    return directory/(name+L".json");
}
struct TimelineCacheHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit TimelineCacheHandle(HANDLE handle):value(handle){}
    ~TimelineCacheHandle(){Close();}
    TimelineCacheHandle(const TimelineCacheHandle&)=delete;
    TimelineCacheHandle& operator=(const TimelineCacheHandle&)=delete;
    TimelineCacheHandle(TimelineCacheHandle&& other) noexcept:value(other.value){other.value=INVALID_HANDLE_VALUE;}
    TimelineCacheHandle& operator=(TimelineCacheHandle&& other) noexcept {
        if(this!=&other){Close();value=other.value;other.value=INVALID_HANDLE_VALUE;}return *this;
    }
    void Close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
};

bool TimelineCachePathNameExact(const std::filesystem::path& path) {
    const auto expected=path.filename().wstring();
    if(expected.empty())return false;
    WIN32_FIND_DATAW data{};
    const auto search=FindFirstFileW(path.c_str(),&data);
    if(search==INVALID_HANDLE_VALUE)return false;
    const bool exact=expected==data.cFileName;
    FindClose(search);
    return exact;
}

uint64_t TimelineCacheModifiedAt(const BY_HANDLE_FILE_INFORMATION& information) {
    return (static_cast<uint64_t>(information.ftLastWriteTime.dwHighDateTime)<<32)|
        information.ftLastWriteTime.dwLowDateTime;
}

bool SameFileSnapshot(const BY_HANDLE_FILE_INFORMATION& before,const LARGE_INTEGER& beforeSize,
    const BY_HANDLE_FILE_INFORMATION& after,const LARGE_INTEGER& afterSize) {
    return beforeSize.QuadPart==afterSize.QuadPart&&
        before.dwFileAttributes==after.dwFileAttributes&&
        before.dwVolumeSerialNumber==after.dwVolumeSerialNumber&&
        before.nFileIndexHigh==after.nFileIndexHigh&&before.nFileIndexLow==after.nFileIndexLow&&
        before.nNumberOfLinks==after.nNumberOfLinks&&
        CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime)==0;
}

bool ReadLocalLyricFile(const std::filesystem::path& requested,std::filesystem::path& openedPath,
    std::string& bytes,std::wstring& error,std::stop_token stop) {
    if(stop.stop_requested())return false;
    std::error_code pathError;
    const auto path=std::filesystem::absolute(requested,pathError).lexically_normal();
    if(pathError||path.empty()||!path.is_absolute()||path.native().size()>MaxLocalFilePathChars||
        path.filename().empty()){
        error=L"所选歌词文件路径无效";return false;
    }
    detail::DirectoryTreeLock directoryLock;DWORD directoryError=ERROR_SUCCESS;
    if(!directoryLock.Acquire(path.parent_path(),directoryError)){
        error=L"歌词文件必须位于本地普通目录";return false;
    }
    if(stop.stop_requested())return false;
    TimelineCacheHandle input(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_OPEN_NO_RECALL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(input.value==INVALID_HANDLE_VALUE){error=L"无法打开所选歌词文件";return false;}
    BY_HANDLE_FILE_INFORMATION before{},after{};LARGE_INTEGER size{},afterSize{};
    if(GetFileType(input.value)!=FILE_TYPE_DISK||!GetFileInformationByHandle(input.value,&before)||
        (before.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
        !GetFileSizeEx(input.value,&size)||size.QuadPart<0){
        error=L"所选歌词文件不是本地普通文件";return false;
    }
    if(size.QuadPart>static_cast<int64_t>(MaxBytes)){error=L"歌词文件超过 2 MiB 上限";return false;}
    bytes.assign(static_cast<size_t>(size.QuadPart),'\0');
    DWORD read=0;
    if(!bytes.empty()&&(!ReadFile(input.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||
        read!=bytes.size())){
        bytes.clear();error=L"歌词文件未完整读入";return false;
    }
    if(!GetFileInformationByHandle(input.value,&after)||!GetFileSizeEx(input.value,&afterSize)||
        !SameFileSnapshot(before,size,after,afterSize)){
        bytes.clear();error=L"歌词文件读取期间发生变化，请重新选择";return false;
    }
    if(stop.stop_requested()){bytes.clear();return false;}
    openedPath=path;return true;
}

std::optional<int64_t> CacheNumber(const Json& object, const char* key, int64_t maximum) {
    const auto* field = Field(object, key);
    if (!field || !field->is_number_integer()) return std::nullopt;
    try {
        if (field->is_number_unsigned() && field->get<uint64_t>() > static_cast<uint64_t>(maximum)) return std::nullopt;
        const auto value = field->get<int64_t>();
        return value >= 0 && value <= maximum ? std::optional<int64_t>(value) : std::nullopt;
    } catch (...) { return std::nullopt; }
}
std::optional<std::wstring> CacheText(const Json& object, const char* key, size_t maximum) {
    const auto* field = Field(object, key);
    if (!field || !field->is_string()) return std::nullopt;
    const auto& encoded = field->get_ref<const std::string&>();
    auto value = FromUtf8(encoded);
    if ((!encoded.empty() && value.empty()) || value.size() > maximum || value.find(L'\0') != std::wstring::npos)
        return std::nullopt;
    return value;
}
bool ValidCachedTimeline(const LyricTimeline& timeline) {
    if (timeline.lines.empty() || timeline.lines.size() > MaxLines) return false;
    size_t units = 0;
    int64_t previousLine = -1;
    for (const auto& line : timeline.lines) {
        if (line.startMs < previousLine || line.startMs < 0 || line.startMs > MaxTime ||
            line.durationMs < 0 || line.durationMs > MaxTime || line.text.size() > MaxLineText ||
            line.text.find(L'\0') != std::wstring::npos || line.words.size() > MaxWordsPerLine) return false;
        previousLine = line.startMs;
        if(line.translation.size()>MaxSupplementText||line.romanization.size()>MaxSupplementText||line.translation.find(L'\0')!=std::wstring::npos||
            line.romanization.find(L'\0')!=std::wstring::npos)return false;
        if(!AddTextUnits(units,line.text.size())||!AddTextUnits(units,line.translation.size())||
            !AddTextUnits(units,line.romanization.size()))return false;
        int64_t previousWord = line.startMs;
        std::wstring reconstructed;
        for (const auto& word : line.words) {
            if (word.startMs < previousWord || word.startMs > MaxTime || word.durationMs < 0 ||
                word.durationMs > 600000 || word.durationMs > MaxTime - word.startMs ||
                word.text.size() > MaxWordText || word.text.find(L'\0') != std::wstring::npos) return false;
            previousWord = word.startMs;
            if(!AddTextUnits(units,word.text.size()))return false;
            reconstructed += word.text;
        }
        if (!line.words.empty() && reconstructed != line.text) return false;
    }
    return true;
}

std::optional<LyricTimeline> LoadTimelineCacheHandle(const std::wstring& songId,HANDLE input,
    bool networkFailed,uint64_t* modifiedAt=nullptr,std::string* rawBytes=nullptr) {
    try {
        if(modifiedAt)*modifiedAt=0;
        BY_HANDLE_FILE_INFORMATION before{},after{};LARGE_INTEGER size{},afterSize{},beginning{};
        if(!input||input==INVALID_HANDLE_VALUE||GetFileType(input)!=FILE_TYPE_DISK||
            !GetFileInformationByHandle(input,&before)||before.nNumberOfLinks!=1||
            (before.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
            !GetFileSizeEx(input,&size)||size.QuadPart<=0||size.QuadPart>static_cast<int64_t>(MaxCacheBytes)||
            !SetFilePointerEx(input,beginning,nullptr,FILE_BEGIN))return {};
        std::string encoded(static_cast<size_t>(size.QuadPart),'\0');DWORD read=0;
        if(!ReadFile(input,encoded.data(),static_cast<DWORD>(encoded.size()),&read,nullptr)||read!=encoded.size()||
            !GetFileInformationByHandle(input,&after)||!GetFileSizeEx(input,&afterSize)||after.nNumberOfLinks!=1||
            !SameFileSnapshot(before,size,after,afterSize))return {};
        Json data = Json::parse(encoded, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 20) throw std::runtime_error("cache depth");
            return true;
        }, false);
        if (data.is_discarded() || !data.is_object() || CacheNumber(data, "schema", 1) != 1 ||
            CacheText(data, "songId", 32) != songId) return std::nullopt;
        const auto* rows = Field(data, "lines");
        if (!rows || !rows->is_array() || rows->empty() || rows->size() > MaxLines) return std::nullopt;
        LyricTimeline timeline;
        timeline.songId = songId;
        size_t textUnits = 0;
        for (const auto& row : *rows) {
            if (!row.is_object()) return std::nullopt;
            LyricLine line;
            const auto start = CacheNumber(row, "startMs", MaxTime), duration = CacheNumber(row, "durationMs", MaxTime);
            auto text = CacheText(row, "text", MaxLineText);
            if (!start || !duration || !text) return std::nullopt;
            line.startMs = *start;
            line.durationMs = *duration;
            line.text = std::move(*text);
            if(Field(row,"translation")){
                auto value=CacheText(row,"translation",MaxSupplementText);if(!value)return std::nullopt;line.translation=std::move(*value);
            }
            if(Field(row,"romanization")){
                auto value=CacheText(row,"romanization",MaxSupplementText);if(!value)return std::nullopt;line.romanization=std::move(*value);
            }
            if(!AddTextUnits(textUnits,line.text.size())||!AddTextUnits(textUnits,line.translation.size())||
                !AddTextUnits(textUnits,line.romanization.size()))return std::nullopt;
            if (const auto* words = Field(row, "words")) {
                if (!words->is_array() || words->size() > MaxWordsPerLine) return std::nullopt;
                for (const auto& entry : *words) {
                    if (!entry.is_object()) return std::nullopt;
                    LyricWord word;
                    const auto wordStart = CacheNumber(entry, "startMs", MaxTime), wordDuration = CacheNumber(entry, "durationMs", 600000);
                    auto wordText = CacheText(entry, "text", MaxWordText);
                    if (!wordStart || !wordDuration || !wordText) return std::nullopt;
                    word.startMs = *wordStart;
                    word.durationMs = *wordDuration;
                    word.text = std::move(*wordText);
                    if(!AddTextUnits(textUnits,word.text.size()))return std::nullopt;
                    line.words.push_back(std::move(word));
                }
            }
            timeline.lines.push_back(std::move(line));
        }
        if (!ValidCachedTimeline(timeline)||!HasReadableLyrics(timeline)) return std::nullopt;
        Finalize(timeline);
        const auto* noticeChecked=Field(data,"emptyNoticeValidated");
        timeline.emptyNoticeValidated=noticeChecked&&noticeChecked->is_boolean()&&noticeChecked->get<bool>();
        // This reader also establishes ownership for quota/transactions.
        // Structurally valid old notices remain known files even though the
        // playback-facing candidate selection must not consume their text.
        if (timeline.lines.empty()) return std::nullopt;
        timeline.fromCache = true;
        timeline.source = timeline.wordTimed ? L"本地歌词缓存 · 逐字" : L"本地歌词缓存 · 逐句";
        timeline.diagnostic = std::wstring(networkFailed ? L"在线歌词请求失败" : L"在线接口暂未返回歌词") +
            (timeline.wordTimed ? L"，已使用本机逐字歌词缓存" : L"，已使用本机逐句歌词缓存");
        if(modifiedAt)*modifiedAt=TimelineCacheModifiedAt(before);
        if(rawBytes)*rawBytes=std::move(encoded);
        return timeline;
    } catch (...) { return std::nullopt; }
}

std::optional<LyricTimeline> LoadTimelineCacheFile(const std::wstring& songId,const std::filesystem::path& file,
    bool networkFailed,uint64_t* modifiedAt=nullptr,std::string* rawBytes=nullptr) {
    TimelineCacheHandle input(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(input.value==INVALID_HANDLE_VALUE||!TimelineCachePathNameExact(file))return {};
    return LoadTimelineCacheHandle(songId,input.value,networkFailed,modifiedAt,rawBytes);
}

std::optional<LyricTimeline> LoadLatestTimelineCacheVariant(const std::filesystem::path& directory,
    const std::wstring& songId,bool wordFile,bool networkFailed,uint64_t& latestAt,
    std::stop_token stop={}) {
    std::optional<LyricTimeline> result;
    latestAt=0;
    for(size_t slot=0;slot<=TimelineCacheVersionSlots;++slot){
        if(stop.stop_requested())return {};
        uint64_t modifiedAt=0;
        auto candidate=LoadTimelineCacheFile(songId,TimelineCachePath(directory,songId,wordFile,slot),
            networkFailed,&modifiedAt);
        if(candidate&&!HasReusableLyricText(*candidate))continue;
        if(candidate&&wordFile&&!candidate->wordTimed)continue;
        if(candidate&&(!result||modifiedAt>=latestAt)){result=std::move(candidate);latestAt=modifiedAt;}
    }
    return result;
}

std::optional<LyricTimeline> LoadTimelineCache(const std::wstring& songId,bool networkFailed=true,
    std::stop_token stop={}) {
    try{
        if(stop.stop_requested()||NormalizeCatalogId(songId)!=songId||songId.empty())return {};
        const auto directory=TimelineCacheDirectory(false);if(!directory)return {};
        detail::DirectoryTreeLock directoryLock;DWORD directoryError=ERROR_SUCCESS;
        if(!directoryLock.Acquire(*directory,directoryError)||stop.stop_requested())return {};
        uint64_t mainAt=0,wordsAt=0;
        auto main=LoadLatestTimelineCacheVariant(*directory,songId,false,networkFailed,mainAt,stop);
        if(stop.stop_requested())return {};
        auto words=LoadLatestTimelineCacheVariant(*directory,songId,true,networkFailed,wordsAt,stop);
        if(stop.stop_requested())return {};
        // File timestamps choose among cached versions only. They never enter
        // PlaybackSnapshot, lyric timestamps, offsets or a playback clock.
        if(words&&words->wordTimed&&(!main||!main->wordTimed||wordsAt>mainAt))return words;
        return main?std::move(main):std::move(words);
    }catch(...){return {};}
}
std::string TimelineCacheUtf8(const std::wstring& text) {
    auto encoded=ToUtf8(text);
    if(!text.empty()&&encoded.empty())throw std::runtime_error("Invalid cache text encoding");
    return encoded;
}
std::string SerializeTimelineCache(const LyricTimeline& timeline) {
    Json rows=Json::array();
    for(const auto& line:timeline.lines){
        Json words=Json::array();
        for(const auto& word:line.words)
            words.push_back({{"startMs",word.startMs},{"durationMs",word.durationMs},{"text",TimelineCacheUtf8(word.text)}});
        rows.push_back({{"startMs",line.startMs},{"durationMs",line.durationMs},{"text",TimelineCacheUtf8(line.text)},
            {"words",std::move(words)},{"translation",TimelineCacheUtf8(line.translation)},{"romanization",TimelineCacheUtf8(line.romanization)}});
    }
    const Json data{{"schema",1},{"songId",ToUtf8(timeline.songId)},{"lines",std::move(rows)},
        {"emptyNoticeValidated",timeline.emptyNoticeValidated}};
    auto encoded=data.dump();if(encoded.empty()||encoded.size()>MaxCacheBytes)return {};
    return encoded;
}

struct TimelineCacheFileName {
    std::wstring songId;
    bool words=false;
    size_t slot=0;
};
std::optional<TimelineCacheFileName> ParseTimelineCacheFileName(std::wstring name) {
    constexpr std::wstring_view extension=L".json",wordsSuffix=L".words",slotMarker=L".cache-";
    const auto original=name;
    if(!name.ends_with(extension))return {};
    name.resize(name.size()-extension.size());
    TimelineCacheFileName parsed;
    const auto marker=name.rfind(slotMarker);
    if(marker!=std::wstring::npos){
        int64_t slot=0;
        const auto value=std::wstring_view(name).substr(marker+slotMarker.size());
        if(!Unsigned(value,slot,static_cast<int64_t>(TimelineCacheVersionSlots))||slot<=0)return {};
        parsed.slot=static_cast<size_t>(slot);name.resize(marker);
    }
    if(name.ends_with(wordsSuffix)){parsed.words=true;name.resize(name.size()-wordsSuffix.size());}
    if(name.empty()||NormalizeCatalogId(name)!=name)return {};
    parsed.songId=std::move(name);
    auto canonical=parsed.songId+(parsed.words?std::wstring(wordsSuffix):std::wstring{});
    if(parsed.slot)canonical+=std::wstring(slotMarker)+std::to_wstring(parsed.slot);
    canonical+=extension;
    if(canonical!=original)return {};
    return parsed;
}

bool EnumerateTimelineCacheNames(const std::filesystem::path& directory,std::vector<std::wstring>& names) {
    names.clear();
    WIN32_FIND_DATAW data{};
    const auto wildcard=directory/L"*";
    HANDLE search=FindFirstFileW(wildcard.c_str(),&data);
    if(search==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    bool complete=true;
    do{
        const std::wstring name=data.cFileName;
        if(name==L"."||name==L"..")continue;
        if(names.size()>=MaxTimelineCacheScanEntries){complete=false;break;}
        names.push_back(name);
    }while(FindNextFileW(search,&data));
    if(complete&&GetLastError()!=ERROR_NO_MORE_FILES)complete=false;
    FindClose(search);
    if(!complete){names.clear();return false;}
    std::sort(names.begin(),names.end());
    return true;
}

bool MeasureTimelineCacheDirectory(const std::filesystem::path& directory,size_t& entryCount,uint64_t& totalBytes) {
    std::vector<std::wstring> names,verifiedNames;
    if(!EnumerateTimelineCacheNames(directory,names))return false;
    std::vector<TimelineCacheHandle> handles;
    std::vector<BY_HANDLE_FILE_INFORMATION> snapshots;
    std::vector<LARGE_INTEGER> sizes;
    handles.reserve(names.size());totalBytes=0;
    snapshots.reserve(names.size());sizes.reserve(names.size());
    for(const auto& name:names){
        const auto path=directory/name;
        TimelineCacheHandle handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION information{};LARGE_INTEGER size{};
        if(handle.value==INVALID_HANDLE_VALUE||GetFileType(handle.value)!=FILE_TYPE_DISK||
            !GetFileInformationByHandle(handle.value,&information)||
            (information.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
            !GetFileSizeEx(handle.value,&size)||size.QuadPart<0||
            static_cast<uint64_t>(size.QuadPart)>std::numeric_limits<uint64_t>::max()-totalBytes)return false;
        totalBytes+=static_cast<uint64_t>(size.QuadPart);
        snapshots.push_back(information);sizes.push_back(size);handles.push_back(std::move(handle));
    }
    if(!EnumerateTimelineCacheNames(directory,verifiedNames)||verifiedNames!=names)return false;
    for(size_t index=0;index<names.size();++index){
        const auto path=directory/names[index];
        TimelineCacheHandle verification(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION current{};LARGE_INTEGER currentSize{};
        if(verification.value==INVALID_HANDLE_VALUE||
            detail::CompareFileObjects(handles[index].value,verification.value)!=detail::FileObjectComparison::Same||
            !GetFileInformationByHandle(verification.value,&current)||!GetFileSizeEx(verification.value,&currentSize)||
            !SameFileSnapshot(snapshots[index],sizes[index],current,currentSize))return false;
    }
    entryCount=names.size();return true;
}

struct TimelineCacheScanEntry {
    std::filesystem::path path;
    TimelineCacheHandle handle{INVALID_HANDLE_VALUE};
    uint64_t bytes=0;
    uint64_t modifiedAt=0;
    BY_HANDLE_FILE_INFORMATION information{};
    std::string rawBytes;
    std::optional<TimelineCacheFileName> name;
    bool valid=false;
    bool deletable=false;
};

bool MakeRoomForTimelineCache(const std::filesystem::path& directory,const std::wstring& protectedSongId,
    size_t incomingBytes) {
    std::vector<std::wstring> names,verifiedNames;
    if(!EnumerateTimelineCacheNames(directory,names))return false;
    std::vector<TimelineCacheScanEntry> entries;
    entries.reserve(names.size());
    uint64_t totalBytes=0;
    for(const auto& name:names){
        TimelineCacheScanEntry entry;
        entry.path=directory/name;
        entry.handle=TimelineCacheHandle(CreateFileW(entry.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(entry.handle.value==INVALID_HANDLE_VALUE)return false;
        BY_HANDLE_FILE_INFORMATION information{};LARGE_INTEGER size{};
        if(GetFileType(entry.handle.value)!=FILE_TYPE_DISK||
            !GetFileInformationByHandle(entry.handle.value,&information)||
            (information.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
            !GetFileSizeEx(entry.handle.value,&size)||size.QuadPart<0)return false;
        entry.bytes=static_cast<uint64_t>(size.QuadPart);
        entry.modifiedAt=TimelineCacheModifiedAt(information);
        entry.information=information;
        entry.deletable=(information.dwFileAttributes&FILE_ATTRIBUTE_READONLY)==0;
        entry.name=ParseTimelineCacheFileName(name);
        if(entry.name&&entry.bytes>0&&entry.bytes<=MaxCacheBytes){
            uint64_t parsedAt=0;
            const auto timeline=LoadTimelineCacheHandle(entry.name->songId,entry.handle.value,false,
                &parsedAt,&entry.rawBytes);
            if(timeline&&(parsedAt!=entry.modifiedAt||entry.rawBytes.size()!=entry.bytes))return false;
            entry.valid=timeline&&(!entry.name->words||timeline->wordTimed);
            if(timeline&&!HasReusableLyricText(*timeline))entry.deletable=false;
            if(!entry.valid)entry.rawBytes.clear();
        }
        // Unknown names and malformed or mistyped cache records are retained as
        // evidence, but they also make ownership and quota recovery ambiguous.
        // Refuse this write even while the directory is below budget instead of
        // continuing to add owned records beside an object we cannot classify.
        if(!entry.valid)return false;
        BY_HANDLE_FILE_INFORMATION after{};LARGE_INTEGER afterSize{};
        if(!GetFileInformationByHandle(entry.handle.value,&after)||!GetFileSizeEx(entry.handle.value,&afterSize)||
            !SameFileSnapshot(information,size,after,afterSize)||
            entry.bytes>std::numeric_limits<uint64_t>::max()-totalBytes)return false;
        totalBytes+=entry.bytes;
        entries.push_back(std::move(entry));
    }
    if(!EnumerateTimelineCacheNames(directory,verifiedNames)||verifiedNames!=names)return false;
    for(size_t index=0;index<entries.size();++index){
        TimelineCacheHandle verification(CreateFileW(entries[index].path.c_str(),GENERIC_READ,
            FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(verification.value==INVALID_HANDLE_VALUE||
            detail::CompareFileObjects(entries[index].handle.value,verification.value)!=detail::FileObjectComparison::Same)return false;
    }
    if(incomingBytes>MaxTimelineCacheDirectoryBytes)return false;
    size_t removed=0;
    auto overBudget=[&]{
        return entries.size()-removed+1>MaxTimelineCacheEntries||
            totalBytes>MaxTimelineCacheDirectoryBytes-incomingBytes;
    };
    if(!overBudget())return true;
    std::vector<size_t> candidates;
    for(size_t index=0;index<entries.size();++index){
        const auto& entry=entries[index];
        if(entry.valid&&entry.deletable&&entry.name&&entry.name->songId!=protectedSongId)candidates.push_back(index);
    }
    std::sort(candidates.begin(),candidates.end(),[&](size_t left,size_t right){
        if(entries[left].modifiedAt!=entries[right].modifiedAt)
            return entries[left].modifiedAt<entries[right].modifiedAt;
        return entries[left].path.wstring()<entries[right].path.wstring();
    });
    std::vector<size_t> selected;
    uint64_t projectedBytes=totalBytes;
    size_t projectedEntries=entries.size();
    for(const auto index:candidates){
        if(projectedEntries+1<=MaxTimelineCacheEntries&&
            projectedBytes<=MaxTimelineCacheDirectoryBytes-incomingBytes)break;
        if(selected.size()>=MaxTimelineCacheEvictions)return false;
        selected.push_back(index);
        projectedBytes-=entries[index].bytes;
        --projectedEntries;
    }
    if(projectedEntries+1>MaxTimelineCacheEntries||
        projectedBytes>MaxTimelineCacheDirectoryBytes-incomingBytes)return false;
    struct PendingDelete {size_t index=0;TimelineCacheHandle handle{INVALID_HANDLE_VALUE};};
    std::vector<PendingDelete> pendingDeletes;
    pendingDeletes.reserve(selected.size());
    for(const auto index:selected){
        auto& entry=entries[index];
        TimelineCacheHandle deletion(CreateFileW(entry.path.c_str(),GENERIC_READ|DELETE,
            FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        uint64_t currentAt=0;std::string currentBytes;
        const auto current=deletion.value==INVALID_HANDLE_VALUE?std::optional<LyricTimeline>{}:
            LoadTimelineCacheHandle(entry.name->songId,deletion.value,false,&currentAt,&currentBytes);
        BY_HANDLE_FILE_INFORMATION currentInformation{};LARGE_INTEGER currentSize{},originalSize{};
        originalSize.QuadPart=static_cast<LONGLONG>(entry.bytes);
        if(deletion.value==INVALID_HANDLE_VALUE||
            !TimelineCachePathNameExact(entry.path)||
            detail::CompareFileObjects(entry.handle.value,deletion.value)!=detail::FileObjectComparison::Same||
            !current||(entry.name->words&&!current->wordTimed)||currentAt!=entry.modifiedAt||
            currentBytes!=entry.rawBytes||!GetFileInformationByHandle(deletion.value,&currentInformation)||
            !GetFileSizeEx(deletion.value,&currentSize)||
            !SameFileSnapshot(entry.information,originalSize,currentInformation,currentSize)||
            (currentInformation.dwFileAttributes&FILE_ATTRIBUTE_READONLY))return false;
        pendingDeletes.push_back({index,std::move(deletion)});
    }
    for(auto& pending:pendingDeletes){
        auto& entry=entries[pending.index];
        if(!detail::DeleteOpenFile(pending.handle.value))return false;
        pending.handle.Close();entry.handle.Close();
        totalBytes-=entry.bytes;++removed;
    }
    size_t finalEntries=0;uint64_t finalBytes=0;
    return MeasureTimelineCacheDirectory(directory,finalEntries,finalBytes)&&
        finalEntries+1<=MaxTimelineCacheEntries&&
        finalBytes<=MaxTimelineCacheDirectoryBytes-incomingBytes;
}

bool ChooseTimelineCacheDestination(const std::filesystem::path& canonical,const std::wstring& songId,
    const std::string& bytes,std::filesystem::path& destination,bool& alreadyStored) {
    alreadyStored=false;
    const auto parsed=ParseTimelineCacheFileName(canonical.filename().wstring());
    if(!parsed||parsed->songId!=songId||parsed->slot!=0)return false;
    std::optional<std::filesystem::path> missing;
    struct ExistingVersion {
        std::filesystem::path path;
        TimelineCacheHandle handle{INVALID_HANDLE_VALUE};
        uint64_t modifiedAt=0;
        BY_HANDLE_FILE_INFORMATION information{};
        std::string rawBytes;
    };
    std::vector<ExistingVersion> valid;
    for(size_t slot=0;slot<=TimelineCacheVersionSlots;++slot){
        const auto path=TimelineCachePath(canonical.parent_path(),songId,parsed->words,slot);
        TimelineCacheHandle input(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(input.value==INVALID_HANDLE_VALUE){
            const auto error=GetLastError();
            if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND){if(!missing)missing=path;continue;}
            return false;
        }
        if(!TimelineCachePathNameExact(path))continue;
        std::string existingBytes;uint64_t modifiedAt=0;
        const auto timeline=LoadTimelineCacheHandle(songId,input.value,false,&modifiedAt,&existingBytes);
        if(!timeline||!HasReusableLyricText(*timeline)||(parsed->words&&!timeline->wordTimed))continue;
        if(existingBytes==bytes){alreadyStored=true;return true;}
        BY_HANDLE_FILE_INFORMATION information{};LARGE_INTEGER observedSize{};
        if(!GetFileInformationByHandle(input.value,&information)||!GetFileSizeEx(input.value,&observedSize)||
            observedSize.QuadPart!=static_cast<LONGLONG>(existingBytes.size())||
            TimelineCacheModifiedAt(information)!=modifiedAt)return false;
        valid.push_back({path,std::move(input),modifiedAt,information,std::move(existingBytes)});
    }
    if(missing){destination=std::move(*missing);return true;}
    if(valid.size()<2)return false;
    std::sort(valid.begin(),valid.end(),[](const auto& left,const auto& right){
        if(left.modifiedAt!=right.modifiedAt)return left.modifiedAt<right.modifiedAt;
        return left.path.wstring()<right.path.wstring();
    });
    destination=valid.front().path;
    TimelineCacheHandle deletion(CreateFileW(destination.c_str(),GENERIC_READ|DELETE,
        FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    uint64_t currentAt=0;std::string currentBytes;
    const auto current=deletion.value==INVALID_HANDLE_VALUE?std::optional<LyricTimeline>{}:
        LoadTimelineCacheHandle(songId,deletion.value,false,&currentAt,&currentBytes);
    BY_HANDLE_FILE_INFORMATION currentInformation{};LARGE_INTEGER currentSize{},originalSize{};
    originalSize.QuadPart=static_cast<LONGLONG>(valid.front().rawBytes.size());
    if(deletion.value==INVALID_HANDLE_VALUE||
        !TimelineCachePathNameExact(destination)||
        detail::CompareFileObjects(valid.front().handle.value,deletion.value)!=detail::FileObjectComparison::Same||
        !current||(parsed->words&&!current->wordTimed)||currentAt!=valid.front().modifiedAt||
        currentBytes!=valid.front().rawBytes||!GetFileInformationByHandle(deletion.value,&currentInformation)||
        !GetFileSizeEx(deletion.value,&currentSize)||
        !SameFileSnapshot(valid.front().information,originalSize,currentInformation,currentSize)||
        (currentInformation.dwFileAttributes&FILE_ATTRIBUTE_READONLY)||
        !detail::DeleteOpenFile(deletion.value))return false;
    deletion.Close();
    valid.front().handle.Close();
    return true;
}

std::optional<bool> TimelineCacheAlreadyStores(const std::filesystem::path& canonical,
    const std::wstring& songId,const std::string& bytes) {
    const auto parsed=ParseTimelineCacheFileName(canonical.filename().wstring());
    if(!parsed||parsed->songId!=songId||parsed->slot!=0)return std::nullopt;
    for(size_t slot=0;slot<=TimelineCacheVersionSlots;++slot){
        const auto path=TimelineCachePath(canonical.parent_path(),songId,parsed->words,slot);
        TimelineCacheHandle input(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(input.value==INVALID_HANDLE_VALUE){
            const auto error=GetLastError();
            if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)continue;
            return std::nullopt;
        }
        if(!TimelineCachePathNameExact(path))continue;
        std::string existingBytes;
        const auto timeline=LoadTimelineCacheHandle(songId,input.value,false,nullptr,&existingBytes);
        if(timeline&&HasReusableLyricText(*timeline)&&(!parsed->words||timeline->wordTimed)&&existingBytes==bytes)return true;
    }
    return false;
}

bool WriteTimelineCacheFile(const std::filesystem::path& file,const std::wstring& songId,
    const std::string& bytes) noexcept {
  try{
    if(bytes.empty()||bytes.size()>MaxCacheBytes)return false;
    detail::DirectoryTreeLock directoryLock;DWORD transactionError=ERROR_SUCCESS;
    if(!directoryLock.Acquire(file.parent_path(),transactionError))return false;
    const auto alreadyStored=TimelineCacheAlreadyStores(file,songId,bytes);
    if(!alreadyStored)return false;
    if(*alreadyStored)return true;
    if(!MakeRoomForTimelineCache(file.parent_path(),songId,bytes.size()))return false;
    GUID guid{};wchar_t token[40]{};
    if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,token,40))return false;
    auto temporary=file;temporary+=std::wstring(L".")+token+L".tmp";
    TimelineCacheHandle output(CreateFileW(temporary.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr));
    if(output.value==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;
    const bool complete=WriteFile(output.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&
        written==bytes.size()&&FlushFileBuffers(output.value);
    if(!complete){detail::DeleteOpenFile(output.value);return false;}
    std::filesystem::path destination;bool storedDuringScan=false;
    if(!ChooseTimelineCacheDestination(file,songId,bytes,destination,storedDuringScan)){
        detail::DeleteOpenFile(output.value);return false;
    }
    if(storedDuringScan)return detail::DeleteOpenFile(output.value);
    if(detail::RenameOpenFile(output.value,destination,false,transactionError)){
        FlushFileBuffers(output.value);output.Close();return true;
    }
    detail::DeleteOpenFile(output.value);return false;
  }catch(...){return false;}
}
using TimelineCacheSave=LyricCacheSaveResult;
TimelineCacheSave SaveTimelineCache(const LyricTimeline& timeline) {
    if (timeline.songId.empty() || NormalizeCatalogId(timeline.songId) != timeline.songId ||
        !ValidCachedTimeline(timeline)||!HasReusableLyricText(timeline)) return TimelineCacheSave::Failed;
    static std::mutex cacheWrites;
    std::lock_guard lock(cacheWrites);
    try {
        const auto directory=TimelineCacheDirectory(true);if(!directory)return TimelineCacheSave::Failed;
        detail::DirectoryTreeLock directoryLock;DWORD directoryError=ERROR_SUCCESS;
        if(!directoryLock.Acquire(*directory,directoryError))return TimelineCacheSave::Failed;
        const auto mainFile=TimelineCachePath(*directory,timeline.songId),wordsFile=TimelineCachePath(*directory,timeline.songId,true);
        const auto encoded=SerializeTimelineCache(timeline);if(encoded.empty())return TimelineCacheSave::Failed;
        if(timeline.wordTimed){
            const bool wordsSaved=WriteTimelineCacheFile(wordsFile,timeline.songId,encoded);
            const bool mainSaved=WriteTimelineCacheFile(mainFile,timeline.songId,encoded);
            return wordsSaved||mainSaved?TimelineCacheSave::Stored:TimelineCacheSave::Failed;
        }
        uint64_t mainAt=0,wordsAt=0;
        const auto main=LoadLatestTimelineCacheVariant(*directory,timeline.songId,false,false,mainAt);
        const auto words=LoadLatestTimelineCacheVariant(*directory,timeline.songId,true,false,wordsAt);
        if(main&&main->wordTimed&&(!words||!words->wordTimed||mainAt>=wordsAt)){
            const auto backup=SerializeTimelineCache(*main);
            if(backup.empty()||!WriteTimelineCacheFile(wordsFile,timeline.songId,backup))return TimelineCacheSave::PreservedWords;
        }
        return WriteTimelineCacheFile(mainFile,timeline.songId,encoded)?TimelineCacheSave::Stored:TimelineCacheSave::Failed;
    } catch (...) { return TimelineCacheSave::Failed; }
}
void AddTimelineCacheNotice(LyricTimeline& timeline,TimelineCacheSave saved) {
    if(saved==TimelineCacheSave::Failed)timeline.diagnostic+=L"；本地缓存未写入，旧文件保留";
    else if(saved==TimelineCacheSave::PreservedWords)timeline.diagnostic+=L"；已保留既有逐字缓存，本次逐句版本未替换缓存";
}
} // namespace

void AttachLyricSupplement(LyricTimeline& base,const LyricTimeline& supplement,bool romanization) { AttachSupplement(base,supplement,romanization); }
LyricCacheSaveResult StoreLyricCache(const LyricTimeline& timeline) noexcept {
    try{return SaveTimelineCache(timeline);}catch(...){return LyricCacheSaveResult::Failed;}
}
std::optional<size_t> MatchExactUniqueCandidate(const PlaybackSnapshot& playback,
    const std::vector<SongCandidate>& candidates) {
    const auto title = NormalizeMatchText(playback.title);
    const auto artist = NormalizeMatchText(playback.artist);
    if (title.empty() || artist.empty()) return std::nullopt;
    std::optional<size_t> matchingIndex;
    std::wstring matchingId;
    for (size_t index = 0; index < candidates.size(); ++index) {
        const auto& candidate = candidates[index];
        const auto id=NormalizeCatalogId(candidate.id);
        if(id.empty()||!CatalogMatchesProvider(id,playback.provider))continue;
        if (NormalizeMatchText(candidate.title) != title || NormalizeMatchText(candidate.artist) != artist) continue;
        if(playback.provider!=MusicProvider::Netease){
            // Non-NetEase metadata matching must identify the same recording.
            if(playback.durationMs<=0||candidate.durationMs<=0||std::abs(playback.durationMs-candidate.durationMs)>3000)continue;
            if(!playback.album.empty()&&!candidate.album.empty()&&NormalizeMatchText(playback.album)!=NormalizeMatchText(candidate.album))continue;
        }
        if (matchingIndex && id != matchingId) return std::nullopt;
        if (!matchingIndex) { matchingIndex = index; matchingId = id; }
    }
    return matchingIndex;
}

LyricTimeline ParseLrc(const std::wstring& text) {
    LyricTimeline output;
    output.source = L"LRC";
    if (text.size() > MaxBytes) { output.state = LyricLoadState::Failed; output.diagnostic = L"歌词文本超过解析上限"; return output; }
    int64_t embeddedOffset = 0;
    size_t textUnits = 0;
    std::wstring failure;
    const bool allLinesRead=EachLine(text, [&](std::wstring_view raw) {
        if(!failure.empty())return;
        const std::wstring trimmed = Trim(raw);
        std::wstring_view line(trimmed);
        if (line.starts_with(L"[offset:") && line.back() == L']') {
            auto value = line.substr(8, line.size() - 9);
            bool negative = false;
            if (!value.empty() && (value.front() == L'-' || value.front() == L'+')) { negative = value.front() == L'-'; value.remove_prefix(1); }
            int64_t offset = 0;
            if (Unsigned(value, offset, 600000)) embeddedOffset = negative ? -offset : offset;
            return;
        }
        std::vector<int64_t> times;
        size_t position = 0;
        while(position<line.size()&&line[position]==L'['&&times.size()<MaxTimestampsPerLine){
            const size_t end = line.find(L']', position + 1);
            int64_t time = 0;
            if (end == std::wstring_view::npos || !Timestamp(line.substr(position + 1, end - position - 1), time)) break;
            times.push_back(time);
            position = end + 1;
        }
        if(times.size()==MaxTimestampsPerLine&&position<line.size()&&line[position]==L'['){
            const size_t end=line.find(L']',position+1);int64_t extraTime=0;
            if(end!=std::wstring_view::npos&&Timestamp(line.substr(position+1,end-position-1),extraTime)){
                failure=L"LRC 单行时间标记数量超过解析上限";return;
            }
        }
        const auto contentView=line.substr(position);
        if(!times.empty()&&(contentView.size()>MaxLineText||contentView.find(L'\0')!=std::wstring_view::npos)){
            failure=contentView.size()>MaxLineText?L"LRC 行文本超过解析上限":L"LRC 行文本包含无效字符";
            return;
        }
        const std::wstring content(contentView);
        for (int64_t time : times) {
            if(output.lines.size()>=MaxLines){failure=L"LRC 歌词行数超过解析上限";break;}
            if(!AddTextUnits(textUnits,content.size())){failure=L"LRC 歌词展开后超过解析上限";break;}
            LyricLine lyric;
            lyric.startMs = time;
            lyric.text = content;
            output.lines.push_back(std::move(lyric));
        }
    });
    if(!allLinesRead&&failure.empty())failure=L"LRC 原文行数超过解析上限";
    if(!failure.empty()){
        output.lines.clear();output.wordTimed=false;output.state=LyricLoadState::Failed;output.diagnostic=std::move(failure);
        return output;
    }
    for (auto& line : output.lines) line.startMs = std::clamp<int64_t>(line.startMs + embeddedOffset, 0, MaxTime);
    Finalize(output);
    return output;
}

bool IsBoundedYrcMetadata(std::wstring_view line) {
    if(line.size()<2||line.size()>MaxLineText||line.front()!=L'{'||line.back()!=L'}'||
       line.find(L'\0')!=std::wstring_view::npos)return false;
    try{
        const auto encoded=ToUtf8(std::wstring(line));if(encoded.empty())return false;
        const auto value=Json::parse(encoded,[](int depth,Json::parse_event_t,Json&){
            if(depth>8)throw std::runtime_error("YRC metadata depth");return true;
        },false);
        if(value.is_discarded()||!value.is_object())return false;
        const auto* time=Field(value,"t");const auto* credits=Field(value,"c");
        if(!time||!credits||!credits->is_array()||credits->size()>MaxYrcMetadataItems)return false;
        if(time->is_number_unsigned()){
            if(time->get<uint64_t>()>static_cast<uint64_t>(MaxTime))return false;
        }else if(time->is_number_integer()){
            const auto timestamp=time->get<int64_t>();if(timestamp<0||timestamp>MaxTime)return false;
        }else return false;
        for(const auto& credit:*credits){
            if(!credit.is_object())return false;
            const auto* text=Field(credit,"tx");if(!text||!text->is_string())return false;
            const auto& bytes=text->get_ref<const std::string&>();
            if(bytes.size()>MaxSupplementText*4)return false;
        }
        return true;
    }catch(...){return false;}
}

LyricTimeline ParseYrc(const std::wstring& text) {
    LyricTimeline output;
    output.source = L"YRC";
    if (text.size() > MaxBytes) { output.state = LyricLoadState::Failed; output.diagnostic = L"歌词文本超过解析上限"; return output; }
    size_t textUnits=0;
    std::wstring failure;
    const bool allLinesRead=EachLine(text, [&](std::wstring_view raw) {
        if(!failure.empty())return;
        const std::wstring trimmed = Trim(raw);
        const std::wstring_view line(trimmed);
        if(line.empty())return;
        // NetEase can prefix YRC with bounded JSON credit metadata. Accept
        // only the documented t/c shape before lyric rows; arbitrary text or
        // JSON embedded among timed rows still invalidates the whole channel.
        if(line.front()!=L'['){
            if(output.lines.empty()&&IsBoundedYrcMetadata(line))return;
            failure=L"YRC 包含畸形的逐字歌词行";return;
        }
        if(output.lines.size()>=MaxLines){failure=L"YRC 歌词行数超过解析上限";return;}
        const size_t end = line.find(L']'), comma = line.find(L',');
        if(end==std::wstring_view::npos||comma==std::wstring_view::npos||comma>end){
            failure=L"YRC 包含畸形的行时间标记";return;
        }
        LyricLine lyric;
        if (!Unsigned(line.substr(1, comma - 1), lyric.startMs) ||
            !Unsigned(line.substr(comma + 1, end - comma - 1), lyric.durationMs, 600000)){
            failure=L"YRC 包含越界或畸形的行时间标记";return;
        }
        size_t position = end + 1;
        bool malformed = false;
        while(position<line.size()){
            if(lyric.words.size()>=MaxWordsPerLine){failure=L"YRC 单行逐字数量超过解析上限";return;}
            if (line[position] != L'(') { malformed = true; break; }
            const size_t tagEnd = line.find(L')', position + 1);
            if (tagEnd == std::wstring_view::npos) { malformed = true; break; }
            const auto tag = line.substr(position + 1, tagEnd - position - 1);
            const size_t first = tag.find(L','), second = first == std::wstring_view::npos ? first : tag.find(L',', first + 1);
            LyricWord word;
            int64_t flag = 0;
            if (first == std::wstring_view::npos || second == std::wstring_view::npos ||
                !Unsigned(tag.substr(0, first), word.startMs) ||
                !Unsigned(tag.substr(first + 1, second - first - 1), word.durationMs, 600000) ||
                !Unsigned(tag.substr(second + 1), flag, INT64_MAX) || word.startMs < lyric.startMs ||
                word.durationMs>MaxTime-word.startMs) {
                malformed = true; break;
            }
            // Parentheses inside lyric text are preserved unless they start a
            // syntactically valid timing tuple, e.g. "(live)" is normal text.
            size_t next = line.find(L'(', tagEnd + 1);
            while (next != std::wstring_view::npos) {
                const size_t close = line.find(L')', next + 1);
                const auto candidate = close == std::wstring_view::npos ? std::wstring_view{} : line.substr(next + 1, close - next - 1);
                const size_t c1 = candidate.find(L','), c2 = c1 == std::wstring_view::npos ? c1 : candidate.find(L',', c1 + 1);
                int64_t n1 = 0, n2 = 0, n3 = 0;
                if (c1 != std::wstring_view::npos && c2 != std::wstring_view::npos &&
                    Unsigned(candidate.substr(0, c1), n1) && Unsigned(candidate.substr(c1 + 1, c2 - c1 - 1), n2, 600000) &&
                    Unsigned(candidate.substr(c2 + 1), n3, INT64_MAX)) break;
                next = line.find(L'(', next + 1);
            }
            const auto wordText=line.substr(tagEnd+1,next==std::wstring_view::npos?next:next-tagEnd-1);
            if(wordText.size()>MaxWordText){failure=L"YRC 逐字文本超过解析上限";return;}
            if(wordText.find(L'\0')!=std::wstring_view::npos){failure=L"YRC 逐字文本包含无效字符";return;}
            if(wordText.size()>MaxLineText-lyric.text.size()){
                failure=L"YRC 行文本超过解析上限";return;
            }
            word.text=std::wstring(wordText);
            lyric.text += word.text;
            lyric.words.push_back(std::move(word));
            if (next == std::wstring_view::npos) { position = line.size(); break; }
            position = next;
        }
        if(position!=line.size()||malformed||lyric.words.empty()){
            failure=L"YRC 包含畸形的逐字时间标记";return;
        }
        for (size_t index = 1; index < lyric.words.size(); ++index)
            if(lyric.words[index].startMs<lyric.words[index-1].startMs){
                failure=L"YRC 逐字时间标记顺序无效";return;
            }
        const size_t lineUnits=lyric.text.size()*2;
        if(!AddTextUnits(textUnits,lineUnits)){failure=L"YRC 歌词展开后超过解析上限";return;}
        output.lines.push_back(std::move(lyric));
    });
    if(!allLinesRead&&failure.empty())failure=L"YRC 原文行数超过解析上限";
    if(!failure.empty()){
        output.lines.clear();output.wordTimed=false;output.state=LyricLoadState::Failed;output.diagnostic=std::move(failure);
        return output;
    }
    Finalize(output);
    return output;
}

std::vector<SongCandidate> LyricsClient::Search(const std::wstring& query, std::wstring& error, MusicProvider provider) {
    if(provider==MusicProvider::SodaMusic)return SearchSodaLyrics(Trim(query),error,stop_);
    if(provider==MusicProvider::Kugou)return SearchKugouLyrics(Trim(query),error,stop_);
    if(provider==MusicProvider::QqMusic)return SearchQqLyrics(Trim(query),error,stop_);
    error.clear();
    if (stop_.stop_requested()) { error = L"歌词查找已取消"; return {}; }
    std::vector<SongCandidate> output;
    const auto term = Trim(query);
    if (term.empty()) { error = L"请输入歌曲名或歌手"; return output; }
    if (term.size() > 300) { error = L"搜索文字超过 300 字符"; return output; }
    const auto bytes = ToUtf8(term);
    if (bytes.empty()) { error = L"搜索文字包含无效字符"; return output; }
    // The legacy /api/search/get returns unrelated numeric-keyword results for
    // these Unicode queries (also via eapi/POST); native flag A/B testing rules
    // out percent re-escaping. cloudsearch/pc preserves the requested terms.
    const auto path = L"/api/cloudsearch/pc?s=" + FromUtf8(Escape(bytes)) + L"&type=1&offset=0&limit=100";
    const Json document = ReadJson(Http(L"music.163.com", path, {}, error, stop_), error);
    if (!error.empty()) return output;
    if (stop_.stop_requested()) { error = L"歌词查找已取消"; return {}; }
    if (Number(document, "code") != 200) { error = L"歌曲搜索服务拒绝请求（code " + std::to_wstring(Number(document, "code")) + L"）"; return output; }
    const auto* result = Field(document, "result");
    const auto* songs = result ? Field(*result, "songs") : nullptr;
    if (!songs || !songs->is_array()) { error = L"歌曲搜索服务返回无效的候选结构"; return output; }
    std::set<int64_t> seen;
    for (const auto& song : *songs) {
        const auto id = Number(song, "id");
        if (id <= 0 || !seen.insert(id).second) continue;
        SongCandidate candidate;
        candidate.id = std::to_wstring(id);
        candidate.title = Text(song, "name");
        if (candidate.title.empty()) continue;
        const auto* artists = Field(song, "artists");
        if (!artists) artists = Field(song, "ar");
        if (artists && artists->is_array()) {
            for (const auto& artist : *artists) {
                const auto name = Text(artist, "name");
                if (name.empty()) continue;
                if (!candidate.artist.empty()) candidate.artist += L" / ";
                candidate.artist += name;
                if (candidate.artist.size() > 512) { candidate.artist.resize(512); break; }
            }
        }
        const auto* album = Field(song, "album");
        if (!album) album = Field(song, "al");
        if (album) candidate.album = Text(*album, "name");
        candidate.durationMs = std::clamp<int64_t>(Number(song, "duration"), 0, MaxTime);
        if (!candidate.durationMs) candidate.durationMs = std::clamp<int64_t>(Number(song, "dt"), 0, MaxTime);
        output.push_back(std::move(candidate));
        if (output.size() == 100) break;
    }
    if (stop_.stop_requested()) { error = L"歌词查找已取消"; return {}; }
    if (output.empty()) error = L"没有搜索候选";
    return output;
}

std::vector<SongCandidate> LyricsClient::Search(const PlaybackSnapshot& playback, std::wstring& error) {
    error.clear();
    if (stop_.stop_requested()) { error = L"歌词查找已取消"; return {}; }
    if(playback.provider==MusicProvider::SodaMusic)return ReadSodaCandidates(error,stop_);
    const auto title = Trim(playback.title);
    const auto artist = Trim(playback.artist);
    if (title.empty()) { error = L"当前歌曲标题不可用"; return {}; }
    std::wstring firstError, secondError;
    auto combined = Search(artist.empty()?title:title+L" "+artist, firstError, playback.provider);
    if (stop_.stop_requested()) { error = L"歌词查找已取消"; return {}; }
    if(playback.provider==MusicProvider::QqMusic&&!firstError.empty()&&firstError!=L"没有搜索候选"){
        error=firstError;return {}; // Do not send a second request into a filtered service.
    }
    std::vector<SongCandidate> titleResults;
    if (!artist.empty()) titleResults = Search(title, secondError, playback.provider);
    if (stop_.stop_requested()) { error = L"歌词查找已取消"; return {}; }
    auto incomplete = [](const std::wstring& message) {
        return !message.empty() && message != L"没有搜索候选";
    };
    // Do not establish uniqueness from one successful query when the other
    // failed: it could have contained another matching version / candidate ID.
    if (incomplete(firstError) || incomplete(secondError)) {
        error = L"歌曲检索未完成，不能自动匹配：" + (incomplete(firstError) ? firstError : secondError);
        return {};
    }
    std::set<std::wstring> ids;
    auto appendCandidate=[&](SongCandidate candidate){
        if(playback.provider==MusicProvider::Kugou){
            // A hash may have alternate catalog names. Keep complete rows so
            // a title-query alias is not lost to the first query's spelling.
            const auto same=[&](const SongCandidate& c){return c.id==candidate.id&&c.title==candidate.title&&
                c.artist==candidate.artist&&c.album==candidate.album&&c.durationMs==candidate.durationMs;};
            if(std::none_of(combined.begin(),combined.end(),same))combined.push_back(std::move(candidate));
        }else if(ids.insert(candidate.id).second)combined.push_back(std::move(candidate));
    };
    for (const auto& candidate : combined) ids.insert(candidate.id);
    for (auto& candidate : titleResults)appendCandidate(std::move(candidate));
    if(playback.provider==MusicProvider::Kugou&&!artist.empty()&&!MatchExactUniqueCandidate(playback,combined)){
        // Some entries are reachable through the artist query only. This adds
        // candidates; it never relaxes title, artist, version or duration checks.
        std::wstring artistError;auto artistResults=Search(artist,artistError,playback.provider);
        if(stop_.stop_requested()){error=L"歌词查找已取消";return {};}
        if(incomplete(artistError)){error=L"歌曲检索未完成，不能自动匹配："+artistError;return {};}
        for(auto& candidate:artistResults)appendCandidate(std::move(candidate));
    }
    const auto normalizedTitle = NormalizeMatchText(playback.title);
    const auto normalizedArtist = NormalizeMatchText(playback.artist);
    if (!normalizedTitle.empty() && !normalizedArtist.empty()) {
        std::stable_partition(combined.begin(), combined.end(), [&](const SongCandidate& candidate) {
            return NormalizeMatchText(candidate.title) == normalizedTitle &&
                NormalizeMatchText(candidate.artist) == normalizedArtist;
        });
    }
    if (stop_.stop_requested()) { error = L"歌词查找已取消"; return {}; }
    if (combined.empty()) error = L"没有搜索候选";
    return combined;
}

std::optional<LyricTimeline> LyricsClient::LoadCached(const std::wstring& normalized,bool networkFailed) {
    std::optional<LyricTimeline> memory;
    if(memoryCache_&&!stop_.stop_requested()){
        try{
            const auto snapshot=memoryCache_(normalized);
            if(snapshot&&SameCatalogId(snapshot->songId,normalized)&&ValidCachedTimeline(*snapshot)&&HasReadableLyrics(*snapshot)){
                auto candidate=*snapshot;Finalize(candidate);
                if(HasReusableLyricText(candidate))memory=std::move(candidate);
            }
            if(memory){
                memory->songId=normalized;
                memory->fromCache=true;memory->retryWordTiming=false;memory->cacheWriteSuggested=false;
                memory->source=memory->wordTimed?L"本次运行歌词缓存 · 逐字":L"本次运行歌词缓存 · 逐句";
                memory->diagnostic=std::wstring(networkFailed?L"在线歌词请求失败":L"在线接口暂未返回歌词")+L"，已使用本次运行歌词缓存（不代表已保存到磁盘）";
            }
        }catch(...){memory.reset();}
    }
    if(stop_.stop_requested())return {};
    if((!memory||!memory->wordTimed)&&preloadedCache_&&preloadedCache_->wordTimed&&
        SameCatalogId(preloadedCache_->songId,normalized)){
        try{
            if(ValidCachedTimeline(*preloadedCache_)&&HasReadableLyrics(*preloadedCache_)){
                auto prepared=*preloadedCache_;Finalize(prepared);
                prepared.fromCache=true;prepared.retryWordTiming=false;prepared.cacheWriteSuggested=false;
                prepared.source=L"本次请求歌词缓存 · 逐字";
                prepared.diagnostic=networkFailed?L"在线歌词请求失败，已保留本次请求读取的逐字缓存":
                    L"已保留本次请求读取的逐字缓存";
                memory=std::move(prepared);
            }
        }catch(...){/* Keep another usable cache candidate when this copy fails. */}
    }
    if(stop_.stop_requested())return {};
    if(memory&&memory->wordTimed)return memory;
    auto disk=LoadTimelineCache(normalized,networkFailed,stop_);
    if(stop_.stop_requested())return {};
    if(disk&&disk->wordTimed)return disk;
    return memory?std::move(memory):std::move(disk);
}
void LyricsClient::CacheOnlineResult(LyricTimeline& timeline) {
    if(cacheMode_==LyricCacheMode::Deferred)timeline.cacheWriteSuggested=true;
    else AddTimelineCacheNotice(timeline,StoreLyricCache(timeline));
}
void LyricsClient::PrepareOnlineResult(LyricTimeline& timeline) {
    if(!timeline.wordTimed&&!stop_.stop_requested()){
        try{
            auto cached=LoadCached(timeline.songId,false);
            // Word timing belongs to the complete original text and its line
            // starts. A same-ID cache alone cannot justify attaching old word
            // times to changed lyrics, reordered rows or a revised line clock.
            const bool sameRows=cached&&cached->wordTimed&&
                SameCatalogId(cached->songId,timeline.songId)&&
                cached->lines.size()==timeline.lines.size()&&
                std::equal(cached->lines.begin(),cached->lines.end(),timeline.lines.begin(),
                    [](const LyricLine& prior,const LyricLine& current){
                        return prior.startMs==current.startMs&&prior.text==current.text;
                    });
            if(sameRows&&!stop_.stop_requested()){
                // Work on a private cache copy until every auxiliary-field and
                // total-size check succeeds. Missing optional channels retain
                // their prior text; they are not evidence of a deliberate erase.
                for(size_t i=0;i<timeline.lines.size();++i){
                    const auto& current=timeline.lines[i];auto& prior=cached->lines[i];
                    if(!current.translation.empty())prior.translation=current.translation;
                    if(!current.romanization.empty())prior.romanization=current.romanization;
                }
                if(ValidCachedTimeline(*cached)&&!stop_.stop_requested()){
                    cached->diagnostic=timeline.source+L" · "+timeline.diagnostic+
                        L"；正文与行首时间一致，已保留本机逐字时间轴并更新可用辅助文字";
                    cached->retryWordTiming=false;cached->cacheWriteSuggested=false;
                    // fromCache remains true: current word times came from a
                    // local timeline, not this online response. Existing retry
                    // policy can refresh them without claiming a new disk save.
                    preloadedCache_.reset();timeline=std::move(*cached);return;
                }
            }
        }catch(...){
            // Optional cache reuse must never turn usable online LRC into a
            // failed request. The online value has not been changed yet.
        }
    }
    preloadedCache_.reset();
    if(!stop_.stop_requested())CacheOnlineResult(timeline);
}
std::optional<LyricTimeline> LyricsClient::Cached(const std::wstring& songId) {
    preloadedCache_.reset();
    const auto normalized=NormalizeCatalogId(songId);
    if(stop_.stop_requested()||normalized.empty())return std::nullopt;
    auto timeline=LoadCached(normalized,true);
    if (stop_.stop_requested()) return std::nullopt;
    if (timeline) timeline->diagnostic=timeline->source.starts_with(L"本次运行")?
        L"已加载本次运行歌词缓存，正在更新在线版本（不代表已保存到磁盘）":timeline->wordTimed?
        L"已加载本机逐字歌词缓存，正在更新在线版本":L"已加载本机逐句歌词缓存，正在更新在线版本";
    if(timeline&&timeline->wordTimed&&!stop_.stop_requested()){
        try{preloadedCache_=std::make_shared<LyricTimeline>(*timeline);}
        catch(...){/* Retaining the optional copy cannot fail the cache read. */}
    }
    if(stop_.stop_requested())return std::nullopt;
    return timeline;
}

LyricTimeline LyricsClient::Fetch(const std::wstring& songId) {
    if(IsQqCatalogId(songId)||IsKugouCatalogId(songId)||IsSodaCatalogId(songId)){
        auto result=IsSodaCatalogId(songId)?FetchSodaLyrics(songId,stop_):IsKugouCatalogId(songId)?FetchKugouLyrics(songId,stop_):FetchQqLyrics(songId,stop_);
        if(stop_.stop_requested()){result.lines.clear();result.wordTimed=false;result.state=LyricLoadState::Cancelled;return result;}
        if(result.state==LyricLoadState::Ready)PrepareOnlineResult(result);
        else if(result.state!=LyricLoadState::Cancelled){
            if(auto cached=LoadCached(NormalizeCatalogId(songId),result.state==LyricLoadState::Failed))result=std::move(*cached);
        }
        if(stop_.stop_requested()){result.lines.clear();result.wordTimed=false;result.cacheWriteSuggested=false;result.state=LyricLoadState::Cancelled;}
        return result;
    }
    LyricTimeline output;
    output.state = LyricLoadState::Failed;
    if (stop_.stop_requested()) { output.state = LyricLoadState::Cancelled; output.diagnostic = L"歌词获取已取消"; return output; }
    int64_t id = 0;
    if (!Unsigned(songId, id, INT64_MAX) || id <= 0) {
        output.diagnostic = L"歌曲 ID 必须是有效的正整数";
        return output;
    }
    const std::wstring normalized = std::to_wstring(id);
    output.songId = normalized;
    std::wstring primaryError;
    Json data{{"id", ToUtf8(normalized)}, {"cp", false}, {"lv", 0}, {"tv", 0}, {"rv", 0}, {"kv", 0},
        {"yv", 0}, {"ytv", 0}, {"yrv", 0}, {"e_r", false}};
    const auto request = Eapi("/api/song/lyric/v1", data, primaryError);
    Json primary;
    if (primaryError.empty()) primary = ReadJson(Http(L"interface.music.163.com", L"/eapi/song/lyric/v1", request, primaryError, stop_), primaryError);
    if (stop_.stop_requested()) { output.state = LyricLoadState::Cancelled; output.diagnostic = L"歌词获取已取消"; return output; }
    if (primaryError.empty() && Number(primary, "code") == 200) {
        auto timeline = ReadTimeline(primary);
        if (stop_.stop_requested()) { output.state = LyricLoadState::Cancelled; output.diagnostic = L"歌词获取已取消"; return output; }
        if (!timeline.lines.empty()) {
            timeline.songId = normalized;
            timeline.source = timeline.wordTimed ? L"网易云 YRC · 原生 WinHTTP/CNG" :
                timeline.source==L"YRC"?L"网易云 YRC 原文 · 逐句降级":L"网易云 LRC · 原生 WinHTTP/CNG";
            if (!timeline.wordTimed) timeline.diagnostic = timeline.retryWordTiming?
                L"逐字数据暂不可用，已使用逐句歌词":L"服务未提供可用逐字歌词，已使用逐句歌词";
            PrepareOnlineResult(timeline);
            if(stop_.stop_requested()){
                output.state=LyricLoadState::Cancelled;output.diagnostic=L"歌词获取已取消";return output;
            }
            return timeline;
        }
        if(timeline.state==LyricLoadState::Failed)primaryError=timeline.diagnostic;
    } else if (primaryError.empty()) primaryError = L"逐字歌词服务返回 code " + std::to_wstring(Number(primary, "code"));
    std::wstring fallbackError;
    const Json fallback = ReadJson(Http(L"music.163.com", L"/api/song/lyric?id=" + normalized +
        L"&lv=-1&kv=-1&tv=-1&rv=-1", {}, fallbackError, stop_), fallbackError);
    if (stop_.stop_requested()) { output.state = LyricLoadState::Cancelled; output.diagnostic = L"歌词获取已取消"; return output; }
    if (fallbackError.empty() && Number(fallback, "code") == 200) {
        output = ReadTimeline(fallback);
        if (stop_.stop_requested()) {
            output.lines.clear();output.wordTimed=false;output.retryWordTiming=false;output.cacheWriteSuggested=false;
            output.state=LyricLoadState::Cancelled;output.diagnostic=L"歌词获取已取消";return output;
        }
        output.songId = normalized;
        output.source = output.wordTimed?L"网易云 YRC · 公开接口降级":
            output.source==L"YRC"?L"网易云 YRC 原文 · 逐句降级":L"网易云 LRC · 公开接口降级";
        if (!output.lines.empty()){
            output.retryWordTiming=!output.wordTimed&&(output.retryWordTiming||!primaryError.empty());
            output.diagnostic=std::wstring(output.wordTimed?L"逐字歌词已就绪":L"逐句歌词已就绪")+
                (primaryError.empty()?(output.wordTimed?std::wstring{}:std::wstring(L"；未提供逐字数据")):L"；逐字请求："+primaryError);
            PrepareOnlineResult(output);
            if(stop_.stop_requested()){
                output.lines.clear();output.wordTimed=false;output.retryWordTiming=false;output.cacheWriteSuggested=false;
                output.state=LyricLoadState::Cancelled;
                output.diagnostic=L"歌词获取已取消";
            }
            return output;
        }
        if(output.state==LyricLoadState::NoLyrics&&primaryError.empty()){
            if(auto cached=LoadCached(normalized,false))return std::move(*cached);
            if(stop_.stop_requested()){
                output.lines.clear();output.wordTimed=false;output.retryWordTiming=false;output.cacheWriteSuggested=false;
                output.state=LyricLoadState::Cancelled;
                output.diagnostic=L"歌词获取已取消";return output;
            }
            output.diagnostic=L"该歌曲没有可用歌词；可导入本地 LRC / YRC";return output;
        }
        fallbackError=output.state==LyricLoadState::NoLyrics?
            L"逐句返回空结果，但逐字请求异常，尚不能确认没有歌词":output.diagnostic;
    }
    if (fallbackError.empty()) fallbackError = L"逐句歌词服务返回 code " + std::to_wstring(Number(fallback, "code"));
    if (auto cached = LoadCached(normalized,true)) return std::move(*cached);
    if(stop_.stop_requested()){
        output.lines.clear();output.wordTimed=false;output.retryWordTiming=false;output.cacheWriteSuggested=false;
        output.state=LyricLoadState::Cancelled;
        output.diagnostic=L"歌词获取已取消";return output;
    }
    output.state = LyricLoadState::Failed;
    output.source = L"网易云歌词请求";
    output.diagnostic = L"歌词未取得：" + fallbackError;
    if (!primaryError.empty()) output.diagnostic += L"；逐字请求：" + primaryError;
    return output;
}

LyricTimeline LyricsClient::Import(const std::filesystem::path& file) {
    LyricTimeline output;
    output.state = LyricLoadState::Failed;
    if (stop_.stop_requested()) { output.state = LyricLoadState::Cancelled; output.diagnostic = L"歌词导入已取消"; return output; }
    std::filesystem::path openedPath;std::string bytes;
    std::wstring error;
    const bool read=ReadLocalLyricFile(file,openedPath,bytes,error,stop_);
    if(stop_.stop_requested()){
        output.state=LyricLoadState::Cancelled;output.diagnostic=L"歌词导入已取消";return output;
    }
    if(!read){output.diagnostic=std::move(error);return output;}
    output.source = L"本地歌词 · " + openedPath.filename().wstring();
    const auto decoded = DecodeFile(bytes, error);
    if(stop_.stop_requested()){
        output.state=LyricLoadState::Cancelled;output.diagnostic=L"歌词导入已取消";return output;
    }
    if (!error.empty()) { output.diagnostic = error; return output; }
    auto timeline = ParseYrc(decoded);
    std::wstring parseFailure=timeline.state==LyricLoadState::Failed?timeline.diagnostic:std::wstring{};
    if (!timeline.wordTimed){
        auto fallback=ParseLrc(decoded);
        if(fallback.state==LyricLoadState::Failed&&!fallback.diagnostic.empty())parseFailure=fallback.diagnostic;
        if(HasReadableLyrics(fallback)||!HasReadableLyrics(timeline))timeline=std::move(fallback);
    }
    if (stop_.stop_requested()) { output.state = LyricLoadState::Cancelled; output.diagnostic = L"歌词导入已取消"; return output; }
    if(!HasReadableLyrics(timeline)){
        timeline.lines.clear();timeline.wordTimed=false;timeline.state=LyricLoadState::Failed;
        timeline.diagnostic=parseFailure.empty()?L"文件中没有非空的有效时间标记歌词":std::move(parseFailure);
    }
    timeline.source = output.source + (timeline.wordTimed ? L" · YRC" : timeline.source==L"YRC"?L" · YRC（逐句显示）":L" · LRC");
    return timeline;
}
} // namespace stage

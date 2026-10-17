#include "languages.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>

namespace asrd {
namespace {

enum Script { kLatin, kCyrillic, kGreek, kArabic, kDevanagari, kThai, kHangul, kKana, kHan, kScripts, kOther = kScripts };

Script script_of(uint32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0x00C0 && c <= 0x024F) || (c >= 0x1E00 && c <= 0x1EFF))
        return kLatin;
    if (c >= 0x0400 && c <= 0x052F) return kCyrillic;
    if (c >= 0x0370 && c <= 0x03FF) return kGreek;
    if ((c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) || (c >= 0xFB50 && c <= 0xFEFF)) return kArabic;
    if (c >= 0x0900 && c <= 0x097F) return kDevanagari;
    if (c >= 0x0E00 && c <= 0x0E7F) return kThai;
    if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F)) return kHangul;
    if (c >= 0x3040 && c <= 0x30FF) return kKana;
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) return kHan;
    return kOther;
}

// The scripts Qwen3-ASR's languages are written in; Japanese mixes kana with kanji.
bool writes_in(const std::string & language, Script script) {
    if (language == "ru" || language == "mk") return script == kCyrillic;
    if (language == "el") return script == kGreek;
    if (language == "ar" || language == "fa") return script == kArabic;
    if (language == "hi") return script == kDevanagari;
    if (language == "th") return script == kThai;
    if (language == "ko") return script == kHangul;
    if (language == "ja") return script == kKana || script == kHan;
    if (language == "zh" || language == "yue") return script == kHan;
    return script == kLatin;
}

Script dominant_script(const std::string & text) {
    std::array<int, kScripts> counts{};
    for (size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        const int n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
        uint32_t c = n == 1 ? b : b & (0x7F >> n);
        for (int k = 1; k < n && i + k < text.size(); ++k) c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        i += n;
        const Script s = script_of(c);
        if (s != kOther) counts[s]++;
    }
    const auto top = std::max_element(counts.begin(), counts.end());
    return *top == 0 ? kOther : static_cast<Script>(top - counts.begin());
}

}  // namespace

std::string base_language(const std::string & tag) {
    std::string out;
    for (const char c : tag) {
        if (c == '-' || c == '_') break;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::vector<std::string> parse_languages(const std::string & list) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= list.size()) {
        const size_t comma = std::min(list.find(',', at), list.size());
        std::string tag = list.substr(at, comma - at);
        tag.erase(0, tag.find_first_not_of(" \t"));
        tag.erase(tag.find_last_not_of(" \t") + 1);
        tag = base_language(tag);
        if (!tag.empty() && tag != "auto" && !language_in(tag, out)) out.push_back(tag);
        at = comma + 1;
    }
    return out;
}

bool language_in(const std::string & tag, const std::vector<std::string> & set) {
    const std::string base = base_language(tag);
    return std::find(set.begin(), set.end(), base) != set.end();
}

std::string closest_language(const std::string & text, const std::vector<std::string> & set) {
    const Script script = dominant_script(text);
    for (const std::string & language : set) {
        if (writes_in(language, script)) return language;
    }
    return set.front();
}

}  // namespace asrd

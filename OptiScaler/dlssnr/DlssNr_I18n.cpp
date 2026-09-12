#include "pch.h"

#include "DlssNr_I18n.h"

#include <SysUtils.h>

#include <imgui/imgui.h>

#include <Windows.h>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace DlssNr::I18n
{

namespace
{
// The combo's order. Codes are compared case-insensitively everywhere, and written back to the
// ini in this (lower-case) form so the manager and the panel never fight over "pt-BR" vs "pt-br".
const char* const kSelectorCodes[] = { "auto", "en", "pt-br", "ru", "ko", "zh-cn", "es", "de" };

std::string g_configured;
bool g_resolved = false;
int g_language = -1; // index into kLanguageCodes, -1 = English

std::string Lower(std::string_view in)
{
    std::string out(in);

    for (char& c : out)
        c = (char) std::tolower((unsigned char) c);

    return out;
}

// Key -> row, built on first use. string_view over the static key table, so nothing is copied.
const std::unordered_map<std::string_view, int>& Index()
{
    static const std::unordered_map<std::string_view, int> index = []
    {
        std::unordered_map<std::string_view, int> m;
        m.reserve((size_t) kKeyCount);

        for (int i = 0; i < kKeyCount; ++i)
            m.emplace(kKeys[i], i);

        return m;
    }();

    return index;
}

int LanguageIndex(std::string_view lowerCode)
{
    for (int i = 0; i < kLanguageCount; ++i)
        if (Lower(kLanguageCodes[i]) == lowerCode)
            return i;

    return -1;
}

// The Windows display language, narrowed to a language the panel ships -- the same narrowing
// the manager's i18n.js does from navigator.language.
int DetectFromWindows()
{
    switch (PRIMARYLANGID(GetUserDefaultUILanguage()))
    {
    case LANG_PORTUGUESE:
        return LanguageIndex("pt-br");
    case LANG_RUSSIAN:
        return LanguageIndex("ru");
    case LANG_KOREAN:
        return LanguageIndex("ko");
    case LANG_CHINESE:
        return LanguageIndex("zh-cn");
    case LANG_SPANISH:
        return LanguageIndex("es");
    case LANG_GERMAN:
        return LanguageIndex("de");
    default:
        return -1;
    }
}
} // namespace

void Refresh(const std::string& configured)
{
    if (g_resolved && configured == g_configured)
        return;

    g_configured = configured;
    g_resolved = true;

    const std::string code = Lower(configured);
    g_language = (code.empty() || code == "auto") ? DetectFromWindows() : LanguageIndex(code);

    LOG_INFO("DLSS 5 panel language: {} (configured '{}')", ActiveCode(), configured);
}

const char* Tr(const char* en)
{
    if (g_language < 0 || en == nullptr)
        return en;

    const auto& index = Index();
    const auto it = index.find(en);

    if (it == index.end())
        return en;

    const char* translated = kLanguageTables[g_language][it->second];
    return (translated != nullptr && translated[0] != 0) ? translated : en;
}

const char* ActiveCode() { return g_language < 0 ? "en" : kLanguageCodes[g_language]; }

int SelectorIndex(const char* configured)
{
    const std::string code = Lower(configured != nullptr ? configured : "");

    for (int i = 0; i < IM_ARRAYSIZE(kSelectorCodes); ++i)
        if (code == kSelectorCodes[i])
            return i;

    return 0;
}

const char* CodeForSelectorIndex(int index)
{
    if (index < 0 || index >= IM_ARRAYSIZE(kSelectorCodes))
        return kSelectorCodes[0];

    return kSelectorCodes[index];
}

void EnsureFonts(ImFontAtlas* atlas, float fontSize)
{
    // One attempt per font per atlas, whether or not the file was there: a missing font is a
    // once-per-atlas log line, not a per-frame disk probe. Per atlas, not per session: the menu
    // destroys and recreates its ImGui context when the game's window handle changes
    // (MenuOverlayDx), and the new atlas starts empty -- a flag that outlived the old atlas would
    // leave the new one without the font, and the panel's Chinese or Korean drawing as boxes.
    static const ImFontAtlas* triedAtlas = nullptr;
    static int triedGeneration = -1; // atlas->Clear() keeps the pointer; the Sources count restarts
    static bool triedChinese = false;
    static bool triedKorean = false;

    if (atlas == nullptr)
        return;

    if (atlas != triedAtlas || atlas->Sources.Size < triedGeneration)
    {
        triedAtlas = atlas;
        triedChinese = false;
        triedKorean = false;
    }

    const std::string code = Lower(ActiveCode());
    const wchar_t* file = nullptr;
    bool* tried = nullptr;

    // Windows ships both on every edition, whatever the system language (they draw the shell's
    // own East Asian text), unlike SimSun, Batang and the rest, which are on-demand features.
    if (code == "zh-cn")
    {
        file = L"msyh.ttc"; // Microsoft YaHei
        tried = &triedChinese;
    }
    else if (code == "ko")
    {
        file = L"malgun.ttf"; // Malgun Gothic
        tried = &triedKorean;
    }

    if (file == nullptr || *tried)
        return;

    *tried = true;

    wchar_t windowsDir[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryW(windowsDir, MAX_PATH);

    if (n == 0 || n >= MAX_PATH)
        return;

    const std::wstring path = std::wstring(windowsDir) + L"\\Fonts\\" + file;

    if (!std::filesystem::exists(path))
    {
        LOG_WARN("DLSS 5 panel: font for '{}' not found at {}; its text will draw as boxes", code,
                 wstring_to_string(path));
        return;
    }

    // MergeMode adds this font's glyphs behind the atlas's last font, which is the menu's base
    // font -- there has to be one to merge into.
    if (atlas->Fonts.empty())
        atlas->AddFontDefault();

    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.FontNo = 0;

    ImFont* merged = atlas->AddFontFromFileTTF(wstring_to_string(path).c_str(), fontSize, &cfg);
    LOG_INFO("DLSS 5 panel: merged {} for '{}' ({})", wstring_to_string(path), code,
             merged != nullptr ? "ok" : "failed");

    // The count with this font in it: an atlas later cleared and rebuilt with only the base font
    // falls below it, and the merge is done again.
    if (merged != nullptr)
        triedGeneration = atlas->Sources.Size;
}

} // namespace DlssNr::I18n

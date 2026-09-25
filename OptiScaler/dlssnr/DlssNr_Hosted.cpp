// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"
#include "DlssNr_Hosted.h"
#include "DlssNr_ReLimiter.h"
#include "DlssNr_RenoDx.h"
#include <json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace DlssNr::Hosted
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr auto kBuildEvery = std::chrono::milliseconds(500);
// Written at least this often even when nothing changed: the app reads `at` to tell a live game from
// a file left behind, and a page of settings nobody touches would otherwise go "stale" while fine.
constexpr auto kWriteAtLeastEvery = std::chrono::milliseconds(1000);
constexpr auto kCommandCheckEvery = std::chrono::milliseconds(250);
// A command file is a handful of keys. Anything this big is not ours, and is not read.
constexpr DWORD kMaxCommandBytes = 64 * 1024;
// ReLimiter's enum values are short words; RenoDX combos are capped the way the in-game page caps them.
constexpr uint32_t kStringBuf = 256;
constexpr uint32_t kMaxComboLabels = 64;

Clock::time_point g_lastBuild {};
Clock::time_point g_lastWrite {};
Clock::time_point g_lastCommandCheck {};
std::string g_lastBody;
unsigned long long g_ack = 0; // the last seq applied in THIS process; 0 = none yet
bool g_publishNow = false;
bool g_writeFailureLogged = false;

// The command file's stamp when last read, so an unchanged file costs one stat and no read.
FILETIME g_cmdTime {};
DWORD g_cmdSize = 0;

unsigned long long NowUnixMs()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return t.QuadPart / 10000ULL - 11644473600000ULL;
}

// A real JSON string, quotes included. Unlike every number Live writes, what goes through here is the
// ADD-ONS' text -- labels, tooltips, group names, choices -- and those carry quotes, backslashes and
// newlines (ReLimiter's tooltips are multi-line). One unescaped quote and the app's JSON.parse throws
// away the whole file, so the pages vanish rather than one label reading oddly.
//
// Bytes from 0x80 up pass through: the add-ons write UTF-8, and JSON is UTF-8. Control characters
// become \u00XX, which is the only other thing JSON forbids raw. A null pointer is written as "".
void AppendString(std::string& out, const char* s)
{
    out += '"';
    if (s != nullptr)
    {
        for (const char* p = s; *p != '\0'; ++p)
        {
            const unsigned char c = (unsigned char) *p;
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned) c);
                    out += buf;
                }
                else
                {
                    out += (char) c;
                }
                break;
            }
        }
    }
    out += '"';
}

void AppendKey(std::string& out, const char* key)
{
    AppendString(out, key);
    out += ':';
}

// NaN and infinity are not JSON; a setting that somehow holds one is written as null rather than as a
// token that makes the whole file unreadable.
void AppendNumber(std::string& out, double v, bool integer)
{
    if (!std::isfinite(v))
    {
        out += "null";
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), integer ? "%.0f" : "%.9g", v);
    out += buf;
}

// RenoDX holds floats. %.9g of one widened to double prints 0.1 as 0.100000001, a precision the add-on
// never had and a number the app would then show; %.7g is what a float can actually tell apart.
void AppendFloat(std::string& out, float v)
{
    if (!std::isfinite(v))
    {
        out += "null";
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%.7g", (double) v);
    out += buf;
}

bool NonEmpty(const char* s) { return s != nullptr && *s != '\0'; }

// ---------------------------------------------------------------------------------------------------
// Publishing. Each builder applies the same filters as its in-game page (DrawPacingPage /
// DrawRenoDxPage in DlssNr_Menu.cpp), so the pop-out never offers a setting the in-game panel would
// not: a row there and not here, or the other way round, is two answers to one question.
// ---------------------------------------------------------------------------------------------------

void AppendPacing(std::string& s)
{
    const ReLimiterApi* api = DlssNrReLimiter::Api();
    s += "\"pacing\":{";
    AppendKey(s, "available");
    s += api != nullptr ? "true" : "false";
    if (api == nullptr)
    {
        // Why not, so the pop-out's greyed page can say it in a sentence (DlssNr_ReLimiter.h).
        s += ',';
        AppendKey(s, "reason");
        AppendString(s, DlssNrReLimiter::UnavailableReason());
        s += ",\"settings\":[]}";
        return;
    }

    s += ',';
    AppendKey(s, "version");
    AppendString(s, DlssNrReLimiter::Version());
    s += ",\"settings\":[";

    bool first = true;
    const uint32_t count = api->setting_count();
    for (uint32_t i = 0; i < count; ++i)
    {
        ReLimiterSettingInfo info {};
        info.struct_size = sizeof(info);
        if (!api->describe_setting(i, &info) || !NonEmpty(info.key))
            continue;

        const char* type = nullptr;
        switch (info.type)
        {
        case RELIMITER_TYPE_BOOL:
            type = "bool";
            break;
        case RELIMITER_TYPE_INT:
            type = "int";
            break;
        case RELIMITER_TYPE_FLOAT:
            type = "float";
            break;
        case RELIMITER_TYPE_DOUBLE:
            type = "double";
            break;
        case RELIMITER_TYPE_ENUM:
            type = "enum";
            break;
        default:
            // KEYBIND needs a key-capture widget and STRING a text field; the in-game page offers
            // neither, and anything newer is not guessed at.
            break;
        }
        if (type == nullptr)
            continue;

        const bool numeric =
            info.type == RELIMITER_TYPE_INT || info.type == RELIMITER_TYPE_FLOAT || info.type == RELIMITER_TYPE_DOUBLE;
        // No range, no control: those settings have no clamp in ReLimiter, and inventing ends would
        // offer numbers it throws away.
        if (numeric && info.min_value == info.max_value)
            continue;
        if (info.type == RELIMITER_TYPE_ENUM && (info.choices == nullptr || info.choice_count == 0))
            continue;

        double number = 0.0;
        char text[kStringBuf] {};
        if (info.type == RELIMITER_TYPE_ENUM)
        {
            if (!api->get_string(info.key, text, kStringBuf))
                continue;
        }
        else if (!api->get_number(info.key, &number))
        {
            continue;
        }

        if (!first)
            s += ',';
        first = false;

        s += '{';
        AppendKey(s, "key");
        AppendString(s, info.key);
        s += ',';
        AppendKey(s, "label");
        AppendString(s, NonEmpty(info.label) ? info.label : info.key);
        s += ',';
        AppendKey(s, "group");
        AppendString(s, info.group);
        s += ',';
        AppendKey(s, "tooltip");
        AppendString(s, info.tooltip);
        s += ',';
        AppendKey(s, "type");
        AppendString(s, type);
        if (numeric)
        {
            s += ',';
            AppendKey(s, "min");
            AppendNumber(s, info.min_value, false);
            s += ',';
            AppendKey(s, "max");
            AppendNumber(s, info.max_value, false);
            if (info.zero_label != nullptr)
            {
                s += ',';
                AppendKey(s, "zeroLabel");
                AppendString(s, info.zero_label);
            }
        }
        if (info.type == RELIMITER_TYPE_ENUM)
        {
            s += ',';
            AppendKey(s, "choices");
            s += '[';
            for (uint32_t c = 0; c < info.choice_count; ++c)
            {
                if (c > 0)
                    s += ',';
                AppendString(s, info.choices[c]);
            }
            s += ']';
        }
        s += ',';
        AppendKey(s, "value");
        if (info.type == RELIMITER_TYPE_ENUM)
            AppendString(s, text);
        else if (info.type == RELIMITER_TYPE_BOOL)
            s += number != 0.0 ? "true" : "false";
        else
            AppendNumber(s, number, info.type == RELIMITER_TYPE_INT);
        s += '}';
    }
    s += "]}";
}

const char* RenoDxKindName(uint32_t kind)
{
    switch (kind)
    {
    case RENODX_HOST_KIND_FLOAT:
        return "float";
    case RENODX_HOST_KIND_INTEGER:
        return "int";
    case RENODX_HOST_KIND_BOOLEAN:
        return "bool";
    case RENODX_HOST_KIND_BUTTON:
        return "button";
    case RENODX_HOST_KIND_LABEL:
        return "label";
    case RENODX_HOST_KIND_BULLET:
        return "bullet";
    case RENODX_HOST_KIND_TEXT:
        return "text";
    case RENODX_HOST_KIND_TEXT_NOWRAP:
        return "textNowrap";
    case RENODX_HOST_KIND_CUSTOM:
        return "custom";
    case RENODX_HOST_KIND_INPUT_TEXT:
        return "inputText";
    default:
        return nullptr;
    }
}

// The rest of RenoDX's overlay for the pop-out (host API version 4): the add-on's title, its preset
// switcher and every row it draws, in order, with what the pop-out needs to draw the same control. Added
// after "settings", which stays exactly as older app builds read it. Without a version-4 add-on only
// "apiVersion" is written.
void AppendHdrV4(std::string& s, const RenoDxHostApi* api)
{
    s += ',';
    AppendKey(s, "apiVersion");
    AppendNumber(s, api->api_version, true);

    const RenoDxHostApi* v4 = DlssNrRenoDx::V4();
    if (v4 == nullptr)
        return;

    s += ',';
    AppendKey(s, "title");
    AppendString(s, v4->overlay_title != nullptr ? v4->overlay_title() : "");

    // Presets: null when the mod has none.
    s += ',';
    AppendKey(s, "presets");
    const uint32_t presetCount = v4->preset_count != nullptr ? v4->preset_count() : 0;
    const int32_t preset = v4->get_preset != nullptr ? v4->get_preset() : -1;
    if (presetCount == 0 || preset < 0)
    {
        s += "null";
    }
    else
    {
        s += '{';
        AppendKey(s, "count");
        AppendNumber(s, presetCount, true);
        s += ',';
        AppendKey(s, "selected");
        AppendNumber(s, preset, true);
        s += ',';
        AppendKey(s, "segmented");
        const uint32_t style = v4->preset_style != nullptr ? v4->preset_style() : RENODX_HOST_STYLE_SEGMENTED;
        s += (style & RENODX_HOST_STYLE_SEGMENTED) != 0 ? "true" : "false";
        s += ",\"labels\":[";
        for (uint32_t p = 0; p < presetCount && p < 16; ++p)
        {
            if (p > 0)
                s += ',';
            const char* name = v4->preset_label != nullptr ? v4->preset_label(p) : nullptr;
            AppendString(s, name != nullptr ? name : "");
        }
        s += "]}";
    }

    s += ",\"rows\":[";
    bool first = true;
    const uint32_t count = v4->setting_count();
    for (uint32_t i = 0; i < count; ++i)
    {
        RenoDxHostSetting info {};
        info.struct_size = sizeof(info);
        if (!v4->describe_setting(i, &info) || info.is_visible == 0)
            continue;
        const char* kind = RenoDxKindName(info.kind);
        if (kind == nullptr)
            continue;

        // BORROWED strings: copied before the next call into the add-on.
        const std::string key = info.key != nullptr ? info.key : "";
        const std::string label = info.label != nullptr ? info.label : "";
        const std::string section = info.section != nullptr ? info.section : "";
        const std::string tooltip = info.tooltip != nullptr ? info.tooltip : "";
        const std::string placeholder = info.placeholder != nullptr ? info.placeholder : "";
        const std::string defaultText = info.default_text != nullptr ? info.default_text : "";
        const uint32_t labelCount = info.label_count < kMaxComboLabels ? info.label_count : kMaxComboLabels;

        std::string row;
        row += '{';
        AppendKey(row, "index");
        AppendNumber(row, i, true);
        row += ',';
        AppendKey(row, "kind");
        AppendString(row, kind);
        row += ',';
        AppendKey(row, "key");
        AppendString(row, key.c_str());
        row += ',';
        AppendKey(row, "label");
        AppendString(row, label.c_str());
        row += ',';
        AppendKey(row, "section");
        AppendString(row, section.c_str());
        row += ',';
        AppendKey(row, "sectionOpen");
        row +=
            v4->section_open_by_default != nullptr && v4->section_open_by_default(section.c_str()) ? "true" : "false";
        row += ',';
        AppendKey(row, "tooltip");
        AppendString(row, tooltip.c_str());
        row += ',';
        AppendKey(row, "enabled");
        row += info.is_enabled != 0 ? "true" : "false";
        row += ',';
        AppendKey(row, "sticky");
        row += info.is_sticky != 0 ? "true" : "false";
        row += ',';
        AppendKey(row, "segmented");
        row += (info.style & RENODX_HOST_STYLE_SEGMENTED) != 0 ? "true" : "false";
        row += ',';
        AppendKey(row, "multiline");
        row += (info.style & RENODX_HOST_STYLE_MULTILINE) != 0 ? "true" : "false";
        row += ',';
        AppendKey(row, "tint");
        if (info.has_tint != 0)
        {
            char tint[8];
            std::snprintf(tint, sizeof(tint), "#%06X", info.tint_rgb & 0xFFFFFFu);
            AppendString(row, tint);
        }
        else
        {
            row += "null";
        }
        row += ',';
        AppendKey(row, "canReset");
        row += info.can_reset != 0 ? "true" : "false";
        row += ',';
        AppendKey(row, "isUsingDefault");
        row += info.is_using_default != 0 ? "true" : "false";

        // Names: a labelled int or bool's choices, or a LABEL row's text (its only entry).
        if (labelCount > 0)
        {
            row += ",\"labels\":[";
            for (uint32_t c = 0; c < labelCount; ++c)
            {
                if (c > 0)
                    row += ',';
                const char* l = v4->label_at(i, c);
                AppendString(row, l != nullptr ? l : "");
            }
            row += ']';
        }

        const bool numeric = info.kind == RENODX_HOST_KIND_FLOAT || info.kind == RENODX_HOST_KIND_INTEGER ||
                             info.kind == RENODX_HOST_KIND_BOOLEAN;
        if (numeric && !key.empty())
        {
            float cur = 0.0f;
            if (!v4->get_number(key.c_str(), &cur))
                continue;
            if (info.kind != RENODX_HOST_KIND_BOOLEAN)
            {
                row += ',';
                AppendKey(row, "min");
                AppendFloat(row, info.min_value);
                row += ',';
                AppendKey(row, "max");
                AppendFloat(row, info.max_value);
                row += ',';
                AppendKey(row, "logarithmic");
                row += info.is_logarithmic != 0 ? "true" : "false";
            }
            row += ',';
            AppendKey(row, "value");
            if (info.kind == RENODX_HOST_KIND_BOOLEAN)
                row += cur != 0.0f ? "true" : "false";
            else if (info.kind == RENODX_HOST_KIND_FLOAT)
                AppendFloat(row, cur);
            else
                AppendNumber(row, cur, true);
            row += ',';
            AppendKey(row, "default");
            if (info.kind == RENODX_HOST_KIND_BOOLEAN)
                row += info.default_value != 0.0f ? "true" : "false";
            else if (info.kind == RENODX_HOST_KIND_FLOAT)
                AppendFloat(row, info.default_value);
            else
                AppendNumber(row, info.default_value, true);
        }
        else if (info.kind == RENODX_HOST_KIND_INPUT_TEXT && !key.empty())
        {
            char text[1024] {};
            if (!v4->get_text(key.c_str(), text, sizeof(text)))
                continue;
            row += ',';
            AppendKey(row, "value");
            AppendString(row, text);
            row += ',';
            AppendKey(row, "default");
            AppendString(row, defaultText.c_str());
            row += ',';
            AppendKey(row, "placeholder");
            AppendString(row, placeholder.c_str());
            row += ',';
            AppendKey(row, "maxLength");
            AppendNumber(row, info.text_max_length, true);
            row += ',';
            AppendKey(row, "inputTextFlags");
            AppendNumber(row, info.input_text_flags, true);
        }
        row += '}';

        if (!first)
            s += ',';
        first = false;
        s += row;
    }
    s += ']';
}

void AppendHdr(std::string& s)
{
    const RenoDxHostApi* api = DlssNrRenoDx::Api();
    s += "\"hdr\":{";
    AppendKey(s, "available");
    s += api != nullptr ? "true" : "false";
    if (api == nullptr)
    {
        s += ',';
        AppendKey(s, "reason");
        AppendString(s, DlssNrRenoDx::UnavailableReason());
        s += ",\"settings\":[]}";
        return;
    }

    // WHICH add-on loaded is what a player needs to confirm with a per-game mod -- see DrawRenoDxPage.
    s += ',';
    AppendKey(s, "module");
    AppendString(s, DlssNrRenoDx::ModuleName());
    s += ',';
    AppendKey(s, "addon");
    AppendString(s, DlssNrRenoDx::AddonName());
    s += ',';
    AppendKey(s, "canReset");
    s += DlssNrRenoDx::CanReset() ? "true" : "false";
    s += ",\"settings\":[";

    bool first = true;
    const uint32_t count = api->setting_count();
    for (uint32_t i = 0; i < count; ++i)
    {
        RenoDxHostSetting info {};
        info.struct_size = sizeof(info);
        if (!api->describe_setting(i, &info))
            continue;

        // Hidden by RenoDX's own rule (mostly Settings Mode), or unlabelled internal state: hidden here.
        if (info.is_visible == 0 || !NonEmpty(info.label) || !NonEmpty(info.key))
            continue;

        const char* type = nullptr;
        switch (info.value_type)
        {
        case RENODX_HOST_VALUE_FLOAT:
            type = "float";
            break;
        case RENODX_HOST_VALUE_INTEGER:
            type = "int";
            break;
        case RENODX_HOST_VALUE_BOOLEAN:
            type = "bool";
            break;
        case RENODX_HOST_VALUE_COMBO:
            type = "combo";
            break;
        default:
            break; // TEXT, and anything newer: the in-game page skips them too
        }
        if (type == nullptr)
            continue;

        const bool numeric = info.value_type == RENODX_HOST_VALUE_FLOAT || info.value_type == RENODX_HOST_VALUE_INTEGER;
        if (numeric && info.min_value == info.max_value)
            continue;
        if (info.value_type == RENODX_HOST_VALUE_COMBO && info.label_count == 0)
            continue;

        // BORROWED: RenoDX's strings are good only until the next call into its API, and get_number
        // and label_at below are such calls. Copied first, or the row would carry whatever the next
        // call left behind.
        const std::string key = info.key;
        const std::string label = info.label;
        const std::string section = info.section != nullptr ? info.section : "";
        const std::string tooltip = info.tooltip != nullptr ? info.tooltip : "";

        float cur = 0.0f;
        if (!api->get_number(key.c_str(), &cur))
            continue;

        std::string labels;
        if (info.value_type == RENODX_HOST_VALUE_COMBO)
        {
            const uint32_t n = info.label_count < kMaxComboLabels ? info.label_count : kMaxComboLabels;
            bool complete = true;
            labels += '[';
            for (uint32_t c = 0; c < n && complete; ++c)
            {
                const char* l = api->label_at(i, c);
                if (l == nullptr)
                {
                    complete = false;
                    break;
                }
                if (c > 0)
                    labels += ',';
                AppendString(labels, l); // copied at once, before the next label_at
            }
            labels += ']';
            // The value IS the index; one the labels cannot name is not drawn in-game either.
            const int sel = (int) cur;
            if (!complete || sel < 0 || sel >= (int) n)
                continue;
        }

        if (!first)
            s += ',';
        first = false;

        s += '{';
        AppendKey(s, "key");
        AppendString(s, key.c_str());
        s += ',';
        AppendKey(s, "label");
        AppendString(s, label.c_str());
        s += ',';
        AppendKey(s, "section");
        AppendString(s, section.c_str());
        s += ',';
        AppendKey(s, "tooltip");
        AppendString(s, tooltip.c_str());
        s += ',';
        AppendKey(s, "type");
        AppendString(s, type);
        if (numeric)
        {
            s += ',';
            AppendKey(s, "min");
            AppendFloat(s, info.min_value);
            s += ',';
            AppendKey(s, "max");
            AppendFloat(s, info.max_value);
        }
        if (info.value_type == RENODX_HOST_VALUE_COMBO)
        {
            s += ",\"labels\":";
            s += labels;
        }
        s += ',';
        AppendKey(s, "enabled");
        s += info.is_enabled != 0 ? "true" : "false";
        s += ',';
        AppendKey(s, "value");
        if (info.value_type == RENODX_HOST_VALUE_BOOLEAN)
            s += cur != 0.0f ? "true" : "false";
        else if (info.value_type == RENODX_HOST_VALUE_FLOAT)
            AppendFloat(s, cur);
        else
            AppendNumber(s, cur, true);
        s += '}';
    }
    s += ']';
    AppendHdrV4(s, api);
    s += '}';
}

std::string BuildBody()
{
    std::string s;
    s.reserve(4096);
    AppendPacing(s);
    s += ',';
    AppendHdr(s);
    return s;
}

void Write(const std::filesystem::path& dir, const std::string& body)
{
    std::string json;
    json.reserve(body.size() + 96);
    json += "{\"v\":1,\"pid\":";
    AppendNumber(json, (double) GetCurrentProcessId(), true);
    json += ",\"at\":";
    AppendNumber(json, (double) NowUnixMs(), true);
    json += ",\"ack\":";
    AppendNumber(json, (double) g_ack, true);
    json += ',';
    json += body;
    json += '}';

    // Atomic, exactly as OptiScaler.live.json: a reader sees the old file or the new one, never half.
    const auto tmp = dir / L"OptiScaler.hosted.json.tmp";
    const auto dest = dir / L"OptiScaler.hosted.json";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = h != INVALID_HANDLE_VALUE;
    if (ok)
    {
        DWORD written = 0;
        ok = WriteFile(h, json.data(), (DWORD) json.size(), &written, nullptr) && written == json.size();
        CloseHandle(h);
    }
    if (ok)
        ok = MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;

    if (!ok && !g_writeFailureLogged)
    {
        g_writeFailureLogged = true;
        LOG_WARN("DLSS-NR hosted pages: could not write {} (error {}); the pop-out panel will not show "
                 "Pacing or HDR this session",
                 dest.string(), GetLastError());
    }
}

// ---------------------------------------------------------------------------------------------------
// Applying. Each value is checked against the add-on's own description of the setting -- type, range,
// choices, enabled -- because the app's copy of that description can be a second old, and an add-on's
// setter is not obliged to validate what it is handed.
// ---------------------------------------------------------------------------------------------------

bool NumberOf(const nlohmann::json& v, double* out)
{
    if (v.is_boolean())
        *out = v.get<bool>() ? 1.0 : 0.0;
    else if (v.is_number())
        *out = v.get<double>();
    else
        return false;
    return std::isfinite(*out);
}

void ApplyPacing(const nlohmann::json& obj)
{
    const ReLimiterApi* api = DlssNrReLimiter::Api();
    if (api == nullptr || !obj.is_object() || obj.empty())
        return;

    bool changed = false;
    const uint32_t count = api->setting_count();
    for (auto it = obj.begin(); it != obj.end(); ++it)
    {
        const std::string& key = it.key();
        ReLimiterSettingInfo info {};
        bool found = false;
        for (uint32_t i = 0; i < count && !found; ++i)
        {
            info = {};
            info.struct_size = sizeof(info);
            found = api->describe_setting(i, &info) && info.key != nullptr && key == info.key;
        }
        if (!found)
        {
            LOG_DEBUG("DLSS-NR hosted pages: ReLimiter has no setting {}", key);
            continue;
        }

        const nlohmann::json& v = it.value();
        switch (info.type)
        {
        case RELIMITER_TYPE_BOOL:
        {
            double n = 0.0;
            if (NumberOf(v, &n))
                changed = api->set_number(info.key, n != 0.0 ? 1.0 : 0.0) != 0 || changed;
            break;
        }
        case RELIMITER_TYPE_ENUM:
        {
            if (!v.is_string() || info.choices == nullptr)
                break;
            const std::string want = v.get<std::string>();
            for (uint32_t c = 0; c < info.choice_count; ++c)
            {
                if (info.choices[c] != nullptr && want == info.choices[c])
                {
                    changed = api->set_string(info.key, info.choices[c]) != 0 || changed;
                    break;
                }
            }
            break;
        }
        case RELIMITER_TYPE_INT:
        case RELIMITER_TYPE_FLOAT:
        case RELIMITER_TYPE_DOUBLE:
        {
            double n = 0.0;
            if (!NumberOf(v, &n) || info.min_value == info.max_value)
                break;
            // A labelled zero is a mode, outside the range (target_fps = 0 is "below the VRR ceiling");
            // anything else is held to the range ReLimiter would clamp it to anyway.
            if (!(info.zero_label != nullptr && n == 0.0))
                n = std::clamp(n, info.min_value, info.max_value);
            if (info.type == RELIMITER_TYPE_INT)
                n = std::round(n);
            changed = api->set_number(info.key, n) != 0 || changed;
            break;
        }
        default:
            break; // keybinds and free strings are not offered, so they are not taken either
        }
    }

    // Once for the whole command, as the in-game page does per change: apply pushes it into the running
    // limiter, save makes it survive -- and is what stops ReLimiter's own save on exit undoing it.
    if (changed)
    {
        api->apply();
        api->save();
    }
}

void ApplyHdr(const nlohmann::json& obj)
{
    const RenoDxHostApi* api = DlssNrRenoDx::Api();
    if (api == nullptr || !obj.is_object() || obj.empty())
        return;

    // The pop-out's "Reset all to defaults" button: RenoDX resets and saves itself, then any other keys in
    // the same command still apply on top.
    if (auto reset = obj.find("$reset"); reset != obj.end() && reset->is_boolean() && reset->get<bool>())
    {
        DlssNrRenoDx::ResetAll();
        DlssNrRenoDx::LogCommit("pop-out", "reset all", nullptr, 0.0, true);
    }

    // Version 4 commands: the preset switcher, one setting's reset button, a BUTTON row's click. Each does
    // exactly what the same click does in RenoDX's overlay, including its own saving.
    if (const RenoDxHostApi* v4 = DlssNrRenoDx::V4(); v4 != nullptr)
    {
        double n = 0.0;
        if (auto it = obj.find("$preset"); it != obj.end() && NumberOf(*it, &n) && v4->set_preset != nullptr)
            DlssNrRenoDx::LogCommit("pop-out", "preset", nullptr, n, v4->set_preset((int32_t) std::lround(n)));
        if (auto it = obj.find("$resetSetting"); it != obj.end() && it->is_string() && v4->reset_setting != nullptr)
        {
            const std::string key = it->get<std::string>();
            DlssNrRenoDx::LogCommit("pop-out", "reset", key.c_str(), 0.0, v4->reset_setting(key.c_str()));
        }
        if (auto it = obj.find("$press"); it != obj.end() && NumberOf(*it, &n) && n >= 0.0 && v4->press != nullptr)
            DlssNrRenoDx::LogCommit("pop-out", "press", nullptr, n, v4->press((uint32_t) std::lround(n)));
    }

    bool changed = false;
    const uint32_t count = api->setting_count();
    for (auto it = obj.begin(); it != obj.end(); ++it)
    {
        const std::string& key = it.key();
        if (!key.empty() && key[0] == '$')
            continue; // commands, handled above
        RenoDxHostSetting info {};
        bool found = false;
        for (uint32_t i = 0; i < count && !found; ++i)
        {
            info = {};
            info.struct_size = sizeof(info);
            found = api->describe_setting(i, &info) && info.key != nullptr && key == info.key;
        }
        if (!found)
        {
            LOG_DEBUG("DLSS-NR hosted pages: RenoDX has no setting {}", key);
            continue;
        }
        // Greyed in RenoDX's overlay means the value is ignored right now; not written behind its back.
        if (info.is_enabled == 0)
            continue;

        // A text box's value arrives as a string (version 4 kind INPUT_TEXT, or an older add-on's TEXT).
        const bool textBox = (DlssNrRenoDx::V4() != nullptr && info.kind == RENODX_HOST_KIND_INPUT_TEXT) ||
                             (DlssNrRenoDx::V4() == nullptr && info.value_type == RENODX_HOST_VALUE_TEXT);
        if (textBox)
        {
            if (it.value().is_string())
            {
                const std::string text = it.value().get<std::string>();
                const bool ok = api->set_text(key.c_str(), text.c_str());
                DlssNrRenoDx::LogCommit("pop-out", "text", key.c_str(), (double) text.size(), ok);
                changed = ok || changed;
            }
            continue;
        }

        double n = 0.0;
        if (!NumberOf(it.value(), &n))
            continue;

        switch (info.value_type)
        {
        case RENODX_HOST_VALUE_BOOLEAN:
            n = n != 0.0 ? 1.0 : 0.0;
            break;
        case RENODX_HOST_VALUE_COMBO:
            if (info.label_count == 0)
                continue;
            n = std::clamp(std::round(n), 0.0, (double) (info.label_count - 1));
            break;
        case RENODX_HOST_VALUE_INTEGER:
        case RENODX_HOST_VALUE_FLOAT:
            if (info.min_value == info.max_value)
                continue;
            n = std::clamp(n, (double) info.min_value, (double) info.max_value);
            if (info.value_type == RENODX_HOST_VALUE_INTEGER)
                n = std::round(n);
            break;
        default:
            continue; // TEXT is not offered
        }
        // `key` (ours), not info.key: describe's strings are borrowed and set_number is the next call.
        const bool ok = api->set_number(key.c_str(), (float) n);
        DlssNrRenoDx::LogCommit("pop-out", "set", key.c_str(), n, ok);
        changed = ok || changed;
    }

    // set_number already applied each value live (RenoDX runs its on_change there); save persists.
    if (changed)
        api->save();
}

void CheckCommands(const std::filesystem::path& dir)
{
    const auto path = dir / L"OptiScaler.hosted.set.json";
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return; // no command yet, the ordinary case
    if (data.nFileSizeHigh != 0 || data.nFileSizeLow > kMaxCommandBytes)
        return;
    if (CompareFileTime(&data.ftLastWriteTime, &g_cmdTime) == 0 && data.nFileSizeLow == g_cmdSize)
        return;

    // Shared for write and delete, so this read never blocks the app's rename of its next command.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return; // mid-rename: the stamp is not recorded, so the next check tries again
    std::string text(data.nFileSizeLow, '\0');
    DWORD read = 0;
    const bool ok = text.empty() || (ReadFile(h, text.data(), (DWORD) text.size(), &read, nullptr) != 0);
    CloseHandle(h);
    if (!ok)
        return;
    text.resize(read);

    // Not exceptions: a torn or hand-edited file is an ordinary thing to meet here, and discarded is
    // the answer for it. Its stamp is not recorded either, so the complete file is read when it lands.
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return;
    g_cmdTime = data.ftLastWriteTime;
    g_cmdSize = data.nFileSizeLow;

    // For this process only. The file outlives the game, and the next session starts at ack 0, so
    // without this a stale command -- every change from last time -- would replay into a fresh game.
    const auto pid = j.find("pid");
    if (pid == j.end() || !pid->is_number() || pid->get<double>() != (double) GetCurrentProcessId())
        return;

    const auto seqIt = j.find("seq");
    if (seqIt == j.end() || !seqIt->is_number())
        return;
    const double seqD = seqIt->get<double>();
    if (!std::isfinite(seqD) || seqD < 1.0)
        return;
    const unsigned long long seq = (unsigned long long) seqD;
    if (seq <= g_ack)
        return; // already applied; the app is waiting to see its ack, which the next write carries

    const auto pacing = j.find("pacing");
    if (pacing != j.end())
        ApplyPacing(*pacing);
    const auto hdr = j.find("hdr");
    if (hdr != j.end())
        ApplyHdr(*hdr);

    // Acknowledged even when a key was refused: the app then shows the add-on's real value, which is
    // the honest answer, rather than waiting forever on a change that will never land.
    g_ack = seq;
    g_publishNow = true;
    LOG_DEBUG("DLSS-NR hosted pages: applied command {}", seq);
}
} // namespace

void Tick(const std::filesystem::path& dir, bool requested)
{
    if (!requested || dir.empty())
        return;

    const auto now = Clock::now();
    if (now - g_lastCommandCheck >= kCommandCheckEvery)
    {
        g_lastCommandCheck = now;
        CheckCommands(dir);
    }

    if (!g_publishNow && now - g_lastBuild < kBuildEvery)
        return;
    g_lastBuild = now;

    std::string body = BuildBody();
    const bool changed = g_publishNow || body != g_lastBody;
    if (!changed && now - g_lastWrite < kWriteAtLeastEvery)
        return;

    g_publishNow = false;
    g_lastWrite = now;
    Write(dir, body);
    g_lastBody = std::move(body);
}
} // namespace DlssNr::Hosted

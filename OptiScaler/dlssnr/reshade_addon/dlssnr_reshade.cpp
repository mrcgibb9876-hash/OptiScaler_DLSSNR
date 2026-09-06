// DLSS 5 Neural Rendering, as a ReShade add-on.
//
// Two routes, two different presentations -- deliberately not unified into one look:
//
//  - OptiScaler: the original hand-built panel (status, GPU ms, curated descriptions), driving
//    OptiScaler's own pass through the flat C ABI in ../DlssNr_Api.h, resolved by name at runtime.
//    Shown only in ReShade's own overlay ("DLSS 5" tab) -- Alt+Home is already OptiScaler's own key
//    for its full native panel, so this add-on does not also bind it for that route; doing so would
//    toggle two different windows on the same keypress.
//  - RenoDX (the DLSS5-Feeder toolchain's own neural consumer): a data-driven panel (fields.h),
//    styled to match the OptiScalerManager desktop app's own "Tune DLSS-NR" panel, reachable both in
//    ReShade's own overlay AND its own standalone Alt+Home window (independent of whether ReShade's
//    overlay is open) -- RenoDX has no equivalent full native panel of its own to collide with.
//    Talks to RenoDX through ReShade's own config API (get_config_value/set_config_value) on the
//    RenoDX.DLSS5 section -- the exact same store RenoDX itself reads and writes. RenoDX has no
//    in-process control ABI of its own, so this is the only way in.
//
// Which route is live is resolved every frame, not once at load, since either engine can finish
// loading after this add-on does.
//
// Build: see the vcxproj beside this file. It needs ReShade's addon SDK headers (reshade.hpp) and
// Dear ImGui 1.92.2b-docking headers -- ImGui is NOT compiled in, ReShade supplies the instance.

// IMGUI_DISABLE_INCLUDE_IMCONFIG_H comes from the vcxproj, not from here -- defining it in both
// places is a macro redefinition warning, and the build is the right place for it because anything
// else compiled into this add-on needs it too.
#include <imgui.h>

// Must come after imgui.h: including it in this order is what rebinds every ImGui function to the
// instance ReShade created, so this add-on draws into ReShade's context rather than one of its own.
#include <reshade.hpp>

// Everything below is resolved by name at runtime, so this only needs the declarations for their
// types. OPTINR_CONSUMER keeps the header from marking them dllexport here, which would otherwise
// re-export OptiScaler's names from this add-on.
#define OPTINR_CONSUMER
#include "../DlssNr_Api.h"

#include "fields.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

using namespace dlssnr_tune;

namespace
{

// ---------------------------------------------------------------------------------------------
// The link to OptiScaler.
// ---------------------------------------------------------------------------------------------

struct OptiNrLink
{
    decltype(&OptiNr_AbiVersion) AbiVersion = nullptr;
    decltype(&OptiNr_GetStatus) GetStatus = nullptr;
    decltype(&OptiNr_GetFloat) GetFloat = nullptr;
    decltype(&OptiNr_SetFloat) SetFloat = nullptr;
    decltype(&OptiNr_GetInt) GetInt = nullptr;
    decltype(&OptiNr_SetInt) SetInt = nullptr;
    decltype(&OptiNr_GetBool) GetBool = nullptr;
    decltype(&OptiNr_SetBool) SetBool = nullptr;
    decltype(&OptiNr_Save) Save = nullptr;
    decltype(&OptiNr_RetryAfterFailure) RetryAfterFailure = nullptr;

    bool ready = false;

    // Why it is not ready, for the overlay to show. A person whose panel is empty deserves to be
    // told which of the several possible reasons applies rather than being left to guess.
    const char* problem = "Looking for OptiScaler...";
};

OptiNrLink g_link;

// OptiScaler masquerades as whichever library the game loads it as, so its module name is not fixed.
// These are the names it actually ships under, plus its own -- checked in the order that puts the
// unambiguous one first.
const wchar_t* const kModuleNames[] = {
    L"OptiScaler.dll", L"dxgi.dll",    L"winmm.dll",   L"version.dll",    L"dbghelp.dll",
    L"d3d12.dll",      L"wininet.dll", L"nvapi64.dll", L"OptiScaler.asi",
};

// Resolving is attempted once per frame until it succeeds, and never again after. It cannot be done
// at DllMain time: ReShade loads add-ons early and OptiScaler may not be in the process yet.
void TryResolve()
{
    if (g_link.ready)
        return;

    for (const wchar_t* name : kModuleNames)
    {
        HMODULE module = GetModuleHandleW(name);

        if (module == nullptr)
            continue;

        // Every module in that list is a real Windows library under its own name, so finding one
        // proves nothing. Only the presence of this export says it is OptiScaler with the ABI.
        auto abiVersion = (decltype(&OptiNr_AbiVersion)) GetProcAddress(module, "OptiNr_AbiVersion");

        if (abiVersion == nullptr)
            continue;

        const int32_t version = abiVersion();

        if (version != OPTINR_ABI_VERSION)
        {
            // Deliberately fatal rather than best-effort. A mismatched ABI is exactly the situation
            // where guessing produces a panel that looks right and writes to the wrong places.
            static char message[160];
            snprintf(message, sizeof(message),
                     "OptiScaler is here but speaks control interface v%d; this add-on speaks v%d. "
                     "Update whichever is older.",
                     (int) version, (int) OPTINR_ABI_VERSION);
            g_link.problem = message;
            return;
        }

        g_link.AbiVersion = abiVersion;
        g_link.GetStatus = (decltype(&OptiNr_GetStatus)) GetProcAddress(module, "OptiNr_GetStatus");
        g_link.GetFloat = (decltype(&OptiNr_GetFloat)) GetProcAddress(module, "OptiNr_GetFloat");
        g_link.SetFloat = (decltype(&OptiNr_SetFloat)) GetProcAddress(module, "OptiNr_SetFloat");
        g_link.GetInt = (decltype(&OptiNr_GetInt)) GetProcAddress(module, "OptiNr_GetInt");
        g_link.SetInt = (decltype(&OptiNr_SetInt)) GetProcAddress(module, "OptiNr_SetInt");
        g_link.GetBool = (decltype(&OptiNr_GetBool)) GetProcAddress(module, "OptiNr_GetBool");
        g_link.SetBool = (decltype(&OptiNr_SetBool)) GetProcAddress(module, "OptiNr_SetBool");
        g_link.Save = (decltype(&OptiNr_Save)) GetProcAddress(module, "OptiNr_Save");
        g_link.RetryAfterFailure =
            (decltype(&OptiNr_RetryAfterFailure)) GetProcAddress(module, "OptiNr_RetryAfterFailure");

        // All or nothing: a half-resolved link would fail at the first null call rather than here,
        // in the middle of someone's frame.
        if (g_link.GetStatus == nullptr || g_link.GetFloat == nullptr || g_link.SetFloat == nullptr ||
            g_link.GetInt == nullptr || g_link.SetInt == nullptr || g_link.GetBool == nullptr ||
            g_link.SetBool == nullptr || g_link.Save == nullptr || g_link.RetryAfterFailure == nullptr)
        {
            g_link = OptiNrLink {};
            g_link.problem = "OptiScaler's control interface is incomplete -- the build looks damaged.";
            return;
        }

        g_link.ready = true;
        g_link.problem = "";
        return;
    }

    g_link.problem = "OptiScaler is not loaded in this game.";
}

// ---------------------------------------------------------------------------------------------
// Presence of RenoDX -- checked by module name, the same way OptiScaler's presence is: ReShade
// loads every add-on as a real DLL into the process under its own file name.
// ---------------------------------------------------------------------------------------------

bool g_renodx_present = false;

void TryResolveRenoDx()
{
    if (g_renodx_present)
        return;

    g_renodx_present = GetModuleHandleW(L"renodx-dlss5.addon64") != nullptr;
}

enum class Backend
{
    None,
    OptiScaler,
    RenoDx
};

Backend ResolveBackend()
{
    TryResolve();
    if (g_link.ready)
        return Backend::OptiScaler;

    TryResolveRenoDx();
    if (g_renodx_present)
        return Backend::RenoDx;

    return Backend::None;
}

// ---------------------------------------------------------------------------------------------
// OptiScaler backend -- small wrappers, so the drawing code below reads like ordinary settings
// code. Every one of them fails soft: a key this OptiScaler does not have returns the fallback and
// the control still draws, it just does nothing.
// ---------------------------------------------------------------------------------------------

float GetF(const char* key, float fallback)
{
    float v = fallback;
    return g_link.GetFloat(key, &v) == OPTINR_OK ? v : fallback;
}

int GetI(const char* key, int fallback)
{
    int32_t v = fallback;
    return g_link.GetInt(key, &v) == OPTINR_OK ? (int) v : fallback;
}

bool GetB(const char* key, bool fallback)
{
    int32_t v = fallback ? 1 : 0;
    return g_link.GetBool(key, &v) == OPTINR_OK ? v != 0 : fallback;
}

// Returns true when the key exists at all, so a control can grey itself out rather than lying.
bool HasF(const char* key)
{
    float v = 0.0f;
    return g_link.GetFloat(key, &v) == OPTINR_OK;
}

bool SliderF(const char* label, const char* key, float mn, float mx, const char* fmt = "%.2f",
             ImGuiSliderFlags flags = 0)
{
    const bool present = HasF(key);

    ImGui::BeginDisabled(!present);

    float v = GetF(key, mn);

    if (ImGui::SliderFloat(label, &v, mn, mx, fmt, flags) && present)
        g_link.SetFloat(key, v);

    const bool done = ImGui::IsItemDeactivatedAfterEdit();

    ImGui::EndDisabled();

    if (!present)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(not in this OptiScaler)");
    }

    return done && present;
}

bool CheckB(const char* label, const char* key)
{
    bool v = GetB(key, false);

    if (ImGui::Checkbox(label, &v))
    {
        g_link.SetBool(key, v ? 1 : 0);
        return true;
    }

    return false;
}

bool ComboI(const char* label, const char* key, const char* const* items, int count)
{
    int v = GetI(key, 0);

    if (v < 0 || v >= count)
        v = 0;

    if (ImGui::Combo(label, &v, items, count))
    {
        g_link.SetInt(key, v);
        return true;
    }

    return false;
}

void Help(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    // BeginItemTooltip is IsItemHovered + BeginTooltip in one, and it returns false when the
    // tooltip window was not begun. EndTooltip must only follow a true -- calling it regardless pops
    // whatever window IS current, which is ReShade's own.
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ---------------------------------------------------------------------------------------------
// The OptiScaler panel -- unchanged from the original hand-built version.
// ---------------------------------------------------------------------------------------------

void DrawOptiScalerControls()
{
    OptiNr_Status status {};
    status.structSize = sizeof(status);

    const bool haveStatus = g_link.GetStatus(&status) == OPTINR_OK;

    // Anything the user changed this frame. The ini is written once at the end rather than per
    // control, so a drag across three sliders is one write.
    bool changed = false;

    changed |= CheckB("Enable Neural Rendering", "Enabled");
    Help("Synthesises detail in the upscaler's output, before frame generation sees it.\n\n"
         "Needs nvngx_dlssnr.dll beside OptiScaler, plus the forwarder that ships with it.");

    if (haveStatus)
    {
        if (status.failureReason != nullptr && status.failureReason[0] != 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Off for this session: %s", status.failureReason);
            ImGui::SameLine();

            if (ImGui::SmallButton("Retry"))
                g_link.RetryAfterFailure();
        }
        else if (status.running || status.runningVulkan)
        {
            const char* where = status.runningVulkan ? " natively on Vulkan" : "";

            // Negative means nothing has been measured yet, which is not the same as free.
            if (status.gpuMs >= 0.0)
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running%s - %.2f ms per frame", where,
                                   status.gpuMs);
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running%s", where);
        }
        else if (status.enabled)
        {
            ImGui::TextDisabled("Waiting for the upscaler to run. Needs DLSS or XeSS selected in the "
                                "game's own video settings, and a save loaded.");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Strength", ImGuiTreeNodeFlags_DefaultOpen))
    {
        changed |= SliderF("Detail strength", "TransferStrength", 0.0f, 2.0f);
        Help("How far the frame moves toward the model's picture. 0 gives back exactly what the "
             "upscaler produced, 1 is the model's picture, above 1 carries on past it.");

        changed |= SliderF("Colour strength", "ColourStrength", 0.0f, 4.0f);
        Help("Whether the model's colour arrives with its light. 0 keeps the game's own hue exactly. "
             "Above 1 over-saturates, keeping hue but growing more vivid.");

        changed |= SliderF("Structure intensity", "LocalStructure", 0.0f, 2.0f);
        changed |= SliderF("Tone intensity", "LocalTone", 0.0f, 2.0f);
    }

    if (ImGui::CollapsingHeader("Colour", ImGuiTreeNodeFlags_DefaultOpen))
    {
        static const char* const kSources[] = { "Paper white only", "The game's own exposure",
                                                "A buffer the scan found" };
        changed |= ComboI("White point from", "WhitePointSource", kSources, 3);
        Help("Where the number that divides the frame comes from. The game's own exposure is the best "
             "source there is, because it is decided upstream and nothing this pass does can move it "
             "-- but not every game supplies one.");

        const int source = GetI("WhitePointSource", 0);

        if (source == 1)
        {
            changed |= SliderF("Trim (x the game's exposure)", "WhitePointTrim", 0.25f, 4.0f, "%.2fx",
                               ImGuiSliderFlags_Logarithmic);
            Help("A multiplier on the exposure the game supplied. 1.00x takes its number exactly, and "
                 "that is the right answer here. Needing it far from 1 is evidence the exposure being "
                 "read is wrong for that game, not that the game wants trimming.");
        }
        else
        {
            changed |= SliderF("Paper white", "WhitePointScale", 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
            Help("What the frame is divided by before the model sees it. Raise it until the picture "
                 "stops improving -- past that point it does not plateau, it gets worse the other way.");
        }

        changed |= SliderF("Highlight guard", "MaxRatio", 1.0f, 8.0f, "%.1fx");
        Help("The most the pass may move any pixel, as a multiple of what it already was. Raise it "
             "only if bright areas look clipped.");

        static const char* const kReversible[] = { "Off (soft knee)", "Neutwo proxy + composed",
                                                   "Neutwo proxy + replace", "Hybrid proxy + composed",
                                                   "Hybrid proxy + replace" };
        changed |= ComboI("Reversible proxy", "ReversibleMode", kReversible, 5);
        Help("What the model is shown, and how its answer comes back. Hybrid composed is the one to "
             "use: identity in the midtones, unclipped roll only in the highlights. Off is the "
             "original behaviour.");
    }

    if (ImGui::CollapsingHeader("Cost"))
    {
        // 25..200: above 100 the model runs above native and is filtered back down.
        changed |= SliderF("Model resolution", "WorkingScale", 0.25f, 2.0f, "%.2fx");
        Help("What fraction of the frame the model works at. Cost falls with the square of this. "
             "Above 1.00x it supersamples -- experimental, and time grows with the area.");
    }

    if (ImGui::CollapsingHeader("Compare"))
    {
        changed |= CheckB("Apply the model", "ApplyModel");
        Help("Whether the model's edit is applied. Off shows the clean upscaler frame while the pass "
             "keeps running, so with Hold frame you can freeze one frame and toggle this to see it "
             "with and without.");

        changed |= CheckB("Hold frame", "HoldFrame");
        Help("Freezes the frame the model works on, so a setting change re-renders it in place. The "
             "only clean way to A/B settings, since a moving scene confounds everything else.");

        static const char* const kCompare[] = { "Off", "Side by side", "Wipe" };
        changed |= ComboI("Compare", "Compare", kCompare, 3);

        if (GetI("Compare", 0) != 0)
        {
            changed |= CheckB("Swap sides", "CompareSwap");
            changed |= SliderF("Split", "CompareSplit", 0.0f, 1.0f);
        }

        static const char* const kDebug[] = { "Off", "What the model sees", "Its raw answer", "What it changed, x20" };
        changed |= ComboI("Debug view", "DebugView", kDebug, 4);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // The full panel has a good deal more than this -- the exposure-scan anchoring workflow, the
    // model presets and styles, frame generation. Saying so is better than letting someone conclude
    // this is all there is.
    ImGui::TextDisabled("OptiScaler's own DLSS 5 panel has the rest: model presets, frame generation, "
                        "and the exposure-scan anchoring. Open it with its own key (Alt+Home by default).");

    if (changed)
        g_link.Save();
}

// ---------------------------------------------------------------------------------------------
// RenoDX backend -- reads/writes through ReShade's own config API on RenoDX.DLSS5, the store
// RenoDX itself uses. Data-driven off fields.h's feeder_table.
// ---------------------------------------------------------------------------------------------

double reshade_get(reshade::api::effect_runtime* runtime, const field& f)
{
    switch (f.type)
    {
    case field_type::boolean:
    {
        bool v = f.default_value != 0;
        reshade::get_config_value(runtime, feeder_table.section, f.key, v);
        return v ? 1.0 : 0.0;
    }
    case field_type::integer:
    case field_type::enumeration:
    {
        int v = static_cast<int>(f.default_value);
        reshade::get_config_value(runtime, feeder_table.section, f.key, v);
        if (f.type == field_type::enumeration && (v < 0 || v >= f.option_count))
            v = static_cast<int>(f.default_value);
        return v;
    }
    case field_type::floating:
    default:
    {
        float v = static_cast<float>(f.default_value);
        reshade::get_config_value(runtime, feeder_table.section, f.key, v);
        return v;
    }
    }
}

void reshade_set(reshade::api::effect_runtime* runtime, const field& f, double value)
{
    switch (f.type)
    {
    case field_type::boolean: reshade::set_config_value(runtime, feeder_table.section, f.key, value != 0); break;
    case field_type::integer:
    case field_type::enumeration:
        reshade::set_config_value(runtime, feeder_table.section, f.key, static_cast<int>(value));
        break;
    case field_type::floating:
    default: reshade::set_config_value(runtime, feeder_table.section, f.key, static_cast<float>(value)); break;
    }
}

const field* find_feeder_field(const char* key)
{
    for (int i = 0; i < feeder_table.count; ++i)
        if (strcmp(feeder_table.fields[i].key, key) == 0)
            return &feeder_table.fields[i];
    return nullptr;
}

bool renodx_condition_met(reshade::api::effect_runtime* runtime, const condition& c)
{
    if (!c.active())
        return true;
    const field* dep = find_feeder_field(c.key);
    if (!dep)
        return true;
    const int v = static_cast<int>(reshade_get(runtime, *dep));
    for (int i = 0; i < c.value_count; ++i)
        if (c.values[i] == v)
            return true;
    return false;
}

void push_style()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0x12 / 255.f, 0x17 / 255.f, 0x1f / 255.f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0x12 / 255.f, 0x17 / 255.f, 0x1f / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0x12 / 255.f, 0x17 / 255.f, 0x1f / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0x1b / 255.f, 0x22 / 255.f, 0x2c / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0x22 / 255.f, 0x2b / 255.f, 0x37 / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0x22 / 255.f, 0x2b / 255.f, 0x37 / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0xf0 / 255.f, 0xf4 / 255.f, 0xf9 / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0x62 / 255.f, 0x6e / 255.f, 0x7c / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0x6c / 255.f, 0xc1 / 255.f, 0x0a / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0x4d / 255.f, 0x92 / 255.f, 0x00 / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0x6c / 255.f, 0xc1 / 255.f, 0x0a / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0x1b / 255.f, 0x22 / 255.f, 0x2c / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0x22 / 255.f, 0x2b / 255.f, 0x37 / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0x22 / 255.f, 0x2b / 255.f, 0x37 / 255.f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.f, 1.f, 1.f, 0.055f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.075f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0x1b / 255.f, 0x22 / 255.f, 0x2c / 255.f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 9.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 9.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 18.0f));
}

void pop_style()
{
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(16);
}

void group_heading(const char* group)
{
    ImGui::Spacing();
    ImGui::TextDisabled("%s", group_title(group));
    ImGui::Separator();
    ImGui::Spacing();
}

// A slightly-narrower-than-default fixed item width caused labels to run into the value box in a
// ReShade overlay narrower than the standalone window's own default size -- scale it to the current
// font instead of a fixed pixel count, so it holds up at different UI scales and window widths.
float ItemWidth()
{
    return -(ImGui::GetFontSize() * 9.0f);
}

bool draw_field(reshade::api::effect_runtime* runtime, const field& f)
{
    if (!renodx_condition_met(runtime, f.show_if))
        return false;

    const bool enabled = renodx_condition_met(runtime, f.disable_if);
    ImGui::BeginDisabled(!enabled);
    ImGui::PushID(f.key);
    ImGui::PushItemWidth(ItemWidth());

    bool changed = false;
    double value = reshade_get(runtime, f);

    switch (f.type)
    {
    case field_type::boolean:
    {
        bool b = value != 0;
        if (ImGui::Checkbox(f.label, &b))
        {
            reshade_set(runtime, f, b ? 1.0 : 0.0);
            changed = true;
        }
        break;
    }
    case field_type::enumeration:
    {
        int idx = static_cast<int>(value);
        if (ImGui::Combo(f.label, &idx, f.options, f.option_count))
        {
            reshade_set(runtime, f, idx);
            changed = true;
        }
        break;
    }
    case field_type::integer:
    {
        int v = static_cast<int>(value);
        if (ImGui::InputInt(f.label, &v))
        {
            reshade_set(runtime, f, v);
            changed = true;
        }
        break;
    }
    case field_type::floating:
    default:
    {
        float v = static_cast<float>(value);
        const char* fmt = f.percent ? "%.0f%%" : "%.2f";
        float shown = f.percent ? v * 100.0f : v;
        const float mn = f.percent ? static_cast<float>(f.min * 100.0) : static_cast<float>(f.min);
        const float mx = f.percent ? static_cast<float>(f.max * 100.0) : static_cast<float>(f.max);
        if (ImGui::SliderFloat(f.label, &shown, mn, mx, fmt))
        {
            reshade_set(runtime, f, f.percent ? shown / 100.0f : shown);
            changed = true;
        }
        break;
    }
    }

    if (f.help != nullptr && ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(f.help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::EndDisabled();

    return changed;
}

// RenoDX keeps its neural-rendering on/off state in memory only -- confirmed by inspecting the real
// add-on's own strings, there is no persisted key for it anywhere, only its own ReShade-overlay
// checkbox ("RenoDX" tab -> "Enable DLSS Neural Rendering") or its own hotkey. There is therefore no
// config value this add-on could show as a real on/off control. What it CAN do: hold the same
// toggle-key binding RenoDX itself polls for (NRToggleKey, already a field below) and, on a button
// press, synthesise that exact key press so RenoDX sees it next frame -- the same effect as pressing
// it yourself, without having to leave this window. If no key is bound yet, F10 is bound first.
//
// Unverified in a real game -- SendInput posts to the system input stream the same way a physical
// key does, which is what ReShade's own is_key_down/is_key_pressed observe, but this has not been
// confirmed against a live RenoDX instance.
void SimulateKeyPress(int vk)
{
    INPUT down {};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = static_cast<WORD>(vk);

    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_KEYUP;

    INPUT inputs[2] = { down, up };
    SendInput(2, inputs, sizeof(INPUT));
}

constexpr int kDefaultToggleKey = VK_F10;

void DrawEnableButton(reshade::api::effect_runtime* runtime)
{
    const field* toggleField = find_feeder_field("NRToggleKey");
    int toggleKey = toggleField ? static_cast<int>(reshade_get(runtime, *toggleField)) : 0;

    if (ImGui::Button("Enable / toggle Neural Rendering", ImVec2(-1.0f, 0.0f)))
    {
        if (toggleKey == 0 && toggleField != nullptr)
        {
            toggleKey = kDefaultToggleKey;
            reshade_set(runtime, *toggleField, toggleKey);
        }

        if (toggleKey != 0)
            SimulateKeyPress(toggleKey);
    }

    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    if (toggleKey == 0)
        ImGui::TextDisabled("No toggle key bound yet -- clicking this will bind F10 and press it.");
    else
        ImGui::TextDisabled("RenoDX keeps this state in memory only, not in any config -- this sends its "
                            "configured toggle key (virtual-key code %d) rather than writing a value.",
                            toggleKey);
    ImGui::PopTextWrapPos();
}

void DrawRenoDxControls(reshade::api::effect_runtime* runtime)
{
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextDisabled("Driving RenoDX's own DLSS 5 Neural Rendering config -- the same store its own "
                        "\"RenoDX\" ReShade tab reads and writes.");
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    DrawEnableButton(runtime);
    ImGui::Spacing();
    ImGui::Separator();

    const char* current_group = nullptr;
    for (int i = 0; i < feeder_table.count; ++i)
    {
        const field& f = feeder_table.fields[i];
        if (f.advanced)
            continue;
        if (current_group == nullptr || strcmp(current_group, f.group) != 0)
        {
            current_group = f.group;
            group_heading(current_group);
        }
        draw_field(runtime, f);
    }
}

// ---------------------------------------------------------------------------------------------
// Entry points.
// ---------------------------------------------------------------------------------------------

void DrawOverlay(reshade::api::effect_runtime* runtime)
{
    const Backend backend = ResolveBackend();

    if (backend == Backend::OptiScaler)
    {
        DrawOptiScalerControls();
        return;
    }

    if (backend == Backend::RenoDx)
    {
        push_style();
        DrawRenoDxControls(runtime);
        pop_style();
        return;
    }

    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextWrapped("Neither OptiScaler nor RenoDX (the DLSS5-Feeder toolchain's neural consumer) were "
                       "detected driving DLSS 5 Neural Rendering in this game.");
    ImGui::Spacing();
    ImGui::TextDisabled("This add-on draws controls for whichever one is actually running the pass -- it "
                        "does not run it itself. If OptiScaler should be here: %s", g_link.problem);
    ImGui::PopTextWrapPos();
}

// Alt+Home toggles a standalone window with the RenoDX panel's content, independent of ReShade's
// own overlay-open state -- mirrors how RenoDX implements its own "NR toggle" hotkey (per-frame
// is_key_down/is_key_pressed poll). Deliberately inert for the OptiScaler route and when neither
// engine is present: OptiScaler already has its own Alt+Home binding for its full native panel, and
// a second window on the same key would toggle both at once.
void OnReshadeOverlayStandalone(reshade::api::effect_runtime* runtime)
{
    static bool visible = false;

    if (ResolveBackend() != Backend::RenoDx)
        return;

    if (runtime->is_key_down(VK_MENU) && runtime->is_key_pressed(VK_HOME))
        visible = !visible;

    if (!visible)
        return;

    // Without this, ReShade never routes mouse/keyboard input to this window unless its own
    // overlay is separately open -- clicks fall straight through to the game.
    runtime->block_input_next_frame();

    push_style();
    ImGui::SetNextWindowSize(ImVec2(520.0f, 680.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 320.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("DLSS 5 Neural Rendering##dlssnr_alt_home", &visible, ImGuiWindowFlags_NoCollapse))
        DrawRenoDxControls(runtime);
    ImGui::End();
    pop_style();
}

} // namespace

extern "C" __declspec(dllexport) const char* NAME = "OptiScaler DLSS 5 Neural Rendering";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Controls OptiScaler's DLSS 5 Neural Rendering from ReShade's own overlay, or RenoDX's (the "
    "DLSS5-Feeder toolchain) from ReShade's overlay or its own Alt+Home window.";

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hinstDLL))
            return FALSE;

        // Registered under a title, so it gets its own window in ReShade's overlay rather than
        // being buried in the add-on settings list.
        reshade::register_overlay("DLSS 5", &DrawOverlay);
        reshade::register_event<reshade::addon_event::reshade_overlay>(OnReshadeOverlayStandalone);
        break;

    case DLL_PROCESS_DETACH:
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(OnReshadeOverlayStandalone);
        reshade::unregister_addon(hinstDLL);
        break;
    }

    return TRUE;
}

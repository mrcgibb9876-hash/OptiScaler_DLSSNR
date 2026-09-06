// DLSS 5 Neural Rendering, as a ReShade add-on.
//
// What this is, and what it deliberately is not.
//
// It is NOT a port of the pass. Whichever engine is actually driving DLSS 5 Neural Rendering --
// OptiScaler or RenoDX (the DLSS5-Feeder toolchain's own neural consumer) -- keeps doing so; this
// add-on is a FRONT END that reaches across and drives whichever one it finds running, drawing one
// identical set of controls either inside ReShade's own overlay (its own overlay key, alongside
// RenoDX's own tab) or in a standalone window toggled with Alt+Home, independent of whether
// ReShade's own overlay is open -- so it works the same way whichever route a game was set up with.
//
// Two backends, chosen live every frame (not once at load, since either engine can finish loading
// after this add-on does):
//
//  - OptiScaler: through the flat C ABI in ../DlssNr_Api.h, resolved by name at runtime. This
//    add-on does not link against OptiScaler and cannot crash it by being out of date; an
//    OptiScaler without the ABI is detected and reported, not crashed into.
//  - RenoDX: through ReShade's own config API (get_config_value/set_config_value) on the
//    RenoDX.DLSS5 section -- the exact same store RenoDX itself reads and writes, so there is no
//    cache/writeback race. RenoDX has no in-process control ABI of its own to link against, so this
//    is the only way in; presence is detected by module name, the same way OptiScaler's is.
//
// Both backends draw from the same data-driven field table (fields.h) and the same styling, matched
// to the OptiScalerManager desktop app's own "Tune DLSS-NR" panel, so the desktop panel and both
// in-game routes present one identical product.
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
// Presence of RenoDX -- the DLSS5-Feeder toolchain's own neural consumer, and the other engine
// this add-on can drive. Checked by module name, the same way OptiScaler's presence is: ReShade
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
// Backend-agnostic field access. Every value is read and written by key -- see fields.h's own
// comment on why -- so a field this build of either engine does not have simply keeps whatever
// the control shows without erroring.
// ---------------------------------------------------------------------------------------------

double opti_get(const field& f)
{
    switch (f.type)
    {
    case field_type::boolean:
    {
        int32_t v = f.default_value != 0;
        return g_link.GetBool(f.key, &v) == OPTINR_OK ? (v != 0 ? 1.0 : 0.0) : f.default_value;
    }
    case field_type::integer:
    case field_type::enumeration:
    {
        int32_t v = static_cast<int32_t>(f.default_value);
        return g_link.GetInt(f.key, &v) == OPTINR_OK ? static_cast<double>(v) : f.default_value;
    }
    case field_type::floating:
    default:
    {
        float v = static_cast<float>(f.default_value);
        return g_link.GetFloat(f.key, &v) == OPTINR_OK ? static_cast<double>(v) : f.default_value;
    }
    }
}

void opti_set(const field& f, double value)
{
    switch (f.type)
    {
    case field_type::boolean: g_link.SetBool(f.key, value != 0 ? 1 : 0); break;
    case field_type::integer:
    case field_type::enumeration: g_link.SetInt(f.key, static_cast<int32_t>(value)); break;
    case field_type::floating:
    default: g_link.SetFloat(f.key, static_cast<float>(value)); break;
    }
}

double reshade_get(reshade::api::effect_runtime* runtime, const char* section, const field& f)
{
    switch (f.type)
    {
    case field_type::boolean:
    {
        bool v = f.default_value != 0;
        reshade::get_config_value(runtime, section, f.key, v);
        return v ? 1.0 : 0.0;
    }
    case field_type::integer:
    case field_type::enumeration:
    {
        int v = static_cast<int>(f.default_value);
        reshade::get_config_value(runtime, section, f.key, v);
        if (f.type == field_type::enumeration && (v < 0 || v >= f.option_count))
            v = static_cast<int>(f.default_value);
        return v;
    }
    case field_type::floating:
    default:
    {
        float v = static_cast<float>(f.default_value);
        reshade::get_config_value(runtime, section, f.key, v);
        return v;
    }
    }
}

void reshade_set(reshade::api::effect_runtime* runtime, const char* section, const field& f, double value)
{
    switch (f.type)
    {
    case field_type::boolean: reshade::set_config_value(runtime, section, f.key, value != 0); break;
    case field_type::integer:
    case field_type::enumeration: reshade::set_config_value(runtime, section, f.key, static_cast<int>(value)); break;
    case field_type::floating:
    default: reshade::set_config_value(runtime, section, f.key, static_cast<float>(value)); break;
    }
}

double get_value(reshade::api::effect_runtime* runtime, Backend backend, const field_table* table, const field& f)
{
    return backend == Backend::OptiScaler ? opti_get(f) : reshade_get(runtime, table->section, f);
}

void set_value(reshade::api::effect_runtime* runtime, Backend backend, const field_table* table, const field& f,
               double value)
{
    if (backend == Backend::OptiScaler)
        opti_set(f, value);
    else
        reshade_set(runtime, table->section, f, value);
}

const field* find_field(const field_table* table, const char* key)
{
    for (int i = 0; i < table->count; ++i)
        if (strcmp(table->fields[i].key, key) == 0)
            return &table->fields[i];
    return nullptr;
}

bool condition_met(reshade::api::effect_runtime* runtime, Backend backend, const field_table* table,
                    const condition& c)
{
    if (!c.active())
        return true;
    const field* dep = find_field(table, c.key);
    if (!dep)
        return true;
    const int v = static_cast<int>(get_value(runtime, backend, table, *dep));
    for (int i = 0; i < c.value_count; ++i)
        if (c.values[i] == v)
            return true;
    return false;
}

// ---------------------------------------------------------------------------------------------
// Styling, matched to the OptiScalerManager desktop app's own "Tune DLSS-NR" panel (its
// src/renderer/style.css .tune-* rules), so the in-game panel -- either route -- and the
// out-of-game one read as the same product.
// ---------------------------------------------------------------------------------------------

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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 22.0f));
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

void Help(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool draw_field(reshade::api::effect_runtime* runtime, Backend backend, const field_table* table, const field& f)
{
    if (!condition_met(runtime, backend, table, f.show_if))
        return false;

    const bool enabled = condition_met(runtime, backend, table, f.disable_if);
    ImGui::BeginDisabled(!enabled);
    ImGui::PushID(f.key);
    ImGui::PushItemWidth(-160.0f);

    bool changed = false;
    double value = get_value(runtime, backend, table, f);

    switch (f.type)
    {
    case field_type::boolean:
    {
        bool b = value != 0;
        if (ImGui::Checkbox(f.label, &b))
        {
            set_value(runtime, backend, table, f, b ? 1.0 : 0.0);
            changed = true;
        }
        break;
    }
    case field_type::enumeration:
    {
        int idx = static_cast<int>(value);
        if (ImGui::Combo(f.label, &idx, f.options, f.option_count))
        {
            set_value(runtime, backend, table, f, idx);
            changed = true;
        }
        break;
    }
    case field_type::integer:
    {
        int v = static_cast<int>(value);
        if (ImGui::InputInt(f.label, &v))
        {
            set_value(runtime, backend, table, f, v);
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
            set_value(runtime, backend, table, f, f.percent ? shown / 100.0f : shown);
            changed = true;
        }
        break;
    }
    }

    if (f.help != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", f.help);

    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::EndDisabled();

    return changed;
}

// ---------------------------------------------------------------------------------------------
// The shared panel content -- drawn identically whether it is hosted in ReShade's own overlay
// (register_overlay, below) or in the standalone Alt+Home window (on_reshade_overlay, below).
// ---------------------------------------------------------------------------------------------

void DrawControls(reshade::api::effect_runtime* runtime)
{
    const Backend backend = ResolveBackend();

    if (backend == Backend::None)
    {
        ImGui::TextWrapped("Neither OptiScaler nor RenoDX (the DLSS5-Feeder toolchain's neural consumer) were "
                           "detected driving DLSS 5 Neural Rendering in this game.");
        ImGui::Spacing();
        ImGui::TextDisabled("This add-on draws controls for whichever one is actually running the pass -- it "
                            "does not run it itself. If OptiScaler should be here: %s", g_link.problem);
        return;
    }

    const field_table* table = backend == Backend::OptiScaler ? &optiscaler_table : &feeder_table;
    bool changed = false;

    if (backend == Backend::OptiScaler)
    {
        OptiNr_Status status {};
        status.structSize = sizeof(status);

        const bool haveStatus = g_link.GetStatus(&status) == OPTINR_OK;

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
    }
    else
    {
        ImGui::TextDisabled("Driving RenoDX's own DLSS 5 Neural Rendering config -- the same store its own "
                            "\"RenoDX\" ReShade tab reads and writes.");
    }

    ImGui::Spacing();
    ImGui::Separator();

    const char* current_group = nullptr;
    for (int i = 0; i < table->count; ++i)
    {
        const field& f = table->fields[i];
        if (f.advanced)
            continue;
        if (current_group == nullptr || strcmp(current_group, f.group) != 0)
        {
            current_group = f.group;
            group_heading(current_group);
        }
        changed |= draw_field(runtime, backend, table, f);
    }

    if (backend == Backend::OptiScaler && changed)
        g_link.Save();
}

void DrawOverlay(reshade::api::effect_runtime* runtime)
{
    DrawControls(runtime);
}

// Alt+Home toggles a standalone window with the same content, independent of ReShade's own
// overlay-open state -- mirrors how RenoDX implements its own "NR toggle" hotkey (per-frame
// is_key_down/is_key_pressed poll), so this works whether or not ReShade's overlay is open.
void OnReshadeOverlayStandalone(reshade::api::effect_runtime* runtime)
{
    static bool visible = false;

    if (runtime->is_key_down(VK_MENU) && runtime->is_key_pressed(VK_HOME))
        visible = !visible;

    if (!visible)
        return;

    // Without this, ReShade never routes mouse/keyboard input to this window unless its own
    // overlay is separately open -- clicks fall straight through to the game.
    runtime->block_input_next_frame();

    push_style();
    ImGui::SetNextWindowSize(ImVec2(480.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("DLSS 5 Neural Rendering##dlssnr_alt_home", &visible, ImGuiWindowFlags_NoCollapse))
        DrawControls(runtime);
    ImGui::End();
    pop_style();
}

} // namespace

extern "C" __declspec(dllexport) const char* NAME = "OptiScaler DLSS 5 Neural Rendering";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Controls DLSS 5 Neural Rendering from ReShade's own overlay or its own Alt+Home window -- drives "
    "OptiScaler's pass when present, or RenoDX's (the DLSS5-Feeder toolchain) otherwise.";

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

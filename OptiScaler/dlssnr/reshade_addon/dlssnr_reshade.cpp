// DLSS 5 Neural Rendering, as a ReShade add-on.
//
// What this is, and what it deliberately is not.
//
// It is NOT a port of the pass. The model needs the game's depth, motion vectors, MV scale, jitter
// reset and pre-exposure, and it needs them labelled and at the right moment in the frame. OptiScaler
// has all of that because it IS the upscaler interceptor -- the game hands it the NGX parameter block
// with every input named. ReShade's add-on API hands an add-on draw calls, resources and a swapchain;
// it has no idea which resource is motion vectors, and for depth it offers a per-game heuristic that
// is wrong often enough to need a UI of its own. Reimplementing the pass here would mean feeding the
// model guesses and calling the result Neural Rendering.
//
// So this is a FRONT END. The pass stays in OptiScaler, where the inputs are; this reaches across and
// drives it, drawing the controls inside ReShade's own overlay so a ReShade user gets them on the
// overlay key they already use, in one ImGui context, with no second window and no hotkey to learn.
//
// It talks to OptiScaler through the flat C ABI in ../DlssNr_Api.h, resolved by name at runtime. That
// means:
//   - This add-on does not link against OptiScaler and cannot crash it by being out of date.
//   - An OptiScaler without the ABI (upstream, or an older build) is detected and reported, not
//     crashed into.
//   - Settings added to OptiScaler later appear to this add-on as keys it does not happen to draw.
//     Nothing breaks; the add-on simply shows what it knows.
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

#include <windows.h>

#include <cstdio>

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

    g_link.problem = "OptiScaler is not loaded in this game. Its DLSS 5 controls need it running.";
}

// ---------------------------------------------------------------------------------------------
// Small wrappers, so the drawing code below reads like ordinary settings code.
//
// Every one of them fails soft. A key this OptiScaler does not have returns the fallback and the
// control still draws -- it just does nothing, which is the right behaviour for an add-on that may
// be talking to a newer or older build than it was written against.
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

// ---------------------------------------------------------------------------------------------
// Controls.
//
// Each returns whether the user let go of it, which is when the ini is written -- writing on every
// frame of a drag would rewrite the file dozens of times a second.
// ---------------------------------------------------------------------------------------------

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
// The overlay.
// ---------------------------------------------------------------------------------------------

void DrawOverlay(reshade::api::effect_runtime*)
{
    TryResolve();

    if (!g_link.ready)
    {
        ImGui::TextWrapped("%s", g_link.problem);

        ImGui::Spacing();
        ImGui::TextDisabled("DLSS 5 Neural Rendering runs inside OptiScaler, which is where the game's "
                            "depth, motion vectors and exposure arrive labelled. This add-on only draws "
                            "its controls -- it cannot run the pass on its own.");
        return;
    }

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

} // namespace

extern "C" __declspec(dllexport) const char* NAME = "DLSS 5 Neural Rendering";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Controls OptiScaler's DLSS 5 Neural Rendering from ReShade's overlay. Needs OptiScaler loaded in "
    "the same game -- this add-on drives the pass, it does not run it.";

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
        break;

    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(hinstDLL);
        break;
    }

    return TRUE;
}

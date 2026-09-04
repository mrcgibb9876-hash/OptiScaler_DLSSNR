#include "pch.h"

#include "DlssNr.h"
#include "DlssNrFeature_Vk.h"
#include "DlssNr_ExposureScan.h"

#include <Config.h>

#include <menu/menu_common.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace DlssNr
{

// A whole standalone overlay -- its own window, its own colours, independent of the rest of
// OptiScaler's shared menu chrome (title bar, graphs, bottom bar) and its user-configurable theme.
// Modelled on NVIDIA's own DLSS 5 Developer Controls panel.

// Palette sampled straight off a capture of NVIDIA's own panel rather than eyeballed, so the
// numbers below are what the screenshot measures, give or take JPEG noise:
//   accent green   #84B63A - #97B948   (slider fill, checkbox fill, selected model)
//   panel bg       #1A191A - #202021   (darker than the grey this overlay used before)
//   unfilled track #282828
//   title text     #E7E7E7   caption text #C6C7CB
//   row label      #A2A2A0   value text   #8A8A8C  (labels sit dimmer than captions)
//   disabled       caption #605E5F, label #4D4C4A, track #41413F, handle #424242
static const ImVec4 kAccent(0.549f, 0.729f, 0.239f, 1.0f);
static const ImVec4 kTitle(0.906f, 0.906f, 0.906f, 1.0f);
static const ImVec4 kCaption(0.776f, 0.780f, 0.796f, 1.0f);
static const ImVec4 kText(0.635f, 0.635f, 0.627f, 1.0f);
static const ImVec4 kValue(0.541f, 0.541f, 0.549f, 1.0f);
static const ImVec4 kTextDim(0.376f, 0.369f, 0.373f, 1.0f);
static const ImVec4 kTrack(0.157f, 0.157f, 0.157f, 1.0f);
static const ImVec4 kPanelBg(0.110f, 0.110f, 0.114f, 1.0f);

static float PanelWidth(float scale) { return 460.0f * scale; }

static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextColored(kTextDim, "(?)");

    // BeginItemTooltip is IsItemHovered + BeginTooltip in one, and it returns false when the tooltip
    // window was not begun at all. EndTooltip must only follow a true: calling it regardless pops
    // whatever window is current instead, which is this panel.
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Upper-cased captions, matching NVIDIA's own panel ("GLOBAL CONTROLS", ...).
static std::string Caps(const char* text)
{
    std::string out;

    for (const char* p = text; *p; ++p)
        out += (char) std::toupper((unsigned char) *p);

    return out;
}

// How far apart the caption letters sit, as a fraction of the font size. NVIDIA's panel tracks
// by roughly a seventh of a character -- open enough to read as styling, nowhere near the full
// space per letter that a plain ImGui text call would force.
static constexpr float kTracking = 0.14f;

// Letter-tracked text.
//
// ImGui cannot express sub-character tracking in a text call: its only lever is inserting whole
// spaces, which is several times too wide and reads as sprayed apart. So the run is drawn glyph
// by glyph through the draw list, advancing by each glyph's own width plus the tracking, and a
// Dummy of the measured width reserves the layout box afterwards. Drawing starts at the cursor
// and the box matches what was drawn, so SameLine, the (?) markers and the right-aligned
// checkboxes all land exactly where they would after ordinary text.
static void TrackedText(const char* text)
{
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float tracking = fontSize * kTracking;
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float x = 0.0f;

    for (const char* p = text; *p; ++p)
    {
        const char* begin = p;
        const char* end = p + 1;

        dl->AddText(font, fontSize, ImVec2(origin.x + x, origin.y), col, begin, end);
        x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, begin, end).x + tracking;
    }

    // The tracking belongs between letters, so the last one does not carry it.
    if (x > 0.0f)
        x -= tracking;

    ImGui::Dummy(ImVec2(x, ImGui::GetTextLineHeight()));
}

static void SectionCaption(const char* text, float rowWidth)
{
    const ImVec2 sp = ImGui::GetStyle().ItemSpacing;

    // A caption heads the rows beneath it, so it sits nearer to them than to the section
    // above: a little air on top, very little between the caption, its rule and the first
    // row. Fractions of ItemSpacing rather than fixed pixels, so it tracks the menu scale.
    ImGui::Dummy(ImVec2(0.0f, sp.y * 0.75f));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp.x, sp.y * 0.30f));

    ImGui::PushStyleColor(ImGuiCol_Text, kCaption);
    TrackedText(Caps(text).c_str());
    ImGui::PopStyleColor();

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p0, ImVec2(p0.x + rowWidth, p0.y),
                                        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.14f)), 1.0f);
    ImGui::Dummy(ImVec2(rowWidth, sp.y * 0.30f));

    ImGui::PopStyleVar();
}

struct SliderResult
{
    bool changed;
    bool released;
};

// Custom-drawn slider: label on the left, a thin track with a filled portion and a round
// handle in the middle, and the value as its own right-aligned text -- not centred inside the
// track the way stock ImGui::SliderFloat draws it, which is what made the handle collide with
// the digits in the first pass.
static SliderResult NrSlider(const char* label, float* value, float vMin, float vMax, const char* fmt, float rowWidth,
                             bool showFill = true, bool logarithmic = false)
{
    ImGui::PushID(label);

    ImGuiStyle& style = ImGui::GetStyle();
    float labelWidth = rowWidth * 0.44f;
    float valueWidth = 52.0f;
    float trackWidth = rowWidth - labelWidth - valueWidth - style.ItemSpacing.x * 2.0f;
    if (trackWidth < 40.0f)
        trackWidth = 40.0f;

    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(labelWidth);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float rowH = ImGui::GetFrameHeight();
    float trackH = 4.0f;
    float radius = 6.5f;
    ImVec2 tMin(pos.x, pos.y + rowH * 0.5f - trackH * 0.5f);
    ImVec2 tMax(pos.x + trackWidth, tMin.y + trackH);

    ImGui::InvisibleButton("track", ImVec2(trackWidth, rowH));
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();

    ImGuiStorage* store = ImGui::GetStateStorage();
    ImGuiID wasActiveId = ImGui::GetID("wasActive");
    bool wasActive = store->GetBool(wasActiveId, false);

    bool changed = false;
    if (active)
    {
        float t = (ImGui::GetIO().MousePos.x - tMin.x) / trackWidth;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        float newVal = logarithmic ? vMin * std::pow(vMax / vMin, t) : vMin + t * (vMax - vMin);
        if (newVal != *value)
        {
            *value = newVal;
            changed = true;
        }
    }

    bool released = wasActive && !active;
    store->SetBool(wasActiveId, active);

    float t = logarithmic ? std::log(*value / vMin) / std::log(vMax / vMin) : (*value - vMin) / (vMax - vMin);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float handleX = tMin.x + t * trackWidth;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(tMin, tMax, ImGui::GetColorU32(kTrack), trackH * 0.5f);

    if (showFill)
        dl->AddRectFilled(tMin, ImVec2(handleX, tMax.y), ImGui::GetColorU32(kAccent), trackH * 0.5f);

    ImVec4 hCol = (hovered || active) ? kAccent : ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.88f);
    ImVec2 hCenter(handleX, tMin.y + trackH * 0.5f);
    dl->AddCircleFilled(hCenter, radius, ImGui::GetColorU32(hCol), 18);
    dl->AddCircle(hCenter, radius, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.45f)), 18, 1.4f);

    ImGui::SameLine(labelWidth + trackWidth + style.ItemSpacing.x);
    ImGui::PushStyleColor(ImGuiCol_Text, kValue);
    ImGui::Text(fmt, *value);
    ImGui::PopStyleColor();

    ImGui::PopID();

    return { changed, released };
}

static bool NrCombo(const char* label, int* v, const char* const* items, int count, float rowWidth)
{
    float labelWidth = rowWidth * 0.44f;

    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(labelWidth);

    ImGui::SetNextItemWidth(rowWidth - labelWidth);
    std::string id = std::string("##") + label;
    return ImGui::Combo(id.c_str(), v, items, count);
}

// One entry in the "Models" row -- the segmented Model A / B / C selector.
static bool ModelButton(const char* label, bool active, float width)
{
    // Sampled off NVIDIA's panel: the selected pill is a dark olive fill (#353D1D) with a green
    // border and green label; the unselected ones are flat neutral grey (#3A3A3A, text #8C8C8C).
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.208f, 0.239f, 0.114f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.247f, 0.286f, 0.137f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.278f, 0.322f, 0.153f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.227f, 0.227f, 0.227f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.267f, 0.267f, 0.267f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.298f, 0.298f, 0.298f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.549f, 0.549f, 0.549f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);

    return clicked;
}

// The small right-aligned "Show Mask" / "Show Masks" toggle that rides on the right end of a
// section row in NVIDIA's panel: label first, small box after it, the pair pushed to the right
// edge of the row rather than following the section label.
static bool NrRightCheckbox(const char* label, bool* v, float rowWidth)
{
    ImGui::PushID(label);

    float boxSize = ImGui::GetFontSize() + 1.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float pairWidth = ImGui::CalcTextSize(label).x + spacing + boxSize;

    ImGui::SameLine(rowWidth - pairWidth);

    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, spacing);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("box", ImVec2(boxSize, boxSize));
    bool hovered = ImGui::IsItemHovered();
    if (clicked)
        *v = !*v;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c0 = pos;
    ImVec2 c1 = ImVec2(pos.x + boxSize, pos.y + boxSize);

    if (*v)
    {
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(kAccent), 2.0f);

        ImU32 dark = ImGui::GetColorU32(ImVec4(0.06f, 0.09f, 0.05f, 1.0f));
        ImVec2 a(pos.x + boxSize * 0.22f, pos.y + boxSize * 0.55f);
        ImVec2 b(pos.x + boxSize * 0.42f, pos.y + boxSize * 0.76f);
        ImVec2 cpt(pos.x + boxSize * 0.80f, pos.y + boxSize * 0.26f);
        dl->AddLine(a, b, dark, 2.0f);
        dl->AddLine(b, cpt, dark, 2.0f);
    }
    else
    {
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, hovered ? 0.10f : 0.05f)), 2.0f);
        dl->AddRect(c0, c1, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.20f)), 2.0f, 0, 1.0f);
    }

    ImGui::PopID();
    return clicked;
}

// A filled green square with a dark checkmark when set, matching NVIDIA's own panel -- not
// stock ImGui::Checkbox's outlined box with a coloured glyph.
static bool NrCheckbox(const char* label, bool* v, bool caps = false)
{
    ImGui::PushID(label);

    float boxSize = ImGui::GetFontSize() + 5.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();

    bool clicked = ImGui::InvisibleButton("box", ImVec2(boxSize, boxSize));
    bool hovered = ImGui::IsItemHovered();
    if (clicked)
        *v = !*v;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c0 = pos;
    ImVec2 c1 = ImVec2(pos.x + boxSize, pos.y + boxSize);

    if (*v)
    {
        ImVec4 fill = hovered ? ImVec4(std::min(kAccent.x + 0.08f, 1.0f), std::min(kAccent.y + 0.08f, 1.0f),
                                       std::min(kAccent.z + 0.08f, 1.0f), 1.0f)
                              : kAccent;
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(fill), 3.0f);

        ImU32 dark = ImGui::GetColorU32(ImVec4(0.06f, 0.09f, 0.05f, 1.0f));
        ImVec2 a(pos.x + boxSize * 0.22f, pos.y + boxSize * 0.55f);
        ImVec2 b(pos.x + boxSize * 0.42f, pos.y + boxSize * 0.76f);
        ImVec2 cpt(pos.x + boxSize * 0.80f, pos.y + boxSize * 0.26f);
        dl->AddLine(a, b, dark, 2.2f);
        dl->AddLine(b, cpt, dark, 2.2f);
    }
    else
    {
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, hovered ? 0.10f : 0.06f)), 3.0f);
        dl->AddRect(c0, c1, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.24f)), 3.0f, 0, 1.2f);
    }

    ImGui::SameLine();
    // Section-level rows ("DLSS ON", "MODEL AUTOMASK", "DEVELOPER MASKING") are letter-tracked
    // caps in the caption colour; the per-object rows under them stay sentence case.
    ImGui::PushStyleColor(ImGuiCol_Text, caps ? kCaption : kText);
    if (caps)
        TrackedText(Caps(label).c_str());
    else
        ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    ImGui::PopID();
    return clicked;
}

void RenderMenu(Config* config, float menuResScale)
{
    ImGuiIO& io = ImGui::GetIO();
    auto& state = State::Instance();
    float rowWidth = PanelWidth(menuResScale);

    // Pinned to the left edge, vertically centred -- the position NVIDIA's own overlay uses.
    // Not user-movable: this is the only thing this build shows, so there is nothing to arrange
    // it around.
    float margin = 24.0f * menuResScale;
    ImGui::SetNextWindowPos(ImVec2(margin, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.0f, 0.5f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kAccent);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.157f, 0.157f, 0.157f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.196f, 0.196f, 0.196f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.227f, 0.227f, 0.227f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.086f, 0.086f, 0.090f, 0.98f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 14.0f) * menuResScale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 9.0f * menuResScale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar;

    bool anyChanged = false;

    if (ImGui::Begin("##DlssNrOverlay", nullptr, flags))
    {
        // Claim focus so keyboard/mouse routes here rather than being left with whatever last had
        // it -- but only when this panel is on its own. With OptiScaler's own menu also open,
        // grabbing focus every frame would make that menu impossible to type into.
        if (!MenuCommon::IsSharedMenuVisible() && !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
            ImGui::SetWindowFocus();

        ImGui::Dummy(ImVec2(rowWidth, 0.0f));

        ImGui::PushStyleColor(ImGuiCol_Text, kTitle);
        TrackedText(Caps("DLSS 5 Developer Controls").c_str());
        ImGui::PopStyleColor();

        ImGui::Spacing();

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (NrCheckbox("DLSS ON", &enabled, true))
        {
            config->DlssNrEnabled = enabled;
            anyChanged = true;
        }

        HelpMarker("Synthesises detail in the upscaler's output, before frame generation sees it."
                   "\n\nNeeds two similarly named files beside OptiScaler, one character apart:"
                   "\n  nvngx_dlssnr.dll       NVIDIA's model (~165 MB) -- you supply it"
                   "\n  nvngx.dll_dlssnr.dll   the forwarder (~13 KB) -- ships in this package"
                   "\nUndocumented and driven directly, so none of this is officially supported.");

        // Both rows are also under Keybinds in OptiScaler's own menu; they are repeated here so the
        // panel is usable on its own, without going looking for the other window.
        MenuCommon::RenderKeybindRow("Toggle key", 14, config->DlssNrToggleKey);
        HelpMarker("Toggles Neural Rendering without opening this panel. Press the button, then the"
                   "\nkey you want. Escape cancels, Backspace unbinds, R resets it.");

        MenuCommon::RenderKeybindRow("Panel key", 15, config->DlssNrPanelKey);
        HelpMarker("Opens and closes this panel. Independent of OptiScaler's own menu key, so the"
                   "\ntwo can be up together or on their own.");

        bool applyModel = config->DlssNrApplyModel.value_or_default();
        if (NrCheckbox("Apply the model", &applyModel))
        {
            config->DlssNrApplyModel = applyModel;
            anyChanged = true;
        }
        HelpMarker("Whether the model's edit is applied. Off shows the clean upscaler frame while the"
                   "\npass keeps running -- so with Hold frame, under Inspect, you can freeze a frame"
                   "\nand toggle this to see the same frozen frame with and without Neural Rendering."
                   "\nLeave it on for normal use.");

        // Either backend. They keep separate state, and on a native Vulkan game the D3D12 side is
        // never touched -- asking only that one reports "waiting" over a pass that is demonstrably
        // running.
        const bool vulkan = DlssNr::IsRunningVk();

        if (!DlssNr::IsRunning() && !vulkan)
        {
            const char* reason = DlssNr::FailureReason();

            if (reason[0] != 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Off for this session: %s.", reason);
                ImGui::SameLine();

                if (ImGui::SmallButton("Retry"))
                    DlssNr::RetryAfterFailure();
            }
            else if (enabled)
            {
                ImGui::TextColored(kTextDim, "Waiting for the upscaler to run.");
                // The one thing the old shared window told you here that this panel otherwise
                // wouldn't: this needs the game's own upscaler active, not just this checkbox.
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
                ImGui::TextColored(kTextDim, "Needs DLSS or XeSS selected as the upscaler in the game's own "
                                             "video settings, and a save loaded -- this (and the rest of "
                                             "OptiScaler) does not run in menus.");
                ImGui::PopTextWrapPos();
            }
        }
        else
        {
            // Either backend's timer. They measure the same thing by different means, and only one
            // of them is running -- Vulkan has its own now, so this no longer has to say it cannot
            // time that path.
            const auto ms = vulkan ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime();

            // With "Apply the model" off the pass STILL RUNS -- it only outputs the clean frame. The
            // cost is real, and saying so stops the reading looking like a bug. Turning DLSS ON off
            // is what zeroes it.
            const char* runSuffix = !applyModel ? "  (model running, edit hidden)" : "";

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1.0f), "Running%s - %.2f ms per frame%s",
                                   vulkan ? " natively on Vulkan" : "", ms.value(), runSuffix);
            else if (vulkan)
                // Measured but not yet read: the first few frames are still in the query ring.
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1.0f), "Running natively on Vulkan - %llu frames%s",
                                   DlssNr::FramesVk(), runSuffix);
            else
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1.0f), "Running.%s", runSuffix);

            ImGui::SameLine();
            ImGui::TextColored(kTextDim, "(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The whole pass: the staging copies and the resolve as well as the"
                                  "\nmodel. Timing only the model would flatter the number."
                                  "\n\nCompare it against the frame time at the bottom of this window to"
                                  "\nsee what it is costing you.");
        }

        // Global Controls -- DlssNrLocalStructure / DlssNrLocalTone: NVIDIA's own name for
        // these two in its DLSS 5 developer overlay.
        SectionCaption("Global Controls", rowWidth);

        float localStructure = config->DlssNrLocalStructure.value_or_default();
        auto rStruct = NrSlider("Structure Intensity", &localStructure, 0.0f, 1.0f, "%.2f", rowWidth);
        if (rStruct.changed)
            config->DlssNrLocalStructure = localStructure;
        if (rStruct.released)
            anyChanged = true;
        HelpMarker("The model's structure-synthesis strength across the whole frame.");

        float localTone = config->DlssNrLocalTone.value_or_default();
        auto rTone = NrSlider("Tone Intensity", &localTone, 0.0f, 1.0f, "%.2f", rowWidth);
        if (rTone.changed)
            config->DlssNrLocalTone = localTone;
        if (rTone.released)
            anyChanged = true;
        HelpMarker("The model's tone-remapping strength across the whole frame.");
        // Model Automask -- DlssNrAutoMask. In NVIDIA's panel this is a letter-tracked caps row
        // of its own with a "Show Mask" toggle on the right, not a section caption with a divider,
        // so it is drawn that way here.
        ImGui::Spacing();

        bool autoMask = config->DlssNrAutoMask.value_or_default();
        if (NrCheckbox("Model Automask", &autoMask, true))
        {
            config->DlssNrAutoMask = autoMask;
            anyChanged = true;
        }
        HelpMarker("Lets the model find skin itself rather than treating the frame uniformly.");

        // Greyed out, and not because a setting is off: the model keeps its mask to itself. It is
        // never handed back as a resource across the interface this fork drives, so there is
        // nothing for an overlay to draw.
        ImGui::BeginDisabled(true);
        bool showMask = false;
        NrRightCheckbox("Show Mask", &showMask, rowWidth);
        ImGui::EndDisabled();

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("NVIDIA's panel can draw the automask over the frame. The model does not hand"
                              "\nits mask back through the interface this fork drives, so there is nothing"
                              "\nhere to display.");

        // Matches NVIDIA's own panel: greyed out while Automask is off. The value underneath is
        // unchanged either way -- this only stops it being dragged while it has nothing to act on.
        ImGui::BeginDisabled(!autoMask);
        ImGui::PushID("Automask");
        float skin = config->DlssNrSkinStructure.value_or_default();
        auto rSkin = NrSlider("Structure Intensity", &skin, -1.0f, 1.0f, "%.2f", rowWidth);
        if (rSkin.changed)
            config->DlssNrSkinStructure = skin;
        if (rSkin.released)
            anyChanged = true;
        ImGui::PopID();
        ImGui::EndDisabled();
        HelpMarker("-1 means follow the Global Controls Structure Intensity above, and is the"
                   "\nmodel's own default. 0 and above set the masked region's structure"
                   "\nindependently of the rest of the frame."
                   "\n\nGreyed out while Model Automask is off -- there is no mask for it to"
                   "\nshape without it.");

        // Developer Masking -- NVIDIA's per-object, engine-level masking. The game's own renderer
        // tags individual objects (the "Pitcher", "Grapes" and "Bottles" of NVIDIA's demo scene)
        // and hands those masks to DLSS through Streamline, so an artist can dial each object
        // separately. There is nothing here for this fork to drive: OptiScaler sits below the
        // engine, in the graphics API, with no object list and no way to author such masks. The
        // row is drawn because the panel this copies has it, and is disabled because it cannot be
        // made to work -- not because a setting is off.
        ImGui::Spacing();

        ImGui::BeginDisabled(true);
        bool devMasking = false;
        bool showMasks = false;
        NrCheckbox("Developer Masking", &devMasking, true);
        NrRightCheckbox("Show Masks", &showMasks, rowWidth);
        ImGui::EndDisabled();

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
        ImGui::TextColored(kTextDim, "Per-object masks come from the game's own renderer, so this one stays "
                                     "NVIDIA-only -- an injector has no object list to mask.");
        ImGui::PopTextWrapPos();

        // Models -- DlssNrPreset. NVIDIA ships no letters in the binary; "Model A/B/C" is this
        // fork's best match to the segmented selector in the developer overlay, and matches the
        // three NVIDIA describes publicly. Default (preset index 0) is kept as a fourth button
        // that NVIDIA's panel does not show, because it is a real, distinct state here: dropping
        // it to match the screenshot exactly would make that state unreachable from the UI.
        SectionCaption("Models", rowWidth);

        static const char* nrPresetNames[] = { "Default", "Model A", "Model B", "Model C" };
        int preset = (int) config->DlssNrPreset.value_or_default();

        {
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float btnWidth = (rowWidth - spacing * (IM_ARRAYSIZE(nrPresetNames) - 1)) / IM_ARRAYSIZE(nrPresetNames);

            for (int i = 0; i < IM_ARRAYSIZE(nrPresetNames); i++)
            {
                if (i > 0)
                    ImGui::SameLine();

                ImGui::PushID(i);
                if (ModelButton(nrPresetNames[i], preset == i, btnWidth))
                {
                    preset = i;
                    config->DlssNrPreset = (uint32_t) preset;
                    anyChanged = true;
                }
                ImGui::PopID();
            }
        }
        HelpMarker("Not the same scale as the super resolution or ray reconstruction presets --"
                   "\nthe same letter means something different here."
                   "\n\nRead when the model is built, so a change rebuilds it after a moment.");

        static const char* nrStyleNames[] = { "Default (standard)", "Natural", "Cinematic" };
        int style = (int) config->DlssNrStyle.value_or_default();
        if (style > 2)
            style = 2;
        if (NrCombo("Style", &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames), rowWidth))
        {
            config->DlssNrStyle = (uint32_t) style;
            anyChanged = true;
        }
        HelpMarker("The model's own processing profiles."
                   "\n\nDefault (standard): the strongest, and most likely to look 'stylised'."
                   "\nNatural: the same detail work with a gentler hand."
                   "\nCinematic: tones down the shine and over-processing for a film-like look."
                   "\n\nThe names come from community testing, unlike the panel labels above --"
                   "\nNVIDIA ships no names for this control in the binaries.");

        float intensity = config->DlssNrIntensity.value_or_default();
        auto rIntensity = NrSlider("Intensity", &intensity, 0.0f, 2.0f, "%.2f", rowWidth);
        if (rIntensity.changed)
            config->DlssNrIntensity = intensity;
        if (rIntensity.released)
            anyChanged = true;
        HelpMarker("The model's own strength control, applied inside it. Distinct from the Global"
                   "\nControls above, and from Detail strength below, which scales the result"
                   "\nafterwards.");

        // Frame Generation -- NVIDIA's own DLSS-G (Streamline), driven the same way the old
        // shared menu's "MFG" combo and "Force Dynamic MFG" checkbox did: straight through
        // config->FGDLSSGInterpolationCount / FGDLSSGForceDMFG, which DLSSG_Dx12::Dispatch()
        // reads every frame and hands to sl::DLSSGOptions.numFramesToGenerate. Nothing here
        // touches OptiFG (the Nukem's FSR3-based fallback used when the game has no native
        // frame generation) -- that path is FGOutput::FSRFG/XeFG, not FGOutput::DLSSG, and is
        // deliberately left out of this panel.
        SectionCaption("Frame Generation", rowWidth);

        if (state.activeFgOutput == FGOutput::DLSSG && state.currentFG != nullptr)
        {
            auto* fg = state.currentFG;

            bool fgActive = config->FGEnabled.value_or_default();
            if (NrCheckbox("Frame Generation", &fgActive))
            {
                config->FGEnabled = fgActive;
                state.fgChanged = true;
                anyChanged = true;
            }
            HelpMarker("NVIDIA's own DLSS Frame Generation, via Streamline. Not OptiFG.");

            int maxCount = fg->GetMaxInterpolationCount();
            if (maxCount > 1)
            {
                static const char* multNames[] = { "2X", "3X", "4X", "5X", "6X" };
                int shown = std::min(maxCount, (int) IM_ARRAYSIZE(multNames));
                int current = std::clamp((int) fg->GetInterpolatedFrameCount() - 1, 0, shown - 1);

                bool dmfgForced = config->FGDLSSGForceDMFG.value_or_default();

                ImGui::BeginDisabled(dmfgForced);
                {
                    float spacing = ImGui::GetStyle().ItemSpacing.x;
                    float btnWidth = (rowWidth - spacing * (shown - 1)) / shown;

                    for (int i = 0; i < shown; i++)
                    {
                        if (i > 0)
                            ImGui::SameLine();

                        ImGui::PushID(i);
                        if (ModelButton(multNames[i], current == i, btnWidth))
                        {
                            LOG_DEBUG("DLSSG Interpolation Count set to: {}", i + 1);
                            config->FGDLSSGInterpolationCount = i + 1;
                            anyChanged = true;
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndDisabled();
                HelpMarker("Sets Streamline's numFramesToGenerate directly -- how many extra frames"
                           "\nDLSS-G inserts between real ones. 2X inserts one, 3X inserts two, and"
                           "\nso on. Capped by what your GPU and driver report supporting."
                           "\n\nGreyed out while Multi is on below -- the driver picks the count then.");

                if (fg->GetDMFGSupport())
                {
                    if (NrCheckbox("Multi (Dynamic Frame Generation)", &dmfgForced))
                    {
                        config->FGDLSSGForceDMFG = dmfgForced;
                        anyChanged = true;
                    }
                    HelpMarker("Lets NVIDIA's driver vary the multiplier itself, frame to frame, to hold"
                               "\nthe FPS target below -- instead of a fixed 2X/3X/4X.");

                    ImGui::BeginDisabled(!dmfgForced);
                    float fpsTarget = config->FGDLSSGFramerateTargetDMFG.value_or_default();
                    auto rFps = NrSlider("DMFG FPS Target", &fpsTarget, 0.0f, 200.0f, "%.0f", rowWidth);
                    if (rFps.changed)
                        config->FGDLSSGFramerateTargetDMFG = fpsTarget;
                    if (rFps.released)
                        anyChanged = true;
                    ImGui::EndDisabled();
                    HelpMarker("0 auto-detects your display's refresh rate.");
                }
            }
        }
        else
        {
            ImGui::TextColored(kTextDim, "NVIDIA DLSS Frame Generation is not the active output right now.");
        }

        // Everything below is this fork's own instrumentation, with no equivalent in NVIDIA's
        // developer overlay -- kept under its original names.
        SectionCaption("Cost", rowWidth);

        static int pendingScale = -1;
        float scalePercent =
            pendingScale >= 0 ? (float) pendingScale : config->DlssNrWorkingScale.value_or_default() * 100.0f;

        auto rScale = NrSlider("Model resolution", &scalePercent, 25.0f, 200.0f, "%.0f%%", rowWidth);
        if (rScale.changed)
            pendingScale = (int) std::lroundf(scalePercent);

        if (rScale.released && pendingScale >= 0)
        {
            config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
            pendingScale = -1;
            anyChanged = true;
        }
        HelpMarker("What fraction of the frame the model works at. Cost falls with the square of"
                   "\nthis, so half resolution is roughly a quarter of the time. Below 100 the frame"
                   "\nitself is never reduced -- only the model's own contribution is computed small"
                   "\nand enlarged. Applied when the handle is let go, not while it is moving.");

        // Above native the model is run supersampled and filtered back down, so the filter is
        // the whole difference between supersampling meaning less noise and meaning more.
        const int shownScale = pendingScale >= 0 ? pendingScale : (int) std::lroundf(scalePercent);

        if (shownScale > 100)
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
            ImGui::TextColored(kTextDim,
                               "Supersampling %.2fx: the model runs ABOVE native, then is sampled back "
                               "down. Experimental, and costly -- time grows with the area.",
                               shownScale / 100.0f);
            ImGui::PopTextWrapPos();

            static const char* const kDownscalerNames[] = { "FSR1",     "Bicubic", "Catmull-Rom", "Lanczos2",
                                                            "Lanczos3", "Kaiser2", "Kaiser3",     "MAGIC" };

            int ds = (int) config->DlssNrScalingDownscaler.value_or_default();

            if (ds < 0 || ds >= IM_ARRAYSIZE(kDownscalerNames))
                ds = (int) Scaler::Lanczos3;

            if (NrCombo("Downscaler", &ds, kDownscalerNames, IM_ARRAYSIZE(kDownscalerNames), rowWidth))
            {
                config->DlssNrScalingDownscaler = (Scaler) ds;
                anyChanged = true;
            }
            HelpMarker("The filter that averages the model's above-native answer back to display size --"
                       "\nthis is what turns supersampling into LESS noise rather than more. Sharper"
                       "\nfilters (Lanczos3, Kaiser3) keep the most detail; softer ones (Bicubic,"
                       "\nCatmull-Rom) are gentler on ringing. Independent of the Output Scaling"
                       "\ndownscaler, so the two can differ and run at the same time.");
        }

        // How the model's work is brought back up when it ran below the frame's size. Classic
        // composes the small picture straight against the full-size frame, which cannot tell the
        // shrink's blur apart from the model's edit.
        const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;

        ImGui::BeginDisabled(!reduced);
        static const char* enlargeNames[] = { "Classic", "Matched residual" };
        int enlarge = config->DlssNrTransfer.value_or_default() == 1 ? 1 : 0;
        if (NrCombo("Enlargement", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames), rowWidth))
        {
            config->DlssNrTransfer = (uint32_t) enlarge;
            anyChanged = true;
        }
        ImGui::EndDisabled();
        HelpMarker("How the model's work is brought back up when it ran below the frame's size."
                   "\n\nClassic composes the model's small picture directly against the full-size frame."
                   "\nThose two disagree by the shrink's blur as well as by the model's edit, and the"
                   "\ncomposition cannot tell them apart."
                   "\n\nGreyed out at 100%, where there is nothing to enlarge.");

        SectionCaption("How much of it lands", rowWidth);

        float transfer = config->DlssNrTransferStrength.value_or_default();
        auto rTransfer = NrSlider("Detail strength", &transfer, 0.0f, 2.0f, "%.2f", rowWidth);
        if (rTransfer.changed)
            config->DlssNrTransferStrength = transfer;
        if (rTransfer.released)
            anyChanged = true;

        ImGui::SameLine();

        if (ImGui::SmallButton("Reset##detail"))
        {
            config->DlssNrTransferStrength = 1.0f;
            anyChanged = true;
        }
        HelpMarker("How far the frame moves toward the model's picture. 0 gives back exactly what"
                   "\nthe upscaler produced. 1 is the model's picture. Above 1 carries on past"
                   "\nit in the same direction.");

        float colour = config->DlssNrColourStrength.value_or_default();
        auto rColour = NrSlider("Colour strength", &colour, 0.0f, 4.0f, "%.2f", rowWidth);
        if (rColour.changed)
            config->DlssNrColourStrength = colour;
        if (rColour.released)
            anyChanged = true;

        ImGui::SameLine();

        if (ImGui::SmallButton("Reset##colour"))
        {
            config->DlssNrColourStrength = 1.0f;
            anyChanged = true;
        }
        HelpMarker("Whether the model's colour arrives with its light. 0 keeps the game's own hue"
                   "\nexactly -- every pixel the original colour, with only its brightness carrying"
                   "\nthe model's verdict. 1 brings the model's colour as well, in its own hue,"
                   "\nclamped into AP1 so nothing unreachable is asked for."
                   "\n\nAbove 1 it over-saturates: the colour keeps its hue but grows more vivid, and"
                   "\nrolls off at the edge of what the display can show rather than clipping into a"
                   "\nflat blown patch. 1 is the model's own colour; push past it for punch.");

        SectionCaption("Colour", rowWidth);

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
        ImGui::TextColored(kTextDim, "The model was trained on finished, sRGB-encoded frames. These decide how "
                                     "the upscaler's linear output is mapped into something it recognises.");
        ImGui::PopTextWrapPos();

        // 0 off (soft knee), 1 Neutwo + composition, 2 Neutwo + pure-inverse replace, 3 hybrid +
        // composed, 4 hybrid + replace. This decides what every control below is working on, so it
        // opens the section rather than trailing it.
        static const char* const kReversibleNames[] = { "Off (soft knee)", "Neutwo proxy + composed",
                                                        "Neutwo proxy + replace", "Hybrid proxy + composed",
                                                        "Hybrid proxy + replace" };

        int reversible = (int) config->DlssNrReversibleMode.value_or_default();

        if (reversible < 0 || reversible > 4)
            reversible = 0;

        if (NrCombo("Reversible proxy", &reversible, kReversibleNames, IM_ARRAYSIZE(kReversibleNames), rowWidth))
        {
            config->DlssNrReversibleMode = (uint32_t) reversible;
            anyChanged = true;
        }
        HelpMarker("What the model is shown, and how its answer comes back. Experimental."
                   "\n\nOff (soft knee): the default, and byte-identical to before. It rolls highlights"
                   "\noff so hard the model cannot resolve detail in them -- fine in soft-lit scenes,"
                   "\nweak in bright ones."
                   "\n\nNeutwo composed: an unclipped curve, so the model sees highlight detail, then"
                   "\neverything above it (strengths, highlight guard, palette). Wins in bright scenes,"
                   "\nbut the curve compresses midtones too, so soft-lit content can be worse than Off."
                   "\nIt also shifts paper white -- re-check that when you switch."
                   "\n\nHybrid composed: the one to use. Identity in the midtones -- as good as Off"
                   "\nthere -- with the unclipped roll only in the highlights, so it recovers the detail"
                   "\nOff crushes without giving up the midtones Neutwo does. Barely shifts paper white."
                   "\n\nReplace: the raw model straight back through the exact inverse, none of the"
                   "\ncomposition -- no guard, no palette, no strengths. Gorgeous where there are no"
                   "\nbright lights, but they FLASH in motion. A reference, not a daily setting."
                   "\n\nHybrid replace: Replace's raw model on the hybrid curve, so the flashing is"
                   "\nconfined to genuine highlights instead of everywhere. Most of Replace's detail,"
                   "\nfar more stable.");

        // Where the number that divides the frame comes from. This used to be a checkbox on
        // DlssNrWhitePointFromExposure; upstream replaced that flag with a three-way source, and the
        // engine no longer reads the old one -- the checkbox would have kept setting a value nothing
        // consults. The scan asks the source whether it is wanted, so choosing it here is the whole
        // of switching it on: there is no second flag to keep in step, and so no way for two to
        // disagree.
        static const char* const kSourceNames[] = { "Paper white only", "The game's own exposure",
                                                    "A buffer the scan found" };

        int wpSource = (int) config->DlssNrWhitePointSource.value_or_default();

        if (wpSource < 0 || wpSource > 2)
            wpSource = 0;

        if (NrCombo("White point from", &wpSource, kSourceNames, 3, rowWidth))
        {
            config->DlssNrWhitePointSource = (uint32_t) wpSource;
            anyChanged = true;
        }
        HelpMarker("Paper white only -- the slider below and nothing else. Right for a game whose"
                   "\nexposure never moves, wrong the moment it does: one constant cannot serve a"
                   "\ncave and a field."
                   "\n\nThe game's own exposure -- read from the texture the game hands the upscaler."
                   "\nThe best source there is, because it is decided upstream and nothing this pass"
                   "\ndoes can move it. Not every game supplies one."
                   "\n\nA buffer the scan found -- for games that compute an exposure and never pass"
                   "\nit on. A guess: candidates are matched by shape, and the anchor's ratio cancels"
                   "\nthe scale. Needs anchoring once, in the Experimental section, and checking after.");

        // Which anchor row the paper-white slider edits, or -1 for the live unanchored point.
        // Menu-local and not persisted; the anchor table under Experimental sets it when a row is
        // clicked. Declared here because the slider and the table read it in the same frame.
        static int selectedAnchor = -1;
        auto anchors = DlssNr::ExposureScan::Anchors();

        if (selectedAnchor >= (int) anchors.size())
            selectedAnchor = -1;

        // Paper white and the trim swap places by source, because they are not the same control:
        // paper white is the absolute number, the trim is a multiplier on a number that came from
        // somewhere else. Showing both at once asked people to set "paper white" next to a control
        // that was not paper white.
        if (wpSource == 2)
        {
            const bool editingRow = selectedAnchor >= 0 && selectedAnchor < (int) anchors.size();

            if (!anchors.empty())
            {
                const float liveScan = DlssNr::ExposureScan::BestValue();

                if (liveScan > 0.0f)
                {
                    const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
                        liveScan, config->DlssNrScanInverted.value_or_default(),
                        config->DlssNrScanTrim.value_or_default());

                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "Scan %.5f  ->  white point %.2f   (%u point%s)", liveScan, w,
                                       (unsigned) anchors.size(), anchors.size() == 1 ? "" : "s");
                }
            }

            // Only where there is a point to set: before the first anchor, or while editing a row.
            // Once points exist and none is selected the white point is fixed by the anchors, and
            // only the trim moves the live picture.
            if (anchors.empty() || editingRow)
            {
                float pw =
                    editingRow ? anchors[selectedAnchor].white : config->DlssNrWhitePointScale.value_or_default();

                char lbl[48];

                if (editingRow)
                    snprintf(lbl, sizeof(lbl), "Paper white (point %d)", selectedAnchor + 1);
                else
                    snprintf(lbl, sizeof(lbl), "Paper white");

                auto rPw = NrSlider(lbl, &pw, 0.25f, 2000.0f, "%.2fx", rowWidth, true, true);

                if (rPw.changed)
                {
                    if (editingRow)
                    {
                        DlssNr::ExposureScan::AnchorSetWhite(selectedAnchor, pw);
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                    }
                    else
                        config->DlssNrWhitePointScale = pw;
                }

                if (rPw.released)
                    anyChanged = true;

                HelpMarker("The white point for the selected calibration point, or -- with no row"
                           "\nselected -- the value the next Anchor press captures."
                           "\n\nSet it until the picture looks right here, then Anchor. Move to very"
                           "\ndifferent light and do it again: two points fix the buffer's real"
                           "\nrelationship and the white point holds between them.");
            }

            // In the steady state this is what stands in for paper white: adjust until the picture
            // looks right in the current light, then Anchor bakes the trimmed value into a new point
            // and resets the trim to 1.
            if (!anchors.empty())
            {
                float trim = config->DlssNrScanTrim.value_or_default();
                auto rTrim = NrSlider("Trim (x the scan)", &trim, 0.25f, 4.0f, "%.2fx", rowWidth, true, true);

                if (rTrim.changed)
                    config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);

                if (rTrim.released)
                    anyChanged = true;

                ImGui::SameLine();

                if (ImGui::SmallButton("Reset##scantrim"))
                {
                    config->DlssNrScanTrim = 1.0f;
                    anyChanged = true;
                }

                HelpMarker("A multiplier on the scan's white point, and the control to adjust between"
                           "\nanchor points: dial it until the picture looks right in the current light,"
                           "\nthen press Anchor under Experimental -- that captures the trimmed value as"
                           "\na new point and resets this to 1.");
            }
        }
        else if (wpSource == 1)
        {
            const auto ex = DlssNr::GameExposureStatus();

            // Whether this game supplies one at all, shown either way -- without it, a game that
            // offers nothing looks identical to the option working quietly.
            if (vulkan)
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                   DlssNr::ExposureOfferedVk()
                                       ? "This game supplies an exposure and it is being read."
                                       : "This game supplies no exposure. Try the scan instead.");
            else if (ex.seenFrames == 0)
                ImGui::TextColored(kTextDim, "Waiting for a frame...");
            else if (!ex.everOffered)
                ImGui::TextColored(ImVec4(0.85f, 0.65f, 0.25f, 1.0f),
                                   "This game supplies no exposure. Try the scan instead.");
            else if (ex.exposure > 1e-6f)
            {
                const float trim = std::clamp(config->DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "Game exposure %.4f  ->  white point %.2f%s",
                                   ex.exposure, ex.preExposure / ex.exposure * trim,
                                   ex.offeredNow ? "" : "  (held: absent this frame)");
            }
            else
                ImGui::TextColored(kTextDim, "Reading the exposure...");

            float trim = config->DlssNrWhitePointTrim.value_or_default();
            auto rTrim = NrSlider("Trim (x the game's exposure)", &trim, 0.25f, 4.0f, "%.2fx", rowWidth, true, true);

            if (rTrim.changed)
                config->DlssNrWhitePointTrim = std::clamp(trim, 0.25f, 4.0f);

            if (rTrim.released)
                anyChanged = true;

            ImGui::SameLine();

            // Always present rather than greyed at 1: the point is that the safe value is one click
            // away without having to know what the safe value is.
            if (ImGui::SmallButton("Reset##wptrim"))
            {
                config->DlssNrWhitePointTrim = 1.0f;
                anyChanged = true;
            }

            HelpMarker("A multiplier on the exposure the game supplied. 1.00x takes its number exactly,"
                       "\nand that is the right answer here."
                       "\n\nThis is not a fudge factor. A game that needs the trim far from 1 to look"
                       "\nright is evidence the exposure being read is wrong for that game, not that the"
                       "\ngame wants trimming. Roughly 0.8 to 1.25 is honest tuning; reaching for 4 means"
                       "\nsomething upstream is broken and this is hiding it."
                       "\n\nYour manual paper white is kept separately and comes back untouched if you"
                       "\nswitch the source back.");
        }
        else
        {
            // Logarithmic, because the useful range is not. A quarter to 2000: the low end because a
            // frame the game already tone mapped wants roughly 1, the high end because there is no
            // principled ceiling -- this is a divisor on an open-ended linear buffer, and how far up
            // a game needs to go is a property of that game rather than anything boundable here.
            float wpScale = config->DlssNrWhitePointScale.value_or_default();
            auto rWp = NrSlider("Paper white", &wpScale, 0.25f, 2000.0f, "%.2fx", rowWidth, true, true);

            if (rWp.changed)
                config->DlssNrWhitePointScale = wpScale;

            if (rWp.released)
                anyChanged = true;

            HelpMarker("What the frame is divided by before the model sees it. There is no other white"
                       "\npoint; this is the whole of it. Above 1 the picture handed over is darker, so"
                       "\nhighlights sit lower on the curve.");
        }

        float maxRatio = config->DlssNrMaxRatio.value_or_default();
        auto rMax = NrSlider("Highlight guard", &maxRatio, 1.0f, 8.0f, "%.1fx", rowWidth);
        if (rMax.changed)
            config->DlssNrMaxRatio = maxRatio;
        if (rMax.released)
            anyChanged = true;

        ImGui::SameLine();

        if (ImGui::SmallButton("Reset##guard"))
        {
            config->DlssNrMaxRatio = 2.0f;
            anyChanged = true;
        }
        HelpMarker("The most the pass may move any pixel, as a multiple of what it already was, in both"
                   "\ndirections -- a pixel may not be brightened past this nor darkened past its"
                   "\nreciprocal. Lights are where the model has least to say and rescaling its answer"
                   "\ndoes the most damage; 2x leaves detail intact while stopping a strip light turning"
                   "\ninto a string of coloured cells. Raise it only if bright areas look clipped.");

        // Directly under the white point, because that is the number it moves and the number the
        // anchor captures. There is deliberately no on/off switch: the source dropdown above says
        // whether the scan is the white point's source, and that is the only reason anyone would
        // want it running. A second control could only agree with the dropdown or contradict it.
        // The ini key survives as a developer override for the one case a user has no reason to
        // want -- running the scan in a game that supplies a real exposure, so the log can compare.
        //
        // Worth writing down, since the panel no longer says it: the scan matches buffers by SHAPE,
        // and shape is a weak filter. In GTA V the best candidate was a 1x1 R32_FLOAT that climbed
        // in a straight line for seventeen minutes while the true exposure held still. That is an
        // accumulator, not an eye adaptation.
        if (DlssNr::ExposureScan::Scanning())
        {
            SectionCaption("Exposure scan", rowWidth);

            const bool isSource = wpSource == 2;

            // Only where it means something: the lamp reads the scan, so offering it beside a white
            // point that comes from somewhere else is offering a control that cannot light up.
            if (isSource)
            {
                if (bool meter = config->DlssNrScanMeter.value_or_default();
                    NrCheckbox("Show the light meter on screen", &meter))
                {
                    config->DlssNrScanMeter = meter;
                    anyChanged = true;
                }
                HelpMarker("A lamp in the corner: red for dark, green for full light, and the shades"
                           "\nbetween, with the reading beside it."
                           "\n\nIt is how you see at a glance that the scan is TRACKING rather than"
                           "\nmerely running. Walk into shade and it should slide toward red; step out"
                           "\nand it should go green. If it moves the wrong way, that is what \"the number"
                           "\nruns the other way\" below is for."
                           "\n\nPurely a readout. It changes nothing.");
            }

            // The absolute white point cannot come out of a buffer whose units are unknown. Every
            // value AFTER the first can: only the ratio against the anchor is used, so whatever the
            // number means, it cancels. That is why this is a button and not a measurement -- the
            // one thing a person can supply that no cleverness can is "this looks right to me".
            const float live = DlssNr::ExposureScan::BestValue();

            ImGui::BeginDisabled(live <= 0.0f || !isSource);

            if (ImGui::Button("Anchor here"))
            {
                // Before the first point, the paper white above -- an absolute value with the wide
                // range a fresh game needs. After that, the EFFECTIVE white point the picture is
                // showing right now (the interpolated value times the trim just dialled in), so a
                // second point in different light captures the trimmed look rather than a frozen
                // paper white, which would make two equal whites and a flat, non-tracking curve.
                // The trim resets afterwards: the new point, which the picture now passes through
                // exactly, must not be multiplied by it a second time.
                const float captureWhite =
                    anchors.empty() ? std::max(0.01f, config->DlssNrWhitePointScale.value_or_default())
                                    : std::max(0.01f, DlssNr::ExposureScan::AnchoredWhitePoint(
                                                          live, config->DlssNrScanInverted.value_or_default(),
                                                          config->DlssNrScanTrim.value_or_default()));

                if (DlssNr::ExposureScan::AnchorAdd(live, captureWhite))
                {
                    config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                    config->DlssNrScanTrim = 1.0f;
                    selectedAnchor = -1;
                    anyChanged = true;
                }
            }

            ImGui::EndDisabled();
            HelpMarker("Make the picture look right, then press this -- it captures the current look as a"
                       "\npoint. For the first point use the Paper white slider above; for every point"
                       "\nafter, move to different light and use the Trim, which this then bakes in."
                       "\n\nOne point calibrates a ratio and the white point follows the scan from there."
                       "\nWalk into very different light and press it again: the second point pins down"
                       "\nthe buffer's real curve, so everything between the two is right rather than"
                       "\nonly the neighbourhood of one anchor. Up to eight."
                       "\n\nThe table is per game and shareable -- one person calibrates a game and the"
                       "\nnumbers are the same for everyone who takes the profile.");

            if (!isSource)
                ImGui::TextColored(kTextDim, "(the scan is only watching -- the white point above comes from "
                                             "somewhere else)");

            if (!anchors.empty())
            {
                // The row nearest the live value in log space is the one driving the picture; mark
                // it, so which calibration is in effect is visible rather than inferred.
                int active = 0;
                float bestDist = 1e30f;
                const float liveLog = std::log(std::max(live, 1e-6f));

                for (size_t i = 0; i < anchors.size(); ++i)
                {
                    const float d = std::fabs(std::log(std::max(anchors[i].scan, 1e-6f)) - liveLog);

                    if (d < bestDist)
                    {
                        bestDist = d;
                        active = (int) i;
                    }
                }

                for (size_t i = 0; i < anchors.size(); ++i)
                {
                    ImGui::PushID((int) i);

                    // Delete first, so its click is never swallowed by the row-wide Selectable.
                    if (ImGui::SmallButton("x"))
                    {
                        DlssNr::ExposureScan::AnchorRemove((int) i);
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                        anyChanged = true;

                        if (selectedAnchor == (int) i)
                            selectedAnchor = -1;
                        else if (selectedAnchor > (int) i)
                            --selectedAnchor;

                        ImGui::PopID();
                        continue;
                    }

                    ImGui::SameLine();

                    const bool sel = (int) i == selectedAnchor;
                    char row[96];
                    snprintf(row, sizeof(row), "%s scan %.4f  ->  white %.2f%s",
                             ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan, anchors[i].white,
                             sel ? "   [editing]" : "");

                    // Click selects the row, so the slider above edits it; click again to let go.
                    if (ImGui::Selectable(row, sel))
                        selectedAnchor = sel ? -1 : (int) i;

                    ImGui::PopID();
                }

                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
                ImGui::TextColored(kTextDim, "Click a row to edit it with the slider above; click it again to "
                                             "control the live point. > is the point in use now.");
                ImGui::PopTextWrapPos();
            }

            // Only means anything with a single point: with two or more, the direction the white
            // point moves is already fixed by the data.
            if (anchors.size() == 1)
            {
                if (bool inverted = config->DlssNrScanInverted.value_or_default();
                    NrCheckbox("The number runs the other way", &inverted))
                {
                    config->DlssNrScanInverted = inverted;
                    anyChanged = true;
                }
                HelpMarker("Flip this if the picture gets worse in the direction it should be getting"
                           "\nbetter. Most engines store an exposure that falls as the scene brightens;"
                           "\nsome store its reciprocal, and a buffer found by shape does not say which."
                           "\nAdd a second anchor point in different light and this is decided for you,"
                           "\nso it disappears.");
            }

            // Read-out rather than control: what the scan is looking at, and how to tell whether it
            // found the right thing. Folded away, because the two decisions that matter are above.
            if (ImGui::TreeNode("Candidates"))
            {
                const auto found = DlssNr::ExposureScan::Report();
                const char* why = DlssNr::ExposureScan::Status();

                if (found.empty())
                {
                    ImGui::TextColored(kTextDim, "%s", why != nullptr && why[0] != 0 ? why : "nothing matched yet.");
                }
                else
                {
                    for (size_t i = 0; i < found.size(); ++i)
                    {
                        const auto& c = found[i];

                        if (c.reads == 0)
                        {
                            ImGui::TextColored(kTextDim, "%zu. %s -- not read yet", i + 1, c.shape.c_str());
                            continue;
                        }

                        // Moving is the whole signal, so it is the thing that is coloured.
                        ImGui::TextColored(c.moves ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f) : kTextDim,
                                           "%zu. %s = %.5f  (seen %.5f..%.5f) %s", i + 1, c.shape.c_str(), c.latest,
                                           c.lowest, c.highest, c.moves ? "MOVES" : "flat so far");
                    }

                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
                    ImGui::TextColored(kTextDim, "Walk from shade into daylight. A real exposure moves. One that "
                                                 "only ever climbs is a counter, not an exposure.");
                    ImGui::PopTextWrapPos();
                }

                ImGui::TreePop();
            }
        }

        // Both of these describe the frame to the model rather than shaping its output, which is
        // why they sit together and away from the strength controls.
        SectionCaption("Guide", rowWidth);

        static const char* depthNames[] = { "Follow the game", "Force normal", "Force inverted" };
        int depthMode = (int) config->DlssNrDepthConvention.value_or_default();
        if (NrCombo("Depth", &depthMode, depthNames, IM_ARRAYSIZE(depthNames), rowWidth))
        {
            config->DlssNrDepthConvention = (uint32_t) depthMode;
            anyChanged = true;
        }
        HelpMarker("Which way round the model is told depth runs. The game states this in the flags it"
                   "\ncreated its own DLSS feature with, and following it is right almost always -- but"
                   "\na game that states it wrongly needs correcting by hand."
                   "\n\nIf the pass looks worst where geometry meets sky, try forcing the other one.");

        if (bool uiCorrection = config->DlssNrUICorrection.value_or_default();
            NrCheckbox("UI correction", &uiCorrection))
        {
            config->DlssNrUICorrection = uiCorrection;
            anyChanged = true;
        }
        HelpMarker("Lets the model account for a UI layer laid over the frame. On is its own default"
                   "\nand right whenever a UI resource reaches it; turn it off if the correction is"
                   "\nitself what looks wrong."
                   "\n\nRead when the model is built.");

        SectionCaption("Inspect", rowWidth);

        if (DlssNr::CaptureInProgress())
        {
            ImGui::TextColored(kTextDim, "Capturing...");
        }
        else if (ImGui::Button("Capture 8 frames"))
        {
            DlssNr::RequestCapture(8);
        }
        HelpMarker("Writes eight consecutive frames twice: as the upscaler produced them, and again"
                   "\nonce the model's edit was applied. Into a dlssnr-capture folder beside"
                   "\nOptiScaler; each run overwrites the last.");

        if (bool autoCapture = config->DlssNrAutoCapture.value_or_default();
            NrCheckbox("Auto-capture once per session", &autoCapture))
        {
            config->DlssNrAutoCapture = autoCapture;
            anyChanged = true;
        }
        HelpMarker("Writes one matched before/after set automatically, without anyone asking. The"
                   "\nfolder is cleared each run, so it holds a single session and never grows.");

        // Freeze the frame the model works on, so a setting change re-renders it in place -- the
        // only clean way to A/B our own settings, since a moving scene confounds every other
        // comparison. See design/frame-hold.md.
        if (bool held = config->DlssNrHoldFrame.value_or_default(); NrCheckbox("Hold frame", &held))
        {
            config->DlssNrHoldFrame = held;
            anyChanged = true;
        }
        HelpMarker("Freezes the frame the model works on. While held, change paper white, the strengths,"
                   "\nthe reversible mode, the model preset -- anything below the upscaler -- and only"
                   "\nthat setting moves; the scene does not. Pairs with \"Apply the model\" at the top:"
                   "\nfreeze a frame, then toggle that to see it with and without."
                   "\n\nWhat it cannot show: upscaler presets or anything upstream of this pass (the"
                   "\nupscaler is not re-run on a held frame), and the game's own HUD and"
                   "\npost-processing, which run after this and keep updating. The white point stops"
                   "\nbeing measured and holds its value, so it cannot drift and confound the"
                   "\ncomparison."
                   "\n\nClose the panel and it stays held. Untick to resume.");

        static const char* compareNames[] = { "Off", "Side by side", "Wipe" };
        int compare = (int) config->DlssNrCompare.value_or_default();
        if (NrCombo("Compare", &compare, compareNames, IM_ARRAYSIZE(compareNames), rowWidth))
        {
            config->DlssNrCompare = (uint32_t) compare;
            anyChanged = true;
        }
        HelpMarker("Shows the pass against itself. Side by side puts the whole frame in each half;"
                   "\nwipe cuts a single frame at the split and plays normally. Neither needs the"
                   "\nmenu open to keep working.");

        if (compare != 0)
        {
            bool swap = config->DlssNrCompareSwap.value_or_default();
            if (NrCheckbox("Swap sides", &swap))
            {
                config->DlssNrCompareSwap = swap;
                anyChanged = true;
            }

            bool tags = config->DlssNrCompareTags.value_or_default();
            if (NrCheckbox("Labels", &tags))
            {
                config->DlssNrCompareTags = tags;
                anyChanged = true;
            }
            HelpMarker("Draws which side is which into the frame's own plane, so a screenshot still"
                       "\nsays it. Clipped per side, so the wipe reveals and hides them exactly as it"
                       "\ndoes the images.");

            if (tags)
            {
                float tagScale = config->DlssNrTagScale.value_or_default();
                auto rTag = NrSlider("Label size", &tagScale, 0.5f, 5.0f, "%.1fx", rowWidth);
                if (rTag.changed)
                    config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
                if (rTag.released)
                    anyChanged = true;
            }
        }

        if (compare == 1)
        {
            float zoom = config->DlssNrCompareZoom.value_or_default();
            auto rZoom = NrSlider("Zoom", &zoom, 1.0f, 2.0f, "%.2f", rowWidth);
            if (rZoom.changed)
                config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);
            if (rZoom.released)
                anyChanged = true;
        }

        if (compare == 2)
        {
            float split = config->DlssNrCompareSplit.value_or_default();
            auto rSplit = NrSlider("Split", &split, 0.0f, 1.0f, "%.2f", rowWidth);
            if (rSplit.changed)
                config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);
            if (rSplit.released)
                anyChanged = true;
        }

        static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                            "Difference (amplified)" };
        int debugView = (int) config->DlssNrDebugView.value_or_default();
        if (NrCombo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames), rowWidth))
        {
            config->DlssNrDebugView = (uint32_t) debugView;
            anyChanged = true;
        }
        HelpMarker("Proxy is the picture handed to the model. Difference shows what the model"
                   "\nactually changed, amplified twenty times and centred on grey.");

        // Both of these are experiments toward dropping the forwarder entirely, which is why they
        // ship off. Config.h calls the probe "a diagnostic, not a feature", and the proxy path
        // "off until it is shown to produce the same picture" -- so they are labelled as such
        // rather than presented as ordinary settings.
        SectionCaption("Experimental", rowWidth);

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
        ImGui::TextColored(kTextDim, "Both are unproven. They exist to test whether the driver's own "
                                     "nvngx.dll can dispatch the model, which would remove the need for "
                                     "the 165 MB copy beside OptiScaler.");
        ImGui::PopTextWrapPos();

        if (bool probe = config->DlssNrProxyProbe.value_or_default(); NrCheckbox("Probe the driver", &probe))
        {
            config->DlssNrProxyProbe = probe;
            anyChanged = true;
        }
        HelpMarker("Asks the driver's nvngx.dll once per session whether it already knows the model."
                   "\nWrites the answer to the log and changes nothing else."
                   "\n\nRead when the model is built, so it applies from the next session.");

        if (bool useProxy = config->DlssNrUseProxy.value_or_default(); NrCheckbox("Run through the driver", &useProxy))
        {
            config->DlssNrUseProxy = useProxy;
            anyChanged = true;
        }
        HelpMarker("Drives the model through the driver's own nvngx.dll instead of the forwarder --"
                   "\nthe way DLSS itself is called. If the picture matches, the forwarder is"
                   "\nunnecessary."
                   "\n\nCompare before trusting it: turn on Compare above and look for a difference.");
    }

    // Outside the if, not inside it. Begin returns false whenever the window is clipped out, and
    // ImGui wants End called for every Begin regardless -- skipping it leaves the window stack
    // unbalanced and trips "Mismatched Begin/End calls" in EndFrame.
    ImGui::End();

    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(14);

    // This overlay saves as you go rather than needing a Save button -- there is nothing else
    // in this build's menu to put one on.
    if (anyChanged)
        config->SaveIni();
}

} // namespace DlssNr

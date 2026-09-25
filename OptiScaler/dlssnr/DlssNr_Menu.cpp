// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"

#include "DlssNr.h"
#include "DlssNrFeature_Vk.h"
#include "DlssNr_ExposureScan.h"
#include "DlssNr_I18n.h"
#include "DlssNr_PresentRoute.h"
#include "DlssNr_ReLimiter.h"
#include "DlssNr_RenoDx.h"
#include "DlssNrBudget.h"

#include <Config.h>
#include <misc/IdentifyGpu.h>
#include <misc/LosslessScaling.h>
#include <hooks/Streamline_Hooks.h>

#include <menu/menu_common.h>

#include <imgui/imgui.h>

#include "DlssNr_PanelLayout.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace DlssNr
{

using I18n::Tr;

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
// Two palettes rather than one set of constants, because a value that reads well on a near-black
// panel is invisible on a light one and the reverse. The old dark set is kept verbatim as Dark().
//
// Every ratio below is measured against that palette's own panel background, and every text colour
// clears 4.5:1. The dark set did not: its dimmed text sat at 2.65:1, which is where "hard to read"
// came from -- and an overlay is read at a glance, over a moving picture, so it has less margin
// than a web page, not more.
struct Palette
{
    ImVec4 accent;     // accent used as TEXT -- darkened on light so it still reads
    ImVec4 accentFill; // accent used as a FILL, where brand colour matters more than contrast
    ImVec4 title;
    ImVec4 caption;
    ImVec4 text;
    ImVec4 value;
    ImVec4 textDim;
    ImVec4 track;
    ImVec4 panelBg;
    ImVec4 popupBg;
    ImVec4 onAccent;    // text drawn ON the accent fill
    float overlayLevel; // 1 = lighten with white, 0 = darken with black

    // The panel shades things by laying translucent white over a dark background. On a light panel
    // that does nothing at all -- the eight places doing it would simply vanish -- so the shade
    // colour flips with the theme and every one of them goes through here.
    ImVec4 overlay(float alpha) const { return ImVec4(overlayLevel, overlayLevel, overlayLevel, alpha); }
};

static const Palette& Dark()
{
    static const Palette p = { ImVec4(0.549f, 0.729f, 0.239f, 1.0f), ImVec4(0.549f, 0.729f, 0.239f, 1.0f),
                               ImVec4(0.906f, 0.906f, 0.906f, 1.0f), ImVec4(0.776f, 0.780f, 0.796f, 1.0f),
                               ImVec4(0.635f, 0.635f, 0.627f, 1.0f), ImVec4(0.541f, 0.541f, 0.549f, 1.0f),
                               ImVec4(0.376f, 0.369f, 0.373f, 1.0f), ImVec4(0.157f, 0.157f, 0.157f, 1.0f),
                               ImVec4(0.110f, 0.110f, 0.114f, 1.0f), ImVec4(0.086f, 0.086f, 0.090f, 0.98f),
                               ImVec4(0.060f, 0.090f, 0.050f, 1.0f), 1.0f };
    return p;
}

// Panel #999999 -- true mid grey, darker than the near-white #CDCED0 this used before (an earlier
// pass tried #B3B3B3, which still read as glaring against a dark game scene). Contrast against it,
// in order: 4.60, --, 7.37, 7.37, 7.37, 5.80, 4.60. Title, caption and text converge on pure black
// at this level -- #999999 doesn't leave enough headroom above it to keep them as separate tiers
// the way the near-white panel could -- so all three render in the same near-black; only value and
// textDim stay visibly distinct from it.
static const Palette& Light()
{
    static const Palette p = { ImVec4(0.152f, 0.208f, 0.054f, 1.0f), ImVec4(0.475f, 0.631f, 0.207f, 1.0f),
                               ImVec4(0.000f, 0.000f, 0.000f, 1.0f), ImVec4(0.000f, 0.000f, 0.000f, 1.0f),
                               ImVec4(0.000f, 0.000f, 0.000f, 1.0f), ImVec4(0.117f, 0.121f, 0.129f, 1.0f),
                               ImVec4(0.185f, 0.190f, 0.201f, 1.0f), ImVec4(0.523f, 0.526f, 0.535f, 1.0f),
                               ImVec4(0.600f, 0.600f, 0.600f, 1.0f), ImVec4(0.650f, 0.650f, 0.650f, 0.98f),
                               ImVec4(1.000f, 1.000f, 1.000f, 1.0f), 0.0f };
    return p;
}

// The same two palettes with AMD's red in place of NVIDIA's green, for a panel drawn over a game
// running on an AMD card. Only the three accent slots change; every neutral stays as measured
// above, so the contrast figures there still hold for everything but the accent. The accent-as-
// text tints were picked for the same 4.5:1 floor against each panel background: #FF6B70 on the
// dark panel (6.2:1 -- AMD's #ED1C24 itself only manages 3.9:1 there, so it is kept for fills),
// #5E0609 on the light one (4.9:1). Text on the red fill is white, as AMD's own branding has it.
static const Palette& DarkAmd()
{
    static const Palette p = []
    {
        Palette q = Dark();
        q.accent = ImVec4(1.000f, 0.420f, 0.439f, 1.0f);     // #FF6B70
        q.accentFill = ImVec4(0.929f, 0.110f, 0.141f, 1.0f); // #ED1C24
        q.onAccent = ImVec4(1.000f, 1.000f, 1.000f, 1.0f);
        return q;
    }();
    return p;
}

static const Palette& LightAmd()
{
    static const Palette p = []
    {
        Palette q = Light();
        q.accent = ImVec4(0.369f, 0.024f, 0.035f, 1.0f);     // #5E0609
        q.accentFill = ImVec4(0.831f, 0.078f, 0.106f, 1.0f); // #D4141B
        q.onAccent = ImVec4(1.000f, 1.000f, 1.000f, 1.0f);
        return q;
    }();
    return p;
}

// Whether the game is running on an AMD card. Asked once: the answer cannot change mid-process,
// and IdentifyGpu enumerates adapters through DXGI, which is not a per-frame cost to pay.
static bool OnAmdGpu()
{
    static const bool amd = IdentifyGpu::getPrimaryGpu().vendorId == VendorId::AMD;
    return amd;
}

// Chosen once per frame in RenderMenu so a mid-frame config change cannot split a single draw
// across two palettes.
static const Palette* g_pal = &Light();

#define kAccent (g_pal->accent)
#define kAccentFill (g_pal->accentFill)
#define kTitle (g_pal->title)
#define kCaption (g_pal->caption)
#define kText (g_pal->text)
#define kValue (g_pal->value)
#define kTextDim (g_pal->textDim)
#define kTrack (g_pal->track)
#define kPanelBg (g_pal->panelBg)

static float PanelWidth(float scale) { return 460.0f * scale; }

// The wipe's divide, over the frame, with a grab handle at its middle. Only while the panel is up:
// that is when the mouse is ours (the panel blocks it from the game), and it is where Compare was
// turned on in the first place. Returns true when a drag finished, so the caller saves the ini.
//
// Drawn on the foreground list rather than as a window: it must sit over the whole frame, take no
// focus, and never join the panel's navigation order. Hit-testing is done by hand against the mouse
// for the same reason -- an InvisibleButton here would be an item in whatever window is current.
static bool DragCompareSplit(Config* config)
{
    if (config->DlssNrCompare.value_or_default() != 2) // Wipe only: side by side has no divide to move
        return false;

    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
        return false;

    static bool dragging = false;

    const float scale = std::max(1.0f, io.DisplaySize.y / 1080.0f);
    const float split = std::clamp(config->DlssNrCompareSplit.value_or_default(), 0.0f, 1.0f);
    const float x = split * io.DisplaySize.x;
    const ImVec2 centre(x, io.DisplaySize.y * 0.5f);
    const float radius = 13.0f * scale;

    // The grip, or anywhere down the line within a finger's width of it.
    const float dx = io.MousePos.x - centre.x;
    const float dy = io.MousePos.y - centre.y;
    const bool overGrip = (dx * dx + dy * dy) <= (radius * radius);
    const bool overLine = std::fabs(dx) <= 12.0f * scale;
    const bool hot = !ImGui::GetIO().WantCaptureMouse && (overGrip || overLine);

    if (hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        dragging = true;
    if (dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        dragging = false;
        return true; // let go: this is the split from now on
    }
    if (dragging)
        config->DlssNrCompareSplit = std::clamp(io.MousePos.x / io.DisplaySize.x, 0.0f, 1.0f);

    if (hot || dragging)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // White on a black hairline, so it reads on a bright frame and a dark one alike.
    const float px = std::clamp(config->DlssNrCompareSplit.value_or_default(), 0.0f, 1.0f) * io.DisplaySize.x;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddLine(ImVec2(px, 0.0f), ImVec2(px, io.DisplaySize.y), IM_COL32(0, 0, 0, 90), 4.0f * scale);
    dl->AddLine(ImVec2(px, 0.0f), ImVec2(px, io.DisplaySize.y), IM_COL32(255, 255, 255, 215), 2.0f * scale);

    const ImVec2 grip(px, io.DisplaySize.y * 0.5f);
    const ImU32 fill = dragging ? ImGui::GetColorU32(kAccent) : IM_COL32(20, 22, 20, 190);
    dl->AddCircleFilled(grip, radius, fill);
    dl->AddCircle(grip, radius, IM_COL32(255, 255, 255, 215), 0, 1.5f * scale);

    // Two arrowheads, left and right: the one thing this control does.
    const float a = radius * 0.46f;
    const ImU32 ink = dragging ? ImGui::GetColorU32(g_pal->onAccent) : IM_COL32(255, 255, 255, 235);
    dl->AddTriangleFilled(ImVec2(grip.x - a * 1.5f, grip.y), ImVec2(grip.x - a * 0.4f, grip.y - a * 0.8f),
                          ImVec2(grip.x - a * 0.4f, grip.y + a * 0.8f), ink);
    dl->AddTriangleFilled(ImVec2(grip.x + a * 1.5f, grip.y), ImVec2(grip.x + a * 0.4f, grip.y - a * 0.8f),
                          ImVec2(grip.x + a * 0.4f, grip.y + a * 0.8f), ink);
    return false;
}

// The engine's failure reasons stay English where they are made -- they also go to OptiScaler.log and
// through the API, where support needs one wording -- and are translated here, at display. A fixed
// reason is a translation key itself. The add-on conflict is built around a file name, so it is
// matched by its tail and rebuilt from the translated pattern.
static std::string TrReason(const char* reason)
{
    static const std::string kConflictTail = " is already doing this -- remove it, or turn this off";
    const std::string text(reason != nullptr ? reason : "");

    if (text.size() > kConflictTail.size() &&
        text.compare(text.size() - kConflictTail.size(), kConflictTail.size(), kConflictTail) == 0)
    {
        const std::string file = text.substr(0, text.size() - kConflictTail.size());
        char buf[512];
        snprintf(buf, sizeof(buf), Tr("%s is already doing this -- remove it, or turn this off"), file.c_str());
        return buf;
    }

    return Tr(text.c_str());
}

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

// Byte length of the UTF-8 sequence that starts with this lead byte. A stray continuation byte
// counts as one, so a malformed string still advances rather than looping.
static int Utf8Len(unsigned char lead)
{
    if (lead < 0x80)
        return 1;
    if ((lead & 0xE0) == 0xC0)
        return 2;
    if ((lead & 0xF0) == 0xE0)
        return 3;
    if ((lead & 0xF8) == 0xF0)
        return 4;
    return 1;
}

// Upper-cased captions, matching NVIDIA's own panel ("GLOBAL CONTROLS", ...). Works on code
// points, not bytes: toupper on a byte of a multi-byte character corrupts it. Covers ASCII, the
// Latin-1 letters the Portuguese/Spanish/German files use, and Cyrillic; anything else (CJK has
// no case) passes through unchanged.
static std::string Caps(const char* text)
{
    std::string out;

    for (const char* p = text; *p;)
    {
        const int len = Utf8Len((unsigned char) *p);

        if (len == 1)
        {
            out += (char) std::toupper((unsigned char) *p);
            p += 1;
            continue;
        }

        if (len == 2 && (p[1] & 0xC0) == 0x80)
        {
            unsigned int cp = ((unsigned char) p[0] & 0x1F) << 6 | ((unsigned char) p[1] & 0x3F);

            if ((cp >= 0x00E0 && cp <= 0x00FE && cp != 0x00F7) || (cp >= 0x0430 && cp <= 0x044F))
                cp -= 0x20;
            else if (cp >= 0x0450 && cp <= 0x045F)
                cp -= 0x50;

            out += (char) (0xC0 | (cp >> 6));
            out += (char) (0x80 | (cp & 0x3F));
            p += 2;
            continue;
        }

        out.append(p, len);
        p += len;
    }

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

    // One code point at a time, not one byte: a byte of a multi-byte character is not a glyph.
    for (const char* p = text; *p;)
    {
        const char* begin = p;
        const char* end = p + Utf8Len((unsigned char) *p);

        dl->AddText(font, fontSize, ImVec2(origin.x + x, origin.y), col, begin, end);
        x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, begin, end).x + tracking;
        p = end;
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
    ImGui::GetWindowDrawList()->AddLine(p0, ImVec2(p0.x + rowWidth, p0.y), ImGui::GetColorU32(g_pal->overlay(0.14f)),
                                        1.0f);
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

    // A label that does not fit its column goes ABOVE its slider instead of being run into by it.
    // The long ones are real ("Trim (x the game's exposure)"), and they get longer in German, in
    // French, and at any font scale above 1x -- so this is the rule, not a special case.
    const bool stacked = ImGui::CalcTextSize(label).x > labelWidth - style.ItemSpacing.x;

    float trackWidth = (stacked ? rowWidth : rowWidth - labelWidth) - valueWidth - style.ItemSpacing.x * 2.0f;
    if (trackWidth < 40.0f)
        trackWidth = 40.0f;

    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (stacked)
        labelWidth = 0.0f; // the slider starts at the left edge, on its own line
    else
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
    dl->AddCircle(hCenter, radius,
                  ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, g_pal->overlayLevel > 0.5f ? 0.45f : 0.22f)), 18, 1.4f);

    ImGui::SameLine(labelWidth + trackWidth + style.ItemSpacing.x);
    ImGui::PushStyleColor(ImGuiCol_Text, kValue);
    ImGui::Text(fmt, *value);
    ImGui::PopStyleColor();

    ImGui::PopID();

    return { changed, released };
}

// A number the user types, for the values where the exact figure is the point and dragging cannot
// reach it. A frame-rate cap is the case that forced it: the reason to set a fixed one at all is to
// match a number chosen somewhere else -- 72 in the game's own limiter, 141 under a 144 Hz ceiling --
// and no track from 30 to 1000 lands on those. Requested 2026-09-25.
//
// Committed on Enter or on leaving the field, never per keystroke: typing "120" passes through 1 and
// 12, and writing those through to the add-on would apply two frame rates nobody asked for on the way
// to the one they did. Out-of-range is clamped rather than refused, and the box is rewritten with
// what was actually stored, so the field never shows a number the add-on is not holding.
struct NumberBoxResult
{
    bool committed;
};

static NumberBoxResult NrNumberBox(const char* label, double* value, double vMin, double vMax, bool isInt,
                                   float rowWidth)
{
    ImGui::PushID(label);

    ImGuiStyle& style = ImGui::GetStyle();
    float labelWidth = rowWidth * 0.44f;

    // The same rule as NrSlider: a label too long for its column goes above its control rather than
    // being run into by it. German and French make this the common case, not the exception.
    const bool stacked = ImGui::CalcTextSize(label).x > labelWidth - style.ItemSpacing.x;

    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (stacked)
        labelWidth = 0.0f;
    else
        ImGui::SameLine(labelWidth);

    float boxWidth = rowWidth - labelWidth;
    if (boxWidth > 120.0f)
        boxWidth = 120.0f;
    if (boxWidth < 60.0f)
        boxWidth = 60.0f;

    bool committed = false;
    ImGui::SetNextItemWidth(boxWidth);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(20, 22, 20, 190));
    ImGui::PushStyleColor(ImGuiCol_Text, kValue);
    if (isInt)
    {
        int v = (int) *value;
        // Step 0 hides the +/- buttons: they are a slider by another name and would put the same
        // "click your way towards it" behaviour back on the row.
        ImGui::InputInt("##v", &v, 0, 0, ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            double clamped = (double) v;
            clamped = clamped < vMin ? vMin : (clamped > vMax ? vMax : clamped);
            *value = clamped;
            committed = true;
        }
    }
    else
    {
        double v = *value;
        ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.3f", ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            double clamped = v < vMin ? vMin : (v > vMax ? vMax : v);
            *value = clamped;
            committed = true;
        }
    }
    ImGui::PopStyleColor(2);

    // What the box will take, said once beside it rather than discovered by having a number refused.
    ImGui::SameLine();
    char range[64] {};
    if (isInt)
        std::snprintf(range, sizeof(range), "%d-%d", (int) vMin, (int) vMax);
    else
        std::snprintf(range, sizeof(range), "%.3f-%.3f", vMin, vMax);
    ImGui::TextColored(kTextDim, "%s", range);

    ImGui::PopID();

    return { committed };
}

// One entry in a row of boxed choices, defined below with the Models row it was written for.
static bool ModelButton(const char* label, bool active, float width);

// A choice of a few: every option in its own box, the chosen one ringed in the accent -- the same
// shape as the Models row and the page buttons, and as Deep Fried Chicken's menu, so a choice looks
// like a choice everywhere in this panel. A list too long or too wide to lay out that way stays a
// dropdown: six filter names across a 460 px row would be three letters each.
static bool NrCombo(const char* label, int* v, const char* const* items, int count, float rowWidth)
{
    float labelWidth = rowWidth * 0.44f;

    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(labelWidth);

    const float controlWidth = rowWidth - labelWidth;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    // Boxes only where every one of them can still be read: the widest label has to fit its share.
    bool boxes = count >= 2 && count <= 4;
    if (boxes)
    {
        float widest = 0.0f;
        for (int i = 0; i < count; ++i)
            widest = std::max(widest, ImGui::CalcTextSize(items[i]).x);

        const float each = (controlWidth - spacing * (count - 1)) / count;
        boxes = each >= widest + ImGui::GetStyle().FramePadding.x * 2.0f;
    }

    if (!boxes)
    {
        ImGui::SetNextItemWidth(controlWidth);
        std::string id = std::string("##") + label;
        return ImGui::Combo(id.c_str(), v, items, count);
    }

    const float each = (controlWidth - spacing * (count - 1)) / count;
    bool changed = false;

    ImGui::PushID(label);
    for (int i = 0; i < count; ++i)
    {
        if (i > 0)
            ImGui::SameLine();

        ImGui::PushID(i);
        if (ModelButton(items[i], *v == i, each))
        {
            *v = i;
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::PopID();

    return changed;
}

// One entry in the "Models" row -- the segmented Model A / B / C selector.
static bool ModelButton(const char* label, bool active, float width)
{
    // Sampled off NVIDIA's panel: the selected pill is a dark olive fill (#353D1D) with a green
    // border and green label; the unselected ones are flat neutral grey (#3A3A3A, text #8C8C8C).
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccentFill.x, kAccentFill.y, kAccentFill.z, 0.28f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(kAccentFill.x, kAccentFill.y, kAccentFill.z, 0.38f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(kAccentFill.x, kAccentFill.y, kAccentFill.z, 0.46f));
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, g_pal->overlay(0.14f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal->overlay(0.20f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_pal->overlay(0.26f));
        ImGui::PushStyleColor(ImGuiCol_Text, kValue);
        ImGui::PushStyleColor(ImGuiCol_Border, g_pal->overlay(0.10f));
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

        ImU32 dark = ImGui::GetColorU32(g_pal->onAccent);
        ImVec2 a(pos.x + boxSize * 0.22f, pos.y + boxSize * 0.55f);
        ImVec2 b(pos.x + boxSize * 0.42f, pos.y + boxSize * 0.76f);
        ImVec2 cpt(pos.x + boxSize * 0.80f, pos.y + boxSize * 0.26f);
        dl->AddLine(a, b, dark, 2.0f);
        dl->AddLine(b, cpt, dark, 2.0f);
    }
    else
    {
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(g_pal->overlay(hovered ? 0.10f : 0.05f)), 2.0f);
        dl->AddRect(c0, c1, ImGui::GetColorU32(g_pal->overlay(0.20f)), 2.0f, 0, 1.0f);
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

        ImU32 dark = ImGui::GetColorU32(g_pal->onAccent);
        ImVec2 a(pos.x + boxSize * 0.22f, pos.y + boxSize * 0.55f);
        ImVec2 b(pos.x + boxSize * 0.42f, pos.y + boxSize * 0.76f);
        ImVec2 cpt(pos.x + boxSize * 0.80f, pos.y + boxSize * 0.26f);
        dl->AddLine(a, b, dark, 2.2f);
        dl->AddLine(b, cpt, dark, 2.2f);
    }
    else
    {
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(g_pal->overlay(hovered ? 0.10f : 0.06f)), 3.0f);
        dl->AddRect(c0, c1, ImGui::GetColorU32(g_pal->overlay(0.24f)), 3.0f, 0, 1.2f);
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

// An absent later-pass setting inherits pass 1. The first combo item represents that absence; the
// remaining items map directly to the model's zero-based profile values.
static bool InheritedProfileCombo(const char* label, CustomOptional<uint32_t, NoDefault>* opt, const char* const* names,
                                  int nameCount, float rowWidth)
{
    int selected = 0;

    if (opt->has_value())
        selected = std::clamp((int) opt->value(), 0, nameCount - 2) + 1;

    if (!NrCombo(label, &selected, names, nameCount, rowWidth))
        return false;

    if (selected == 0)
        *opt = std::optional<uint32_t> {};
    else
        *opt = (uint32_t) (selected - 1);

    return true;
}

// Adaptive model resolution: the controls, and a live sentence saying what it is doing.
//
// The sentence matters as much as the controls. A scale that moves on its own is indistinguishable
// from a bug unless the panel says why it moved, and the one case a player most needs told -- the
// frame rate they asked for is out of reach for reasons that have nothing to do with this pass --
// is the case where a silent floor looks most like a failure.
static void DrawAutoScale(Config* config, float rowWidth, bool& anyChanged)
{
    // The floor cannot go below the controller's own bottom rung; asking for less would set a number
    // the controller could not honour and then quietly stop above it.
    const int kFloorMin = (int) std::lroundf(DlssNrBudget::Rungs[DlssNrBudget::RungCount - 1] * 100.0f);

    bool autoOn = config->DlssNrAutoScale.value_or_default();

    if (NrCheckbox(Tr("Adjust it for me"), &autoOn))
    {
        config->DlssNrAutoScale = autoOn;
        anyChanged = true;
    }

    HelpMarker(Tr("Moves Model resolution up and down while you play, so the pass costs what you asked"
                  "\nit to cost instead of what one number chosen before the game started happens to"
                  "\ncost in this scene."
                  "\n\nIt only ever changes the MODEL's resolution. The frame is never reduced, so this"
                  "\ncannot soften the picture the way a dynamic render resolution does -- the most it"
                  "\ncan cost is some of the model's own detail."
                  "\n\nIt steps between four settings a few seconds apart at most, because each change"
                  "\nrebuilds the model and rebuilding it every frame would be slower than doing nothing."));

    if (!autoOn)
        return;

    // Native Vulkan runs its own pass with its own timer and does not go through the controller yet.
    // Saying so is better than showing controls that quietly do nothing on that route.
    if (IsRunningVk())
    {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
        ImGui::TextColored(kTextDim, "%s",
                           Tr("Not on the native Vulkan path yet - it runs its own pass. Model resolution "
                              "stays where you put it here."));
        ImGui::PopTextWrapPos();
        return;
    }

    // The three ways of saying what the budget is, in the words a player would use rather than the
    // controller's own. Order matches DlssNrBudget::Mode.
    // Not static: Tr() resolves against the language chosen this frame, and a static array would
    // freeze whichever language happened to be up the first time this drew.
    const char* const kModeNames[] = {
        Tr("Share of the frame"),
        Tr("Milliseconds"),
        Tr("Frame rate"),
    };

    int mode = (int) config->DlssNrAutoScaleMode.value_or_default();

    if (mode < 0 || mode >= IM_ARRAYSIZE(kModeNames))
        mode = 2;

    // ReLimiter is already holding the frame rate, so aiming the model's resolution at one as well
    // means shedding detail to close a gap the limiter will never allow to close. Said here, next to
    // the control, rather than left as a mystery when the manager turns Adjust it for me off.
    if (DlssNrReLimiter::PacingActive())
    {
        ImGui::TextDisabled("%s", Tr("Frame pacing is holding the frame rate (see Pacing)."));
        HelpMarker(Tr("ReLimiter is in this game and paces frames to a target. Aiming at a frame rate here"
                      "\ntoo would have the model shed resolution trying to reach a number the limiter"
                      "\nwill not let the game pass, and keep shedding it -- detail lost for no frames"
                      "\ngained. Pick Milliseconds or Share of the frame to cap what the pass costs, or"
                      "\nremove frame pacing if you would rather this aimed at the frame rate."));
    }

    if (NrCombo(Tr("Aim at"), &mode, kModeNames, IM_ARRAYSIZE(kModeNames), rowWidth))
    {
        config->DlssNrAutoScaleMode = (uint32_t) mode;
        anyChanged = true;
    }

    HelpMarker(Tr("Frame rate: aim at a number of frames per second. The one most people want, and the"
                  "\nonly one that can fall short -- the pass can give back what it costs and no more,"
                  "\nso if the game itself cannot reach the number, the panel says so."
                  "\n\nMilliseconds: hold the pass under a flat time. Exactly what the cost line above"
                  "\nmeasures, with no arithmetic in between."
                  "\n\nShare of the frame: let the pass take at most a percentage of each frame. This one"
                  "\nlooks after itself as the frame rate moves - 15% is 2.5 ms at 60 fps and 1.25 at 120."));

    if (mode == 2)
    {
        float fps = (float) config->DlssNrAutoScaleFps.value_or_default();
        auto r = NrSlider(Tr("Frame rate"), &fps, 30.0f, 240.0f, "%.0f fps", rowWidth);

        if (r.changed || r.released)
        {
            config->DlssNrAutoScaleFps = std::clamp((int) std::lroundf(fps), 30, 240);
            if (r.released)
                anyChanged = true;
        }

        HelpMarker(Tr("The frame rate to aim at. Applied live - there is nothing to rebuild for a change"
                      "\nof target, only for a change of model resolution it leads to."));
    }
    else if (mode == 1)
    {
        float ms = config->DlssNrAutoScaleMs.value_or_default();
        auto r = NrSlider(Tr("Cost ceiling"), &ms, 0.5f, 10.0f, "%.1f ms", rowWidth);

        if (r.changed || r.released)
        {
            config->DlssNrAutoScaleMs = std::clamp(ms, 0.5f, 10.0f);
            if (r.released)
                anyChanged = true;
        }

        HelpMarker(Tr("The most the pass may cost, in milliseconds. Compare it with the cost shown at the"
                      "\ntop of this panel, which is the same measurement."));
    }
    else
    {
        float share = (float) config->DlssNrAutoScaleShare.value_or_default();
        auto r = NrSlider(Tr("Share of the frame"), &share, 2.0f, 50.0f, "%.0f%%", rowWidth);

        if (r.changed || r.released)
        {
            config->DlssNrAutoScaleShare = std::clamp((int) std::lroundf(share), 2, 50);
            if (r.released)
                anyChanged = true;
        }

        HelpMarker(Tr("How much of each frame the pass may take."));
    }

    float floorPercent = config->DlssNrAutoScaleFloor.value_or_default() * 100.0f;
    auto rFloor = NrSlider(Tr("Never go below"), &floorPercent, (float) kFloorMin, 100.0f, "%.0f%%", rowWidth);

    if (rFloor.changed || rFloor.released)
    {
        config->DlssNrAutoScaleFloor = std::clamp((int) std::lroundf(floorPercent), kFloorMin, 100) / 100.0f;
        if (rFloor.released)
            anyChanged = true;
    }

    HelpMarker(Tr("The lowest model resolution this may choose. Raise it to keep more of the model's"
                  "\ndetail and let the frame rate give way instead."
                  "\n\nIt stops here because this is where the trade changes character: above it the"
                  "\nmodel is simply working on a smaller picture, and below it fine detail - hair,"
                  "\nfoliage, thin edges - starts to break down rather than soften."));

    const AutoScaleStatus st = AutoScale();

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);

    if (!st.running)
    {
        // On, but nothing to steer on: either the timer is not trusted on this card and route, or
        // this route supplies no frame time. Both are already said elsewhere in the panel; here it
        // only needs to be clear that nothing is moving.
        ImGui::TextColored(kTextDim, "%s", Tr("Waiting for readings - model resolution is not moving yet."));
    }
    else if (st.gameLimited)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f),
                           Tr("At %.0f%% and still short of %d fps - the rest of the frame is the game's, "
                              "not DLSS 5's."),
                           st.scale * 100.0f, config->DlssNrAutoScaleFps.value_or_default());
    }
    else
    {
        const ImVec4 kOk(0.55f, 0.85f, 0.45f, 1.0f);

        if (mode == 2)
            ImGui::TextColored(kOk, Tr("Holding %d fps - model at %.0f%%%s"),
                               config->DlssNrAutoScaleFps.value_or_default(), st.scale * 100.0f,
                               st.atFloor ? Tr(", as low as it goes") : "");
        else if (mode == 1)
            ImGui::TextColored(kOk, Tr("Holding the pass under %.1f ms - model at %.0f%%%s"),
                               config->DlssNrAutoScaleMs.value_or_default(), st.scale * 100.0f,
                               st.atFloor ? Tr(", as low as it goes") : "");
        else
            ImGui::TextColored(kOk, Tr("Holding the pass to %d%% of the frame - model at %.0f%%%s"),
                               config->DlssNrAutoScaleShare.value_or_default(), st.scale * 100.0f,
                               st.atFloor ? Tr(", as low as it goes") : "");

        if (st.lastPassMs > 0.0)
            ImGui::TextColored(kTextDim, Tr("Pass %.2f ms against a %.2f ms budget."), st.lastPassMs, st.lastBudgetMs);
    }

    ImGui::PopTextWrapPos();
}
// ── the panel's pages ─────────────────────────────────────────────────────────────────────────
//
// One long scroll is hard to read over a moving picture and hard to point anyone at ("under Cost,
// keep scrolling"). These are the same rows, grouped onto six pages picked at the top, so the panel
// is a readable height whatever is open and every control has an address. Deep Fried Chicken's own
// menu is built this way, and ours is what its restyle will follow, so the two match.
//
// The page is UI state, not a setting: it lasts the session and opens on Main next time. Nothing
// here is written to the ini.
enum PanelPage
{
    kPageMain = 0, // whether the pass runs at all, and what it is doing right now
    kPageModel,    // which model, how strong, and what it must leave alone
    kPageCost,     // passes, model resolution, and the budget controller
    kPageImage,    // the filters that scale it, the guards, and how much of it lands
    kPageInspect,  // what the model is told, and the tools for looking at its work
    kPageSetup,    // keys and appearance
    kPagePacing,   // ReLimiter's frame pacing. Always listed; greyed with the reason when it is not here.
    kPageHdr,      // RenoDX's HDR and tone mapping, same deal: greyed unless it is here and drivable.
    kPageCount,
};

static int g_page = kPageMain;
static bool OnPage(int page) { return g_page == page; }

// The state of the pass in one word, filled in the accent when it is actually running and drawn flat
// when it is not, so "is this doing anything" is answered before any sentence is read.
static void StatusBadge(const char* text, bool live)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pad(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y * 0.6f);
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x + pad.x * 2.0f, min.y + size.y + pad.y * 2.0f);

    dl->AddRectFilled(min, max, ImGui::GetColorU32(live ? kAccentFill : g_pal->overlay(0.14f)), 3.0f);
    dl->AddText(ImVec2(min.x + pad.x, min.y + pad.y), ImGui::GetColorU32(live ? g_pal->onAccent : kTextDim), text);

    ImGui::Dummy(ImVec2(max.x - min.x, max.y - min.y));
}

// The panel moves by its top strip only -- the title row down to the page buttons. Everything below
// is controls, and a drag that starts there is someone who missed a slider, not someone moving the
// panel. The window carries NoMove; this is the move. Called with the strip's bottom edge in screen
// space, right after the page buttons are drawn.
static void DragByHeader(float stripBottomY)
{
    static bool dragging = false;
    static ImVec2 grab(0.0f, 0.0f);

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const bool inStrip = io.MousePos.x >= pos.x && io.MousePos.x <= pos.x + size.x && io.MousePos.y >= pos.y &&
                         io.MousePos.y <= stripBottomY;

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        dragging = false;
    // Not over a control: the strip carries the theme button, Reset layout, the X, DLSS ON and the
    // page buttons, and every one of them is a press, not a handle.
    else if (!dragging && inStrip && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() &&
             !ImGui::IsAnyItemActive())
    {
        dragging = true;
        grab = ImVec2(io.MousePos.x - pos.x, io.MousePos.y - pos.y);
    }

    if (dragging)
    {
        ImGui::SetWindowPos(ImVec2(io.MousePos.x - grab.x, io.MousePos.y - grab.y), ImGuiCond_Always);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
}

// ReLimiter's own settings, drawn from what it reports rather than from a list kept here.
//
// The point of enumerating is that this function never has to change when ReLimiter gains a setting:
// its registry grows by one line and the row appears here. Hardcoding the list would mean revisiting
// this file every time they release, which is the cost that makes a fork expensive.
//
// Only what a controller-and-overlay UI can honestly present is drawn. Keybinds are left to ReLimiter's
// own overlay: capturing a key combo needs the capture UI it already has, and half of one here would be
// worse than a pointer to it.
// A Pacing or HDR page whose add-on cannot be driven in this game: greyed, with one plain line saying
// why and what to do. The tab stays listed either way -- hidden, a player who had heard of the feature
// had nowhere to look and nothing to tell him why it was not there.
static void DrawAddonMissing(const char* caption, const char* why, float rowWidth)
{
    ImGui::BeginDisabled();
    SectionCaption(caption, rowWidth);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
    ImGui::TextUnformatted(why);
    ImGui::PopTextWrapPos();
    ImGui::EndDisabled();
}

// The codes are DlssNrReLimiter::UnavailableReason()'s; the pop-out explains the same codes in its own
// words, from OptiScaler.hosted.json.
static const char* PacingMissingText(const char* reason)
{
    if (reason != nullptr && std::strcmp(reason, "no-api") == 0)
        return Tr("ReLimiter is running, but this build of it cannot be driven from this panel -- its own "
                  "overlay still works. Adding frame pacing again from the app installs one that can.");
    if (reason != nullptr && std::strcmp(reason, "api-version") == 0)
        return Tr("ReLimiter is running, but it speaks a different version of the panel's interface than "
                  "this DLSS 5 engine. Update DLSS 5 or frame pacing from the app.");
    return Tr("Frame pacing is not installed on this game -- turn it on from the app's card or the pop-out; "
              "it takes effect the next time the game starts.");
}

static const char* HdrMissingText(const char* reason)
{
    if (reason != nullptr && std::strcmp(reason, "no-api") == 0)
        return Tr("RenoDX is running, but this build of it cannot be driven from this panel -- its own "
                  "overlay still works.");
    if (reason != nullptr && std::strcmp(reason, "api-version") == 0)
        return Tr("RenoDX is running, but it speaks a different version of the panel's interface than "
                  "this DLSS 5 engine. Update DLSS 5 or RenoDX from the app.");
    return Tr("HDR (RenoDX) is not installed on this game -- turn it on from the app's card or the pop-out; "
              "it takes effect the next time the game starts.");
}

static void DrawPacingPage(float rowWidth)
{
    const ReLimiterApi* api = DlssNrReLimiter::Api();
    if (api == nullptr)
    {
        DrawAddonMissing(Tr("Frame pacing"), PacingMissingText(DlssNrReLimiter::UnavailableReason()), rowWidth);
        return;
    }

    SectionCaption(Tr("Frame pacing"), rowWidth);

    const char* version = DlssNrReLimiter::Version();
    ImGui::TextDisabled("ReLimiter %s", version ? version : "?");
    HelpMarker(Tr("ReLimiter holds the frame rate steady for a G-Sync or VRR display rather than making"
                  "\nmore frames. It is a separate add-on with its own overlay; these are its settings,"
                  "\nshown here so there is one panel to look at instead of two."
                  "\n\nBecause it aims at a frame rate, Cost > Adjust it for me cannot aim at one too --"
                  "\nsee the note on that page."));

    const char* lastGroup = nullptr;
    const uint32_t settings = api->setting_count();

    for (uint32_t i = 0; i < settings; ++i)
    {
        ReLimiterSettingInfo info {};
        info.struct_size = sizeof(info);
        if (!api->describe_setting(i, &info))
            continue;

        // A keybind needs a key-capture widget, which this panel has for its OWN keys only; offering a
        // broken one for ReLimiter's would be worse than leaving them where they already work.
        if (info.type == RELIMITER_TYPE_KEYBIND)
            continue;

        // ReLimiter groups its settings already, so the captions come from it rather than from a
        // mapping here that would go stale.
        if (info.group != nullptr && (lastGroup == nullptr || std::strcmp(lastGroup, info.group) != 0))
        {
            SectionCaption(info.group, rowWidth);
            lastGroup = info.group;
        }

        switch (info.type)
        {
        case RELIMITER_TYPE_BOOL:
        {
            double cur = 0.0;
            if (!api->get_number(info.key, &cur))
                break;
            bool on = cur != 0.0;
            if (NrCheckbox(info.label, &on))
            {
                api->set_number(info.key, on ? 1.0 : 0.0);
                api->apply();
                api->save();
            }
            if (info.tooltip != nullptr && *info.tooltip != '\0')
                HelpMarker(info.tooltip);
            break;
        }
        case RELIMITER_TYPE_ENUM:
        {
            char buf[128] {};
            if (!api->get_string(info.key, buf, (uint32_t) sizeof(buf)))
                break;
            int sel = 0;
            for (uint32_t c = 0; c < info.choice_count; ++c)
                if (std::strcmp(info.choices[c], buf) == 0)
                    sel = (int) c;
            if (NrCombo(info.label, &sel, info.choices, (int) info.choice_count, rowWidth))
            {
                api->set_string(info.key, info.choices[sel]);
                api->apply();
                api->save();
            }
            if (info.tooltip != nullptr && *info.tooltip != '\0')
                HelpMarker(info.tooltip);
            break;
        }
        case RELIMITER_TYPE_INT:
        case RELIMITER_TYPE_FLOAT:
        case RELIMITER_TYPE_DOUBLE:
        {
            // No range, no box: dmfg_output_cap and oled_care_idle_minutes have no clamp in
            // ReLimiter's own validation, and inventing ends for them would offer numbers it discards.
            if (info.min_value == info.max_value)
                break;

            double cur = 0.0;
            if (!api->get_number(info.key, &cur))
                break;

            // A labelled zero is a named mode -- target_fps = 0 is "stay below the VRR ceiling", not
            // 0 fps -- and it sits outside the box's range. So it gets its own checkbox: a mode is
            // not a number, and leaving it as one end of a numeric field invites someone to type 0
            // meaning "no limit" and get the opposite.
            if (info.zero_label != nullptr)
            {
                bool autoMode = cur == 0.0;
                if (NrCheckbox(info.zero_label, &autoMode))
                {
                    // Leaving auto lands on the range's low end, which is a real value ReLimiter keeps.
                    api->set_number(info.key, autoMode ? 0.0 : info.min_value);
                    api->apply();
                    api->save();
                    cur = autoMode ? 0.0 : info.min_value;
                }
                if (autoMode)
                {
                    if (info.tooltip != nullptr && *info.tooltip != '\0')
                        HelpMarker(info.tooltip);
                    break;
                }
            }

            // The frame-rate cap alone is typed (see NrNumberBox): it has to land on 72 or 141 exactly,
            // which a track from 30 to 1000 cannot do. Every other number is a slider, written as it
            // moves and pushed into the limiter and saved once, on release.
            if (std::strcmp(info.key, "target_fps") == 0)
            {
                double v = cur;
                auto r = NrNumberBox(info.label, &v, info.min_value, info.max_value, info.type == RELIMITER_TYPE_INT,
                                     rowWidth);
                if (r.committed && v != cur)
                {
                    api->set_number(info.key, v);
                    api->apply();
                    api->save();
                }
            }
            else
            {
                const bool isInt = info.type == RELIMITER_TYPE_INT;
                const bool wide = info.max_value - info.min_value > 10.0;
                float v = (float) cur;
                auto r = NrSlider(info.label, &v, (float) info.min_value, (float) info.max_value,
                                  isInt || wide ? "%.0f" : "%.2f", rowWidth);
                if (isInt)
                    v = std::round(v);
                if (r.changed && (double) v != cur)
                    api->set_number(info.key, (double) v);
                if (r.released)
                {
                    api->apply();
                    api->save();
                }
            }
            if (info.tooltip != nullptr && *info.tooltip != '\0')
                HelpMarker(info.tooltip);
            break;
        }
        default:
            break; // a type this build does not know: skipped, not guessed at
        }
    }
}

// RenoDX's own settings, drawn from what it reports -- the same arrangement as DrawPacingPage and for
// the same reason: RenoDX gains settings on its own schedule, and a table kept here would go stale on
// their release rather than ours.
//
// Two things it does that the pacing page does not, both because RenoDX's settings are a real tree
// rather than a flat list:
//
//   is_visible   RenoDX's first setting is "Settings Mode" (Simple / Intermediate / Advanced) and most
//                of the rest are visible only above Simple. Honouring it means this page shows exactly
//                what RenoDX's own overlay would at the same mode, including collapsing again when the
//                player puts it back to Simple -- no list of which-settings-are-advanced kept here.
//   is_enabled   A control RenoDX would grey is greyed, rather than accepting a value it discards.
//
// `group` is deliberately ignored. RenoDX uses it to put several settings on one line in its overlay;
// this panel is one control per row because a controller has to be able to land on each of them.
static void DrawRenoDxPage(float rowWidth)
{
    const RenoDxHostApi* api = DlssNrRenoDx::Api();
    if (api == nullptr)
    {
        DrawAddonMissing(Tr("HDR and tone mapping"), HdrMissingText(DlssNrRenoDx::UnavailableReason()), rowWidth);
        return;
    }

    SectionCaption(Tr("HDR and tone mapping"), rowWidth);

    // WHICH add-on loaded is the thing to confirm here, unlike ReLimiter: RenoDX ships one per game,
    // and the engine-wide build (renodx-unrealengine.addon64) looks identical in the folder to a
    // bespoke one. The module name is the only place that distinction is visible.
    const char* module = DlssNrRenoDx::ModuleName();
    ImGui::TextDisabled("RenoDX -- %s", module ? module : "?");
    HelpMarker(Tr("RenoDX replaces this game's tone mapping to give it real HDR, rather than expanding"
                  "\nan SDR picture afterwards. It is a separate add-on with its own overlay; these are"
                  "\nits settings, shown here so there is one panel to look at instead of two."
                  "\n\nIt is written against this game's own shaders, so what appears below is whatever"
                  "\nthis particular mod exposes -- it differs from game to game."));

    const char* lastSection = nullptr;
    const uint32_t settings = api->setting_count();

    for (uint32_t i = 0; i < settings; ++i)
    {
        RenoDxHostSetting info {};
        info.struct_size = sizeof(info);
        if (!api->describe_setting(i, &info))
            continue;

        // Hidden by the add-on's own rule, so hidden here. Drawn but greyed would be a different
        // claim -- that the setting exists and is merely unavailable -- and RenoDX means neither.
        if (info.is_visible == 0)
            continue;

        // A label is what makes a row readable; an unlabelled setting is internal state RenoDX draws
        // nothing for either.
        if (info.label == nullptr || *info.label == '\0')
            continue;

        if (info.section != nullptr && *info.section != '\0' &&
            (lastSection == nullptr || std::strcmp(lastSection, info.section) != 0))
        {
            SectionCaption(info.section, rowWidth);
            lastSection = info.section;
        }

        const bool disabled = info.is_enabled == 0;
        if (disabled)
            ImGui::BeginDisabled();

        switch (info.value_type)
        {
        case RENODX_HOST_VALUE_BOOLEAN:
        {
            float cur = 0.0f;
            if (api->get_number(info.key, &cur))
            {
                bool on = cur != 0.0f;
                if (NrCheckbox(info.label, &on))
                {
                    api->set_number(info.key, on ? 1.0f : 0.0f);
                    api->save();
                }
            }
            break;
        }
        case RENODX_HOST_VALUE_COMBO:
        {
            // The value IS the index, which is why this reads as a number and writes back as one.
            float cur = 0.0f;
            if (info.label_count > 0 && api->get_number(info.key, &cur))
            {
                // Gathered per draw rather than cached: label_at hands back pointers into the
                // add-on's own std::strings, and holding those across a call into it is the lifetime
                // bug its own comment warns about.
                const char* labels[32];
                const uint32_t count = info.label_count < 32 ? info.label_count : 32;
                bool complete = true;
                for (uint32_t c = 0; c < count; ++c)
                {
                    labels[c] = api->label_at(i, c);
                    complete = complete && labels[c] != nullptr;
                }
                int sel = (int) cur;
                if (complete && sel >= 0 && sel < (int) count &&
                    NrCombo(info.label, &sel, labels, (int) count, rowWidth))
                {
                    api->set_number(info.key, (float) sel);
                    api->save();
                }
            }
            break;
        }
        case RENODX_HOST_VALUE_INTEGER:
        case RENODX_HOST_VALUE_FLOAT:
        {
            // No range, no box -- the same rule the pacing page follows. RenoDX leaves min and max at
            // zero for a setting it does not clamp, and inventing ends would offer numbers it ignores.
            if (info.min_value == info.max_value)
                break;

            // A slider, not a typed box: brightness and grading are judged by eye while they move, and
            // set_number applies live, so the picture follows the handle. Saved once, on release.
            float cur = 0.0f;
            if (api->get_number(info.key, &cur))
            {
                const bool isInt = info.value_type == RENODX_HOST_VALUE_INTEGER;
                const bool wide = info.max_value - info.min_value > 10.0f;
                float v = cur;
                auto r =
                    NrSlider(info.label, &v, info.min_value, info.max_value, isInt || wide ? "%.0f" : "%.2f", rowWidth);
                if (isInt)
                    v = std::round(v);
                if (r.changed && v != cur)
                    api->set_number(info.key, v);
                if (r.released)
                    api->save();
            }
            break;
        }
        default:
            // TEXT, and anything a later RenoDX adds. Skipped rather than guessed at: this panel has
            // no text field, and a path or a preset name typed on a controller is not a thing worth
            // building badly when RenoDX's own overlay already has it.
            break;
        }

        if (disabled)
            ImGui::EndDisabled();

        if (info.tooltip != nullptr && *info.tooltip != '\0')
            HelpMarker(info.tooltip);
    }
}

// Drawn once, under the status lines: the same buttons the Models row uses, so the panel has one
// way of offering a choice of several.
static void PagePicker(float rowWidth)
{
    const char* names[kPageCount] = { Tr("Main"),    Tr("Model"), Tr("Cost"),   Tr("Image"),
                                      Tr("Inspect"), Tr("Setup"), Tr("Pacing"), Tr("HDR") };

    // Every page is listed, Pacing and HDR included whether or not their add-on is in this game. They
    // used to be hidden without it, which left a player who had heard of the feature with nowhere to
    // find it and nothing to say why; the page now greys itself and says what to do instead
    // (DrawAddonMissing). The strip is still built from a list so a page can be dropped again cheaply.
    int visible[kPageCount];
    int count = 0;
    for (int i = 0; i < kPageCount; ++i)
        visible[count++] = i;

    // If the page we were on has just gone away, land somewhere real instead of drawing nothing.
    bool onVisible = false;
    for (int i = 0; i < count; ++i)
        onVisible = onVisible || visible[i] == g_page;
    if (!onVisible)
        g_page = kPageMain;

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = (rowWidth - spacing * (count - 1)) / (float) count;

    for (int i = 0; i < count; ++i)
    {
        if (i > 0)
            ImGui::SameLine();

        const int page = visible[i];
        ImGui::PushID(page);
        if (ModelButton(names[page], g_page == page, width))
            g_page = page;
        ImGui::PopID();
    }
}

void RenderMenu(Config* config, float menuResScale)
{
    ImGuiIO& io = ImGui::GetIO();
    auto& state = State::Instance();

    // Same for the language: resolved once per frame from the config, so every string below agrees.
    I18n::Refresh(config->DlssNrLanguage.value_or_default());

    // Picked once, here, so a config change mid-frame cannot draw half the panel in each palette.
    // Vendor colours: NVIDIA green is the panel's native look; on an AMD card it wears AMD red
    // unless [DlssNr] VendorColours says otherwise.
    {
        const bool light = config->DlssNrLightTheme.value_or_default();
        const bool amd = config->DlssNrVendorColours.value_or_default() && OnAmdGpu();
        g_pal = light ? (amd ? &LightAmd() : &Light()) : (amd ? &DarkAmd() : &Dark());
    }

    // Where the panel goes and how big it is: see DlssNr_PanelLayout.h for the whole behaviour (drag
    // the background to move, edges or the corner to resize, a strip always left on screen, fit to
    // content until resized). Position and size live in [DlssNr] PanelX/Y/W/H, per game.
    //
    // rowWidth is the rows' layout width: fixed until the panel is resized, then the panel's inner
    // width, never below minRowWidth -- narrower and the 44% label column runs into the sliders.
    float rowWidth = PanelWidth(menuResScale);
    const PanelLayout::Metrics layoutMetrics { rowWidth,
                                               std::round(rowWidth * 0.8f),
                                               160.0f * menuResScale,
                                               24.0f * menuResScale,
                                               64.0f * menuResScale,
                                               ImVec2(18.0f, 14.0f) * menuResScale };
    static PanelLayout::State s_layout;
    PanelLayout::Settings layout { config->DlssNrPanelX.value_or_default(), config->DlssNrPanelY.value_or_default(),
                                   config->DlssNrPanelW.value_or_default(), config->DlssNrPanelH.value_or_default() };
    PanelLayout::BeforeBegin(s_layout, layout, layoutMetrics);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, g_pal->overlay(0.10f));
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kAccent);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kTrack);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, g_pal->overlay(0.18f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, g_pal->overlay(0.24f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_Button, g_pal->overlay(0.07f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_pal->overlay(0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_pal->overlay(0.16f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, g_pal->popupBg);
    // The resize grip (bottom-right) and the scrollbar a size-capped panel gets, in the panel's colours
    // rather than stock ImGui blue/grey.
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, g_pal->overlay(0.10f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, g_pal->overlay(0.16f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, g_pal->overlay(0.26f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.70f));
    const int kPanelColourCount = 21;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, layoutMetrics.pad);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 9.0f * menuResScale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

    // No NoMove: that is what makes it draggable. No NoResize / AlwaysAutoResize: that is what makes it
    // resizable (the fit is done by hand above). No NoScrollbar: a panel shorter than its content
    // scrolls. NoSavedSettings stays -- position and size live in OptiScaler.ini, not an imgui.ini.
    //
    // NoNavInputs: the panel is driven with the mouse. The shared context has keyboard and gamepad
    // navigation on (for OptiScaler's own menu), and every navigation move scrolls the window to the
    // widget it lands on -- so input the game keeps producing (a controller's resting stick, arrow keys)
    // pinned this panel to one row and the mouse wheel could not scroll past it (Cyberpunk 2077,
    // 2026-09-16: stuck on the Language combo).
    // NoMove: the panel is moved by its top strip only (DragByHeader below), not by its background.
    // Dragging the background meant a missed slider or an empty gap beside a row moved the whole
    // panel, which is not what anyone was reaching for.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoMove;

    bool anyChanged = false;

    if (ImGui::Begin("##DlssNrOverlay", nullptr, flags))
    {
        // Scales this window's text only; the shared menu keeps whatever FontSize says.
        //
        // SetWindowFontScale is marked [OBSOLETE] in this ImGui, and the header points at reloading
        // the font and rebuilding the atlas instead. That is the right answer for scaling the whole
        // UI and the wrong one here: rebuilding the atlas would resize OptiScaler's menu too, and
        // this panel is meant to be the only thing that changes. It is obsolete, not removed.
        //
        // Clamped because row widths are computed from the font size, so a large enough scale walks
        // labels into their values.
        //
        // PushFontSize, not SetWindowFontScale: the latter stretches the already-rasterized glyph
        // bitmap, which is why this panel's text used to look soft at anything but 1.0x. PushFontSize
        // re-rasterizes at the requested size instead, the same mechanism the shared menu's own
        // UseHQFont path uses -- and since that path already pushed its own size before this panel
        // draws, GetFontSize() here is that size, not the atlas default.
        float fontScale = config->DlssNrFontScale.value_or_default();
        fontScale = fontScale < 0.75f ? 0.75f : (fontScale > 2.0f ? 2.0f : fontScale);
        ImGui::PushFontSize(std::round(fontScale * ImGui::GetFontSize()));

        // Claim focus so keyboard/mouse routes here rather than being left with whatever last had
        // it -- but only when this panel is on its own. With OptiScaler's own menu also open,
        // grabbing focus every frame would make that menu impossible to type into.
        if (!MenuCommon::IsSharedMenuVisible() && !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
            ImGui::SetWindowFocus();

        // Measured before anything is placed, so the cursor is at the start of the first line: the
        // width a row can use, with the scrollbar (when there is one) already taken off.
        const float innerWidth = ImGui::GetContentRegionAvail().x;

        if (PanelLayout::AfterBegin(s_layout, layout, layoutMetrics, rowWidth))
        {
            // The user finished moving or resizing it: keep that with the rest of the panel's state.
            config->DlssNrPanelX = layout.x;
            config->DlssNrPanelY = layout.y;
            config->DlssNrPanelW = layout.w;
            config->DlssNrPanelH = layout.h;
            anyChanged = true;
        }

        ImGui::Dummy(ImVec2(rowWidth, 0.0f));

        // The top of the panel: a bare strip to take hold of, then the pages. No title row -- the
        // panel says what it is on every row in it, and the strip is what a hand reaches for. Light
        // and Reset layout are settings and live under Setup; the panel closes on its own key.
        ImGui::Dummy(ImVec2(rowWidth, ImGui::GetTextLineHeight() * 0.45f));
        PagePicker(rowWidth);
        DragByHeader(ImGui::GetCursorScreenPos().y);

        ImGui::Spacing();

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (NrCheckbox(Tr("DLSS ON"), &enabled, true))
        {
            config->DlssNrEnabled = enabled;
            anyChanged = true;
        }

        HelpMarker(Tr("Synthesises detail in the upscaler's frame, before frame generation sees it."
                      "\n\nNeeds two similarly named files beside OptiScaler, one character apart:"
                      "\n  nvngx_dlssnr.dll       NVIDIA's model (~165 MB) -- you supply it"
                      "\n  nvngx.dll_dlssnr.dll   the forwarder (~13 KB) -- ships in this package"
                      "\nUndocumented and driven directly, so none of this is officially supported."));

        // Only shown for a Feeder game -- a native-DLSS game is the common case and needs no
        // extra line here. Worth surfacing when it applies: a Feeder-fed evaluate runs on
        // *estimated* motion vectors (ReShade depth + optical flow, not the game's real ones),
        // so it ghosts more in fast motion and softens thin geometry more than a native-DLSS-fed
        // one -- the same model, a rougher input.
        if (DlssNr::IsFeederPresent())
        {
            ImGui::TextColored(kTextDim, "%s", Tr("Source: DLSS5 Feeder (no native DLSS in this game)"));
            HelpMarker(Tr("This game has no DLSS of its own, so there is no evaluate call for Neural "
                          "Rendering to attach to. The DLSS5 Feeder ReShade add-on builds one from "
                          "ReShade's own depth and estimated motion vectors instead."
                          "\n\nEstimated motion vectors are rougher than a game's real ones -- expect "
                          "more ghosting in fast motion and softer thin geometry than a native-DLSS "
                          "game gets from the same model."));
        }

        // Lossless Scaling is offered for every game, not just Feeder ones: it runs as a separate
        // process and never touches this game's own rendering, so nothing about the game rules it
        // out -- and OptiScaler's own Frame Generation crashes with the Feeder anyway (see
        // FGHooks::CheckForFGStatus). Per-game setup lives in OptiDLSS5-UI, which writes the
        // ExePath/GameTitle/Mode keys below. See LosslessScaling.h for what these controls do.
        // Kept deliberately compact -- this row grew too tall/text-heavy on the first pass.
        {
            auto losslessExePath = config->LosslessScalingExePath.value_or_default();
            auto losslessGameTitle = config->LosslessScalingGameTitle.value_or_default();
            const bool losslessConfigured = !losslessExePath.empty() && !losslessGameTitle.empty();
            const bool losslessAdaptive = config->LosslessScalingMode.value_or_default() == L"ADAPTIVE";
            if (!losslessConfigured)
            {
                // Only worth a line where it is the only Frame Generation route there is.
                if (DlssNr::IsFeederPresent())
                    ImGui::TextColored(kTextDim, "%s", Tr("Lossless Scaling: not configured (OptiDLSS5-UI)."));
            }
            else
            {
                // The global-hotkey chord OptiDLSS5-UI read from Lossless Scaling's own settings
                // (default Ctrl+Alt+S). Everything below drives Lossless Scaling through it and
                // through process launch/close only -- never its window, which is never shown.
                const int lsMods = config->LosslessScalingHotkeyMods.value_or_default();
                const int lsVk = config->LosslessScalingHotkeyVk.value_or_default();

                // What this row believes about Lossless Scaling's Frame Generation: the last
                // request made here. It cannot be read back (no API), but one thing is certain:
                // with no Lossless Scaling process there is no scaling. So a belief of "on" is
                // dropped once the process is gone -- after a grace period, because Active
                // launches it asynchronously and the process takes a few seconds to appear.
                // Without this, closing it and pressing Active again sent the toggle chord to a
                // fresh instance as an "off", which turned it on.
                static bool scalingBelieved = false;
                static uint64_t scalingRequestedAt = 0;

                bool lsRunning = LosslessScaling::IsRunning();
                if (!lsRunning && scalingBelieved && GetTickCount64() - scalingRequestedAt > 10000)
                    scalingBelieved = false;

                if (NrCheckbox(Tr("Lossless Scaling"), &lsRunning))
                {
                    if (lsRunning)
                    {
                        LosslessScaling::Launch(losslessExePath);
                    }
                    else
                    {
                        LosslessScaling::Close();
                        scalingBelieved = false;
                    }
                }
                ImGui::SameLine();
                HelpMarker(Tr("Launches/closes Lossless Scaling in the background (minimized to tray, no "
                              "window shown). Turning Active on below launches it for you too."));

                bool scalingRow = scalingBelieved;
                ImGui::SameLine();
                if (NrCheckbox(Tr("Active"), &scalingRow))
                {
                    scalingBelieved = scalingRow;
                    if (scalingRow)
                    {
                        scalingRequestedAt = GetTickCount64();
                        // Launches Lossless Scaling if needed, then synthesises its toggle hotkey --
                        // its handler scales whatever profile matches the foreground window (this
                        // game). No window is shown.
                        LosslessScaling::ActivateAsync(losslessExePath, lsMods, lsVk);

                        // Two frame generators at once stack their generated frames -- stutter at
                        // best, a crash at worst. OptiScaler's own FG is ours to switch off here; the
                        // game's native DLSS Frame Generation is a game setting, so it gets a
                        // reminder below.
                        if (config->FGEnabled.value_or_default())
                        {
                            config->FGEnabled = false;
                            state.fgChanged = true;
                            anyChanged = true;
                        }
                    }
                    else
                    {
                        LosslessScaling::DeactivateAsync(lsMods, lsVk);
                    }
                }
                ImGui::SameLine();
                HelpMarker(Tr("Turns Lossless Scaling's Frame Generation on/off for this game via its own "
                              "global hotkey -- no window is shown, and it launches Lossless Scaling first "
                              "if needed. Shows what was last requested, not a confirmed live state.\n\n"
                              "Turning this on switches OptiScaler's own Frame Generation off: two frame "
                              "generators at once stack."));
                if (losslessAdaptive)
                {
                    // Adaptive mode has no multiplier to step -- Lossless Scaling decides per frame
                    // how many to generate to hold the target. The target itself is set in
                    // OptiDLSS5-UI (it lives in the profile, not here).
                    ImGui::SameLine();
                    ImGui::TextColored(kTextDim, Tr("Adaptive: holds %d fps"),
                                       config->LosslessScalingTarget.value_or_default());
                    ImGui::SameLine();
                    HelpMarker(Tr("Adaptive Frame Generation: Lossless Scaling generates only as many frames "
                                  "as it takes to hold this target. Change the target (or switch to a fixed "
                                  "multiplier) in OptiDLSS5-UI. Needs this game running Borderless or "
                                  "Windowed, not exclusive Fullscreen (DX12 games are usually fine either way)."));
                }
                else
                {
                    static int multiplierBelieved = config->LosslessScalingMultiplier.value_or_default();
                    for (int m : { 2, 3, 4 })
                    {
                        ImGui::SameLine();
                        bool selected = (multiplierBelieved == m);
                        char label[8];
                        snprintf(label, sizeof(label), "%dx", m);
                        if (NrCheckbox(label, &selected) && selected)
                        {
                            multiplierBelieved = m;
                            // Writes the new multiplier into this game's profile and, if Lossless
                            // Scaling is running, restarts it to pick the value up (it only reads
                            // profiles at startup), re-scaling afterwards when it was already active.
                            LosslessScaling::SetMultiplierAsync(losslessExePath, losslessGameTitle, m, lsMods, lsVk,
                                                                scalingBelieved);
                        }
                    }
                    ImGui::SameLine();
                    HelpMarker(Tr("Frames generated per real one. If Lossless Scaling is already running it "
                                  "briefly restarts to apply -- Frame Gen blinks off for a second. Needs "
                                  "this game running Borderless or Windowed, not exclusive Fullscreen (DX12 "
                                  "games are usually fine either way)."));
                }
                if (lsRunning)
                    ImGui::TextColored(kTextDim, "%s",
                                       Tr("Keep the game's own DLSS Frame Generation off while this runs."));
            }
        }

        bool beforeSr = config->DlssNrRunBeforeSr.value_or_default();
        if (NrCheckbox(Tr("Before Super Resolution"), &beforeSr))
        {
            config->DlssNrRunBeforeSr = beforeSr;
            // One or the other, never both: the two together froze inZOI on the spot (issue #55,
            // 2026-09-19), and before SR the frame carries no UI for the correction to act on anyway.
            if (beforeSr)
                config->DlssNrUICorrection = false;
            anyChanged = true;
        }
        HelpMarker(Tr("Where the pass sits. Off is the original placement: the model runs on the finished"
                      "\nupscaled frame. On runs it at render resolution on the colour SR is about to"
                      "\nconsume, so SR then accumulates and upscales an already-enhanced picture."
                      "\n\nRay Reconstruction always stays on the post-upscale path -- its inputs are a"
                      "\ndifferent contract. A colour image padded inside a larger texture is staged at its"
                      "\nreal size; one offset from the corner still falls back after upscaling."
                      "\n\nD3D12 and its D3D11/Vulkan bridges only; native Vulkan keeps the old placement."));

        // Experimental: the same placement for Ray Reconstruction, whose colour input is the noisy frame it
        // denoises. Only meaningful with Before Super Resolution on, so greyed out without it.
        bool beforeRr = config->DlssNrRunBeforeRr.value_or_default();
        ImGui::BeginDisabled(!config->DlssNrRunBeforeSr.value_or_default());
        if (NrCheckbox(Tr("Before Ray Reconstruction (experimental)"), &beforeRr))
        {
            config->DlssNrRunBeforeRr = beforeRr;
            anyChanged = true;
        }
        ImGui::EndDisabled();
        HelpMarker(Tr("Also runs the pass before Ray Reconstruction, at render resolution, on the colour it is"
                      "\nabout to denoise and upscale -- far cheaper than after it. EXPERIMENTAL: that colour"
                      "\nis the noisy ray-traced frame rather than a finished one, so the model may enhance"
                      "\nnoise and Ray Reconstruction may smear what it added. Try it, compare, and turn it"
                      "\noff if it looks worse. Needs Before Super Resolution on."));

        // Either backend. They keep separate state, and on a native Vulkan game the D3D12 side is
        // never touched -- asking only that one reports "waiting" over a pass that is demonstrably
        // running.
        const bool vulkan = DlssNr::IsRunningVk();

        // One badge, above the sentence that explains it: the answer to "is this actually doing
        // anything in this game" should be readable at a glance, on every page, without reading a
        // line of prose. Deep Fried Chicken's menu carries the same badge in the same place, so a
        // player moving between a DLSS 5 game and a Chicken game reads one panel, not two.
        StatusBadge(!enabled                          ? Tr("Paused")
                    : (DlssNr::IsRunning() || vulkan) ? Tr("Ready")
                    : DlssNr::FailureReason()[0] != 0 ? Tr("Blocked")
                                                      : Tr("Waiting"),
                    enabled && (DlssNr::IsRunning() || vulkan));

        // Switched off says so. IsRunning() is "a model is built", and the built model is deliberately kept a
        // while after DLSS 5 goes off so turning it back on is instant -- which read as "Running - 9.36 ms"
        // under an unticked DLSS ON, the last timing from before it went off (inZOI, issue #55, 2026-09-19).
        if (!enabled)
        {
            ImGui::TextColored(kTextDim, "%s", Tr("Off"));
        }
        else if (!DlssNr::IsRunning() && !vulkan)
        {
            const char* reason = DlssNr::FailureReason();

            if (reason[0] != 0)
            {
                // Wrapped: some reasons name a file and what to do about it, which does not fit on
                // one line at this panel's width.
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), Tr("Off for this session: %s."),
                                   TrReason(reason).c_str());
                ImGui::PopTextWrapPos();

                ImGui::SameLine();

                if (ImGui::SmallButton(Tr("Retry")))
                    DlssNr::RetryAfterFailure();
            }
            else if (enabled)
            {
                ImGui::TextColored(kTextDim, "%s", Tr("Waiting for the upscaler to run."));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
                if (DlssNr::PresentRoute::PresentCount() > 0)
                    // The Present route with no upscale call: nothing to select in the game's settings --
                    // the pass is waiting for the depth tracker to recognise the scene depth.
                    ImGui::TextColored(kTextDim, "%s",
                                       Tr("Running at Present on this game's own anti-aliasing: looking for "
                                          "the game's scene depth. Load into gameplay -- menus have none."));
                else if (DlssNr::IsFeederPresent())
                    // The Feeder route: there is no native DLSS/XeSS setting to point at here --
                    // the evaluate this pass is waiting for is the Feeder's own synthetic one, so
                    // what is missing is the Feeder itself doing its job, not a game setting.
                    ImGui::TextColored(kTextDim, "%s",
                                       Tr("The DLSS5 Feeder add-on is loaded, but has not fed a DLSS "
                                          "evaluate yet -- check dlss5-feed.log in the game folder for "
                                          "\"technique MISSING\" if this does not clear once you are "
                                          "in-game."));
                else
                    // The one thing the old shared window told you here that this panel otherwise
                    // wouldn't: this needs the game's own upscaler active, not just this checkbox.
                    ImGui::TextColored(kTextDim, "%s",
                                       Tr("Needs DLSS or XeSS selected as the upscaler in the game's own "
                                          "video settings, and a save loaded -- this (and the rest of "
                                          "OptiScaler) does not run in menus."));
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
            // Read from the config, not from the checkbox: that row lives under Inspect now, and this
            // line is drawn on every page.
            const char* runSuffix =
                !config->DlssNrApplyModel.value_or_default() ? Tr("  (model running, edit hidden)") : "";

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1.0f), Tr("Running%s - %.2f ms per frame%s"),
                                   vulkan ? Tr(" natively on Vulkan") : "", ms.value(), runSuffix);
            else if (vulkan)
                // Measured but not yet read: the first few frames are still in the query ring.
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1.0f), Tr("Running natively on Vulkan - %llu frames%s"),
                                   DlssNr::FramesVk(), runSuffix);
            else
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1.0f), Tr("DLSS 5 on%s"), runSuffix);

            ImGui::SameLine();
            ImGui::TextColored(kTextDim, "(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", Tr("The whole pass: the staging copies and the resolve as well as the"
                                           "\nmodel. Timing only the model would flatter the number."
                                           "\n\nCompare it against the frame time at the bottom of this window to"
                                           "\nsee what it is costing you."));
        }

        // Live frame rate and video memory, beside the cost. The memory is what decides whether another
        // model pass can be built: near the budget the pass waits rather than risk a lost device, and the
        // readout turns amber so the reason is on screen.
        {
            const float fps = ImGui::GetIO().Framerate;
            uint64_t used = 0, budget = 0;

            if (DlssNr::VideoMemory(&used, &budget))
            {
                const double usedGb = used / (1024.0 * 1024.0 * 1024.0);
                const double budgetGb = budget / (1024.0 * 1024.0 * 1024.0);
                const bool tight = used * 10 >= budget * 9;
                ImGui::TextColored(tight ? ImVec4(0.95f, 0.70f, 0.20f, 1.0f) : kTextDim,
                                   Tr("%.0f fps   VRAM %.1f / %.1f GB"), fps, usedGb, budgetGb);
                ImGui::SameLine();
                ImGui::TextColored(kTextDim, "(?)");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s",
                                      Tr("Frames per second this panel is drawn at, and the video memory this game"
                                         "\nis using out of the budget Windows gives it on this GPU."
                                         "\n\nAmber above 90%: a new model pass is only built when it fits, so"
                                         "\nnear the budget extra passes wait instead of risking a crash."));
            }
            else
            {
                ImGui::TextColored(kTextDim, Tr("%.0f fps"), fps);
            }
        }

        // Global Controls -- DlssNrLocalStructure / DlssNrLocalTone: NVIDIA's own name for
        // these two in its DLSS 5 developer overlay.
        if (OnPage(kPageModel))
        {
            SectionCaption(Tr("Global Controls"), rowWidth);

            float localStructure = config->DlssNrLocalStructure.value_or_default();
            auto rStruct = NrSlider(Tr("Structure Intensity"), &localStructure, 0.0f, 1.0f, "%.2f", rowWidth);
            if (rStruct.changed)
                config->DlssNrLocalStructure = localStructure;
            if (rStruct.released)
                anyChanged = true;
            HelpMarker(Tr("The model's structure-synthesis strength across the whole frame."));

            float localTone = config->DlssNrLocalTone.value_or_default();
            auto rTone = NrSlider(Tr("Tone Intensity"), &localTone, 0.0f, 1.0f, "%.2f", rowWidth);
            if (rTone.changed)
                config->DlssNrLocalTone = localTone;
            if (rTone.released)
                anyChanged = true;
            HelpMarker(Tr("The model's tone-remapping strength across the whole frame."));
            // Model Automask -- DlssNrAutoMask. In NVIDIA's panel this is a letter-tracked caps row
            // of its own with a "Show Mask" toggle on the right, not a section caption with a divider,
            // so it is drawn that way here.
            ImGui::Spacing();

            bool autoMask = config->DlssNrAutoMask.value_or_default();
            if (NrCheckbox(Tr("Model Automask"), &autoMask, true))
            {
                config->DlssNrAutoMask = autoMask;
                anyChanged = true;
            }
            HelpMarker(Tr("Lets the model find skin itself rather than treating the frame uniformly."));

            // Greyed out, and not because a setting is off: the model keeps its mask to itself. It is
            // never handed back as a resource across the interface this fork drives, so there is
            // nothing for an overlay to draw.
            ImGui::BeginDisabled(true);
            bool showMask = false;
            NrRightCheckbox(Tr("Show Mask"), &showMask, rowWidth);
            ImGui::EndDisabled();

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s",
                                  Tr("NVIDIA's panel can draw the automask over the frame. The model does not hand"
                                     "\nits mask back through the interface this fork drives, so there is nothing"
                                     "\nhere to display."));

            // Matches NVIDIA's own panel: greyed out while Automask is off. The value underneath is
            // unchanged either way -- this only stops it being dragged while it has nothing to act on.
            ImGui::BeginDisabled(!autoMask);
            ImGui::PushID("Automask");
            float skin = config->DlssNrSkinStructure.value_or_default();
            auto rSkin = NrSlider(Tr("Structure Intensity"), &skin, -1.0f, 1.0f, "%.2f", rowWidth);
            if (rSkin.changed)
                config->DlssNrSkinStructure = skin;
            if (rSkin.released)
                anyChanged = true;
            ImGui::PopID();
            ImGui::EndDisabled();
            HelpMarker(Tr("-1 means follow the Global Controls Structure Intensity above, and is the"
                          "\nmodel's own default. 0 and above set the masked region's structure"
                          "\nindependently of the rest of the frame."
                          "\n\nGreyed out while Model Automask is off -- there is no mask for it to"
                          "\nshape without it."));

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
            NrCheckbox(Tr("Developer Masking"), &devMasking, true);
            NrRightCheckbox(Tr("Show Masks"), &showMasks, rowWidth);
            ImGui::EndDisabled();

            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
            ImGui::TextColored(kTextDim, "%s",
                               Tr("Per-object masks come from the game's own renderer, so this one stays "
                                  "NVIDIA-only -- an injector has no object list to mask."));
            ImGui::PopTextWrapPos();

            // Models -- DlssNrPreset. NVIDIA ships no letters in the binary; "Model A/B/C" is this
            // fork's best match to the segmented selector in the developer overlay, and matches the
            // three NVIDIA describes publicly. Default (preset index 0) is kept as a fourth button
            // that NVIDIA's panel does not show, because it is a real, distinct state here: dropping
            // it to match the screenshot exactly would make that state unreachable from the UI.
            SectionCaption(Tr("Models"), rowWidth);

            const char* nrPresetNames[] = { Tr("Default"), Tr("Model A"), Tr("Model B"), Tr("Model C") };
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
            HelpMarker(Tr("Not the same scale as the super resolution or ray reconstruction presets --"
                          "\nthe same letter means something different here."
                          "\n\nRead when the model is built, so a change rebuilds it after a moment."));

            const char* nrStyleNames[] = { Tr("Default (standard)"), Tr("Natural"), Tr("Cinematic") };
            int style = (int) config->DlssNrStyle.value_or_default();
            if (style > 2)
                style = 2;
            if (NrCombo(Tr("Style"), &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames), rowWidth))
            {
                config->DlssNrStyle = (uint32_t) style;
                anyChanged = true;
            }
            HelpMarker(Tr("The model's own processing profiles."
                          "\n\nDefault (standard): the strongest, and most likely to look 'stylised'."
                          "\nNatural: the same detail work with a gentler hand."
                          "\nCinematic: tones down the shine and over-processing for a film-like look."
                          "\n\nThe names come from community testing, unlike the panel labels above --"
                          "\nNVIDIA ships no names for this control in the binaries."));

            float intensity = config->DlssNrIntensity.value_or_default();
            auto rIntensity = NrSlider(Tr("Intensity"), &intensity, 0.0f, 2.0f, "%.2f", rowWidth);
            if (rIntensity.changed)
                config->DlssNrIntensity = intensity;
            if (rIntensity.released)
                anyChanged = true;
            HelpMarker(Tr("The model's own strength control, applied inside it. Distinct from the Global"
                          "\nControls above, and from Detail strength below, which scales the result"
                          "\nafterwards."));

            // Frame Generation -- NVIDIA's own DLSS-G (Streamline). Two ways it can be running:
            //
            //  1. The GAME's own DLSS Frame Generation (the common case for every title that ships
            //     sl.dlss_g.dll: Cyberpunk, Stellar Blade, Witcher 3...). Turning it on or off is the
            //     game's own video setting; what this panel can do is override the multiplier the game
            //     asks the driver for, through config->FGDLSSGOverrideInterpolationCount /
            //     FGDLSSGOverrideForceDMFG, which StreamlineHooks::hkslDLSSGSetOptions applies to every
            //     slDLSSGSetOptions the game makes (and updateDlssgOptions() re-sends the last one so a
            //     change here lands immediately, not on the game's next settings change).
            //
            //  2. OptiScaler's OWN DLSS-G instance (FGOutput=dlssg, for a game that only has DLSS
            //     upscaling), driven the same way the old shared menu's "MFG" combo did: straight
            //     through config->FGDLSSGInterpolationCount / FGDLSSGForceDMFG, which
            //     DLSSG_Dx12::Dispatch() reads every frame.
            //
            // Until 2026-09-12 only case 2 had controls here, and the manager app never enables that
            // output (it leaves frame gen to the game -- see OptiDLSS5-UI's autoConfigureGame), so on
            // every native-DLSS game the section only ever said "not the active output".
            //
            // Nothing here touches OptiFG (the FSR3-based fallback used when the game has no frame
            // generation at all) -- that path is FGOutput::FSRFG/XeFG and is deliberately left out.
        }

        if (OnPage(kPageMain))
        {
            SectionCaption(Tr("Frame Generation"), rowWidth);

            const bool optiDlssg = state.activeFgOutput == FGOutput::DLSSG && state.currentFG != nullptr;
            // The game's sl.dlss_g.dll got hooked -- it ships and loads DLSS-G of its own. Streamline
            // loads the plugin at startup whether or not the user has the option on, so this is true
            // from the first frame; dlssgMfgMax (below) only fills in once FG has actually been on.
            const bool gameDlssg =
                !optiDlssg && state.activeFgInput != FGInput::DLSSG && StreamlineHooks::isDlssgHooked();

            if (gameDlssg)
            {
                // What the game is doing right now: the count NGX saw on its last DLSS-G evaluate
                // (0 = off, 1 = 2X, ...), reset to 0 a few frames after FG stops being evaluated.
                const int live = state.dlssgDetectedInterpolationCount;
                if (live > 0)
                {
                    char running[96];
                    snprintf(running, sizeof(running), Tr("Game's DLSS Frame Generation: running at %dX"), live + 1);
                    ImGui::TextUnformatted(running);
                }
                else
                {
                    ImGui::TextColored(kTextDim, "%s",
                                       Tr("Game's DLSS Frame Generation: off in the game's video settings."));
                }
                HelpMarker(Tr("This game has NVIDIA DLSS Frame Generation of its own. Turn it on or off in the"
                              "\ngame's video settings as usual -- the row below only changes the multiplier"
                              "\nit asks the driver for."));

                // 4X (3 generated frames) is the most any card does today, and the hook clamps the
                // override to what the driver reported once FG has been on (dlssgMfgMax) -- so before
                // that is known, offer up to 4X and let it clamp. An RTX 40 card gets 2X either way.
                const int maxCount = std::clamp(state.dlssgMfgMax.value_or(3), 1, 5);
                static const char* gameMultNames[] = { "2X", "3X", "4X", "5X", "6X" };
                const int shown = maxCount + 1; // "Game" + one button per multiplier
                const int currentOverride = config->FGDLSSGOverrideInterpolationCount.has_value()
                                                ? config->FGDLSSGOverrideInterpolationCount.value()
                                                : 0;

                const bool dmfgForced =
                    state.dlssgGameDMFGSupported && config->FGDLSSGOverrideForceDMFG.value_or_default();

                ImGui::BeginDisabled(dmfgForced);
                {
                    float spacing = ImGui::GetStyle().ItemSpacing.x;
                    float btnWidth = (rowWidth - spacing * (shown - 1)) / shown;

                    for (int i = 0; i < shown; i++)
                    {
                        if (i > 0)
                            ImGui::SameLine();

                        ImGui::PushID(100 + i);
                        const char* label = i == 0 ? Tr("Game") : gameMultNames[i - 1];
                        if (ModelButton(label, currentOverride == i, btnWidth))
                        {
                            if (i == 0)
                            {
                                LOG_DEBUG("DLSSG override cleared -- game's own multiplier");
                                config->FGDLSSGOverrideInterpolationCount.reset();
                            }
                            else
                            {
                                LOG_DEBUG("DLSSG override interpolation count set to: {}", i);
                                config->FGDLSSGOverrideInterpolationCount = i;
                            }
                            StreamlineHooks::updateDlssgOptions();
                            anyChanged = true;
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndDisabled();
                HelpMarker(Tr("Overrides how many extra frames the game's DLSS-G inserts between real ones."
                              "\n\"Game\" leaves it at whatever the game's own menu says. 2X inserts one, 3X"
                              "\ninserts two, and so on. 3X and 4X need an RTX 50 series -- other cards are"
                              "\ncapped at 2X by the driver, whatever is picked here."
                              "\n\nGreyed out while Multi is on below -- the driver picks the count then."));

                if (state.dlssgGameDMFGSupported)
                {
                    bool dynamic = config->FGDLSSGOverrideForceDMFG.value_or_default();
                    if (NrCheckbox(Tr("Multi (Dynamic Frame Generation)"), &dynamic))
                    {
                        config->FGDLSSGOverrideForceDMFG = dynamic;
                        StreamlineHooks::updateDlssgOptions();
                        anyChanged = true;
                    }
                    HelpMarker(Tr("Lets NVIDIA's driver vary the multiplier itself, frame to frame, to hold"
                                  "\nthe FPS target below -- instead of a fixed 2X/3X/4X."));

                    ImGui::BeginDisabled(!dynamic);
                    float fpsTarget = config->FGDLSSGFramerateTargetDMFG.value_or_default();
                    auto rFps = NrSlider(Tr("DMFG FPS Target"), &fpsTarget, 0.0f, 200.0f, "%.0f", rowWidth);
                    if (rFps.changed)
                        config->FGDLSSGFramerateTargetDMFG = fpsTarget;
                    if (rFps.released)
                    {
                        StreamlineHooks::updateDlssgOptions();
                        anyChanged = true;
                    }
                    ImGui::EndDisabled();
                    HelpMarker(Tr("0 auto-detects your display's refresh rate."));
                }
            }
            else if (optiDlssg)
            {
                auto* fg = state.currentFG;

                bool fgActive = config->FGEnabled.value_or_default();
                if (NrCheckbox(Tr("Frame Generation"), &fgActive))
                {
                    config->FGEnabled = fgActive;
                    state.fgChanged = true;
                    anyChanged = true;

                    // The other half of the Lossless Scaling row's rule: only one frame generator at a time.
                    if (fgActive && LosslessScaling::IsRunning())
                        LosslessScaling::Close();
                }
                HelpMarker(Tr("NVIDIA's own DLSS Frame Generation, via Streamline. Not OptiFG."));

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
                    HelpMarker(Tr("Sets Streamline's numFramesToGenerate directly -- how many extra frames"
                                  "\nDLSS-G inserts between real ones. 2X inserts one, 3X inserts two, and"
                                  "\nso on. Capped by what your GPU and driver report supporting."
                                  "\n\nGreyed out while Multi is on below -- the driver picks the count then."));

                    if (fg->GetDMFGSupport())
                    {
                        if (NrCheckbox(Tr("Multi (Dynamic Frame Generation)"), &dmfgForced))
                        {
                            config->FGDLSSGForceDMFG = dmfgForced;
                            anyChanged = true;
                        }
                        HelpMarker(Tr("Lets NVIDIA's driver vary the multiplier itself, frame to frame, to hold"
                                      "\nthe FPS target below -- instead of a fixed 2X/3X/4X."));

                        ImGui::BeginDisabled(!dmfgForced);
                        float fpsTarget = config->FGDLSSGFramerateTargetDMFG.value_or_default();
                        auto rFps = NrSlider(Tr("DMFG FPS Target"), &fpsTarget, 0.0f, 200.0f, "%.0f", rowWidth);
                        if (rFps.changed)
                            config->FGDLSSGFramerateTargetDMFG = fpsTarget;
                        if (rFps.released)
                            anyChanged = true;
                        ImGui::EndDisabled();
                        HelpMarker(Tr("0 auto-detects your display's refresh rate."));
                    }
                }
            }
            else
            {
                // Neither the game nor OptiScaler has DLSS-G here. (The old text, "not the active
                // output", read as a fault on every native-DLSS game; it was really this case.)
                //
                // OptiScaler's own generators, XeFG and FSR FG, when the manager armed one for this game
                // (2026-09-23). Which one is fixed at launch -- it is built into the swapchain the game
                // creates, and OptiScaler makes it once per session -- so only its on/off and HUD fix are
                // offered here, and both do exactly what OptiScaler's own key and menu do to them.
                const bool optiFgOutput =
                    state.activeFgOutput == FGOutput::XeFG || state.activeFgOutput == FGOutput::FSRFG;
                const bool optiFg =
                    optiFgOutput && state.activeFgInput != FGInput::NoFG && state.currentFGSwapchain != nullptr;
                if (optiFg)
                {
                    char running[96];
                    snprintf(running, sizeof(running), Tr("OptiScaler Frame Generation: %s"),
                             state.activeFgOutput == FGOutput::XeFG ? "XeFG" : "FSR FG");
                    ImGui::TextUnformatted(running);
                    HelpMarker(Tr("The generator is chosen per game in OptiDLSS5-UI (Edit > Frame generation) and"
                                  "\napplies on the game's next launch. The switches below take effect at once."));

                    bool fgOn = config->FGEnabled.value_or_default();
                    if (ImGui::Checkbox(Tr("Frame Generation on"), &fgOn))
                    {
                        config->FGEnabled = fgOn;
                        // As the FG key does: a generator switched back on starts clean.
                        if (fgOn)
                            state.fgChanged = true;
                        anyChanged = true;
                    }

                    const bool hudfixSupported = !config->FGDisableHUDFix.value_or_default() &&
                                                 (state.swapchainInteropApi == SwapchainInteropApi::None ||
                                                  state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12);
                    if (hudfixSupported)
                    {
                        bool hudfix = config->FGHUDFix.value_or_default();
                        if (ImGui::Checkbox(Tr("HUD fix"), &hudfix))
                        {
                            config->FGHUDFix = hudfix;
                            // As OptiScaler's own menu does: no stale HUD-less capture survives the switch.
                            state.clearCapturedHudlesses = true;
                            state.fgChanged = true;
                            anyChanged = true;
                        }
                        HelpMarker(Tr("Keeps the HUD and subtitles from warping in generated frames. OptiScaler"
                                      "\nwarns it can crash some games -- if this game crashes with it on, leave"
                                      "\nit off."));
                    }
                }
                else
                {
                    ImGui::TextColored(kTextDim, "%s", Tr("This game has no NVIDIA DLSS Frame Generation of its own."));
                    HelpMarker(Tr("OptiScaler can generate frames here instead: pick XeFG or FSR FG for this game in"
                                  "\nOptiDLSS5-UI (Edit > Frame generation). It is set up when the game starts, so"
                                  "\nit applies on the next launch; after that it switches on and off right here."));
                }
            }

            // Everything below is this fork's own instrumentation, with no equivalent in NVIDIA's
            // developer overlay -- kept under its original names.

            // The two resets, at the foot of the first page: one puts the panel back where it opens, the
            // other puts this game's tuning back to what it ships with. Together at the bottom rather than
            // in the title row -- a reset is a thing you go looking for, not something to have under a
            // thumb while reaching for the close button.
            ImGui::Spacing();

            if (ImGui::SmallButton((std::string(Tr("Reset layout")) + "##panelpos").c_str()))
            {
                config->DlssNrPanelX = -1.0f;
                config->DlssNrPanelY = -1.0f;
                config->DlssNrPanelW = -1.0f;
                config->DlssNrPanelH = -1.0f;
                s_layout.placeFrames = 2;
                anyChanged = true;
            }
            HelpMarker(Tr("Puts the panel back where it opens -- the left edge, halfway down, at its own size."));

            ImGui::SameLine();

            if (ImGui::SmallButton((std::string(Tr("Reset all to defaults")) + "##resetall").c_str()))
            {
                DlssNr::ResetSettingsToDefaults();
                anyChanged = true;
            }
            HelpMarker(Tr("Every DLSS 5 setting back to what it ships with: the model and its strengths, the"
                          "\ncost controls, the picture and the guides."
                          "\n\nKept: where this panel sits and how big it is (Reset layout, beside this), the"
                          "\nkeys that open it, and whether it is light or dark. Those are yours, not this"
                          "\ngame's tuning."));
        }

        if (OnPage(kPageCost))
        {
            SectionCaption(Tr("Cost"), rowWidth);

            // Sequential model layers between one encode and one final composition. Deferred on release
            // for the same reason Model resolution below is: each layer owns a persistent feature and
            // history, so every distinct value tears those down and rebuilds them.
            static int pendingPasses = -1;
            float passes = pendingPasses >= 0
                               ? (float) pendingPasses
                               : (float) std::clamp(config->DlssNrPasses.value_or_default(), 1u, DlssNr::MaxPassCount);

            auto rPasses = NrSlider(Tr("Model passes"), &passes, 1.0f, (float) DlssNr::MaxPassCount, "%.0f", rowWidth);
            if (rPasses.changed)
                pendingPasses = (int) std::lroundf(passes);

            if (rPasses.released && pendingPasses >= 0)
            {
                config->DlssNrPasses = (uint32_t) std::clamp(pendingPasses, 1, (int) DlssNr::MaxPassCount);
                pendingPasses = -1;
                anyChanged = true;
            }
            HelpMarker(Tr("How many times the model runs before its answer is composed. Each extra layer is"
                          "\nfed the previous layer's output and keeps its own temporal history."
                          "\n\nThe base frame stays untouched and the composition happens once at the end, so"
                          "\ncolour and transfer strength do not compound -- but the model is being asked to"
                          "\nenhance its own output, which is outside what it was trained on."
                          "\n\nCost is very nearly linear: the model is almost the whole expense of the pass"
                          "\nand every layer pays it again. Three is the ceiling because later layers converge"
                          "\nwhile still costing full price."));

            {
                const int shownPasses = pendingPasses >= 0 ? pendingPasses : (int) std::lroundf(passes);
                if (shownPasses > 1)
                {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
                    ImGui::TextColored(shownPasses == 2 ? ImVec4(0.95f, 0.70f, 0.20f, 1.0f)
                                                        : ImVec4(0.92f, 0.30f, 0.25f, 1.0f),
                                       Tr("%dx model cost. Two often reads as richer; three is usually visibly "
                                          "over-processed."),
                                       shownPasses);
                    ImGui::PopTextWrapPos();

                    bool chainedHistory = config->DlssNrChainedHistory.value_or_default();
                    if (NrCheckbox(Tr("Chained temporal history"), &chainedHistory))
                    {
                        config->DlssNrChainedHistory = chainedHistory;
                        anyChanged = true;
                    }
                    HelpMarker(Tr("What the stacked passes do with their temporal history between frames."
                                  "\n\nOn (default): every pass keeps its own history, so each layer accumulates the"
                                  "\nway pass one does. Off: passes 2+ are reset every frame -- stateless refinement,"
                                  "\nwhich cannot compound ghosting."
                                  "\n\nThe trade is real both ways. Keeping history is richer and can compound ghosting"
                                  "\nbehind fast movement; resetting every frame cannot, but NVIDIA documents"
                                  "\nreset-per-frame as a flicker and aliasing risk -- which is what shimmering on two"
                                  "\nor three passes usually is. Try the other setting when a stacked picture shimmers,"
                                  "\nand keep whichever the game looks better with."
                                  "\n\nOnly does anything with more than one pass."));

                    const char* const kInheritedPresetNames[] = { Tr("Auto (inherit pass 1)"), Tr("Default"),
                                                                  Tr("Model A"), Tr("Model B"), Tr("Model C") };
                    const char* const kInheritedStyleNames[] = { Tr("Auto (inherit pass 1)"), Tr("Default (standard)"),
                                                                 Tr("Natural"), Tr("Cinematic") };

                    anyChanged |=
                        InheritedProfileCombo(Tr("Pass 2 model"), &config->DlssNrPass2Preset, kInheritedPresetNames,
                                              IM_ARRAYSIZE(kInheritedPresetNames), rowWidth);
                    anyChanged |=
                        InheritedProfileCombo(Tr("Pass 2 style"), &config->DlssNrPass2Style, kInheritedStyleNames,
                                              IM_ARRAYSIZE(kInheritedStyleNames), rowWidth);

                    if (shownPasses > 2)
                    {
                        anyChanged |=
                            InheritedProfileCombo(Tr("Pass 3 model"), &config->DlssNrPass3Preset, kInheritedPresetNames,
                                                  IM_ARRAYSIZE(kInheritedPresetNames), rowWidth);
                        anyChanged |=
                            InheritedProfileCombo(Tr("Pass 3 style"), &config->DlssNrPass3Style, kInheritedStyleNames,
                                                  IM_ARRAYSIZE(kInheritedStyleNames), rowWidth);
                    }

                    HelpMarker(Tr("Which built-in profile each later layer runs. These select a different"
                                  "\nprofile inside the same NVIDIA model file -- nothing extra is loaded."
                                  "\n\nAuto means the layer runs whatever pass 1 is set to. Changing one"
                                  "\nrebuilds only that layer's feature, and only while that layer is active."));
                }
            }

            // Model resolution, by hand or by itself. The automatic block below drives this same number,
            // so the slider is shown disabled rather than hidden while it is on: the value moving is the
            // clearest possible statement of what the controller is doing.
            const bool autoScaleOn = config->DlssNrAutoScale.value_or_default();

            static int pendingScale = -1;
            float scalePercent =
                pendingScale >= 0 ? (float) pendingScale : config->DlssNrWorkingScale.value_or_default() * 100.0f;

            ImGui::BeginDisabled(autoScaleOn);
            auto rScale = NrSlider(Tr("Model resolution"), &scalePercent, 25.0f, 200.0f, "%.0f%%", rowWidth);
            if (rScale.changed)
                pendingScale = (int) std::lroundf(scalePercent);

            if (rScale.released && pendingScale >= 0)
            {
                config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
                pendingScale = -1;
                anyChanged = true;
            }
            ImGui::EndDisabled();
            HelpMarker(Tr("What fraction of the frame the model works at. Cost falls with the square of"
                          "\nthis, so half resolution is roughly a quarter of the time. Below 100 the frame"
                          "\nitself is never reduced -- only the model's own contribution is computed small"
                          "\nand enlarged. Applied when the handle is let go, not while it is moving."));

            DrawAutoScale(config, rowWidth, anyChanged);

            // Above native the model is run supersampled and filtered back down, so the filter is
            // the whole difference between supersampling meaning less noise and meaning more.
            const int shownScale = pendingScale >= 0 ? pendingScale : (int) std::lroundf(scalePercent);

            if (shownScale > 100)
            {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
                ImGui::TextColored(kTextDim,
                                   Tr("Supersampling %.2fx: the model runs ABOVE native, then is sampled back "
                                      "down. Experimental, and costly -- time grows with the area."),
                                   shownScale / 100.0f);
                ImGui::PopTextWrapPos();

                static const char* const kDownscalerNames[] = { "FSR1",     "Bicubic", "Catmull-Rom", "Lanczos2",
                                                                "Lanczos3", "Kaiser2", "Kaiser3",     "MAGIC" };

                int ds = (int) config->DlssNrScalingDownscaler.value_or_default();

                if (ds < 0 || ds >= IM_ARRAYSIZE(kDownscalerNames))
                    ds = (int) Scaler::Lanczos3;

                if (NrCombo(Tr("Downscaler"), &ds, kDownscalerNames, IM_ARRAYSIZE(kDownscalerNames), rowWidth))
                {
                    config->DlssNrScalingDownscaler = (Scaler) ds;
                    anyChanged = true;
                }
                HelpMarker(Tr("The filter that averages the model's above-native answer back to display size --"
                              "\nthis is what turns supersampling into LESS noise rather than more. Sharper"
                              "\nfilters (Lanczos3, Kaiser3) keep the most detail; softer ones (Bicubic,"
                              "\nCatmull-Rom) are gentler on ringing. Independent of the Output Scaling"
                              "\ndownscaler, so the two can differ and run at the same time."));
            }
        }

        if (OnPage(kPageImage))
        {
            // How the model's work is brought back up when it ran below the frame's size. Classic
            // composes the small picture straight against the full-size frame, which cannot tell the
            // shrink's blur apart from the model's edit.
            const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;

            ImGui::BeginDisabled(!reduced);
            const char* enlargeNames[] = { Tr("Classic"), Tr("Matched residual") };
            int enlarge = config->DlssNrTransfer.value_or_default() == 1 ? 1 : 0;
            if (NrCombo(Tr("Enlargement"), &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames), rowWidth))
            {
                config->DlssNrTransfer = (uint32_t) enlarge;
                anyChanged = true;
            }
            ImGui::EndDisabled();
            HelpMarker(Tr("How the model's work is brought back up when it ran below the frame's size."
                          "\n\nClassic composes the model's small picture directly against the full-size frame."
                          "\nThose two disagree by the shrink's blur as well as by the model's edit, and the"
                          "\ncomposition cannot tell them apart."
                          "\n\nGreyed out at 100%, where there is nothing to enlarge."));

            // The filter that enlarges the proxy for a model working ABOVE the frame's size, and the four
            // controls that shape the one of them that reads them.
            //
            // Greyed below 100%, which is the opposite of what this said until 2026-09-22. Only the
            // supersample leg uses this filter (DlssNr_Dx12.cpp: `if (workScale > 1.0f)` builds superUp
            // with it); a model working BELOW the frame is enlarged inside the resolve shader, which has
            // its own control -- Enlargement, above. So the rows were live exactly when they did nothing
            // and dead when they mattered, which is how a player came to change every filter at 50% and
            // measure no difference at all.
            // Live whenever the model is not working at exactly the frame's size, in either direction: it
            // enlarges the proxy for a model working ABOVE it, and the model's answer for one working below.
            const float workScaleNow = config->DlssNrWorkingScale.value_or_default();
            const bool scaledEither = workScaleNow < 0.999f || workScaleNow > 1.001f;

            const char* upscalerNames[] = { Tr("Bicubic"),        Tr("EWA Lanczos"),   Tr("xBR-lv2"),
                                            Tr("Sharp bilinear"), Tr("Integer scale"), Tr("Nearest") };

            int upscaler = (int) config->DlssNrScalingUpscaler.value_or_default();

            if (upscaler < 0 || upscaler >= IM_ARRAYSIZE(upscalerNames))
                upscaler = (int) Upsampler::Bicubic;

            ImGui::BeginDisabled(!scaledEither);
            if (NrCombo(Tr("Upscaler"), &upscaler, upscalerNames, IM_ARRAYSIZE(upscalerNames), rowWidth))
            {
                config->DlssNrScalingUpscaler = (Upsampler) upscaler;
                anyChanged = true;
            }
            ImGui::EndDisabled();
            HelpMarker(Tr("The filter used whenever the model is not working at the frame's own size: it"
                          "\nenlarges the model's answer back up when Model resolution is below 100%, and"
                          "\nenlarges the frame for the model when it is above. Greyed out at exactly 100%,"
                          "\nwhere nothing is being resized."
                          "\n\nBicubic is the default because it is the cheapest and cannot go wrong, not"
                          "\nbecause it is good -- it is soft. On a rendered 3D game the one to try is EWA"
                          "\nLanczos, which weighs pixels by how far away they really are rather than by row"
                          "\nand column, so a diagonal edge comes out as clean as a horizontal one."
                          "\n\nxBR-lv2, Sharp bilinear, Integer scale and Nearest are for PIXEL ART and 2D."
                          "\nOn a rendered 3D frame they will look wrong."));

            // EWA Lanczos's own four. All 0 to 1 with 0 the gentlest, so they read as one set.
            const bool ewa = scaledEither && config->DlssNrScalingUpscaler.value_or_default() == Upsampler::EwaLanczos;

            ImGui::BeginDisabled(!ewa);

            float nrSharpness = config->DlssNrScalingSharpness.value_or_default();
            auto rNrSharp = NrSlider(Tr("Sharpness"), &nrSharpness, 0.0f, 1.0f, "%.2f", rowWidth);
            if (rNrSharp.changed)
                config->DlssNrScalingSharpness = nrSharpness;
            if (rNrSharp.released)
                anyChanged = true;
            HelpMarker(Tr("How hard the filter is pulled in. One slider across the three EWA filters there"
                          "\nare: 0.00 the gentlest, about 0.16 the middle one, 1.00 the sharpest."
                          "\n\nThe top of it reads 100 pixels for every one it draws, against 64 at the"
                          "\nbottom, so watch the cost line as you climb. Raise Ring suppression with it."));

            float nrRing = config->DlssNrScalingAntiRinging.value_or_default();
            auto rNrRing = NrSlider(Tr("Ring suppression"), &nrRing, 0.0f, 1.0f, "%.2f", rowWidth);
            if (rNrRing.changed)
                config->DlssNrScalingAntiRinging = nrRing;
            if (rNrRing.released)
                anyChanged = true;
            HelpMarker(Tr("The bright or dark rim sharpening buys, held back: it keeps the filter's answer"
                          "\ninside the range of the pixels it is interpolating between. 0 leaves the"
                          "\nfilter's own answer, 1 allows no overshoot at all."
                          "\n\nNot the same control as Highlight guard: that one bounds what the MODEL did,"
                          "\nthis one bounds what the scaling filter did."));

            float nrSigmoid = config->DlssNrScalingSigmoid.value_or_default();
            auto rNrSigmoid = NrSlider(Tr("Sigmoidal light"), &nrSigmoid, 0.0f, 1.0f, "%.2f", rowWidth);
            if (rNrSigmoid.changed)
                config->DlssNrScalingSigmoid = nrSigmoid;
            if (rNrSigmoid.released)
                anyChanged = true;
            HelpMarker(Tr("Resample on an S-shaped curve, so an overshoot near black or near white is"
                          "\ncompressed instead of clipping into a flat band. The slider is how hard the"
                          "\ncurve bends, with the reference setting at 1.00."
                          "\n\nSDR only by construction: anything brighter than white passes through"
                          "\nuntouched, so an HDR frame is barely affected."));

            float nrDither = config->DlssNrScalingDither.value_or_default();
            auto rNrDither = NrSlider(Tr("Dither"), &nrDither, 0.0f, 1.0f, "%.2f", rowWidth);
            if (rNrDither.changed)
                config->DlssNrScalingDither = nrDither;
            if (rNrDither.released)
                anyChanged = true;
            ImGui::EndDisabled();
            HelpMarker(Tr("Breaks a band by adding a pattern finer than one step of colour, moved on each"
                          "\nframe so it does not settle into something you can pick out. 1.00 is half a"
                          "\nstep of an 8-bit picture."
                          "\n\nFor banding in a sky or a gradient, where Ring suppression is for a rim"
                          "\nalong an edge."));

            SectionCaption(Tr("How much of it lands"), rowWidth);

            float transfer = config->DlssNrTransferStrength.value_or_default();
            auto rTransfer = NrSlider(Tr("Detail strength"), &transfer, 0.0f, 2.0f, "%.2f", rowWidth);
            if (rTransfer.changed)
                config->DlssNrTransferStrength = transfer;
            if (rTransfer.released)
                anyChanged = true;

            ImGui::SameLine();

            if (ImGui::SmallButton((std::string(Tr("Reset")) + "##detail").c_str()))
            {
                config->DlssNrTransferStrength = 1.0f;
                anyChanged = true;
            }
            HelpMarker(Tr("How far the frame moves toward the model's picture. 0 gives back exactly what"
                          "\nthe upscaler produced. 1 is the model's picture. Above 1 carries on past"
                          "\nit in the same direction."));

            float colour = config->DlssNrColourStrength.value_or_default();
            auto rColour = NrSlider(Tr("Colour strength"), &colour, 0.0f, 4.0f, "%.2f", rowWidth);
            if (rColour.changed)
                config->DlssNrColourStrength = colour;
            if (rColour.released)
                anyChanged = true;

            ImGui::SameLine();

            if (ImGui::SmallButton((std::string(Tr("Reset")) + "##colour").c_str()))
            {
                config->DlssNrColourStrength = 1.0f;
                anyChanged = true;
            }
            HelpMarker(Tr("Whether the model's colour arrives with its light. 0 keeps the game's own hue"
                          "\nexactly -- every pixel the original colour, with only its brightness carrying"
                          "\nthe model's verdict. 1 brings the model's colour as well, in its own hue,"
                          "\nclamped into AP1 so nothing unreachable is asked for."
                          "\n\nAbove 1 it over-saturates: the colour keeps its hue but grows more vivid, and"
                          "\nrolls off at the edge of what the display can show rather than clipping into a"
                          "\nflat blown patch. 1 is the model's own colour; push past it for punch."));

            // The tone trim (2026-09-23: "some games come out so dark"). Same row shape as colour
            // strength above; both are read by the resolve every frame, so no rebuild and no hitch.
            // Auto sits beside each slider (2026-09-23). While it is on the slider shows what Auto is using
            // and cannot be dragged; the slider's own value is kept and comes back when Auto goes off.
            const DlssNr::AutoToneReading autoTone = DlssNr::AutoTone();

            const bool autoBrightness = config->DlssNrAutoBrightness.value_or_default();
            float brightness = autoBrightness && autoTone.measuring ? autoTone.brightness
                                                                    : config->DlssNrBrightness.value_or_default();
            ImGui::BeginDisabled(autoBrightness);
            auto rBrightness = NrSlider(Tr("Brightness"), &brightness, 0.5f, 2.0f, "%.2f", rowWidth);
            if (rBrightness.changed)
                config->DlssNrBrightness = brightness;
            if (rBrightness.released)
                anyChanged = true;

            ImGui::SameLine();

            if (ImGui::SmallButton((std::string(Tr("Reset")) + "##brightness").c_str()))
            {
                config->DlssNrBrightness = 1.0f;
                anyChanged = true;
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            bool autoB = autoBrightness;
            if (ImGui::Checkbox((std::string(Tr("Auto")) + "##autobrightness").c_str(), &autoB))
            {
                config->DlssNrAutoBrightness = autoB;
                anyChanged = true;
            }
            HelpMarker(Tr("Lifts the shadows and midtones for a game that comes out too dark. Black stays"
                          "\nblack and white stays white -- only what lies between is raised -- so the"
                          "\nhighlights do not blow out. Below 1 darkens the same way. 1 changes nothing."
                          "\n\nAuto measures the picture and lifts it when it is darker than usual, easing"
                          "\nover a moment rather than jumping. It only ever brightens, and only part of the"
                          "\nway, so a scene meant to be dark stays darker than a lit one. DX12, DX11 and"
                          "\nRE Engine games; on a Vulkan game the slider stays in charge."));

            const bool autoContrast = config->DlssNrAutoContrast.value_or_default();
            float contrast =
                autoContrast && autoTone.measuring ? autoTone.contrast : config->DlssNrContrast.value_or_default();
            ImGui::BeginDisabled(autoContrast);
            auto rContrast = NrSlider(Tr("Contrast"), &contrast, 0.5f, 2.0f, "%.2f", rowWidth);
            if (rContrast.changed)
                config->DlssNrContrast = contrast;
            if (rContrast.released)
                anyChanged = true;

            ImGui::SameLine();

            if (ImGui::SmallButton((std::string(Tr("Reset")) + "##contrast").c_str()))
            {
                config->DlssNrContrast = 1.0f;
                anyChanged = true;
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            bool autoC = autoContrast;
            if (ImGui::Checkbox((std::string(Tr("Auto")) + "##autocontrast").c_str(), &autoC))
            {
                config->DlssNrAutoContrast = autoC;
                anyChanged = true;
            }
            HelpMarker(Tr("How far apart the darks and the lights sit. Above 1 is punchier: darks go"
                          "\ndeeper and lights brighter around the middle grey. Below 1 is flatter and"
                          "\nshows more in the shadows. Black and white themselves never move. 1 changes"
                          "\nnothing."
                          "\n\nAuto adds a little contrast to a flat, washed-out picture and takes a little off"
                          "\none that is already harsh, within 0.85 to 1.25. DX12, DX11 and RE Engine games."));

            SectionCaption(Tr("Colour"), rowWidth);

            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
            ImGui::TextColored(kTextDim, "%s",
                               Tr("The model was trained on finished, sRGB-encoded frames. These decide how "
                                  "the upscaler's linear output is mapped into something it recognises."));
            ImGui::PopTextWrapPos();

            // 0 off (soft knee), 1 Neutwo + composition, 2 Neutwo + pure-inverse replace, 3 hybrid +
            // composed, 4 hybrid + replace. This decides what every control below is working on, so it
            // opens the section rather than trailing it.
            const char* const kReversibleNames[] = { Tr("Off (soft knee)"), Tr("Neutwo proxy + composed"),
                                                     Tr("Neutwo proxy + replace"), Tr("Hybrid proxy + composed"),
                                                     Tr("Hybrid proxy + replace") };

            int reversible = (int) config->DlssNrReversibleMode.value_or_default();

            if (reversible < 0 || reversible > 4)
                reversible = 0;

            if (NrCombo(Tr("Reversible proxy"), &reversible, kReversibleNames, IM_ARRAYSIZE(kReversibleNames),
                        rowWidth))
            {
                config->DlssNrReversibleMode = (uint32_t) reversible;
                anyChanged = true;
            }
            HelpMarker(Tr("What the model is shown, and how its answer comes back. Experimental."
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
                          "\nfar more stable."));

            // Where the number that divides the frame comes from. This used to be a checkbox on
            // DlssNrWhitePointFromExposure; upstream replaced that flag with a three-way source, and the
            // engine no longer reads the old one -- the checkbox would have kept setting a value nothing
            // consults. The scan asks the source whether it is wanted, so choosing it here is the whole
            // of switching it on: there is no second flag to keep in step, and so no way for two to
            // disagree.
            const char* const kSourceNames[] = { Tr("Paper white only"), Tr("The game's own exposure"),
                                                 Tr("A buffer the scan found") };

            int wpSource = (int) config->DlssNrWhitePointSource.value_or_default();

            if (wpSource < 0 || wpSource > 2)
                wpSource = 0;

            if (NrCombo(Tr("White point from"), &wpSource, kSourceNames, 3, rowWidth))
            {
                config->DlssNrWhitePointSource = (uint32_t) wpSource;
                anyChanged = true;
            }
            HelpMarker(Tr("Paper white only -- the slider below and nothing else. Right for a game whose"
                          "\nexposure never moves, wrong the moment it does: one constant cannot serve a"
                          "\ncave and a field."
                          "\n\nThe game's own exposure -- read from the texture the game hands the upscaler."
                          "\nThe best source there is, because it is decided upstream and nothing this pass"
                          "\ndoes can move it. Not every game supplies one."
                          "\n\nA buffer the scan found -- for games that compute an exposure and never pass"
                          "\nit on. A guess: candidates are matched by shape, and the anchor's ratio cancels"
                          "\nthe scale. Needs anchoring once, in the Experimental section, and checking after."));

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
                                           anchors.size() == 1 ? Tr("Scan %.5f  ->  white point %.2f   (1 point)")
                                                               : Tr("Scan %.5f  ->  white point %.2f   (%u points)"),
                                           liveScan, w, (unsigned) anchors.size());
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
                        snprintf(lbl, sizeof(lbl), Tr("Paper white (point %d)"), selectedAnchor + 1);
                    else
                        snprintf(lbl, sizeof(lbl), Tr("Paper white"));

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

                    HelpMarker(Tr("The white point for the selected calibration point, or -- with no row"
                                  "\nselected -- the value the next Anchor press captures."
                                  "\n\nSet it until the picture looks right here, then Anchor. Move to very"
                                  "\ndifferent light and do it again: two points fix the buffer's real"
                                  "\nrelationship and the white point holds between them."));
                }

                // In the steady state this is what stands in for paper white: adjust until the picture
                // looks right in the current light, then Anchor bakes the trimmed value into a new point
                // and resets the trim to 1.
                if (!anchors.empty())
                {
                    float trim = config->DlssNrScanTrim.value_or_default();
                    auto rTrim = NrSlider(Tr("Exposure (scan)"), &trim, 0.25f, 4.0f, "%.2fx", rowWidth, true, true);

                    if (rTrim.changed)
                        config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);

                    if (rTrim.released)
                        anyChanged = true;

                    ImGui::SameLine();

                    if (ImGui::SmallButton((std::string(Tr("Reset")) + "##scantrim").c_str()))
                    {
                        config->DlssNrScanTrim = 1.0f;
                        anyChanged = true;
                    }

                    HelpMarker(Tr("A multiplier on the scan's white point, and the control to adjust between"
                                  "\nanchor points: dial it until the picture looks right in the current light,"
                                  "\nthen press Anchor under Experimental -- that captures the trimmed value as"
                                  "\na new point and resets this to 1."));
                }
            }
            else if (wpSource == 1)
            {
                const auto ex = DlssNr::GameExposureStatus();

                // Whether this game supplies one at all, shown either way -- without it, a game that
                // offers nothing looks identical to the option working quietly.
                if (vulkan)
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "%s",
                                       DlssNr::ExposureOfferedVk()
                                           ? Tr("This game supplies an exposure and it is being read.")
                                           : Tr("This game supplies no exposure. Try the scan instead."));
                else if (ex.seenFrames == 0)
                    ImGui::TextColored(kTextDim, "%s", Tr("Waiting for a frame..."));
                else if (!ex.everOffered)
                    ImGui::TextColored(ImVec4(0.85f, 0.65f, 0.25f, 1.0f), "%s",
                                       Tr("This game supplies no exposure. Try the scan instead."));
                else if (ex.exposure > 1e-6f)
                {
                    const float trim = std::clamp(config->DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
                    ImGui::TextColored(
                        ImVec4(0.45f, 0.8f, 0.45f, 1.0f), Tr("Game exposure %.4f  ->  white point %.2f%s"), ex.exposure,
                        ex.preExposure / ex.exposure * trim, ex.offeredNow ? "" : Tr("  (held: absent this frame)"));
                }
                else
                    ImGui::TextColored(kTextDim, "%s", Tr("Reading the exposure..."));

                float trim = config->DlssNrWhitePointTrim.value_or_default();
                auto rTrim = NrSlider(Tr("Exposure"), &trim, 0.25f, 4.0f, "%.2fx", rowWidth, true, true);

                if (rTrim.changed)
                    config->DlssNrWhitePointTrim = std::clamp(trim, 0.25f, 4.0f);

                if (rTrim.released)
                    anyChanged = true;

                ImGui::SameLine();

                // Always present rather than greyed at 1: the point is that the safe value is one click
                // away without having to know what the safe value is.
                if (ImGui::SmallButton((std::string(Tr("Reset")) + "##wptrim").c_str()))
                {
                    config->DlssNrWhitePointTrim = 1.0f;
                    anyChanged = true;
                }

                HelpMarker(Tr("A multiplier on the exposure the game supplied. 1.00x takes its number exactly,"
                              "\nand that is the right answer here."
                              "\n\nThis is not a fudge factor. A game that needs the trim far from 1 to look"
                              "\nright is evidence the exposure being read is wrong for that game, not that the"
                              "\ngame wants trimming. Roughly 0.8 to 1.25 is honest tuning; reaching for 4 means"
                              "\nsomething upstream is broken and this is hiding it."
                              "\n\nYour manual paper white is kept separately and comes back untouched if you"
                              "\nswitch the source back."));
            }
            else
            {
                // Logarithmic, because the useful range is not. A quarter to 2000: the low end because a
                // frame the game already tone mapped wants roughly 1, the high end because there is no
                // principled ceiling -- this is a divisor on an open-ended linear buffer, and how far up
                // a game needs to go is a property of that game rather than anything boundable here.
                float wpScale = config->DlssNrWhitePointScale.value_or_default();
                auto rWp = NrSlider(Tr("Paper white"), &wpScale, 0.25f, 2000.0f, "%.2fx", rowWidth, true, true);

                if (rWp.changed)
                    config->DlssNrWhitePointScale = wpScale;

                if (rWp.released)
                    anyChanged = true;

                HelpMarker(Tr("What the frame is divided by before the model sees it. There is no other white"
                              "\npoint; this is the whole of it. Above 1 the picture handed over is darker, so"
                              "\nhighlights sit lower on the curve."));
            }

            float maxRatio = config->DlssNrMaxRatio.value_or_default();
            auto rMax = NrSlider(Tr("Highlight guard"), &maxRatio, 1.0f, 8.0f, "%.1fx", rowWidth);
            if (rMax.changed)
                config->DlssNrMaxRatio = maxRatio;
            if (rMax.released)
                anyChanged = true;

            ImGui::SameLine();

            if (ImGui::SmallButton((std::string(Tr("Reset")) + "##guard").c_str()))
            {
                config->DlssNrMaxRatio = 2.0f;
                anyChanged = true;
            }
            HelpMarker(Tr("The most the pass may move any pixel, as a multiple of what it already was, in both"
                          "\ndirections -- a pixel may not be brightened past this nor darkened past its"
                          "\nreciprocal. Lights are where the model has least to say and rescaling its answer"
                          "\ndoes the most damage; 2x leaves detail intact while stopping a strip light turning"
                          "\ninto a string of coloured cells. Raise it only if bright areas look clipped."));

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
                SectionCaption(Tr("Exposure scan"), rowWidth);

                const bool isSource = wpSource == 2;

                // Only where it means something: the lamp reads the scan, so offering it beside a white
                // point that comes from somewhere else is offering a control that cannot light up.
                if (isSource)
                {
                    if (bool meter = config->DlssNrScanMeter.value_or_default();
                        NrCheckbox(Tr("Show the light meter on screen"), &meter))
                    {
                        config->DlssNrScanMeter = meter;
                        anyChanged = true;
                    }
                    HelpMarker(Tr("A lamp in the corner: red for dark, green for full light, and the shades"
                                  "\nbetween, with the reading beside it."
                                  "\n\nIt is how you see at a glance that the scan is TRACKING rather than"
                                  "\nmerely running. Walk into shade and it should slide toward red; step out"
                                  "\nand it should go green. If it moves the wrong way, that is what \"the number"
                                  "\nruns the other way\" below is for."
                                  "\n\nPurely a readout. It changes nothing."));
                }

                // The absolute white point cannot come out of a buffer whose units are unknown. Every
                // value AFTER the first can: only the ratio against the anchor is used, so whatever the
                // number means, it cancels. That is why this is a button and not a measurement -- the
                // one thing a person can supply that no cleverness can is "this looks right to me".
                const float live = DlssNr::ExposureScan::BestValue();

                ImGui::BeginDisabled(live <= 0.0f || !isSource);

                if (ImGui::Button(Tr("Anchor here")))
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
                HelpMarker(Tr("Make the picture look right, then press this -- it captures the current look as a"
                              "\npoint. For the first point use the Paper white slider above; for every point"
                              "\nafter, move to different light and use the Trim, which this then bakes in."
                              "\n\nOne point calibrates a ratio and the white point follows the scan from there."
                              "\nWalk into very different light and press it again: the second point pins down"
                              "\nthe buffer's real curve, so everything between the two is right rather than"
                              "\nonly the neighbourhood of one anchor. Up to eight."
                              "\n\nThe table is per game and shareable -- one person calibrates a game and the"
                              "\nnumbers are the same for everyone who takes the profile."));

                if (!isSource)
                    ImGui::TextColored(kTextDim, "%s",
                                       Tr("(the scan is only watching -- the white point above comes from "
                                          "somewhere else)"));

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
                        snprintf(row, sizeof(row), Tr("%s scan %.4f  ->  white %.2f%s"),
                                 ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan, anchors[i].white,
                                 sel ? Tr("   [editing]") : "");

                        // Click selects the row, so the slider above edits it; click again to let go.
                        if (ImGui::Selectable(row, sel))
                            selectedAnchor = sel ? -1 : (int) i;

                        ImGui::PopID();
                    }

                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
                    ImGui::TextColored(kTextDim, "%s",
                                       Tr("Click a row to edit it with the slider above; click it again to "
                                          "control the live point. > is the point in use now."));
                    ImGui::PopTextWrapPos();
                }

                // Only means anything with a single point: with two or more, the direction the white
                // point moves is already fixed by the data.
                if (anchors.size() == 1)
                {
                    if (bool inverted = config->DlssNrScanInverted.value_or_default();
                        NrCheckbox(Tr("The number runs the other way"), &inverted))
                    {
                        config->DlssNrScanInverted = inverted;
                        anyChanged = true;
                    }
                    HelpMarker(Tr("Flip this if the picture gets worse in the direction it should be getting"
                                  "\nbetter. Most engines store an exposure that falls as the scene brightens;"
                                  "\nsome store its reciprocal, and a buffer found by shape does not say which."
                                  "\nAdd a second anchor point in different light and this is decided for you,"
                                  "\nso it disappears."));
                }

                // Read-out rather than control: what the scan is looking at, and how to tell whether it
                // found the right thing. Folded away, because the two decisions that matter are above.
                if (ImGui::TreeNode(Tr("Candidates")))
                {
                    const auto found = DlssNr::ExposureScan::Report();
                    const char* why = DlssNr::ExposureScan::Status();

                    if (found.empty())
                    {
                        ImGui::TextColored(kTextDim, "%s",
                                           // The scan's status is English at the source (it is logged too).
                                           why != nullptr && why[0] != 0 ? Tr(why) : Tr("nothing matched yet."));
                    }
                    else
                    {
                        for (size_t i = 0; i < found.size(); ++i)
                        {
                            const auto& c = found[i];

                            if (c.reads == 0)
                            {
                                ImGui::TextColored(kTextDim, Tr("%zu. %s -- not read yet"), i + 1, c.shape.c_str());
                                continue;
                            }

                            // Moving is the whole signal, so it is the thing that is coloured.
                            ImGui::TextColored(c.moves ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f) : kTextDim,
                                               Tr("%zu. %s = %.5f  (seen %.5f..%.5f) %s"), i + 1, c.shape.c_str(),
                                               c.latest, c.lowest, c.highest,
                                               c.moves ? Tr("MOVES") : Tr("flat so far"));
                        }

                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
                        ImGui::TextColored(kTextDim, "%s",
                                           Tr("Walk from shade into daylight. A real exposure moves. One that "
                                              "only ever climbs is a counter, not an exposure."));
                        ImGui::PopTextWrapPos();
                    }

                    ImGui::TreePop();
                }
            }

            // Both of these describe the frame to the model rather than shaping its output, which is
            // why they sit together and away from the strength controls.
        }

        if (OnPage(kPageInspect))
        {
            SectionCaption(Tr("Guide"), rowWidth);

            // Read-only, and first, because it is the one thing on this page that can be WRONG
            // rather than merely set badly -- and when it is wrong it looks like nothing. With no
            // vectors the model is handed a zero-motion texture and runs anyway: every log line
            // says it ran, and the picture is sharp when still and smears when moving. Until this
            // row, the only way to find that out was to read ReShadePreset.ini from outside the
            // game and infer.
            //
            // It says what it sees and stops there. Which provider to use instead is a question
            // about files, licences and downloads, and none of that belongs in a game process --
            // the manager's panel offers the swap.
            {
                const DlssNr::MotionReading motion = DlssNr::MotionState();
                ImGui::PushStyleColor(ImGuiCol_Text, kText);
                ImGui::TextUnformatted(Tr("Motion"));
                ImGui::PopStyleColor();
                // The same split the rows below use (NrSlider/NrCombo), so this lines up with them.
                ImGui::SameLine(rowWidth * 0.44f);

                if (motion.evaluates == 0)
                {
                    ImGui::TextColored(kTextDim, "%s", Tr("nothing measured yet"));
                }
                else if (motion.blindEvaluates == 0)
                {
                    ImGui::TextColored(kAccent, "%s",
                                       motion.usingFlow ? Tr("arriving (this engine's optical flow)") : Tr("arriving"));
                }
                else if (motion.blindEvaluates >= motion.evaluates)
                {
                    // Never once fed. On a Feeder game this is the provider: its shader is missing,
                    // it failed to compile, or the enabled technique and DLSS5_MV_PROVIDER disagree.
                    ImGui::TextColored(kText, "%s", Tr("NONE -- the model is running blind"));
                }
                else
                {
                    // Intermittent, which is a different fault from never: a provider that feeds
                    // most frames and drops some. Saying "some" rather than a percentage on
                    // purpose -- the exact ratio moves with the scene and would read as precision
                    // this does not have.
                    ImGui::TextColored(kText, "%s", Tr("arriving, but not on every frame"));
                }
                HelpMarker(Tr("Whether motion vectors are actually reaching the model."
                              "\n\nWithout them it still runs, and the result is sharp when you stand still"
                              "\nand smears when you move. Nothing errors, so this row is the only place"
                              "\nit shows."
                              "\n\nOn a Feeder game they come from a ReShade shader, and which one is"
                              "\nchosen in the app -- it can swap them in one press. On this engine's own"
                              "\nPresent route they come from its optical flow module instead, and no"
                              "\nReShade provider is involved."));
            }

            const char* depthNames[] = { Tr("Follow the game"), Tr("Force normal"), Tr("Force inverted") };
            int depthMode = (int) config->DlssNrDepthConvention.value_or_default();
            if (NrCombo(Tr("Depth"), &depthMode, depthNames, IM_ARRAYSIZE(depthNames), rowWidth))

            {
                config->DlssNrDepthConvention = (uint32_t) depthMode;
                anyChanged = true;
            }
            HelpMarker(Tr("Which way round the model is told depth runs. The game states this in the flags it"
                          "\ncreated its own DLSS feature with, and following it is right almost always -- but"
                          "\na game that states it wrongly needs correcting by hand."
                          "\n\nIf the pass looks worst where geometry meets sky, try forcing the other one."));

            if (bool uiCorrection = config->DlssNrUICorrection.value_or_default();
                NrCheckbox(Tr("UI correction"), &uiCorrection))
            {
                config->DlssNrUICorrection = uiCorrection;
                // The other half of Before Super Resolution's rule above: turning this on moves the
                // pass back after the upscaler, where the UI is. The two together froze inZOI on the
                // spot (issue #55, 2026-09-19).
                if (uiCorrection)
                    config->DlssNrRunBeforeSr = false;
                anyChanged = true;
            }
            HelpMarker(Tr("Lets the model account for a UI layer laid over the frame. On is its own default"
                          "\nand right whenever a UI resource reaches it; turn it off if the correction is"
                          "\nitself what looks wrong."
                          "\n\nRead when the model is built."));

            // The fix for the rippling on the Present route. It belongs here because it is about what the
            // model is TOLD -- specifically what it is told when there is nothing honest to tell it.
            if (bool resetWhenBlind = config->DlssNrResetWhenBlind.value_or_default();
                NrCheckbox(Tr("Forget history when motion is unknown"), &resetWhenBlind))
            {
                config->DlssNrResetWhenBlind = resetWhenBlind;
                anyChanged = true;
            }
            HelpMarker(Tr("Textures that ripple, pulse or swim while you move the view -- and nowhere else --"
                          "\nare this."
                          "\n\nWhere a game makes no upscale call of its own, motion has to be worked out from"
                          "\nthe finished frames, and sometimes it cannot be. What the model had then was not"
                          "\n'no motion' but 'motion that says nothing moved', and it believed it: it has a"
                          "\nmemory, and it lines that memory up using exactly those numbers. Standing still"
                          "\nthat is correct. Moving, it blends what is on screen now against what used to be"
                          "\nsomewhere else entirely, over and over."
                          "\n\nOn (default), it keeps no memory at all in that situation, so there is nothing"
                          "\nmisaligned left to blend."
                          "\n\nThat memory is also what steadies a picture, so a game can come out slightly"
                          "\ncrawlier on fine edges instead. If one does, turn this off for it. Costs no"
                          "\nperformance either way, and does nothing in a game that hands over real motion."));

            SectionCaption(Tr("Inspect"), rowWidth);

            bool applyModel = config->DlssNrApplyModel.value_or_default();
            if (NrCheckbox(Tr("Apply the model"), &applyModel))
            {
                config->DlssNrApplyModel = applyModel;
                anyChanged = true;
            }
            HelpMarker(Tr("Whether the model's edit is applied. Off shows the clean upscaler frame while the"
                          "\npass keeps running -- so with Hold frame, under Inspect, you can freeze a frame"
                          "\nand toggle this to see the same frozen frame with and without Neural Rendering."
                          "\nLeave it on for normal use."));

            if (DlssNr::CaptureInProgress())
            {
                ImGui::TextColored(kTextDim, "%s", Tr("Capturing..."));
            }
            else if (ImGui::Button(Tr("Capture 8 frames")))
            {
                DlssNr::RequestCapture(8);
            }
            HelpMarker(Tr("Writes eight consecutive frames twice: as the upscaler produced them, and again"
                          "\nonce the model's edit was applied. Into a dlssnr-capture folder beside"
                          "\nOptiScaler; each run overwrites the last."));

            if (bool autoCapture = config->DlssNrAutoCapture.value_or_default();
                NrCheckbox(Tr("Auto-capture once per session"), &autoCapture))
            {
                config->DlssNrAutoCapture = autoCapture;
                anyChanged = true;
            }
            HelpMarker(Tr("Writes one matched before/after set automatically, without anyone asking. The"
                          "\nfolder is cleared each run, so it holds a single session and never grows."));

            // Freeze the frame the model works on, so a setting change re-renders it in place -- the
            // only clean way to A/B our own settings, since a moving scene confounds every other
            // comparison. See design/frame-hold.md.
            if (bool held = config->DlssNrHoldFrame.value_or_default(); NrCheckbox(Tr("Hold frame"), &held))
            {
                config->DlssNrHoldFrame = held;
                anyChanged = true;
            }
            HelpMarker(Tr("Freezes the frame the model works on. While held, change paper white, the strengths,"
                          "\nthe reversible mode, the model preset -- anything below the upscaler -- and only"
                          "\nthat setting moves; the scene does not. Pairs with \"Apply the model\" at the top:"
                          "\nfreeze a frame, then toggle that to see it with and without."
                          "\n\nWhat it cannot show: upscaler presets or anything upstream of this pass (the"
                          "\nupscaler is not re-run on a held frame), and the game's own HUD and"
                          "\npost-processing, which run after this and keep updating. The white point stops"
                          "\nbeing measured and holds its value, so it cannot drift and confound the"
                          "\ncomparison."
                          "\n\nClose the panel and it stays held. Untick to resume."));

            const char* compareNames[] = { Tr("Off"), Tr("Side by side"), Tr("Wipe") };
            int compare = (int) config->DlssNrCompare.value_or_default();
            if (NrCombo(Tr("Compare"), &compare, compareNames, IM_ARRAYSIZE(compareNames), rowWidth))
            {
                config->DlssNrCompare = (uint32_t) compare;
                anyChanged = true;
            }
            HelpMarker(Tr("Shows the pass against itself. Side by side puts the whole frame in each half;"
                          "\nwipe cuts a single frame at the split and plays normally. Neither needs the"
                          "\nmenu open to keep working."));

            if (compare != 0)
            {
                bool swap = config->DlssNrCompareSwap.value_or_default();
                if (NrCheckbox(Tr("Swap sides"), &swap))
                {
                    config->DlssNrCompareSwap = swap;
                    anyChanged = true;
                }

                bool tags = config->DlssNrCompareTags.value_or_default();
                if (NrCheckbox(Tr("Labels"), &tags))
                {
                    config->DlssNrCompareTags = tags;
                    anyChanged = true;
                }
                HelpMarker(Tr("Draws which side is which into the frame's own plane, so a screenshot still"
                              "\nsays it. Clipped per side, so the wipe reveals and hides them exactly as it"
                              "\ndoes the images."));

                if (tags)
                {
                    float tagScale = config->DlssNrTagScale.value_or_default();
                    auto rTag = NrSlider(Tr("Label size"), &tagScale, 0.5f, 5.0f, "%.1fx", rowWidth);
                    if (rTag.changed)
                        config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
                    if (rTag.released)
                        anyChanged = true;
                }
            }

            if (compare == 1)
            {
                float zoom = config->DlssNrCompareZoom.value_or_default();
                auto rZoom = NrSlider(Tr("Zoom"), &zoom, 1.0f, 2.0f, "%.2f", rowWidth);
                if (rZoom.changed)
                    config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);
                if (rZoom.released)
                    anyChanged = true;
            }

            if (compare == 2)
            {
                float split = config->DlssNrCompareSplit.value_or_default();
                auto rSplit = NrSlider(Tr("Split"), &split, 0.0f, 1.0f, "%.2f", rowWidth);
                if (rSplit.changed)
                    config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);
                if (rSplit.released)
                    anyChanged = true;
            }

            const char* debugNames[] = { Tr("Off"), Tr("Proxy (what the model sees)"), Tr("Model output (raw)"),
                                         Tr("Difference (amplified)") };
            int debugView = (int) config->DlssNrDebugView.value_or_default();
            if (NrCombo(Tr("Debug view"), &debugView, debugNames, IM_ARRAYSIZE(debugNames), rowWidth))
            {
                config->DlssNrDebugView = (uint32_t) debugView;
                anyChanged = true;
            }
            HelpMarker(Tr("Proxy is the picture handed to the model. Difference shows what the model"
                          "\nactually changed, amplified twenty times and centred on grey."));

            // Both of these are experiments toward dropping the forwarder entirely, which is why they
            // ship off. Config.h calls the probe "a diagnostic, not a feature", and the proxy path
            // "off until it is shown to produce the same picture" -- so they are labelled as such
            // rather than presented as ordinary settings.
        }

        if (OnPage(kPageSetup))
        {
            SectionCaption(Tr("Keys"), rowWidth);

            // Both rows are also under Keybinds in OptiScaler's own menu; they are repeated here so the
            // panel is usable on its own, without going looking for the other window.
            MenuCommon::RenderKeybindRow(Tr("Toggle key"), 14, config->DlssNrToggleKey);
            HelpMarker(Tr("Toggles Neural Rendering without opening this panel. Press the button, then the"
                          "\nkey you want. Escape cancels, Backspace unbinds, R resets it."));

            MenuCommon::RenderKeybindRow(Tr("Panel key"), 15, config->DlssNrPanelKey);
            HelpMarker(Tr("Opens and closes this panel. Independent of OptiScaler's own menu key, so the"
                          "\ntwo can be up together or on their own."));

            SectionCaption(Tr("Appearance"), rowWidth);

            // The theme was a button in the title row until 2026-09-22. It is a setting, and settings
            // are here -- as the row that was already here, not as a second control for the same key.
            if (bool light = config->DlssNrLightTheme.value_or_default(); NrCheckbox(Tr("Light panel"), &light))
            {
                config->DlssNrLightTheme = light;
                anyChanged = true;
            }
            HelpMarker(Tr("Light is the default. The dark palette this panel was originally styled after put"
                          "\nits dimmed text at 2.65:1 against the background, against the 4.5:1 that reads"
                          "\ncomfortably -- and an overlay is read at a glance, over a moving picture."
                          "\n\nUnticking restores NVIDIA's own colouring."));

            if (bool vendor = config->DlssNrVendorColours.value_or_default(); NrCheckbox(Tr("Vendor colours"), &vendor))
            {
                config->DlssNrVendorColours = vendor;
                anyChanged = true;
            }
            {
                std::string tip =
                    Tr("The panel's accent follows the card it is drawn on: NVIDIA green on an NVIDIA GPU,"
                       "\nAMD red on an AMD one. Untick to keep the green everywhere.");
                tip += OnAmdGpu() ? Tr("\n\nThis game is running on an AMD card.")
                                  : Tr("\n\nThis game is not running on an AMD card, so this changes nothing here.");
                HelpMarker(tip.c_str());
            }

            // No Language control here. The panel speaks [DlssNr] Language, which OptiDLSS5-UI writes from its
            // own Language setting (auto follows Windows), so the manager is the one place to choose it. A combo
            // here disagreed with the manager and, sitting in the navigation order, kept pulling the scroll back
            // to itself (Cyberpunk 2077, 2026-09-16). I18n::Refresh still reads the key every frame.

            // Not 'fontScale' -- that name is already taken at the top of this function, where the scale
            // is applied to the window.
            float fontScaleEdit = config->DlssNrFontScale.value_or_default();
            auto rFont = NrSlider(Tr("Font size"), &fontScaleEdit, 0.75f, 2.0f, "%.2fx", rowWidth);

            if (rFont.changed)
                config->DlssNrFontScale = std::clamp(fontScaleEdit, 0.75f, 2.0f);

            if (rFont.released)
                anyChanged = true;

            HelpMarker(Tr("This panel's text only -- OptiScaler's own menu keeps its [Menu] FontSize."
                          "\n\nRow widths are worked out from the font size, so far above 1.5x labels start"
                          "\nrunning into their values."));
        }

        if (OnPage(kPagePacing))
            DrawPacingPage(rowWidth);
        if (OnPage(kPageHdr))
            DrawRenoDxPage(rowWidth);

        // Must be popped before End(), and on every path out of this block -- it is a stack, not a
        // per-window property like the SetWindowFontScale it replaced.
        ImGui::PopFontSize();
    }

    // Outside the if, not inside it. Begin returns false whenever the window is clipped out, and
    // ImGui wants End called for every Begin regardless -- skipping it leaves the window stack
    // unbalanced and trips "Mismatched Begin/End calls" in EndFrame.
    ImGui::End();

    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(kPanelColourCount);

    // The wipe's divide, drawn over the frame and draggable by it: the split is a thing you point at,
    // so pointing at it is how it should move. The Split slider stays -- it is the only way in when
    // the mouse belongs to the game -- and the two are the same value.
    if (DragCompareSplit(config))
        anyChanged = true;

    // This overlay saves as you go rather than needing a Save button -- there is nothing else
    // in this build's menu to put one on.
    if (anyChanged)
        config->SaveIni();
}

} // namespace DlssNr

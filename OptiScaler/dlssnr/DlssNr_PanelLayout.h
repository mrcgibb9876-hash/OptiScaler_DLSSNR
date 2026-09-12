#pragma once

// The DLSS 5 panel's move and resize behaviour, kept apart from the panel's contents so the panel
// (DlssNr_Menu.cpp) and a headless test (tests/dlssnr_panel_layout_smoke.cpp) run the same code against
// the real ImGui this build compiles.
//
// Position: the default is the left edge, vertically centred -- where NVIDIA's own overlay sits. Drag the
// background to move it: ImGui moves a title-less window by its body, so this only has to stop re-pinning
// it. It is placed explicitly after it opens (once its size is known), after a display-size change and
// after Reset layout; otherwise it stays where ImGui has it. It may hang off any edge, as long as `keep`
// pixels stay on screen on each axis, so it can always be grabbed back. An untouched panel whose content
// grows is kept wholly on screen instead. Once the mouse is up after a move, the corner is saved.
//
// Size: every edge and the bottom-right corner resize it. Until then it fits its content (what
// AlwaysAutoResize used to do, which also turns resize grips off) but no taller than the display, with a
// scrollbar. After a resize the size is saved and the rows follow the panel's width.
//
// Positions and sizes are saved as fractions of the display, so they come back at any resolution.

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h> // GetActiveID / GetWindowResize*ID, ImGuiWindow::ContentSizeIdeal

#include <algorithm>
#include <cmath>

namespace DlssNr::PanelLayout
{

// What the ini holds: fractions of the display. -1 is the default (fit / default place). A corner can be
// slightly negative (hanging off the left or top) but never reaches -1, because `keep` pixels stay on.
struct Settings
{
    float x = -1.0f, y = -1.0f, w = -1.0f, h = -1.0f;

    bool CustomPos() const { return x > -0.999f && y > -0.999f; }
    bool CustomSize() const { return w > 0.0f && h > 0.0f; }
};

// Per-panel memory between frames.
struct State
{
    ImVec2 display { 0.0f, 0.0f };
    ImVec2 size { 0.0f, 0.0f };         // last frame's window size
    ImVec2 content { 0.0f, 0.0f };      // last frame's content extent, for fitting
    ImVec2 placed { -1.0f, -1.0f };     // where this code last put the window
    int placeFrames = 0;                // frames of explicit placement still to do
    bool placeSized = false;            // this frame's placement used a real size
    ImVec2 placeContent { 0.0f, 0.0f }; // the content extent this frame's placement was sized from
    bool resizing = false;              // an edge or corner was held last frame
    int lastFrame = -10;
};

// Scale-dependent sizes, from the caller.
struct Metrics
{
    float rowWidth;    // the default layout width of a row
    float minRowWidth; // narrower than this and labels run into their values
    float minHeight;
    float margin; // default distance from the left edge
    float keep;   // how much of the panel always stays on screen
    ImVec2 pad;   // the window padding the caller pushes
};

// Free movement, except that `keep` pixels of the panel (or all of it, when it is smaller than that)
// stay on the display on each axis.
inline ImVec2 KeepOnScreen(ImVec2 pos, ImVec2 size, ImVec2 display, float keep)
{
    const float kx = std::min(keep, size.x);
    const float ky = std::min(keep, size.y);
    const float minX = kx - size.x;
    const float minY = ky - size.y;
    return ImVec2(std::clamp(pos.x, minX, std::max(minX, display.x - kx)),
                  std::clamp(pos.y, minY, std::max(minY, display.y - ky)));
}

// The size a panel nobody has resized takes: content plus padding, no taller than maxSize, plus the
// width of the scrollbar that cap brings. (0, 0) until a content size is known: ImGui fits it once.
inline ImVec2 FitSize(ImVec2 content, ImVec2 pad, float rowWidth, float scrollbarSize, ImVec2 maxSize)
{
    if (content.y <= 0.0f)
        return ImVec2(0.0f, 0.0f);

    const float height = content.y + pad.y * 2.0f;
    const bool scrolls = height > maxSize.y;
    return ImVec2(std::max(rowWidth, content.x) + pad.x * 2.0f + (scrolls ? scrollbarSize : 0.0f),
                  std::min(height, maxSize.y));
}

// Whether one of this window's own resize corners or borders is being dragged: ImGui makes them the
// active item, under ids it exposes (ImGuiDir_Left..Down are 0..3, as are the corners).
inline bool IsResizing(ImGuiWindow* window)
{
    const ImGuiID active = ImGui::GetActiveID();
    if (window == nullptr || active == 0)
        return false;

    for (int n = 0; n < 4; ++n)
    {
        if (active == ImGui::GetWindowResizeCornerID(window, n) ||
            active == ImGui::GetWindowResizeBorderID(window, (ImGuiDir) n))
            return true;
    }
    return false;
}

// Before ImGui::Begin: size constraints, this frame's size and, while placing, its position.
inline void BeforeBegin(State& s, const Settings& cfg, const Metrics& m)
{
    const ImGuiIO& io = ImGui::GetIO();

    if (ImGui::GetFrameCount() != s.lastFrame + 1)
        s.placeFrames = 2; // just opened: this is only called while the panel is visible
    s.lastFrame = ImGui::GetFrameCount();

    if (s.display.x != io.DisplaySize.x || s.display.y != io.DisplaySize.y)
    {
        s.display = io.DisplaySize;
        s.placeFrames = 2;
    }

    const ImVec2 minSize(std::min(m.minRowWidth + m.pad.x * 2.0f, io.DisplaySize.x),
                         std::min(m.minHeight, io.DisplaySize.y));
    const ImVec2 maxSize(std::max(minSize.x, io.DisplaySize.x), std::max(minSize.y, io.DisplaySize.y));
    ImGui::SetNextWindowSizeConstraints(minSize, maxSize);

    const ImVec2 want = cfg.CustomSize()
                            ? ImVec2(cfg.w * io.DisplaySize.x, cfg.h * io.DisplaySize.y)
                            : FitSize(s.content, m.pad, m.rowWidth, ImGui::GetStyle().ScrollbarSize, maxSize);

    // While an edge or corner is held, ImGui owns the size; forcing it here would fight the drag.
    if (!s.resizing)
        ImGui::SetNextWindowSize(want, ImGuiCond_Always);

    s.placeSized = false;
    s.placeContent = s.content;
    if (s.placeFrames > 0)
    {
        s.placeSized = want.x > 0.0f;
        const ImVec2 size = s.placeSized ? want : s.size;
        const ImVec2 target =
            cfg.CustomPos()
                ? KeepOnScreen(ImVec2(cfg.x * io.DisplaySize.x, cfg.y * io.DisplaySize.y), size, io.DisplaySize, m.keep)
                : ImVec2(m.margin, std::max(0.0f, io.DisplaySize.y * 0.5f - size.y * 0.5f));
        ImGui::SetNextWindowPos(target, ImGuiCond_Always);
        s.placed = target;
    }
}

// Right after ImGui::Begin, before any item. Updates cfg when the user finished a move or a resize and
// returns true then (the caller saves). rowWidth becomes the row layout width for this frame.
inline bool AfterBegin(State& s, Settings& cfg, const Metrics& m, float& rowWidth)
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 pos = ImGui::GetWindowPos();
    const float innerWidth = ImGui::GetContentRegionAvail().x; // scrollbar already taken off
    bool changed = false;

    s.size = ImGui::GetWindowSize();
    s.content = window->ContentSizeIdeal;

    const bool resizingNow = IsResizing(window);
    if (s.resizing && !resizingNow && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f)
    {
        // Let go of an edge or corner: this is the panel's size from now on. A resize from the left or
        // top also moved it; the position check below saves that.
        cfg.w = s.size.x / io.DisplaySize.x;
        cfg.h = s.size.y / io.DisplaySize.y;
        changed = true;
    }
    s.resizing = resizingNow;

    if (s.placeFrames > 0)
    {
        // Placement only counts once it was done with a settled size. The content measurement lags a
        // frame: on the very first open there is none yet, and on a reopen it is still the size from
        // before (a section opened or closed meanwhile). Centring on either left the panel off centre or
        // off screen, so a frame counts only when this frame's measurement matches what it was sized by.
        const bool fitSettled = cfg.CustomSize() || (std::fabs(s.content.x - s.placeContent.x) < 0.5f &&
                                                     std::fabs(s.content.y - s.placeContent.y) < 0.5f);
        if (s.placeSized && fitSettled)
            s.placeFrames--;
    }
    else
    {
        const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        ImVec2 kept = KeepOnScreen(pos, s.size, io.DisplaySize, m.keep);
        const bool userMoved = !mouseDown && (kept.x != s.placed.x || kept.y != s.placed.y);

        if (!cfg.CustomPos() && !mouseDown && !userMoved)
        {
            // An untouched panel stays wholly on screen as its content grows. Not a user move: not saved.
            kept.y = std::clamp(kept.y, 0.0f, std::max(0.0f, io.DisplaySize.y - s.size.y));
            s.placed = kept;
        }

        // Every frame, during a drag too: it follows the mouse until only the strip is left, and stops.
        if (kept.x != pos.x || kept.y != pos.y)
            ImGui::SetWindowPos(kept, ImGuiCond_Always);

        if (userMoved && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f)
        {
            s.placed = kept;
            cfg.x = kept.x / io.DisplaySize.x;
            cfg.y = kept.y / io.DisplaySize.y;
            changed = true;
        }
    }

    // Rows follow a resized panel's width (and follow it live while an edge is dragged); an untouched
    // panel keeps its fixed layout width.
    if (cfg.CustomSize() || s.resizing)
        rowWidth = std::max(m.minRowWidth, innerWidth);

    return changed;
}

} // namespace DlssNr::PanelLayout

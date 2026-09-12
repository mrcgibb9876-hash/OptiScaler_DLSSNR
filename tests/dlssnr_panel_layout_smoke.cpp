// Headless regression for the DLSS 5 panel's move/resize behaviour. It drives the same
// PanelLayout::BeforeBegin / AfterBegin the panel calls (OptiScaler/dlssnr/DlssNr_PanelLayout.h),
// against the ImGui this build compiles (OptiScaler/include/imgui), with the mouse fed through ImGui's
// own input queue. No renderer: frames are built and discarded. Rows of Dummy items stand in for the
// panel's controls.
//
// Build from a VS x64 Native Tools prompt at the repository root:
//   cl /nologo /std:c++20 /EHsc /MD /IOptiScaler\include /IOptiScaler\include\imgui /Iexternal\freetype ^
//      tests\dlssnr_panel_layout_smoke.cpp OptiScaler\include\imgui\imgui.cpp ^
//      OptiScaler\include\imgui\imgui_draw.cpp OptiScaler\include\imgui\imgui_widgets.cpp ^
//      OptiScaler\include\imgui\imgui_tables.cpp OptiScaler\include\imgui\misc\freetype\imgui_freetype.cpp ^
//      /Fe:x64\dlssnr_panel_layout_smoke.exe /link external\freetype\freetype.lib
//   x64\dlssnr_panel_layout_smoke.exe
#include <imgui/imgui.h>
#include "../OptiScaler/dlssnr/DlssNr_PanelLayout.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace DlssNr::PanelLayout;

static void expect(bool ok, const std::string& what)
{
    if (!ok)
        throw std::runtime_error(what);
}

static bool near(float a, float b, float tol = 1.5f) { return std::fabs(a - b) <= tol; }

static std::string v2(ImVec2 v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "(%.1f, %.1f)", v.x, v.y);
    return buf;
}

struct Panel
{
    Settings cfg;          // what the ini holds
    State state;           // what the panel keeps between frames
    int rows = 60;         // content height = rows * 24 px
    float rowWidth = 0.0f; // this frame's layout width, as the panel computes it
    int saves = 0;         // how many times the panel would have saved its ini

    void Render()
    {
        const float scale = 1.0f;
        rowWidth = 460.0f * scale;
        const Metrics m { rowWidth, std::round(rowWidth * 0.8f), 160.0f * scale, 24.0f * scale, 64.0f * scale,
                          ImVec2(18.0f, 14.0f) * scale };

        BeforeBegin(state, cfg, m);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, m.pad);
        ImGui::Begin("##DlssNrOverlay", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

        if (AfterBegin(state, cfg, m, rowWidth))
            saves++;

        ImGui::Dummy(ImVec2(rowWidth, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        for (int i = 0; i < rows; ++i)
            ImGui::Dummy(ImVec2(rowWidth, 24.0f));
        ImGui::PopStyleVar();

        ImGui::End();
        ImGui::PopStyleVar();
    }
};

static Panel g_panel;

static void Frame(int count = 1)
{
    for (int i = 0; i < count; ++i)
    {
        ImGui::NewFrame();
        g_panel.Render();
        ImGui::Render();
    }
}

// Final window geometry, read after the frame.
static ImVec2 Pos() { return ImGui::FindWindowByName("##DlssNrOverlay")->Pos; }
static ImVec2 Size() { return ImGui::FindWindowByName("##DlssNrOverlay")->Size; }

static void MouseTo(ImVec2 p, int frames = 1)
{
    ImGui::GetIO().AddMousePosEvent(p.x, p.y);
    Frame(frames);
}

static void Button(bool down)
{
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    Frame(2);
}

// Press at `from`, move to `to` in steps, release.
static void Drag(ImVec2 from, ImVec2 to, int steps = 12)
{
    MouseTo(from, 2);
    Button(true);
    for (int i = 1; i <= steps; ++i)
        MouseTo(from + (to - from) * ((float) i / steps));
    Button(false);
    Frame(2);
}

// Closing and reopening the panel: a gap in frames, which BeforeBegin reads as "just opened".
static void Reopen()
{
    ImGui::NewFrame();
    ImGui::Render();
    Frame(6);
}

int main()
{
    try
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1920.0f, 1080.0f);
        io.DeltaTime = 1.0f / 60.0f;
        // No renderer: claim texture support so the 1.92 atlas builds on demand, and never upload.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();

        const float keep = 64.0f;
        const float scrollbar = ImGui::GetStyle().ScrollbarSize;
        const float fullWidth = 460.0f + 36.0f;

        // 1. First ever open, content taller than the display: fits the width, caps the height, adds the
        //    scrollbar's width, and is placed with its real size -- at the left edge, top on screen.
        g_panel.rows = 60; // 1440 px of rows
        Frame(8);
        expect(near(Size().y, 1080.0f), "tall panel should cap at the display height, got " + v2(Size()));
        expect(near(Size().x, fullWidth + scrollbar), "tall panel width should add the scrollbar, got " + v2(Size()));
        expect(near(Pos().x, 24.0f) && near(Pos().y, 0.0f), "tall panel should sit at the left edge, top, got " + v2(Pos()));
        expect(g_panel.saves == 0, "opening must not save anything");
        std::printf("PASS: first open, tall content: %s at %s, nothing saved\n", v2(Size()).c_str(), v2(Pos()).c_str());

        // 2. Reopened with short content: exactly content + padding, vertically centred, no scrollbar.
        g_panel.rows = 20; // 480 px
        Reopen();
        // rows + the default item spacing after the zero-height width row + padding
        expect(near(Size().y, 480.0f + ImGui::GetStyle().ItemSpacing.y + 28.0f, 1.0f),
               "short panel should fit its content, got " + v2(Size()));
        expect(near(Size().x, fullWidth), "short panel should have no scrollbar width, got " + v2(Size()));
        expect(near(Pos().y, 540.0f - Size().y * 0.5f, 3.0f), "short panel should be centred, got " + v2(Pos()));
        std::printf("PASS: reopen, short content: %s centred at %s\n", v2(Size()).c_str(), v2(Pos()).c_str());

        // 3. Untouched panel whose content grows: stays wholly on screen, and that is not saved as a move.
        g_panel.rows = 42; // 1008 px: taller than the space below its centred top
        Frame(6);
        expect(Pos().y >= 0.0f && Pos().y + Size().y <= 1080.0f + 0.5f,
               "growing untouched panel should stay on screen, got " + v2(Pos()) + v2(Size()));
        expect(!g_panel.cfg.CustomPos() && g_panel.saves == 0, "keeping it on screen must not save a position");
        std::printf("PASS: content grows, untouched panel kept on screen at %s, nothing saved\n", v2(Pos()).c_str());
        g_panel.rows = 20;
        Frame(6);

        // 4. Body drag far past the left and bottom edges: follows the mouse until only the strip is left,
        //    stays there after release, and the corner is saved as a (negative) fraction.
        {
            const ImVec2 grab = Pos() + ImVec2(200.0f, 200.0f);
            Drag(grab, grab + ImVec2(-5000.0f, 5000.0f));
            const ImVec2 p = Pos();
            expect(near(p.x, keep - Size().x), "left edge should stop with the strip visible, got " + v2(p));
            expect(near(p.y, 1080.0f - keep), "bottom edge should stop with the strip visible, got " + v2(p));
            expect(g_panel.cfg.x < 0.0f && g_panel.cfg.x > -0.999f,
                   "a corner off the left should save negative, got " + std::to_string(g_panel.cfg.x));
            std::printf("PASS: drag past left/bottom stops at the %.0f px strip, at %s, saved (%.3f, %.3f)\n", keep,
                        v2(p).c_str(), g_panel.cfg.x, g_panel.cfg.y);
        }

        // 5. The strip is enough to drag it back; a mid-screen spot is kept exactly.
        {
            const ImVec2 grab = Pos() + ImVec2(Size().x - keep * 0.5f, keep * 0.5f);
            Drag(grab, ImVec2(900.0f, 300.0f));
            const ImVec2 p = Pos();
            expect(p.x > 0.0f && p.x + Size().x < 1920.0f, "should come back fully on screen, got " + v2(p));
            expect(near(g_panel.cfg.x * 1920.0f, p.x) && near(g_panel.cfg.y * 1080.0f, p.y), "position should be saved");
            std::printf("PASS: grabbed back by the strip to %s\n", v2(p).c_str());
        }

        // 6. Dragged partly off the top: allowed, and it stays there (no snap back on release).
        {
            const ImVec2 grab = Pos() + ImVec2(100.0f, Size().y - 30.0f);
            Drag(grab, grab + ImVec2(0.0f, -(Pos().y + 200.0f)));
            expect(near(Pos().y, -200.0f, 2.0f), "should hang 200 px off the top, got " + v2(Pos()));
            Frame(10);
            expect(near(Pos().y, -200.0f, 2.0f), "should stay hanging off the top, got " + v2(Pos()));
            std::printf("PASS: hangs off the top at %s and stays\n", v2(Pos()).c_str());
            Drag(Pos() + ImVec2(100.0f, Size().y - 30.0f), ImVec2(1000.0f, 700.0f));
        }

        // 7. Bottom-right corner resize: recognised as a resize while held, sticks after release, saved,
        //    and the rows follow the new width.
        {
            const ImVec2 before = Size();
            const ImVec2 corner = Pos() + Size() - ImVec2(3.0f, 3.0f);
            MouseTo(corner, 2);
            Button(true);
            for (int i = 1; i <= 10; ++i)
                MouseTo(corner + ImVec2(20.0f * i, -15.0f * i));
            expect(g_panel.state.resizing, "holding the corner should register as a resize");
            Button(false);
            Frame(4);
            const ImVec2 after = Size();
            expect(near(after.x, before.x + 200.0f, 3.0f) && near(after.y, before.y - 150.0f, 3.0f),
                   "corner resize should stick: before " + v2(before) + " after " + v2(after));
            expect(g_panel.cfg.CustomSize() && near(g_panel.cfg.w * 1920.0f, after.x) && near(g_panel.cfg.h * 1080.0f, after.y),
                   "resized size should be saved as fractions");
            expect(near(g_panel.rowWidth, after.x - 36.0f, 2.0f) || near(g_panel.rowWidth, after.x - 36.0f - scrollbar, 2.0f),
                   "rows should follow the new inner width, got " + std::to_string(g_panel.rowWidth));
            std::printf("PASS: corner resize %s -> %s, saved (%.3f, %.3f), rows %.0f px\n", v2(before).c_str(),
                        v2(after).c_str(), g_panel.cfg.w, g_panel.cfg.h, g_panel.rowWidth);
        }

        // 8. Left border resize: recognised too, grows leftwards with the right edge kept.
        {
            const ImVec2 p0 = Pos(), s0 = Size();
            const ImVec2 edge = ImVec2(p0.x + 1.0f, p0.y + s0.y * 0.5f);
            MouseTo(edge, 2);
            Button(true);
            for (int i = 1; i <= 10; ++i)
                MouseTo(edge + ImVec2(-10.0f * i, 0.0f));
            expect(g_panel.state.resizing, "holding the left border should register as a resize");
            Button(false);
            Frame(4);
            expect(near(Size().x, s0.x + 100.0f, 3.0f) && near(Pos().x + Size().x, p0.x + s0.x, 3.0f),
                   "left border resize should grow leftwards: " + v2(p0) + v2(s0) + " -> " + v2(Pos()) + v2(Size()));
            expect(near(g_panel.cfg.x * 1920.0f, Pos().x, 2.0f), "the moved left edge should be saved too");
            std::printf("PASS: left border resize %s -> %s, right edge kept\n", v2(s0).c_str(), v2(Size()).c_str());
        }

        // 9. It cannot be resized below the minimum that keeps rows legible.
        {
            const ImVec2 corner = Pos() + Size() - ImVec2(3.0f, 3.0f);
            Drag(corner, corner - ImVec2(3000.0f, 3000.0f));
            expect(Size().x >= std::round(460.0f * 0.8f) + 36.0f - 1.0f && Size().y >= 159.0f,
                   "minimum size should hold, got " + v2(Size()));
            std::printf("PASS: minimum size holds at %s\n", v2(Size()).c_str());
        }

        // 10. Stored size and position come back after a reopen, and scale at another resolution.
        {
            const Settings saved = g_panel.cfg;
            const ImVec2 s0 = Size(), p0 = Pos();
            Reopen();
            expect(near(Size().x, s0.x, 2.0f) && near(Size().y, s0.y, 2.0f) && near(Pos().x, p0.x, 2.0f) &&
                       near(Pos().y, p0.y, 2.0f),
                   "reopen should restore " + v2(p0) + v2(s0) + ", got " + v2(Pos()) + v2(Size()));
            io.DisplaySize = ImVec2(2560.0f, 1440.0f);
            Frame(8);
            expect(near(Size().x, saved.w * 2560.0f, 2.0f) && near(Size().y, saved.h * 1440.0f, 2.0f),
                   "size should scale with the display, got " + v2(Size()));
            expect(near(Pos().x, saved.x * 2560.0f, 2.0f) && near(Pos().y, saved.y * 1440.0f, 2.0f),
                   "position should scale with the display, got " + v2(Pos()));
            std::printf("PASS: reopen restores it; at 2560x1440 it is %s at %s\n", v2(Size()).c_str(), v2(Pos()).c_str());
        }

        // 11. Reset layout (what the panel's button does): fits its content at the default place again.
        {
            g_panel.cfg = Settings {};
            g_panel.state.placeFrames = 2;
            Frame(8);
            expect(near(Size().x, fullWidth) && near(Pos().x, 24.0f), "reset should restore the default, got " +
                                                                           v2(Pos()) + v2(Size()));
            std::printf("PASS: reset layout restores %s at %s\n", v2(Size()).c_str(), v2(Pos()).c_str());
        }

        ImGui::DestroyContext();
        std::printf("PASS: DLSS 5 panel move/resize (headless ImGui %s)\n", IMGUI_VERSION);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}

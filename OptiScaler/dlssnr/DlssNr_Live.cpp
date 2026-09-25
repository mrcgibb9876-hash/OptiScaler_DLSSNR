// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"
#include "DlssNr_Live.h"
#include "DlssNr.h"
#include "DlssNr_Hosted.h"
#include "DlssNr_RenoDx.h"
#include "DlssNrFeature_Vk.h"
#include <Config.h>
#include <State.h>
#include <Util.h>
#include <hooks/Streamline_Hooks.h>
#include <framegen/IFGFeature_Dx12.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

namespace DlssNr::Live
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr auto kRequestCheckEvery = std::chrono::seconds(1);
constexpr auto kWriteEvery = std::chrono::milliseconds(500);
// A request older than this is from an app that has stopped asking.
constexpr unsigned long long kRequestFreshMs = 10000;

Clock::time_point g_lastRequestCheck {};
Clock::time_point g_lastWrite {};
bool g_requested = false;
bool g_failureLogged = false;

// Frame rate measured here, at Present: ImGui's Framerate only moves while the menu draws.
Clock::time_point g_windowStart {};
unsigned int g_windowFrames = 0;
double g_fps = 0.0;
double g_frameMs = 0.0;

std::filesystem::path g_dir;

unsigned long long NowUnixMs()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    // FILETIME is 100 ns ticks since 1601; Unix epoch is 11644473600 s later.
    return t.QuadPart / 10000ULL - 11644473600000ULL;
}

bool RequestIsFresh()
{
    WIN32_FILE_ATTRIBUTE_DATA data {};
    const auto req = g_dir / L"OptiScaler.live.request";
    if (!GetFileAttributesExW(req.c_str(), GetFileExInfoStandard, &data))
        return false;
    ULARGE_INTEGER t;
    t.LowPart = data.ftLastWriteTime.dwLowDateTime;
    t.HighPart = data.ftLastWriteTime.dwHighDateTime;
    const unsigned long long writtenMs = t.QuadPart / 10000ULL - 11644473600000ULL;
    const unsigned long long now = NowUnixMs();
    return now >= writtenMs ? (now - writtenMs) <= kRequestFreshMs : true; // a clock a little ahead is fine
}

void AppendNum(std::string& s, const char* key, double v, int decimals)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "\"%s\":%.*f", key, decimals, v);
    s += buf;
}

void AppendOptNum(std::string& s, const char* key, bool has, double v, int decimals)
{
    if (has)
        AppendNum(s, key, v, decimals);
    else
    {
        s += '"';
        s += key;
        s += "\":null";
    }
}

void AppendBool(std::string& s, const char* key, bool v)
{
    s += '"';
    s += key;
    s += v ? "\":true" : "\":false";
}

std::string BuildJson()
{
    Config* config = Config::Instance();
    auto& state = State::Instance();
    std::string s;
    s.reserve(640);

    s += "{\"v\":1,";
    AppendNum(s, "pid", (double) GetCurrentProcessId(), 0);
    s += ',';
    AppendNum(s, "at", (double) NowUnixMs(), 0);
    s += ',';
    AppendOptNum(s, "fps", g_fps > 0.0, g_fps, 1);
    s += ',';
    AppendOptNum(s, "frameMs", g_frameMs > 0.0, g_frameMs, 2);
    s += ',';

    uint64_t used = 0, budget = 0;
    const bool haveVram = DlssNr::VideoMemory(&used, &budget);
    const double gb = 1024.0 * 1024.0 * 1024.0;
    AppendOptNum(s, "vramUsedGb", haveVram, used / gb, 2);
    s += ',';
    AppendOptNum(s, "vramBudgetGb", haveVram, budget / gb, 2);
    s += ',';

    // nr -- the same reads the panel's status line makes.
    {
        const bool vulkan = DlssNr::IsRunningVk();
        // Not while switched off: a built model is kept a while after DLSS 5 goes off (issue #55).
        const bool running = config->DlssNrEnabled.value_or_default() && (DlssNr::IsRunning() || vulkan);
        const auto ms = vulkan ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime();
        s += "\"nr\":{";
        AppendBool(s, "enabled", config->DlssNrEnabled.value_or_default());
        s += ',';
        AppendBool(s, "running", running);
        s += ',';
        AppendOptNum(s, "modelMs", running && ms.has_value(), ms.value_or(0.0), 2);
        s += "},";
    }

    // cleanup -- Image Clean Up: the mode ([DlssNr] CleanUpMode, 0 off, 1 auto, 2 manual), the strength
    // the composition ran with last (Auto's own choice in Auto), the halo meter's two readings in stops
    // (the model's glow, and what is left of it after the clean up) and the composition pass's GPU time,
    // which the clean up runs inside. null where there is no reading.
    {
        const DlssNr::CleanUpReading clean = DlssNr::CleanUpState();
        s += "\"cleanup\":{";
        AppendNum(s, "mode", (double) config->DlssNrCleanUpMode.value_or_default(), 0);
        s += ',';
        AppendOptNum(s, "strength", clean.measuring, clean.strength, 2);
        s += ',';
        AppendOptNum(s, "haloBefore", clean.measuring && clean.haloBefore >= 0.0f, clean.haloBefore, 3);
        s += ',';
        AppendOptNum(s, "haloAfter", clean.measuring && clean.haloAfter >= 0.0f, clean.haloAfter, 3);
        s += ',';
        AppendOptNum(s, "composeMs", clean.composeMs.has_value(), clean.composeMs.value_or(0.0), 3);
        s += "},";
    }

    // renodxActive -- a RenoDX add-on with the host API is loaded: the in-game panel hides the tone trim
    // (Brightness, Contrast, their Auto) and sends it as identity, and the pop-out hides the same rows.
    AppendBool(s, "renodxActive", DlssNrRenoDx::ToneTrimSuppressed());
    s += ',';

    // tone -- what Auto brightness / Auto contrast are applying right now, so the pop-out's sliders can
    // show it the way the in-game panel does. null until a first reading lands.
    {
        const DlssNr::AutoToneReading tone = DlssNr::AutoTone();
        s += "\"tone\":{";
        AppendOptNum(s, "brightness", tone.measuring, tone.brightness, 2);
        s += ',';
        AppendOptNum(s, "contrast", tone.measuring, tone.contrast, 2);
        s += "},";
    }

    // motion -- is anything actually feeding the model? The in-game panel shows this on Inspect >
    // Guide; sending it out here is what lets the pop-out show the same row rather than inferring
    // from the deployed files, which is all it can otherwise see.
    //
    // It is the one reading in this file that reports a FAULT rather than a number, and the fault
    // is invisible everywhere else: with no vectors the model is handed a zero-motion texture and
    // runs, so "running" above is true, modelMs is a real number, and the picture still smears.
    {
        const DlssNr::MotionReading motion = DlssNr::MotionState();
        s += "\"motion\":{";
        AppendNum(s, "evaluates", (double) motion.evaluates, 0);
        s += ',';
        AppendNum(s, "blind", (double) motion.blindEvaluates, 0);
        s += ',';
        AppendBool(s, "lastBlind", motion.lastBlind);
        s += ',';
        // On the Present route the vectors are this engine's own optical flow and no ReShade
        // provider is involved, so the manager must not offer to swap one.
        AppendBool(s, "usingFlow", motion.usingFlow);
        s += "},";
    }

    // fg -- what the Frame Generation section decides between.
    {
        auto* fg = state.currentFG;
        const bool optiDlssg = state.activeFgOutput == FGOutput::DLSSG && fg != nullptr;
        const bool gameDlssg = !optiDlssg && state.activeFgInput != FGInput::DLSSG && StreamlineHooks::isDlssgHooked();
        const int live = state.dlssgDetectedInterpolationCount;
        s += "\"fg\":{";
        AppendBool(s, "gameDlssg", gameDlssg);
        s += ',';
        AppendOptNum(s, "liveMultiplier", gameDlssg && live > 0, (double) (live + 1), 0);
        s += ',';
        AppendBool(s, "gameDmfgSupported", state.dlssgGameDMFGSupported);
        s += ',';
        AppendBool(s, "optiDlssg", optiDlssg);
        s += ',';
        AppendBool(s, "optiFgEnabled", config->FGEnabled.value_or_default());
        s += ',';
        if (optiDlssg)
            AppendNum(s, "maxCount", (double) fg->GetMaxInterpolationCount(), 0);
        else if (gameDlssg && state.dlssgMfgMax.has_value())
            AppendNum(s, "maxCount", (double) state.dlssgMfgMax.value(), 0);
        else
            s += "\"maxCount\":null";
        s += ',';
        AppendBool(s, "dmfgSupported", optiDlssg && fg->GetDMFGSupport());
        s += "}}";
    }
    return s;
}

void Write()
{
    const std::string json = BuildJson();
    const auto tmp = g_dir / L"OptiScaler.live.json.tmp";
    const auto dest = g_dir / L"OptiScaler.live.json";

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

    if (!ok && !g_failureLogged)
    {
        g_failureLogged = true;
        LOG_WARN("DLSS-NR live readings: could not write {} (error {}); the pop-out panel will not see live "
                 "numbers this session",
                 dest.string(), GetLastError());
    }
}
} // namespace

void Tick()
{
    const auto now = Clock::now();

    // The frame rate is counted on every frame, requested or not, so the first write has a real number.
    if (g_windowFrames == 0)
        g_windowStart = now;
    g_windowFrames++;
    const double windowMs = std::chrono::duration<double, std::milli>(now - g_windowStart).count();
    if (windowMs >= 500.0)
    {
        const double frames = (double) (g_windowFrames - 1);
        if (frames > 0)
        {
            g_frameMs = windowMs / frames;
            g_fps = 1000.0 / g_frameMs;
        }
        g_windowFrames = 1;
        g_windowStart = now;
    }

    if (now - g_lastRequestCheck >= kRequestCheckEvery)
    {
        g_lastRequestCheck = now;
        if (g_dir.empty())
            g_dir = Util::DllPath().parent_path();
        const bool was = g_requested;
        g_requested = RequestIsFresh();
        if (g_requested != was)
            LOG_INFO("DLSS-NR live readings for the pop-out panel: {}", g_requested ? "on" : "off");
    }

    // Pacing and HDR for the pop-out ride the same request, on this same thread -- the one both add-ons'
    // host APIs ask to be called from. Before the early return below, which is about live.json only.
    DlssNr::Hosted::Tick(g_dir, g_requested);

    if (!g_requested || now - g_lastWrite < kWriteEvery)
        return;
    g_lastWrite = now;
    Write();
}
} // namespace DlssNr::Live

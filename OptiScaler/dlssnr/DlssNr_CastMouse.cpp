// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"

#include "DlssNr_CastMouse.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace DlssNr::CastMouse
{
namespace
{
// The Feeder's close button sits in the cast's top-right corner, kCastCloseSize (28) px times the cast scale
// when that is above 1 (dlss5-feed32.cpp CastCloseRect). Clicks there are the Feeder's.
constexpr int kCloseSize = 28;
constexpr ULONGLONG kReadEveryMs = 100;
// Enough for any run's log; a larger jump is skipped to its tail rather than read whole.
constexpr LONGLONG kMaxRead = 256 * 1024;

struct State
{
    bool init = false;
    bool enabled = false;
    std::wstring logPath;
    std::wstring cfgPath;
    DWORD gamePid = 0;
    LONGLONG offset = 0;
    std::string partial;
    bool shown = false;
    int dstW = 0;
    int dstH = 0;
    float scale = 1.0f;
    int anchor = 1; // the Feeder's default: top-right
    ULONGLONG lastRead = 0;
    HWND game = nullptr;
    bool loggedMapping = false;
};

State s;

void Init()
{
    s.init = true;

    wchar_t exe[MAX_PATH] {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    auto cut = dir.find_last_of(L"\\/");
    if (cut == std::wstring::npos)
        return;
    dir.resize(cut); // ...\host64
    cut = dir.find_last_of(L"\\/");
    if (cut == std::wstring::npos)
        return;
    const std::wstring gameDir = dir.substr(0, cut);
    s.logPath = gameDir + L"\\dlss5-feed.log";
    s.cfgPath = gameDir + L"\\dlss5-feed.cfg";

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr)
    {
        if (argc >= 2)
            s.gamePid = wcstoul(argv[1], nullptr, 10);
        LocalFree(argv);
    }

    s.enabled = s.gamePid != 0;
    LOG_INFO("DLSS 5 panel cast mouse: {} (game pid {}, following dlss5-feed.log for the cast)",
             s.enabled ? "on" : "off -- no game pid on the helper's command line", s.gamePid);
}

int ReadAnchor()
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, s.cfgPath.c_str(), L"r") != 0 || f == nullptr)
        return 1;
    char line[256];
    int anchor = 1;
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        int v = 0;
        if (sscanf_s(line, " cast_anchor = %d", &v) == 1 || sscanf_s(line, " cast_anchor=%d", &v) == 1)
            anchor = std::clamp(v, 0, 3);
    }
    fclose(f);
    return anchor;
}

void ProcessLine(const std::string& line)
{
    if (line.find("[feed32] cast: shown") != std::string::npos)
    {
        s.shown = true;
        s.anchor = ReadAnchor();
        return;
    }
    if (line.find("[feed32] cast: hidden") != std::string::npos)
    {
        s.shown = false;
        return;
    }
    // "[feed32] cast: 804x1472 of the host window shown at 804x1472 (scale 1.00)", or "... of the panel texture ..."
    if (line.find("[feed32] cast: ") == std::string::npos)
        return;
    const auto at = line.find(" shown at ");
    if (at == std::string::npos)
        return;
    int w = 0, h = 0;
    float scale = 0.0f;
    if (sscanf_s(line.c_str() + at + 10, "%dx%d (scale %f)", &w, &h, &scale) == 3 && w > 0 && h > 0 && scale > 0.0f)
    {
        s.dstW = w;
        s.dstH = h;
        s.scale = scale;
    }
}

void ReadLog()
{
    HANDLE h = CreateFileW(s.logPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;

    LARGE_INTEGER size {};
    if (GetFileSizeEx(h, &size))
    {
        // A new game session starts the log afresh: everything known belongs to the old one.
        if (size.QuadPart < s.offset)
        {
            s.offset = 0;
            s.partial.clear();
            s.shown = false;
        }
        if (size.QuadPart - s.offset > kMaxRead)
        {
            s.offset = size.QuadPart - kMaxRead;
            s.partial.clear();
        }
        if (size.QuadPart > s.offset)
        {
            LARGE_INTEGER pos {};
            pos.QuadPart = s.offset;
            if (SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
            {
                std::string buf(static_cast<size_t>(size.QuadPart - s.offset), '\0');
                DWORD got = 0;
                if (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0)
                {
                    s.offset += got;
                    s.partial.append(buf.data(), got);
                    size_t start = 0;
                    for (size_t nl; (nl = s.partial.find('\n', start)) != std::string::npos; start = nl + 1)
                        ProcessLine(s.partial.substr(start, nl - start));
                    s.partial.erase(0, start);
                }
            }
        }
    }
    CloseHandle(h);
}

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM param)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != s.gamePid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;
    RECT rc {};
    GetClientRect(hwnd, &rc);
    auto* best = reinterpret_cast<std::pair<HWND, LONGLONG>*>(param);
    const LONGLONG area = static_cast<LONGLONG>(rc.right) * rc.bottom;
    if (area > best->second)
        *best = { hwnd, area };
    return TRUE;
}

void Refresh()
{
    if (!s.init)
        Init();
    if (!s.enabled)
        return;
    const ULONGLONG now = GetTickCount64();
    if (now - s.lastRead >= kReadEveryMs)
    {
        s.lastRead = now;
        ReadLog();
    }
}
} // namespace

bool CastShown()
{
    Refresh();
    return s.enabled && s.shown;
}

bool MapToPanel(const POINT& screen, POINT& client)
{
    if (!CastShown() || s.dstW <= 0 || s.dstH <= 0 || s.scale <= 0.0f)
        return false;

    if (s.game == nullptr || !IsWindow(s.game))
    {
        std::pair<HWND, LONGLONG> best { nullptr, 0 };
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&best));
        s.game = best.first;
        if (s.game == nullptr)
            return false;
    }

    // The same layout the Feeder computes in the game's client area (dlss5-feed32.cpp CastShow).
    RECT gc {};
    if (!GetClientRect(s.game, &gc))
        return false;
    const bool right = s.anchor == 1 || s.anchor == 3;
    const bool bottom = s.anchor >= 2;
    POINT origin { right ? gc.right - s.dstW : 0, bottom ? gc.bottom - s.dstH : 0 };
    if (!ClientToScreen(s.game, &origin))
        return false;

    const LONG rx = screen.x - origin.x;
    const LONG ry = screen.y - origin.y;
    if (rx < 0 || ry < 0 || rx >= s.dstW || ry >= s.dstH)
        return false;
    const int close = static_cast<int>(kCloseSize * (s.scale > 1.0f ? s.scale : 1.0f));
    if (rx >= s.dstW - close && ry < close)
        return false;

    client.x = static_cast<LONG>(rx / s.scale);
    client.y = static_cast<LONG>(ry / s.scale);

    if (!s.loggedMapping)
    {
        s.loggedMapping = true;
        LOG_INFO("DLSS 5 panel cast mouse: reading the real cursor over the cast ({}x{} at scale {:.2f}, anchor {}, "
                 "game window {:X})",
                 s.dstW, s.dstH, s.scale, s.anchor, (size_t) s.game);
    }
    return true;
}
} // namespace DlssNr::CastMouse

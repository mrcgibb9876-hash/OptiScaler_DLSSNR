#include <pch.h>
#include "LosslessScaling.h"

#include <tlhelp32.h>
#include <filesystem>
#include <thread>
#include <chrono>

static DWORD FindProcessId(const wchar_t* exeName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    DWORD pid = 0;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, exeName) == 0)
            {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pid;
}

bool LosslessScaling::IsRunning() { return FindProcessId(L"LosslessScaling.exe") != 0; }

bool LosslessScaling::Launch(const std::wstring& exePath)
{
    if (exePath.empty() || !std::filesystem::exists(exePath))
        return false;

    if (IsRunning())
        return true; // Nothing to do -- already on.

    std::filesystem::path path(exePath);
    STARTUPINFOW si {};
    si.cb = sizeof(si);
    // Its own window would otherwise open directly over the game -- start minimized so AutoScale
    // (set in its per-game profile by OptiDLSS5-UI) gets a chance to pick up the game's window on
    // its own instead. Still reachable from the taskbar if the user needs to open it.
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi {};

    // CreateProcessW may write into its lpCommandLine argument, so it needs a mutable buffer even
    // though we pass nothing on the command line -- lpApplicationName alone is enough here.
    std::wstring mutableCmd = L"\"" + exePath + L"\"";

    BOOL ok = CreateProcessW(exePath.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             path.parent_path().wstring().c_str(), &si, &pi);

    if (ok)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    return ok != 0;
}

namespace
{
struct CloseContext
{
    DWORD pid;
    bool closedAny;
};

BOOL CALLBACK CloseWindowsForPid(HWND hwnd, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<CloseContext*>(lParam);

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);

    if (windowPid == ctx->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr)
    {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        ctx->closedAny = true;
    }

    return TRUE; // keep enumerating -- a WPF app can have more than one top-level window
}
} // namespace

bool LosslessScaling::Close()
{
    DWORD pid = FindProcessId(L"LosslessScaling.exe");
    if (pid == 0)
        return false;

    CloseContext ctx { pid, false };
    EnumWindows(CloseWindowsForPid, reinterpret_cast<LPARAM>(&ctx));

    // WM_CLOSE alone isn't enough -- confirmed live (Batman: Arkham Knight, 2026-09-10): it closes
    // the window (a real, visible effect -- MainWindowHandle goes to 0), but Lossless Scaling keeps
    // running in the background regardless (same PID, same CPU/memory use, no MinimizeToTray or
    // CloseToTray setting needed for this -- it does it either way), so Frame Generation never
    // actually stops. The checkbox has to guarantee "off" means off. Give it a moment to exit
    // cleanly on its own first (WM_CLOSE did close its window, so something is happening), then
    // force it if it's still there. Runs on its own thread, detached, so a slow/stuck Lossless
    // Scaling can't stall the game's own render thread while this waits.
    std::thread([pid]()
    {
        for (int i = 0; i < 20; i++) // up to ~2s in 100ms steps
        {
            HANDLE probe = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (probe == nullptr)
                return; // already gone
            DWORD waitResult = WaitForSingleObject(probe, 0);
            CloseHandle(probe);
            if (waitResult == WAIT_OBJECT_0)
                return; // exited on its own
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (proc != nullptr)
        {
            TerminateProcess(proc, 0);
            CloseHandle(proc);
        }
    }).detach();

    return true;
}

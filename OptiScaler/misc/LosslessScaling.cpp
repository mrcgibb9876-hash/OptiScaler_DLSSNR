#include <pch.h>
#include "LosslessScaling.h"

#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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
    // Minimized, and Lossless Scaling's own -StartMinimized arg (App.cs reads it) makes it go
    // straight to the tray rather than showing its window at all -- nothing pops over the game.
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi {};

    // CreateProcessW may write into lpCommandLine, so it needs a mutable buffer.
    std::wstring mutableCmd = L"\"" + exePath + L"\" -StartMinimized";

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

    if (windowPid == ctx->pid && GetWindow(hwnd, GW_OWNER) == nullptr)
    {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        ctx->closedAny = true;
    }

    return TRUE; // keep enumerating -- a WPF app can have more than one top-level window
}

// Waits (up to ~timeoutMs) for a Lossless Scaling process to be present. Lossless Scaling launched
// non-elevated relaunches itself elevated and the first process exits, so the PID we can observe
// changes shortly after launch -- poll for "any LosslessScaling.exe", not a specific PID.
bool WaitForRunning(int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 100)
    {
        if (LosslessScaling::IsRunning())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return LosslessScaling::IsRunning();
}

// One tap of a key (down or up) via SendInput.
void SendKey(WORD vk, bool up)
{
    INPUT in {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(INPUT));
}

// Synthesise the Lossless Scaling toggle chord (default Ctrl+Alt+S) with a real gap between each
// event: its low-level-hook handler samples the modifier keys with Keyboard.IsKeyDown at the
// instant it sees the base key, so a zero-gap burst is missed (confirmed live). ~60ms is
// comfortably enough and imperceptible.
void SendToggleChord(int mods, int vk)
{
    constexpr auto gap = std::chrono::milliseconds(60);
    auto tap = [&](WORD k, bool up)
    {
        SendKey(k, up);
        std::this_thread::sleep_for(gap);
    };

    if (mods & LosslessScaling::HK_CTRL)
        tap(VK_CONTROL, false);
    if (mods & LosslessScaling::HK_ALT)
        tap(VK_MENU, false);
    if (mods & LosslessScaling::HK_SHIFT)
        tap(VK_SHIFT, false);
    if (mods & LosslessScaling::HK_WIN)
        tap(VK_LWIN, false);

    tap(static_cast<WORD>(vk), false);
    tap(static_cast<WORD>(vk), true);

    // Release in reverse order.
    if (mods & LosslessScaling::HK_WIN)
        tap(VK_LWIN, true);
    if (mods & LosslessScaling::HK_SHIFT)
        tap(VK_SHIFT, true);
    if (mods & LosslessScaling::HK_ALT)
        tap(VK_MENU, true);
    if (mods & LosslessScaling::HK_CTRL)
        SendKey(VK_CONTROL, true);
}

std::filesystem::path SettingsXmlPath()
{
    // %LOCALAPPDATA%\Lossless Scaling\Settings.xml -- the env var avoids a shell32/shlobj link.
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    return std::filesystem::path(buf) / L"Lossless Scaling" / L"Settings.xml";
}

// Replaces the text between <tag>...</tag> with newInner, but only within [from, to) of content.
// Returns true if the tag was found and rewritten in that range. Leaves everything else byte-for-
// byte intact -- no structural change, no re-serialisation, no XML declaration touched.
bool ReplaceTagInRange(std::string& content, size_t from, size_t to, const std::string& tag,
                       const std::string& newInner)
{
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    size_t o = content.find(open, from);
    if (o == std::string::npos || o >= to)
        return false;
    size_t innerStart = o + open.size();
    size_t c = content.find(close, innerStart);
    if (c == std::string::npos || c >= to)
        return false;
    content.replace(innerStart, c - innerStart, newInner);
    return true;
}

// Minimal, backed-up, block-scoped edit of Settings.xml: find this game's <Profile> (by its exact
// <Title>) and set its <LSFG3Multiplier> and force <LSFG3Mode1>FIXED</LSFG3Mode1>. Validated
// against the real live Settings.xml before shipping (Node harness, 2026-09-11). Returns true on a
// successful write.
bool PatchMultiplierInSettings(const std::wstring& gameTitle, int multiplier)
{
    if (gameTitle.empty() || multiplier < 1)
        return false;

    std::filesystem::path xml = SettingsXmlPath();
    if (xml.empty() || !std::filesystem::exists(xml))
        return false;

    std::string content;
    {
        std::ifstream in(xml, std::ios::binary);
        if (!in)
            return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        content = ss.str();
    }
    if (content.empty())
        return false;

    // Settings.xml is UTF-8; wstring_to_string (SysUtils.h) emits UTF-8, so the title bytes match.
    const std::string titleTag = "<Title>" + wstring_to_string(gameTitle) + "</Title>";
    size_t ti = content.find(titleTag);
    if (ti == std::string::npos)
        return false;

    size_t blockStart = content.rfind("<Profile>", ti);
    size_t blockEnd = content.find("</Profile>", ti);
    if (blockStart == std::string::npos || blockEnd == std::string::npos)
        return false;

    bool changed = ReplaceTagInRange(content, blockStart, blockEnd, "LSFG3Multiplier",
                                     std::to_string(multiplier));
    // Best-effort; a profile OptiDLSS5-UI wrote always has this, but don't fail the write if not.
    ReplaceTagInRange(content, blockStart, blockEnd, "LSFG3Mode1", "FIXED");
    if (!changed)
        return false;

    // Recoverable copy before overwriting -- this file holds the user's other LS game profiles too.
    std::error_code ec;
    std::filesystem::copy_file(xml, std::filesystem::path(xml).concat(L".bak-lsfork"),
                               std::filesystem::copy_options::overwrite_existing, ec);

    std::ofstream out(xml, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}
} // namespace

bool LosslessScaling::Close()
{
    DWORD pid = FindProcessId(L"LosslessScaling.exe");
    if (pid == 0)
        return false;

    CloseContext ctx { pid, false };
    EnumWindows(CloseWindowsForPid, reinterpret_cast<LPARAM>(&ctx));

    // WM_CLOSE alone leaves Lossless Scaling running in the background (its CloseToTray), so
    // guarantee it is gone. Detached so a slow exit can't stall the caller. TerminateProcess can be
    // denied if Lossless Scaling is higher integrity than us -- that path is why OptiDLSS5-UI sets
    // <StartAsAdmin>false.
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

void LosslessScaling::ActivateAsync(const std::wstring& exePath, int mods, int vk)
{
    std::thread([exePath, mods, vk]()
    {
        bool wasRunning = IsRunning();
        if (!wasRunning)
        {
            if (!Launch(exePath))
                return;
            // Wait for the process, then give its keyboard hook time to install (it is set up in
            // Lossless Scaling's MainWindow constructor, after settings load).
            if (!WaitForRunning(6000))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
        SendToggleChord(mods, vk);
    }).detach();
}

void LosslessScaling::DeactivateAsync(int mods, int vk)
{
    std::thread([mods, vk]()
    {
        if (!IsRunning())
            return;
        SendToggleChord(mods, vk);
    }).detach();
}

void LosslessScaling::SetMultiplierAsync(const std::wstring& exePath, const std::wstring& gameTitle, int multiplier,
                                         int mods, int vk, bool wasActive)
{
    std::thread([exePath, gameTitle, multiplier, mods, vk, wasActive]()
    {
        if (!PatchMultiplierInSettings(gameTitle, multiplier))
            return; // Couldn't write it -- nothing to restart for.

        if (!IsRunning())
            return; // Not running: the new value is read on next launch. Done.

        // Running: it only reads the profile at startup, so restart to pick up the new multiplier.
        Close();
        // Wait for it to actually exit before relaunching.
        for (int i = 0; i < 40 && IsRunning(); i++) // up to ~4s
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!Launch(exePath))
            return;
        if (!WaitForRunning(6000))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        // Re-scale only if it was scaling before the multiplier change.
        if (wasActive)
            SendToggleChord(mods, vk);
    }).detach();
}

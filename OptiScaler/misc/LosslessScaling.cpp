#include <pch.h>
#include "LosslessScaling.h"

#include <tlhelp32.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <wrl/client.h>
#include <UIAutomationClient.h>

using Microsoft::WRL::ComPtr;

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
    // Shows normally (no show-state override) rather than minimized: confirmed live that UI
    // Automation driving its Scale button only takes real effect once the window has actually been
    // shown at least once since launch -- see LosslessScaling.h for the full story. TriggerScale/
    // SetMultiplier minimize it again once they're done with it, so this first show is brief.
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

namespace
{
// EnumWindows finds a window by title regardless of show state (confirmed live it still finds a
// minimized or even SW_HIDE'd window) -- AutomationElement/IUIAutomation's own top-level traversal
// (RootElement's Children, or Process.MainWindowHandle on the .NET side) does not reliably see a
// non-normal-state window, but wrapping an HWND found this way with ElementFromHandle works fine
// regardless of its show state. Confirmed live, 2026-09-10.
HWND FindLosslessWindow(DWORD pid)
{
    struct Ctx
    {
        DWORD pid;
        HWND result;
    } ctx { pid, nullptr };

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL
        {
            auto* ctx = reinterpret_cast<Ctx*>(lParam);
            DWORD windowPid = 0;
            GetWindowThreadProcessId(hwnd, &windowPid);
            if (windowPid == ctx->pid)
            {
                wchar_t title[256] = {};
                GetWindowTextW(hwnd, title, 256);
                if (wcscmp(title, L"Lossless Scaling") == 0)
                {
                    ctx->result = hwnd;
                    return FALSE;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    return ctx.result;
}

ComPtr<IUIAutomationElement> FindByAutomationId(IUIAutomation* automation, IUIAutomationElement* root, const wchar_t* id)
{
    VARIANT v {};
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(id);
    ComPtr<IUIAutomationCondition> cond;
    automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &cond);
    VariantClear(&v);

    ComPtr<IUIAutomationElement> found;
    if (cond)
        root->FindFirst(TreeScope_Descendants, cond.Get(), &found);
    return found;
}

// Selects the ProfileList entry whose own descendant text exactly matches gameTitle (the list
// items themselves carry no AutomationId, only their child Text elements show the profile's real
// title -- confirmed against the real control tree, 2026-09-10). Returns true if a match was
// found and selected.
bool SelectProfileByTitle(IUIAutomation* automation, IUIAutomationElement* window, const std::wstring& gameTitle)
{
    auto profileList = FindByAutomationId(automation, window, L"ProfileList");
    if (!profileList)
        return false;

    ComPtr<IUIAutomationCondition> trueCond;
    automation->CreateTrueCondition(&trueCond);

    ComPtr<IUIAutomationElementArray> items;
    profileList->FindAll(TreeScope_Children, trueCond.Get(), &items);
    if (!items)
        return false;

    int count = 0;
    items->get_Length(&count);

    for (int i = 0; i < count; i++)
    {
        ComPtr<IUIAutomationElement> item;
        items->GetElement(i, &item);
        if (!item)
            continue;

        ComPtr<IUIAutomationElementArray> texts;
        item->FindAll(TreeScope_Descendants, trueCond.Get(), &texts);
        if (!texts)
            continue;

        int textCount = 0;
        texts->get_Length(&textCount);
        bool matched = false;
        for (int j = 0; j < textCount && !matched; j++)
        {
            ComPtr<IUIAutomationElement> textEl;
            texts->GetElement(j, &textEl);
            if (!textEl)
                continue;
            BSTR name = nullptr;
            textEl->get_CurrentName(&name);
            if (name != nullptr)
            {
                if (gameTitle == name)
                    matched = true;
                SysFreeString(name);
            }
        }

        if (matched)
        {
            ComPtr<IUIAutomationSelectionItemPattern> selectPattern;
            if (SUCCEEDED(item->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selectPattern))) && selectPattern)
            {
                selectPattern->Select();
                return true;
            }
        }
    }

    return false;
}

// Runs the whole "briefly show -> select profile -> (optionally set multiplier) -> (optionally
// click Scale) -> minimize again" sequence on its own thread with a fresh COM apartment, so this
// never touches whatever COM state the game itself may already have on its own threads, and never
// blocks the caller (the game's own render thread, if called from the in-game panel). Runs both
// actions in one pass when both are requested, rather than two separate show/hide cycles.
void RunAutomationAsync(const std::wstring& gameTitle, bool clickScale, int multiplierOrZero)
{
    std::thread(
        [gameTitle, clickScale, multiplierOrZero]()
        {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
                return;

            {
                // Restored at the end -- SetForegroundWindow below genuinely takes focus from the
                // game for the duration of this sequence, unlike the earlier SW_SHOWNOACTIVATE
                // attempt, which didn't.
                HWND previousForeground = GetForegroundWindow();

                DWORD pid = FindProcessId(L"LosslessScaling.exe");
                HWND hwnd = pid ? FindLosslessWindow(pid) : nullptr;
                if (hwnd != nullptr)
                {
                    // SW_SHOWNOACTIVATE alone wasn't enough -- confirmed live that a click driven
                    // through UI Automation while the window was only ever non-activated silently
                    // did nothing, and worked as soon as the window got a real activation (the user
                    // cycling the Launch/Close checkbox, whose Launch() uses a normal activating
                    // show, fixed it). SetForegroundWindow from an unrelated external process was
                    // already confirmed blocked by Windows' foreground-lock protection -- but this
                    // code runs inside the game's own process, which already holds the foreground,
                    // so the "calling process is the foreground process" exception to that
                    // protection should apply here even though it didn't in that external test.
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    // Give WPF a moment to actually render/activate at least one frame -- confirmed
                    // live that acting on it too soon after showing behaves the same as never shown.
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));

                    ComPtr<IUIAutomation> automation;
                    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) &&
                        automation)
                    {
                        ComPtr<IUIAutomationElement> window;
                        if (SUCCEEDED(automation->ElementFromHandle(hwnd, &window)) && window)
                        {
                            if (SelectProfileByTitle(automation.Get(), window.Get(), gameTitle))
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                                if (multiplierOrZero > 0)
                                {
                                    auto multEl = FindByAutomationId(automation.Get(), window.Get(), L"LSFG3Multiplier");
                                    if (multEl)
                                    {
                                        ComPtr<IUIAutomationValuePattern> valuePattern;
                                        if (SUCCEEDED(multEl->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&valuePattern))) &&
                                            valuePattern)
                                        {
                                            BSTR val = SysAllocString(std::to_wstring(multiplierOrZero).c_str());
                                            valuePattern->SetValue(val);
                                            SysFreeString(val);
                                        }
                                    }
                                }

                                if (clickScale)
                                {
                                    auto scaleBtn = FindByAutomationId(automation.Get(), window.Get(), L"ScaleButton");
                                    if (scaleBtn)
                                    {
                                        ComPtr<IUIAutomationInvokePattern> invokePattern;
                                        if (SUCCEEDED(scaleBtn->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invokePattern))) &&
                                            invokePattern)
                                        {
                                            invokePattern->Invoke();
                                        }
                                    }
                                }
                            }
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    ShowWindow(hwnd, SW_MINIMIZE);
                }

                if (previousForeground != nullptr && previousForeground != hwnd)
                    SetForegroundWindow(previousForeground);
            }

            CoUninitialize();
        })
        .detach();
}
} // namespace

void LosslessScaling::TriggerScaleAsync(const std::wstring& gameTitle) { RunAutomationAsync(gameTitle, true, 0); }

void LosslessScaling::SetMultiplierAsync(const std::wstring& gameTitle, int multiplier)
{
    RunAutomationAsync(gameTitle, false, multiplier);
}

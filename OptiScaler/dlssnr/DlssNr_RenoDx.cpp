// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"

#include "DlssNr_RenoDx.h"

#include <string>
#include <vector>

namespace DlssNrRenoDx
{
namespace
{
// Retried, not resolved once -- the same reason as DlssNr_ReLimiter: the panel's first frame can come
// before ReShade has loaded its add-ons, and a miss then used to hide the HDR page for the session.
// A miss is asked again every couple of seconds; a found add-on, or one that exports the API but was
// refused, is final (the refusal cannot change for this process, and it keeps the warning to one line).
const RenoDxHostApi* s_api = nullptr;
bool s_givenUp = false;
ULONGLONG s_lastTry = 0;
constexpr ULONGLONG kRetryEveryMs = 2000;
std::string s_module;

// K32EnumProcessModules rather than EnumProcessModules: it lives in kernel32 on every Windows this
// engine runs on, so resolving it by name costs one GetProcAddress and adds no link dependency.
// Adding Psapi.lib would mean editing four AdditionalDependencies lines in the vcxproj to gain
// nothing. If it is somehow absent, Available() is false and the page simply does not appear.
using EnumProcessModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);

// The module's file name without its directory. GetModuleFileNameW is kernel32, so this needs no
// psapi either.
std::string BaseNameOf(HMODULE mod)
{
    wchar_t path[MAX_PATH] {};
    DWORD n = GetModuleFileNameW(mod, path, (DWORD) MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    std::wstring wide(path, n);
    size_t slash = wide.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        wide.erase(0, slash + 1);
    std::string out;
    out.reserve(wide.size());
    // These names are ASCII by construction (renodx-*.addon64), so a narrowing copy is honest here
    // and avoids dragging a conversion helper in for a log line.
    for (wchar_t c : wide)
        out.push_back(c < 128 ? (char) c : '?');
    return out;
}

void Resolve()
{
    if (s_api != nullptr || s_givenUp)
        return;
    const ULONGLONG now = GetTickCount64();
    if (s_lastTry != 0 && now - s_lastTry < kRetryEveryMs)
        return;
    s_lastTry = now;

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enumModules = k32 ? (EnumProcessModulesFn) GetProcAddress(k32, "K32EnumProcessModules") : nullptr;
    if (enumModules == nullptr)
    {
        s_givenUp = true; // no way to look, now or later
        return;
    }

    // Asked for the count first, because a process with an add-on loaded has well over a hundred
    // modules and a fixed array would be the kind of guess that works until it does not.
    DWORD needed = 0;
    if (!enumModules(GetCurrentProcess(), nullptr, 0, &needed) || needed == 0)
        return;
    std::vector<HMODULE> mods(needed / sizeof(HMODULE));
    if (!enumModules(GetCurrentProcess(), mods.data(), (DWORD) (mods.size() * sizeof(HMODULE)), &needed))
        return;
    mods.resize(needed / sizeof(HMODULE));

    // Every module is asked, not just the ones whose name looks like RenoDX's. A name filter would be
    // a second place that knows how these files are called, and it would miss a renamed copy for no
    // gain: GetProcAddress for a name a module does not export is an export-table lookup that fails,
    // done once at startup.
    for (HMODULE mod : mods)
    {
        auto get = (RenoDxGetHostApiFn) GetProcAddress(mod, "RenoDxGetHostApi");
        if (get == nullptr)
            continue;

        // Exports the API: whatever happens next, this module's answer is final.
        s_givenUp = true;

        const RenoDxHostApi* api = get(RENODX_HOST_API_VERSION);
        if (api == nullptr)
        {
            LOG_WARN("DLSS-NR: {} does not speak RenoDX host API version {} -- not driving it from the panel",
                     BaseNameOf(mod), RENODX_HOST_API_VERSION);
            continue;
        }
        // A newer add-on may return a LARGER struct, which is fine -- we read the prefix we know.
        // Smaller would mean reading past its end, so it is refused rather than trusted.
        if (api->struct_size < sizeof(RenoDxHostApi))
        {
            LOG_WARN("DLSS-NR: {}'s host API struct is {} bytes, smaller than the {} this build expects "
                     "-- not driving it from the panel",
                     BaseNameOf(mod), api->struct_size, (unsigned) sizeof(RenoDxHostApi));
            continue;
        }

        s_api = api;
        s_module = BaseNameOf(mod);
        LOG_INFO("DLSS-NR: RenoDX found in {} (addon \"{}\"), host API v{} ({} settings)", s_module,
                 api->addon_name ? api->addon_name() : "?", api->api_version,
                 api->setting_count ? api->setting_count() : 0);
        return;
    }
    // No RenoDX (yet -- asked again in a couple of seconds unless one was refused above), or one without
    // the export. The ordinary case: every shipped build is the latter.
}
} // namespace

bool Available()
{
    Resolve();
    return s_api != nullptr;
}

const RenoDxHostApi* Api()
{
    Resolve();
    return s_api;
}

const char* AddonName()
{
    Resolve();
    return s_api && s_api->addon_name ? s_api->addon_name() : nullptr;
}

const char* ModuleName()
{
    Resolve();
    return s_module.empty() ? nullptr : s_module.c_str();
}
} // namespace DlssNrRenoDx

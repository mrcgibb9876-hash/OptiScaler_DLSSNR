// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"

#include "DlssNr_RenoDx.h"

#include <cstring>
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
// Why s_api is null, as the stable code UnavailableReason() hands out.
const char* s_reason = "not-loaded";

// K32EnumProcessModules rather than EnumProcessModules: it lives in kernel32 on every Windows this
// engine runs on, so resolving it by name costs one GetProcAddress and adds no link dependency.
// Adding Psapi.lib would mean editing four AdditionalDependencies lines in the vcxproj to gain
// nothing. If it is somehow absent, Available() is false and the page says RenoDX is not there.
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

EnumProcessModulesFn EnumModulesFn()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    return k32 ? (EnumProcessModulesFn) GetProcAddress(k32, "K32EnumProcessModules") : nullptr;
}

// Every module in the process, or an empty list if they could not be listed this time.
std::vector<HMODULE> LoadedModules(EnumProcessModulesFn enumModules)
{
    // Asked for the count first, because a process with an add-on loaded has well over a hundred
    // modules and a fixed array would be the kind of guess that works until it does not.
    DWORD needed = 0;
    if (!enumModules(GetCurrentProcess(), nullptr, 0, &needed) || needed == 0)
        return {};
    std::vector<HMODULE> mods(needed / sizeof(HMODULE));
    if (!enumModules(GetCurrentProcess(), mods.data(), (DWORD) (mods.size() * sizeof(HMODULE)), &needed))
        return {};
    mods.resize(needed / sizeof(HMODULE));
    return mods;
}

void Resolve()
{
    if (s_api != nullptr || s_givenUp)
        return;
    const ULONGLONG now = GetTickCount64();
    if (s_lastTry != 0 && now - s_lastTry < kRetryEveryMs)
        return;
    s_lastTry = now;

    auto enumModules = EnumModulesFn();
    if (enumModules == nullptr)
    {
        s_givenUp = true; // no way to look, now or later
        return;
    }

    std::vector<HMODULE> mods = LoadedModules(enumModules);
    if (mods.empty())
        return;

    // Every module is asked, not just the ones whose name looks like RenoDX's. A name filter would be
    // a second place that knows how these files are called, and it would miss a renamed copy for no
    // gain: GetProcAddress for a name a module does not export is an export-table lookup that fails,
    // done once at startup.
    bool sawByName = false;
    for (HMODULE mod : mods)
    {
        auto get = (RenoDxGetHostApiFn) GetProcAddress(mod, "RenoDxGetHostApi");
        if (get == nullptr)
        {
            // Only for the page's explanation, never for finding it: a RenoDX add-on WITHOUT the export
            // (every upstream build today) is "installed but cannot be driven from here", which is a
            // different thing to tell a player from "not installed".
            if (!sawByName && _strnicmp(BaseNameOf(mod).c_str(), "renodx", 6) == 0)
                sawByName = true;
            continue;
        }

        // Exports the API: whatever happens next, this module's answer is final.
        s_givenUp = true;

        const RenoDxHostApi* api = get(RENODX_HOST_API_VERSION);
        if (api == nullptr)
        {
            LOG_WARN("DLSS-NR: {} does not speak RenoDX host API version {} -- not driving it from the panel",
                     BaseNameOf(mod), RENODX_HOST_API_VERSION);
            s_reason = "api-version";
            continue;
        }
        // A newer add-on may return a LARGER struct, which is fine -- we read the prefix we know.
        // Smaller would mean reading past its end, so it is refused rather than trusted.
        // The version 2 members are optional (see ResolveClone), so a version 1 add-on is still driven.
        if (api->struct_size < RENODX_HOST_API_V1_SIZE)
        {
            LOG_WARN("DLSS-NR: {}'s host API struct is {} bytes, smaller than the {} this build expects "
                     "-- not driving it from the panel",
                     BaseNameOf(mod), api->struct_size, (unsigned) RENODX_HOST_API_V1_SIZE);
            s_reason = "api-version";
            continue;
        }

        s_api = api;
        s_module = BaseNameOf(mod);
        s_reason = nullptr;
        LOG_INFO("DLSS-NR: RenoDX found in {} (addon \"{}\"), host API v{} ({} settings)", s_module,
                 api->addon_name ? api->addon_name() : "?", api->api_version,
                 api->setting_count ? api->setting_count() : 0);
        return;
    }
    if (!s_givenUp)
        s_reason = sawByName ? "no-api" : "not-loaded";
    // No RenoDX (yet -- asked again in a couple of seconds unless one was refused above), or one without
    // the export. The ordinary case: every shipped build is the latter.
}
} // namespace

bool Available()
{
    Resolve();
    return s_api != nullptr;
}

namespace
{
// The version 2 members, when the add-on has them.
const RenoDxHostApi* GraphicsApi()
{
    Resolve();
    if (s_api == nullptr || s_api->api_version < 2 ||
        s_api->struct_size < offsetof(RenoDxHostApi, encode_for_swapchain) + sizeof(s_api->encode_for_swapchain))
        return nullptr;
    return s_api;
}
} // namespace

bool TagApiAvailable() { return GraphicsApi() != nullptr; }

bool ResolveClone(void* nativeResource, void** outNativeResource)
{
    auto* api = GraphicsApi();
    return api != nullptr && api->resolve_clone != nullptr && api->resolve_clone(nativeResource, outNativeResource);
}

bool EncodeForSwapchain(void* nativeResource, uint32_t d3d12State, void** outNativeResource, uint32_t* outD3d12State)
{
    auto* api = GraphicsApi();
    return api != nullptr && api->encode_for_swapchain != nullptr &&
           api->encode_for_swapchain(nativeResource, d3d12State, outNativeResource, outD3d12State);
}

bool EncodeUiForSwapchain(void* nativeResource, uint32_t d3d12State, void** outNativeResource, uint32_t* outD3d12State)
{
    auto* api = GraphicsApi();
    return api != nullptr &&
           api->struct_size >=
               offsetof(RenoDxHostApi, encode_ui_for_swapchain) + sizeof(api->encode_ui_for_swapchain) &&
           api->encode_ui_for_swapchain != nullptr &&
           api->encode_ui_for_swapchain(nativeResource, d3d12State, outNativeResource, outD3d12State);
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

const char* UnavailableReason()
{
    Resolve();
    return s_api != nullptr ? nullptr : s_reason;
}

std::string AddonInProcess()
{
    // Not through Resolve(): that one is rate-limited for the panel, and this is asked once, at the
    // moment the game builds its swap chain, when an answer from two seconds ago would be stale.
    auto enumModules = EnumModulesFn();
    if (enumModules == nullptr)
        return {};

    for (HMODULE mod : LoadedModules(enumModules))
    {
        std::string name = BaseNameOf(mod);
        if (GetProcAddress(mod, "RenoDxGetHostApi") != nullptr)
            return name;

        // Upstream builds have no export, so the file name is what identifies them: renodx-*.addon64
        // (or .addon for a 32-bit ReShade). The extension keeps a stray renodx*.dll from counting.
        const size_t dot = name.find_last_of('.');
        const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
        if (_strnicmp(name.c_str(), "renodx", 6) == 0 &&
            (_stricmp(ext.c_str(), ".addon64") == 0 || _stricmp(ext.c_str(), ".addon") == 0))
            return name;
    }
    return {};
}
} // namespace DlssNrRenoDx

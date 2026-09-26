// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"

#include "DlssNr_ReLimiter.h"

namespace DlssNrReLimiter
{
namespace
{
// GetModuleHandle does not take a reference, which is what we want: ReShade owns the add-on's lifetime
// and this must not keep it alive past that.
//
// NOT resolved once. It used to be, and the first call comes from the first frame the panel draws --
// which can be before ReShade has loaded its add-ons (OptiScaler loads ReShade itself under
// LoadReshade=true, and ReShade loads add-ons when it initialises, not when it is mapped). A miss on
// that first frame then hid the Pacing page for the whole session with ReLimiter running. So a miss
// is retried every couple of seconds, and only an answer that cannot change -- the add-on is here but
// exports no API, or one we do not speak -- stops the asking.
const ReLimiterApi* s_api = nullptr;
bool s_givenUp = false;
// Why s_api is null, as the stable code UnavailableReason() hands out.
const char* s_reason = "not-loaded";
ULONGLONG s_lastTry = 0;
constexpr ULONGLONG kRetryEveryMs = 2000;

void Resolve()
{
    if (s_api != nullptr || s_givenUp)
        return;
    const ULONGLONG now = GetTickCount64();
    if (s_lastTry != 0 && now - s_lastTry < kRetryEveryMs)
        return;
    s_lastTry = now;

    // Both bitnesses, because the 32-bit helper process loads the 32-bit add-on and the panel runs
    // there too on that route.
    HMODULE mod = GetModuleHandleW(L"relimiter.addon64");
    if (mod == nullptr)
        mod = GetModuleHandleW(L"relimiter.addon32");
    if (mod == nullptr)
        return; // not loaded (yet), which is the ordinary case -- asked again later

    // From here on the module is present, and nothing below can change for this process, so every
    // refusal is final. That is also what keeps these log lines to one each.
    s_givenUp = true;

    auto get = (ReLimiterGetApiFn) GetProcAddress(mod, "ReLimiterGetApi");
    if (get == nullptr)
    {
        // A ReLimiter older than the host API. Nothing is wrong; it simply cannot be driven from here.
        LOG_INFO("DLSS-NR: ReLimiter is loaded but exports no host API -- its own overlay still works");
        s_reason = "no-api";
        return;
    }

    const ReLimiterApi* api = get(RELIMITER_API_VERSION);
    if (api == nullptr)
    {
        LOG_WARN("DLSS-NR: ReLimiter does not speak host API version {} -- not driving it from the panel",
                 RELIMITER_API_VERSION);
        s_reason = "api-version";
        return;
    }
    // A build newer than this header could return a LARGER struct; smaller would mean fields we would
    // read past the end of. Refused rather than trusted.
    if (api->struct_size < sizeof(ReLimiterApi))
    {
        LOG_WARN("DLSS-NR: ReLimiter's host API struct is {} bytes, smaller than the {} this build "
                 "expects -- not driving it from the panel",
                 api->struct_size, (unsigned) sizeof(ReLimiterApi));
        s_reason = "api-version";
        return;
    }
    s_givenUp = false;

    s_api = api;
    s_reason = nullptr;
    LOG_INFO("DLSS-NR: ReLimiter {} found, host API v{} ({} settings)",
             api->product_version ? api->product_version() : "?", api->api_version,
             api->setting_count ? api->setting_count() : 0);
}
} // namespace

bool Available()
{
    Resolve();
    return s_api != nullptr;
}

const ReLimiterApi* Api()
{
    Resolve();
    return s_api;
}

const char* Version()
{
    Resolve();
    return s_api && s_api->product_version ? s_api->product_version() : nullptr;
}

const char* UnavailableReason()
{
    Resolve();
    return s_api != nullptr ? nullptr : s_reason;
}
} // namespace DlssNrReLimiter

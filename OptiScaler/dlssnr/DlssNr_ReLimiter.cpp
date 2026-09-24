// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#include "pch.h"

#include "DlssNr_ReLimiter.h"

namespace DlssNrReLimiter
{
namespace
{
// Resolved once. GetModuleHandle does not take a reference, which is what we want: ReShade owns the
// add-on's lifetime and this must not keep it alive past that.
bool s_tried = false;
const ReLimiterApi* s_api = nullptr;

void Resolve()
{
    if (s_tried)
        return;
    s_tried = true;

    // Both bitnesses, because the 32-bit helper process loads the 32-bit add-on and the panel runs
    // there too on that route.
    HMODULE mod = GetModuleHandleW(L"relimiter.addon64");
    if (mod == nullptr)
        mod = GetModuleHandleW(L"relimiter.addon32");
    if (mod == nullptr)
        return; // not installed here, which is the ordinary case

    auto get = (ReLimiterGetApiFn) GetProcAddress(mod, "ReLimiterGetApi");
    if (get == nullptr)
    {
        // A ReLimiter older than the host API. Nothing is wrong; it simply cannot be driven from here.
        LOG_INFO("DLSS-NR: ReLimiter is loaded but exports no host API -- its own overlay still works");
        return;
    }

    const ReLimiterApi* api = get(RELIMITER_API_VERSION);
    if (api == nullptr)
    {
        LOG_WARN("DLSS-NR: ReLimiter does not speak host API version {} -- not driving it from the panel",
                 RELIMITER_API_VERSION);
        return;
    }
    // A build newer than this header could return a LARGER struct; smaller would mean fields we would
    // read past the end of. Refused rather than trusted.
    if (api->struct_size < sizeof(ReLimiterApi))
    {
        LOG_WARN("DLSS-NR: ReLimiter's host API struct is {} bytes, smaller than the {} this build "
                 "expects -- not driving it from the panel",
                 api->struct_size, (unsigned) sizeof(ReLimiterApi));
        return;
    }

    s_api = api;
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

bool PacingActive() { return Available(); }
} // namespace DlssNrReLimiter

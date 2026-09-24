#pragma once
// VENDORED COPY -- do not edit here.
//
// The authoritative version lives in the ReLimiter fork (src/api.h, feat/host-api). This is a verbatim
// copy so the engine can talk to a loaded relimiter.addon64 without taking a build dependency on that
// repository. If the two ever disagree the fork wins; check api_version at runtime, which is what it
// is for.
//
// ReLimiter is MIT licensed, Copyright (c) 2026 Mat, Laz, Rank (RankFTW, Lazorr, UltraMatt):
// https://github.com/RankFTW/ReLimiter. The notice below travels with this file for that reason.
// ReLimiter host API — a stable C ABI another module in the same process can drive.
//
// WHY THIS EXISTS. ReLimiter draws its own ImGui overlay through ReShade, which is the right UI when
// ReLimiter is what you are running. It is the wrong one when ReLimiter is one part of a stack that
// already has a panel: the player then has two overlays, two keybinds and two places to look. This
// lets a host — OptiScaler's DLSS 5 panel is the first — present ReLimiter's settings inside its own
// UI, without ReLimiter having to know anything about that host.
//
// HOW A HOST USES IT.
//
//   auto mod = GetModuleHandleW(L"relimiter.addon64");          // already loaded by ReShade
//   auto get = (ReLimiterGetApiFn) GetProcAddress(mod, "ReLimiterGetApi");
//   const ReLimiterApi* api = get ? get(RELIMITER_API_VERSION) : nullptr;
//
// A null return means "this build does not speak your version" and is not an error to work around.
//
// THE POINT OF describe_setting. A host enumerates the settings and builds its UI from what it finds,
// rather than hardcoding a list. That is deliberate and it is what keeps a fork cheap: when a setting
// is added to Config it is added to one table in api.cpp, and every host picks it up with no change
// and no rebuild. A host that hardcodes the list gets to do this work again every release.
//
// THREADING. Call these from the thread that renders the host's UI. Setters write the same globals
// ReLimiter's own overlay writes and are no more or less safe than the overlay is.
//
// Copyright (c) 2026 Mat, Laz, Rank. MIT — see LICENSE.

#include <stdint.h>

#define RELIMITER_API_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ReLimiterType {
    RELIMITER_TYPE_BOOL = 1,
    RELIMITER_TYPE_INT = 2,
    RELIMITER_TYPE_FLOAT = 3,
    RELIMITER_TYPE_DOUBLE = 4,
    RELIMITER_TYPE_STRING = 5,
    RELIMITER_TYPE_ENUM = 6,   // a string with a fixed set of choices; render a combo
    RELIMITER_TYPE_KEYBIND = 7 // a string holding a key combo; render a key capture
} ReLimiterType;

typedef struct ReLimiterSettingInfo {
    uint32_t struct_size;      // set by the caller to sizeof(ReLimiterSettingInfo)
    const char* key;           // the INI key, and the key for get/set. Stable.
    const char* label;         // short human label
    const char* group;         // the section it belongs under, for a host that groups its UI
    const char* tooltip;       // may be empty, never null
    uint32_t type;             // ReLimiterType
    double min_value;          // numeric range; both 0 when there is no meaningful range
    double max_value;
    const char* const* choices; // RELIMITER_TYPE_ENUM only, else null
    uint32_t choice_count;
    // When non-null, 0 is a valid SPECIAL value meaning this, and min_value/max_value describe the
    // range only ABOVE zero. Three settings work that way -- target_fps, background_fps and
    // fg_off_fps are each "0 = automatic" with a clamp applied to anything else -- and without this a
    // host builds a slider from 0 and offers values ValidateConfig will silently throw away, or
    // labels the automatic mode "0 fps". A host should render 0 as this text, then the range.
    const char* zero_label;
} ReLimiterSettingInfo;

typedef struct ReLimiterApi {
    uint32_t struct_size;      // sizeof(ReLimiterApi) as this build sees it
    uint32_t api_version;      // RELIMITER_API_VERSION of this build

    const char* (*product_version)(void);   // "3.3.5"

    uint32_t (*setting_count)(void);
    // Fills `out` for `index` < setting_count(). Returns 1 on success, 0 otherwise.
    int (*describe_setting)(uint32_t index, ReLimiterSettingInfo* out);

    // Numbers cover bool, int, float and double: one ABI instead of four. The descriptor's type tells
    // a host how to render and how to round. Return 1 on success, 0 for an unknown or wrong-typed key.
    int (*get_number)(const char* key, double* out);
    int (*set_number)(const char* key, double value);

    // Strings, including enums and keybinds. get_string always null-terminates when buf_size > 0 and
    // returns 0 if the value did not fit, so a caller can retry with a larger buffer.
    int (*get_string)(const char* key, char* buf, uint32_t buf_size);
    int (*set_string)(const char* key, const char* value);

    void (*apply)(void);       // push the live config into the running limiter (ApplyConfig)
    void (*save)(void);        // persist it to the INI (SaveConfig)
} ReLimiterApi;

// The single export. Returns null when `requested_version` is one this build cannot serve.
__declspec(dllexport) const ReLimiterApi* ReLimiterGetApi(uint32_t requested_version);

typedef const ReLimiterApi* (*ReLimiterGetApiFn)(uint32_t);

#ifdef __cplusplus
}
#endif

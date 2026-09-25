// Copyright (c) 2026 mrcgibb9876-hash. Original to this fork, not part of upstream OptiScaler.
// Distributed under the GNU General Public License version 3 -- see LICENSE.
#pragma once
// RenoDX host API -- the C ABI a loaded RenoDX add-on exports, mirrored for this side of the call.
//
// NOT A VERBATIM COPY, unlike ReLimiter_Api.h, and the difference matters. RenoDX declares this API
// inside src/utils/settings.hpp, a C++ header that pulls in imgui and reshade.hpp and every add-on
// compiles; copying it here would drag those in too. So this is a hand-written mirror of the same
// LAYOUT, and layout is the whole contract: field order, field types and struct size must match
// renodx::utils::settings::RenoDxHostSetting / RenoDxHostApi exactly.
//
// The authoritative version is that header. If the two ever disagree the add-on wins, which is what
// api_version and struct_size are checked for at runtime -- a mismatch means we do not drive it,
// never that we read a struct we half understand.
//
// RenoDX is MIT licensed, Copyright (c) 2025 Carlos Lopez Jr.: https://github.com/clshortfuse/renodx
//
// NOTE ON AVAILABILITY. No shipped RenoDX build exports this yet; it comes from
// mrcgibb9876-hash/renodx (feat/host-api), offered upstream. An add-on without the export is the
// ordinary case and simply means the panel shows no RenoDX page.
#include <cstdint>

#define RENODX_HOST_API_VERSION 1

// Matches renodx::utils::settings::RenoDxHostValueType.
enum RenoDxHostValueType : uint32_t
{
    RENODX_HOST_VALUE_FLOAT = 0,
    RENODX_HOST_VALUE_INTEGER = 1,
    RENODX_HOST_VALUE_BOOLEAN = 2,
    // An integer whose values are named: label_count is non-zero and label_at names each.
    RENODX_HOST_VALUE_COMBO = 3,
    RENODX_HOST_VALUE_TEXT = 4,
};

struct RenoDxHostSetting
{
    uint32_t struct_size;
    // Borrowed from the add-on's own Setting and valid only until the next call into this API on this
    // thread. Anything kept past that has to be copied.
    const char* key;
    const char* label;
    // RenoDX's collapsing header ("Tone Mapping", "Color Grading"). This is the caption to draw.
    const char* section;
    // RenoDX puts settings sharing a group on ONE LINE in its own overlay. This panel is one control
    // per row on purpose -- a controller has to land on each of them -- so this is read and ignored.
    const char* group;
    const char* tooltip;
    const char* format;
    uint32_t value_type;
    // float, not double: RenoDX binds its settings to floats, and widening here would invent
    // precision the add-on does not have.
    float min_value;
    float max_value;
    uint32_t label_count;
    // The add-on's own answer to "should this be shown / is it live right now", so this panel can
    // hide and grey exactly what RenoDX's overlay would rather than offering a value it will ignore.
    int32_t is_visible;
    int32_t is_enabled;
    int32_t is_global;
};

struct RenoDxHostApi
{
    uint32_t struct_size;
    uint32_t api_version;
    // The ReShade config section this add-on reads and writes (RenoDX's `global_name`). Worth having
    // even though this panel does not touch the ini: it is what names the add-on in the UI, and
    // guessing "renodx" is wrong for any mod that sets its own.
    const char* (*addon_name)();
    uint32_t (*setting_count)();
    // False when index is out of range, or when out->struct_size is smaller than the add-on's struct.
    bool (*describe_setting)(uint32_t index, RenoDxHostSetting* out);
    // One label of a COMBO setting, by setting index and label index.
    const char* (*label_at)(uint32_t index, uint32_t label_index);
    bool (*get_number)(const char* key, float* out);
    bool (*set_number)(const char* key, float value);
    bool (*get_text)(const char* key, char* buf, uint32_t buf_size);
    bool (*set_text)(const char* key, const char* value);
    // Persist to the current preset's config section, as RenoDX's overlay does after a change.
    void (*save)();
};

using RenoDxGetHostApiFn = const RenoDxHostApi* (*) (uint32_t requested_version);

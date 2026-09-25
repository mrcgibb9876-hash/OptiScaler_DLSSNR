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
#include <cstddef>
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

    // ---- Version 4 (appended). An add-on that speaks 1-3 fills none of these, so they are read only
    // when api_version >= 4 -- see DlssNrRenoDx::V4(). ----

    // What RenoDX's overlay actually draws: RenoDxHostSettingKind (its SettingValueType). value_type above
    // stays as it was for older hosts, so a version-4 host switches on this instead.
    uint32_t kind;
    // What the per-setting reset restores (numeric kinds; INPUT_TEXT uses default_text). A mod may move
    // it at run time, so read it each time.
    float default_value;
    // The overlay draws a reset button for this setting (it also hides it while the preset is Off).
    int32_t can_reset;
    // The value equals its default.
    int32_t is_using_default;
    // INPUT_TEXT: the default text and the greyed hint shown while the box is empty. Borrowed.
    const char* default_text;
    const char* placeholder;
    // INPUT_TEXT: longest value set_text keeps, in bytes; 0 = no limit.
    uint32_t text_max_length;
    // INPUT_TEXT: the ImGuiInputTextFlags the mod gave its box, in the add-on's ImGui's bit values.
    uint32_t input_text_flags;
    // RENODX_HOST_STYLE_* bits.
    uint32_t style;
    // The mod's accent colour for this control, 0xRRGGBB, when has_tint is 1.
    int32_t has_tint;
    uint32_t tint_rgb;
    // FLOAT/INTEGER: the overlay's slider is logarithmic.
    int32_t is_logarithmic;
    // The overlay draws sticky settings above the preset switcher, the rest below it.
    int32_t is_sticky;
};

// RenoDxHostSetting::kind -- matches renodx::utils::settings::RenoDxHostSettingKind (= SettingValueType).
enum RenoDxHostSettingKind : uint32_t
{
    RENODX_HOST_KIND_FLOAT = 0,
    RENODX_HOST_KIND_INTEGER = 1, // a slider; a combo when label_count > 0
    RENODX_HOST_KIND_BOOLEAN = 2, // label_count is 0 (Off/On) or 2 (the mod's own two names)
    RENODX_HOST_KIND_BUTTON = 3,  // no value; press(index) runs it
    RENODX_HOST_KIND_LABEL = 4,   // read-only "label: text"; the text is label_at(index, 0)
    RENODX_HOST_KIND_BULLET = 5,
    RENODX_HOST_KIND_TEXT = 6,
    RENODX_HOST_KIND_TEXT_NOWRAP = 7,
    RENODX_HOST_KIND_CUSTOM = 8, // drawn by the mod inside ReShade's overlay; nothing to drive from here
    RENODX_HOST_KIND_INPUT_TEXT = 9,
};

enum RenoDxHostStyle : uint32_t
{
    RENODX_HOST_STYLE_SEGMENTED = 1u << 0,
    RENODX_HOST_STYLE_MULTILINE = 1u << 1,
};

// RenoDxHostSetting as versions 1-3 defined it (ended at is_global).
#define RENODX_HOST_SETTING_V1_SIZE (offsetof(RenoDxHostSetting, is_global) + sizeof(int32_t))

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

    // Version 2 (mrcgibb9876-hash/renodx feat/dlssg-tags). Read only when api_version >= 2 and struct_size
    // reaches them -- see DlssNrRenoDx::ResolveClone / EncodeForSwapchain. Native D3D12 resources and
    // D3D12_RESOURCE_STATES throughout; false means "nothing to substitute, tag the original".
    //
    // The clone RenoDX redirects the resource's writes to (the lookup of RenoDX's dlssfix slSetTag hook).
    bool (*resolve_clone)(void* native_resource, void** out_native_resource);
    // For a colour image DLSS-G compares with the presented frame: a swap-chain-format texture that
    // receives RenoDX's swap chain proxy pass over the image at each present, before the frame leaves
    // ReShade.
    bool (*encode_for_swapchain)(void* native_resource, uint32_t d3d12_state, void** out_native_resource,
                                 uint32_t* out_d3d12_state);
    // The same for a UI colour-and-alpha image: its own format and alpha kept, only colour encoded. Added
    // after the first version 2 test build, so struct_size is checked for it on its own.
    bool (*encode_ui_for_swapchain)(void* native_resource, uint32_t d3d12_state, void** out_native_resource,
                                    uint32_t* out_d3d12_state);
    // Copy-back: the same pass, its result copied back into the tagged image itself at each present
    // (alpha kept when is_ui), so the tag stays as the game set it. Checked by struct_size on its own.
    bool (*encode_in_place_for_swapchain)(void* native_resource, uint32_t d3d12_state, bool is_ui);
    // Whether the add-on clones the back buffers and writes the presented frame itself at present. False
    // for a shader-only add-on (Witcher 3, Cyberpunk 2077), whose output is the game's own.
    bool (*uses_swapchain_proxy)();

    // Version 3: RenoDX's own overlay reset (non-global, resettable settings to their defaults), then
    // saved. Checked by struct_size on its own -- see DlssNrRenoDx::CanReset.
    void (*reset_settings)();

    // ---- Version 4 ---- the rest of RenoDX's overlay, each doing exactly what the same click does there.
    // Presets: preset_count entries (0 when the mod has presets off; then get_preset is -1). Entry 0 is
    // Off, while which the overlay greys every control and hides every reset button.
    uint32_t (*preset_count)();
    const char* (*preset_label)(uint32_t preset); // borrowed
    int32_t (*get_preset)();
    bool (*set_preset)(int32_t preset);
    uint32_t (*preset_style)(); // RENODX_HOST_STYLE_* for the switcher
    // The per-setting reset button (refused while the preset is Off or the setting has none).
    bool (*reset_setting)(const char* key);
    // A BUTTON setting's click, by setting index.
    bool (*press)(uint32_t index);
    const char* (*overlay_title)();
    bool (*section_open_by_default)(const char* section);
};

// What a version 1 add-on's struct holds; anything it reports at least this size of is drivable.
#define RENODX_HOST_API_V1_SIZE (offsetof(RenoDxHostApi, resolve_clone))

using RenoDxGetHostApiFn = const RenoDxHostApi* (*) (uint32_t requested_version);

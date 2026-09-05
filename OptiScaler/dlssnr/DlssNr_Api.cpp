#include "pch.h"

#include "DlssNr_Api.h"

#include "DlssNr.h"
#include "DlssNrFeature_Vk.h"

#include <Config.h>

#include <cstring>

// The tables below are the whole of this file's content, and they are generated from Config.cpp's
// own load block rather than typed out -- key, type and member all come from the one place that
// already had to agree with the ini. If a setting is added there and not here, it simply is not
// drivable from outside; nothing breaks.
//
// Pointers-to-member rather than lambdas or a switch: the table stays declarative, a new setting is
// one line, and there is no per-key code that could get a member wrong while still compiling.

namespace
{

template <class T> struct Entry
{
    const char* key;
    CustomOptional<T> Config::* member;
};

// One entry per line, deliberately. The formatter packs these into columns, which turns a table
// meant to be read against Config.h and the ini into something you have to scan sideways.
// clang-format off

// ---- float -------------------------------------------------------------------------------------
constexpr Entry<float> kFloats[] = {
    { "ColourStrength", &Config::DlssNrColourStrength },
    { "CompareSplit", &Config::DlssNrCompareSplit },
    { "CompareZoom", &Config::DlssNrCompareZoom },
    { "Intensity", &Config::DlssNrIntensity },
    { "LocalStructure", &Config::DlssNrLocalStructure },
    { "LocalTone", &Config::DlssNrLocalTone },
    { "MaxRatio", &Config::DlssNrMaxRatio },
    { "ScanAnchorValue", &Config::DlssNrScanAnchorValue },
    { "ScanAnchorWhitePoint", &Config::DlssNrScanAnchorWhitePoint },
    { "ScanTrim", &Config::DlssNrScanTrim },
    { "SkinStructure", &Config::DlssNrSkinStructure },
    { "TagScale", &Config::DlssNrTagScale },
    { "TransferStrength", &Config::DlssNrTransferStrength },
    { "WhitePointScale", &Config::DlssNrWhitePointScale },
    { "WhitePointTrim", &Config::DlssNrWhitePointTrim },
    { "WorkingScale", &Config::DlssNrWorkingScale },
};

// ---- bool --------------------------------------------------------------------------------------
constexpr Entry<bool> kBools[] = {
    { "ApplyModel", &Config::DlssNrApplyModel },
    { "AutoCapture", &Config::DlssNrAutoCapture },
    { "AutoMask", &Config::DlssNrAutoMask },
    { "CompareSwap", &Config::DlssNrCompareSwap },
    { "CompareTags", &Config::DlssNrCompareTags },
    { "Enabled", &Config::DlssNrEnabled },
    { "HoldFrame", &Config::DlssNrHoldFrame },
    { "ProbeD3D11", &Config::DlssNrProbeD3D11 },
    { "ProxyProbe", &Config::DlssNrProxyProbe },
    { "ScanExposure", &Config::DlssNrScanExposure },
    { "ScanInverted", &Config::DlssNrScanInverted },
    { "ScanMeter", &Config::DlssNrScanMeter },
    { "UICorrection", &Config::DlssNrUICorrection },
    { "UseProxy", &Config::DlssNrUseProxy },
};

// ---- signed int --------------------------------------------------------------------------------
// Keybinds, and only keybinds. UnboundKey is -1, which is why these cannot live with the unsigned
// ones.
constexpr Entry<int> kInts[] = {
    { "PanelKey", &Config::DlssNrPanelKey },
    { "ToggleKey", &Config::DlssNrToggleKey },
};

// ---- unsigned int ------------------------------------------------------------------------------
// Modes and counts. Exposed through the same Get/SetInt as the signed ones, because a caller does
// not care about the distinction -- but a negative write is refused here rather than wrapping into
// a huge unsigned mode nobody asked for.
constexpr Entry<uint32_t> kUInts[] = {
    { "Compare", &Config::DlssNrCompare },
    { "DebugView", &Config::DlssNrDebugView },
    { "DepthConvention", &Config::DlssNrDepthConvention },
    { "Passes", &Config::DlssNrPasses },
    { "Preset", &Config::DlssNrPreset },
    { "ReversibleMode", &Config::DlssNrReversibleMode },
    { "Style", &Config::DlssNrStyle },
    { "Transfer", &Config::DlssNrTransfer },
    { "WhitePointSource", &Config::DlssNrWhitePointSource },
};

// ---- enum --------------------------------------------------------------------------------------
// Scaler is a uint32_t enum class, so it travels as an int like the rest and is cast at the edge.
constexpr Entry<Scaler> kScalers[] = {
    { "ScalingDownscaler", &Config::DlssNrScalingDownscaler },
};
// clang-format on

// Two settings in Config.h are deliberately not here.
//
// DlssNrScanAnchors is a serialized anchor table, not a setting: the only sensible producer is the
// scan itself, and handing a consumer a string to mangle would let it corrupt a calibration that
// took someone a while to make. If a consumer ever needs to move anchors, that wants its own typed
// calls rather than a string key.
//
// DlssNrWhitePointFromExposure is dead. It was replaced by the three-way DlssNrWhitePointSource and
// nothing reads it any more. It still exists in Config.h so an old ini does not error, but exposing
// it here would hand consumers a control that silently does nothing -- the same reason it came out
// of the overlay. Use WhitePointSource.

template <class T> const Entry<T>* Find(const Entry<T>* table, size_t count, const char* key)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (std::strcmp(table[i].key, key) == 0)
            return &table[i];
    }

    return nullptr;
}

} // namespace

extern "C"
{

    OPTINR_API int32_t OptiNr_AbiVersion(void) { return OPTINR_ABI_VERSION; }

    OPTINR_API int32_t OptiNr_GetStatus(OptiNr_Status* out)
    {
        // structSize lets this grow later without breaking consumers, but only if the direction is
        // right, and the obvious guard has it backwards.
        //
        // A consumer built against an OLDER header passes a SMALLER struct. That one must be
        // accepted and filled up to where its struct ends -- refusing it is exactly the breakage the
        // scheme exists to prevent, and it would hit every add-on already in the wild the first time
        // a field is appended here.
        //
        // A consumer built against a NEWER header passes a LARGER struct, and that is the one to
        // refuse: it believes in fields this build has never heard of, and filling the prefix would
        // leave it reading whatever its own zero-init left behind while thinking the call succeeded.
        if (out == nullptr)
            return OPTINR_BAD_ARGUMENT;

        if (out->structSize < OPTINR_STATUS_SIZE_V1 || out->structSize > sizeof(OptiNr_Status))
            return OPTINR_BAD_ARGUMENT;

        const auto* config = Config::Instance();

        // Written through a byte cursor rather than directly, so each field is only touched when the
        // caller's struct actually reaches it. Today every field is inside v1 and all of these pass;
        // the shape is here so that appending a field later is a one-line change that cannot
        // accidentally write past a shorter caller's buffer.
        const size_t capacity = out->structSize;
        const auto fits = [capacity](size_t offset, size_t size) { return offset + size <= capacity; };

#define OPTINR_FILL(field, value)                                                                                      \
    if (fits(offsetof(OptiNr_Status, field), sizeof(out->field)))                                                      \
    out->field = (value)

        OPTINR_FILL(running, DlssNr::IsRunning() ? 1 : 0);
        OPTINR_FILL(runningVulkan, DlssNr::IsRunningVk() ? 1 : 0);
        OPTINR_FILL(enabled, config->DlssNrEnabled.value_or_default() ? 1 : 0);
        OPTINR_FILL(framesVulkan, DlssNr::FramesVk());

        // Negative rather than zero for "not measured". Zero is a real reading and would make a pass
        // that has not run yet look free.
        const auto ms = DlssNr::IsRunningVk() ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime();
        OPTINR_FILL(gpuMs, ms.has_value() ? ms.value() : -1.0);

        // Static storage inside the feature, so it outlives this call. Never freed by the caller.
        // The two backends latch their failures separately, and on a native Vulkan game the D3D12
        // one is never touched -- so reporting only that would show an empty reason over a Vulkan
        // failure, and a stale D3D12 one over a Vulkan session that is running fine.
        const char* reason = DlssNr::FailureReasonVk();

        if (reason == nullptr || reason[0] == 0)
            reason = DlssNr::FailureReason();

        OPTINR_FILL(failureReason, reason != nullptr ? reason : "");

#undef OPTINR_FILL

        return OPTINR_OK;
    }

    OPTINR_API int32_t OptiNr_GetFloat(const char* key, float* out)
    {
        if (key == nullptr || out == nullptr)
            return OPTINR_BAD_ARGUMENT;

        if (const auto* e = Find(kFloats, std::size(kFloats), key))
        {
            *out = (Config::Instance()->*(e->member)).value_or_default();
            return OPTINR_OK;
        }

        return OPTINR_UNKNOWN_KEY;
    }

    OPTINR_API int32_t OptiNr_SetFloat(const char* key, float value)
    {
        if (key == nullptr)
            return OPTINR_BAD_ARGUMENT;

        if (const auto* e = Find(kFloats, std::size(kFloats), key))
        {
            Config::Instance()->*(e->member) = value;
            return OPTINR_OK;
        }

        return OPTINR_UNKNOWN_KEY;
    }

    OPTINR_API int32_t OptiNr_GetInt(const char* key, int32_t* out)
    {
        if (key == nullptr || out == nullptr)
            return OPTINR_BAD_ARGUMENT;

        if (const auto* e = Find(kInts, std::size(kInts), key))
        {
            *out = (Config::Instance()->*(e->member)).value_or_default();
            return OPTINR_OK;
        }

        if (const auto* e = Find(kUInts, std::size(kUInts), key))
        {
            *out = (int32_t) (Config::Instance()->*(e->member)).value_or_default();
            return OPTINR_OK;
        }

        if (const auto* e = Find(kScalers, std::size(kScalers), key))
        {
            *out = (int32_t) (uint32_t) (Config::Instance()->*(e->member)).value_or_default();
            return OPTINR_OK;
        }

        return OPTINR_UNKNOWN_KEY;
    }

    OPTINR_API int32_t OptiNr_SetInt(const char* key, int32_t value)
    {
        if (key == nullptr)
            return OPTINR_BAD_ARGUMENT;

        if (const auto* e = Find(kInts, std::size(kInts), key))
        {
            Config::Instance()->*(e->member) = (int) value;
            return OPTINR_OK;
        }

        // A negative here would wrap to an enormous mode number and be silently accepted, so it is
        // refused instead. Nothing unsigned in this pass has a meaningful negative value.
        if (const auto* e = Find(kUInts, std::size(kUInts), key))
        {
            if (value < 0)
                return OPTINR_BAD_ARGUMENT;

            Config::Instance()->*(e->member) = (uint32_t) value;
            return OPTINR_OK;
        }

        if (const auto* e = Find(kScalers, std::size(kScalers), key))
        {
            if (value < 0 || value >= (int32_t) Scaler::Count)
                return OPTINR_BAD_ARGUMENT;

            Config::Instance()->*(e->member) = (Scaler) (uint32_t) value;
            return OPTINR_OK;
        }

        return OPTINR_UNKNOWN_KEY;
    }

    OPTINR_API int32_t OptiNr_GetBool(const char* key, int32_t* out)
    {
        if (key == nullptr || out == nullptr)
            return OPTINR_BAD_ARGUMENT;

        if (const auto* e = Find(kBools, std::size(kBools), key))
        {
            *out = (Config::Instance()->*(e->member)).value_or_default() ? 1 : 0;
            return OPTINR_OK;
        }

        return OPTINR_UNKNOWN_KEY;
    }

    OPTINR_API int32_t OptiNr_SetBool(const char* key, int32_t value)
    {
        if (key == nullptr)
            return OPTINR_BAD_ARGUMENT;

        if (const auto* e = Find(kBools, std::size(kBools), key))
        {
            Config::Instance()->*(e->member) = value != 0;
            return OPTINR_OK;
        }

        return OPTINR_UNKNOWN_KEY;
    }

    OPTINR_API int32_t OptiNr_Save(void)
    {
        // SaveIni answers whether it actually wrote, and a consumer that has just taken a user's
        // settings deserves to know they did not land rather than being told OK regardless.
        return Config::Instance()->SaveIni() ? OPTINR_OK : OPTINR_BAD_ARGUMENT;
    }

    OPTINR_API int32_t OptiNr_RetryAfterFailure(void)
    {
        DlssNr::RetryAfterFailure();
        return OPTINR_OK;
    }

    OPTINR_API int32_t OptiNr_EnumKey(int32_t index, const char** outKey, int32_t* outType)
    {
        if (outKey == nullptr || outType == nullptr || index < 0)
            return OPTINR_BAD_ARGUMENT;

        // One flat sequence over the four tables, in a fixed order. The order is not part of the
        // contract -- a consumer that cares about order should sort what it collects.
        size_t i = (size_t) index;

        if (i < std::size(kFloats))
        {
            *outKey = kFloats[i].key;
            *outType = OPTINR_TYPE_FLOAT;
            return OPTINR_OK;
        }

        i -= std::size(kFloats);

        if (i < std::size(kBools))
        {
            *outKey = kBools[i].key;
            *outType = OPTINR_TYPE_BOOL;
            return OPTINR_OK;
        }

        i -= std::size(kBools);

        if (i < std::size(kInts))
        {
            *outKey = kInts[i].key;
            *outType = OPTINR_TYPE_INT;
            return OPTINR_OK;
        }

        i -= std::size(kInts);

        if (i < std::size(kUInts))
        {
            *outKey = kUInts[i].key;
            *outType = OPTINR_TYPE_INT;
            return OPTINR_OK;
        }

        i -= std::size(kUInts);

        if (i < std::size(kScalers))
        {
            *outKey = kScalers[i].key;
            *outType = OPTINR_TYPE_INT;
            return OPTINR_OK;
        }

        return OPTINR_UNKNOWN_KEY;
    }

} // extern "C"

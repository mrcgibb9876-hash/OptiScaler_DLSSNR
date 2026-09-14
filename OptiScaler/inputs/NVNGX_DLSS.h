#pragma once

#include <cstdint>

template <typename FeatureType> struct ContextData
{
    std::unique_ptr<FeatureType> feature;
    NVSDK_NGX_Parameter* createParams = nullptr;
    int changeBackendCounter = 0;
};

// The SDK version to give NVIDIA's own init. A game built against SDK 0x13 or older is refused by current
// drivers as out of date (BAD0000C, Monster Hunter: World with 0x12), which leaves DLSS unusable -- but
// OptiScaler talks to the driver itself and always hands it a complete, current feature info, so it can
// say so. Newer versions go through untouched.
inline NVSDK_NGX_Version DriverSdkVersion(NVSDK_NGX_Version gameVersion)
{
    if (gameVersion > (NVSDK_NGX_Version) 0x0000013)
        return gameVersion;

    static bool said = false;
    if (!said)
    {
        said = true;
        LOG_INFO("NGX init: the game was built against SDK 0x{:X}; telling the driver 0x{:X}, the version this "
                 "build speaks",
                 (unsigned) gameVersion, (unsigned) NVSDK_NGX_Version_API);
    }

    return NVSDK_NGX_Version_API;
}

// Whether the whole feature-info struct can be read. Kept apart: __try cannot share a frame with objects
// that unwind.
inline bool ReadableFeatureInfo(const NVSDK_NGX_FeatureCommonInfo* info)
{
    __try
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(info);
        volatile unsigned char probe = bytes[0];
        probe = bytes[sizeof(NVSDK_NGX_FeatureCommonInfo) - 1];
        (void) probe;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The feature-info pointer an NGX init was handed, or null when it is not one.
//
// Games built against an old NGX SDK (version 0x13 and before) call Init with the SDK version where the
// feature info now sits: the "pointer" is a small integer, and copying 40 bytes from it faults inside this
// module. Monster Hunter: World crashed a second after launch that way, as soon as its DLSS initialised
// (2026-09-14). Anything in the first 64 KB -- never a valid user-mode address -- or not readable for the
// whole struct is treated as no feature info, which is what those SDKs meant.
inline const NVSDK_NGX_FeatureCommonInfo* SanitizeFeatureInfo(const NVSDK_NGX_FeatureCommonInfo* info)
{
    if (info == nullptr)
        return nullptr;

    if (reinterpret_cast<std::uintptr_t>(info) < 0x10000 || !ReadableFeatureInfo(info))
    {
        LOG_WARN("NGX init: the feature info argument ({}) is not a readable pointer -- an older NGX SDK passing its "
                 "version there. Ignored.",
                 static_cast<const void*>(info));
        return nullptr;
    }

    return info;
}

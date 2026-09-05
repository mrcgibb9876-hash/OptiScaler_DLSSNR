#pragma once

// The DLSS Neural Rendering control ABI.
//
// A flat C interface exported from OptiScaler.dll so something outside it -- the ReShade add-on in
// reshade_addon/, and anything else that wants to -- can read and drive the pass without linking
// against OptiScaler or knowing anything about Config, ImGui or NGX.
//
// This header is deliberately standalone: it includes nothing from the rest of the project and uses
// nothing but fixed-width integers, so a consumer can copy this one file and be done. The add-on
// does exactly that.
//
// Why string keys rather than a struct or one function per setting.
//
//   Every setting added on this side would otherwise be a new export, and a consumer built against
//   an older header would either fail to load or silently read a struct of the wrong shape. Settings
//   get added to this pass constantly. With keys, an old add-on keeps working against a new
//   OptiScaler and simply does not know about the new settings, and a new add-on asking a new
//   OptiScaler for an old setting gets a clean "no such key" instead of undefined behaviour.
//
//   The keys are exactly the ini names under [DlssNr], so OptiScaler.ini doubles as the reference
//   for what can be driven, and there is no second vocabulary to keep in step.
//
// Threading: call these from one thread at a time. They touch the same config the overlay does and
// take no lock, which matches how the overlay itself writes -- the values are small and independent,
// and a torn read of a float that a person is dragging is not worth a lock in a render path.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Bumped only when the SHAPE of something below changes -- a signature, a struct layout, the
// meaning of a return value. Adding a key is not a change to the shape and does not bump it.
#define OPTINR_ABI_VERSION 1

// Every function returns one of these.
#define OPTINR_OK 1            // did what was asked
#define OPTINR_UNKNOWN_KEY 0   // no such setting; nothing was read or written
#define OPTINR_BAD_ARGUMENT -1 // a null pointer, or a struct whose structSize is not recognised

    // What the pass is doing right now. Read-only, and cheap enough to call every frame.
    //
    // Set structSize to sizeof(OptiNr_Status) before calling. That is what lets this grow later
    // without breaking a consumer built against this version: fields are only ever appended, and
    // OptiScaler fills in as much as the caller's structSize says it can hold.
    typedef struct OptiNr_Status
    {
        uint32_t structSize;

        int32_t running;       // the D3D12 path is up
        int32_t runningVulkan; // the native Vulkan path is up
        int32_t enabled;       // the setting, which is not the same as running

        // Cost of the whole pass in milliseconds, or a negative number when nothing has been
        // measured yet. Not zero: zero is a real and different answer.
        double gpuMs;

        uint64_t framesVulkan;

        // Why it is off for this session, or "" when it is not off. Points at storage owned by
        // OptiScaler that outlives the call; do not free it, and do not keep it past the next call.
        const char* failureReason;
    } OptiNr_Status;

// The size of the struct as it stood in ABI v1, which is the oldest shape OptiScaler will fill.
// Append-only: when a field is added, this stays where it is and the new field gets a marker of its
// own, so a v1 consumer keeps being filled correctly by a much later OptiScaler.
#define OPTINR_STATUS_SIZE_V1 (offsetof(OptiNr_Status, failureReason) + sizeof(const char*))

    // The version of THIS interface in the OptiScaler that answered. Call it first: if it returns
    // something other than OPTINR_ABI_VERSION, do not call anything else.
    int32_t OptiNr_AbiVersion(void);

    // Set structSize to sizeof(OptiNr_Status) before calling.
    //
    // A consumer built against an OLDER version passes a smaller size and is filled up to where its
    // struct ends. One built against a NEWER version than the OptiScaler it found is refused, since
    // there is no honest way to fill fields that OptiScaler has never heard of -- check
    // OptiNr_AbiVersion first and that case does not arise.
    int32_t OptiNr_GetStatus(OptiNr_Status* out);

    // Reading a key that exists but has never been set returns its default, which is what the pass
    // is actually using -- so a consumer never has to know whether a value came from the ini.
    int32_t OptiNr_GetFloat(const char* key, float* out);
    int32_t OptiNr_SetFloat(const char* key, float value);

    // Signed and unsigned settings both go through these; an unsigned setting rejects a negative.
    int32_t OptiNr_GetInt(const char* key, int32_t* out);
    int32_t OptiNr_SetInt(const char* key, int32_t value);

    int32_t OptiNr_GetBool(const char* key, int32_t* out);
    int32_t OptiNr_SetBool(const char* key, int32_t value);

    // Writes the ini. The overlay saves as you go; a consumer that changes settings should call this
    // when the user is done rather than on every frame of a drag.
    int32_t OptiNr_Save(void);

    // Clears the session failure latch, so a failure caused by transient thrash does not cost a
    // restart. The same thing the overlay's Retry button does.
    int32_t OptiNr_RetryAfterFailure(void);

    // Walks the keys, so a consumer can discover what this OptiScaler supports rather than assuming.
    // index counts from 0; returns OPTINR_UNKNOWN_KEY once it runs off the end.
    //
    // outKey receives a pointer to a static string owned by OptiScaler. outType receives one of the
    // OPTINR_TYPE_ values below.
#define OPTINR_TYPE_FLOAT 1
#define OPTINR_TYPE_INT 2
#define OPTINR_TYPE_BOOL 3

    int32_t OptiNr_EnumKey(int32_t index, const char** outKey, int32_t* outType);

#ifdef __cplusplus
}
#endif

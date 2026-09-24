# Removing the forwarder — evidence log

The forwarder (`nvngx.dll_dlssnr.dll`) exists only to satisfy the snippet's caller check: the model
resolves its caller's module via `RtlPcToFileHeader` and rejects anything whose path does not contain
`nvngx.dll`, with `FAIL_PlatformError`, before looking at a single argument. Naming the shim
`nvngx.dll_dlssnr.dll` gets past that.

The way to remove it is the **proxy path**: don't call the snippet directly, call the driver core's
`NVSDK_NGX_D3D12_CreateFeature(18)` and let the core call the snippet — the snippet then sees the core
(`_nvngx.dll`) as its caller and the check passes for free. This is how RenoDX avoids a forwarder: it
detours the core's Create/Evaluate rather than calling the snippet itself.

The proxy path is already past the caller check. It fails later, at feature creation, with
`0xBAD0000B FAIL_UnableToInitializeFeature`. Everything below is about that.

Each entry: what was tried, what the log said, what it rules out.

---

## What is established

- The caller check is **not** the proxy path's blocker. The proxy is past it; `0xBAD0000B` is a real
  "could not build the feature", downstream of the caller check.
- The core routes feature 18 (it does not answer "unknown feature"), so the snippet is being reached.
  It is the *initialisation* of the feature that fails.
- Re-initialising the core with `Init_Ext` is idempotent: it returns success and changes nothing,
  reporting the app id and SDK version the core first came up with. So the proxy path cannot change
  the app id or SDK version out from under the game's own DLSS. (log: "re-init at SDK 0x15 returned
  0x1 (idempotent)")
- The float setter lives at vtable slot 6 on the driver's capability block, same as the forwarder
  path finds. So the block is being driven correctly.

## Theories tried and disproven

### Warm-up retry — DISPROVEN (2026-09-01)
Feeder projects note the feature "re-creates a few seconds in, which normally clears" a failed state.
Tried: retry `CreateFeature(18)` up to 20 times, ~1 attempt / 20 frames, ~1.3s total, in Cyberpunk
with the game's own DLSS running (core fully warm).
Result: all 20 attempts returned `0xBAD0000B`, none succeeded.
Rules out: a transient warm-up window as the cause. The failure is stable, not timing.

## Theories not yet tried

- **Snippet discovery path.** The core loads snippets from the path list it was given at `Init` (the
  game's DLSS directory) plus the app directory. If `nvngx_dlssnr.dll` is not on a path the core
  searches, it finds feature 18 and has nothing to build it from -> UnableToInitialize. Since Init is
  idempotent we cannot add a path after the game's own Init; the snippet would have to sit where the
  core already looks. Test: place `nvngx_dlssnr.dll` beside the exe / in the game's DLSS plugin dir
  and check whether the core loads it (Streamline/NGX log should show the load).
- **Feature registration / discovery step.** The game only ever registers the SR/RR/FG snippets with
  the core, never NR. Feature 18 may need a discovery call (`GetFeatureRequirements` /
  `UpdateFeature`) before `CreateFeature` will build it. Test: call the D3D12 requirements query for
  feature 18 through the core before creating, and see whether that changes the create result.
- **Scratch buffer.** `CreateFeature` may need `GetScratchBufferSize(18)` satisfied first. Untested.

### The driver had no name for feature 18 — NOT YET TESTED, and it comes with a trap (2026-09-24)

Reported by a third party, not measured here. NIGos/dlss5-bridge (MIT) states that `_nvngx.dll`
carries a 19-entry table of feature id -> snippet name in writable data, and that **entry 18 reads
`L""` on driver 32.0.16.1656 and `"dlssnr"` on 32.0.16.1664** — the only entry that differs between
those two drivers in the whole table. Their account of the loader is that `NGXSecureLoadFeature`
loads nothing and returns `0xBAD00001` when the name is empty, and that 1664 is the first driver
whose loader routes feature 18 into `nvngx_dlssnr.dll` at all. They also note `DLSSNR.Available`
exists only in the newer file.

If that is right it is a simpler answer than any of the three above: up to 1656 the core has no name
to load the snippet by, so it knows feature 18 (hence not "unknown feature") and cannot build it
(hence `0xBAD0000B`), whatever the search paths, the registration step or the scratch buffer say.
That matches every established fact in this log without needing any of them. It is also why nothing
tried here moved the result: the missing piece was not ours to supply.

**The trap.** The same report says the version where this route starts working is the version where
it faults. On 1664/1686 with snippet **310.8.0.0** — the newest in circulation on their 2026-09-04 —
the snippet hands `ID3D12GraphicsCommandList::SetDescriptorHeaps` a descriptor heap whose
description reads back as garbage, D3D12 faults validating the node mask, and the game terminates or
stops presenting. They measured it on an RTX 5090 with a captured fault stack naming the snippet,
with neither add-on between it and D3D12, and their fix is to put `L""` back into entry 18 in memory
so the add-on drives feature 18 itself — i.e. to force the behaviour this forwarder already has.

So the shape to expect, if the hypothesis holds: `UseProxy=true` goes from a clean `0xBAD0000B` on
older drivers to *either* working *or* taking the game down, depending on the snippet, with no
version check anywhere on our side to tell the two apart.

Nothing is changed on the strength of this. It is one project's measurement on hardware we do not
have, and this repository has already shipped one default built on someone else's inference (the
v1.0.21 `RENDER_TARGET` barrier, reverted in `d66cc0f`). What it does change is how to run the test:

- Test on **32.0.16.1664 or newer**. On 1656 or older the result below is expected and proves
  nothing new.
- Record the **snippet version** (`nvngx_dlssnr.dll`'s file version) alongside the driver in any
  result, because the report makes the pairing the variable, not either half.
- Expect a possible hard fault rather than a return code, and do not test it first on a machine
  whose session matters.

If the pairing is real, a version gate on this path is worth having before it is ever defaulted on —
theirs self-heals by treating any snippet newer than the measured one as fixed, which is the right
shape: a gate that refuses forever outlives the bug.

## How to reproduce
Set `[DlssNr] UseProxy=true`. The path is off by default and does not fall back automatically, so a
failure is visible rather than masked by the forwarder quietly doing the work.

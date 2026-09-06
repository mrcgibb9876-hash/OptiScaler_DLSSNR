# DLSS 5 Neural Rendering — ReShade add-on

Draws one identical set of DLSS 5 Neural Rendering controls whichever engine is actually running the
pass — OptiScaler, or RenoDX (the DLSS5-Feeder toolchain's own neural consumer) — and whichever way
you reach them: docked in ReShade's own overlay (its own overlay key, alongside RenoDX's own tab), or
in a standalone window toggled with **Alt+Home**, independent of whether ReShade's overlay is open at
all. Styled to match the OptiScalerManager desktop app's own "Tune DLSS-NR" panel, so all three —
desktop, OptiScaler route, Feeder/RenoDX route — present the same product.

## What it is, and what it deliberately is not

**It does not run the pass.** It drives whichever engine already is.

That is not laziness, it is where the inputs are. The model needs the game's depth, motion vectors,
MV scale, jitter reset and pre-exposure — labelled, and at the right moment in the frame. Both
OptiScaler and RenoDX have all of that because each *is* the upscaler interceptor for its own route.
ReShade's add-on API hands an add-on draw calls, resources, a swapchain and the native device handle —
it has no idea which resource is motion vectors, and reimplementing either pass here would mean
feeding the model guesses and calling the result Neural Rendering.

So the pass stays wherever the truth is, and this reaches across to it.

## Two backends, chosen live

Checked every frame (not once at load, since either engine can finish loading after this add-on
does) — see `ResolveBackend()` in `dlssnr_reshade.cpp`:

1. **OptiScaler**, through the flat C ABI in [`../DlssNr_Api.h`](../DlssNr_Api.h), resolved by name
   at runtime with `GetProcAddress`. The add-on does not link against OptiScaler and cannot crash it
   by being out of date; an OptiScaler without the ABI is detected and reported, not crashed into.
   The ABI is keyed by string, using the ini names under `[DlssNr]` — settings added to OptiScaler
   later show up as keys this add-on happens not to draw yet, nothing breaks. `OptiNr_AbiVersion()`
   is checked before anything else; a mismatch is fatal on purpose, since carrying on would produce a
   panel that looks right and writes to the wrong places.
2. **RenoDX**, through ReShade's own config API (`get_config_value`/`set_config_value`) on the
   `RenoDX.DLSS5` section — the exact same store RenoDX itself reads and writes, so there is no
   cache/writeback race. RenoDX has no in-process control ABI of its own, so this is the only way in.
   Presence is checked by module name (`renodx-dlss5.addon64`), the same way OptiScaler's is.

If neither is found, the panel says so plainly rather than showing empty controls.

Both field tables live in [`fields.h`](fields.h), hand-kept in sync with the desktop app's own
`OPTISCALER_FIELDS`/`FEEDER_FIELDS` — same keys, same ranges, same conditionals (e.g. Compare's
Side-by-side/Wipe rows, Automask's Skin-structure slider), so nothing is missing relative to the
desktop panel on either route.

## Reaching it in game

- **ReShade's own overlay**: open it on whatever key your `ReShade.ini` binds (`KeyOverlay`, Home by
  default) and look for the **DLSS 5** tab.
- **Alt+Home**: opens a standalone window with the identical content, regardless of whether ReShade's
  own overlay is open. Useful when a game's ReShade build doesn't expose its own overlay key, or you
  just don't want to open the whole ReShade overlay for a quick tweak.

## Installing

1. Put `OptiScaler_DlssNr.addon64` next to the game executable, alongside a real add-on-capable
   ReShade build (the plain one has add-ons disabled).
2. For the OptiScaler route: both OptiScaler and ReShade have to load together, which needs a word of
   care — see below. For the Feeder/RenoDX route: RenoDX's own installer already gets ReShade loaded,
   nothing extra is needed here.

## Getting OptiScaler and ReShade to load together (OptiScaler route only)

They both want to be the library the game loads, usually `dxgi.dll`, and only one of them can be.
OptiScaler already knows how to resolve this — it will load ReShade itself once it has the hook:

1. Rename ReShade's DLL to `ReShade64.dll`, next to OptiScaler.
2. Set `LoadReShade=true` under `[Plugins]` in `OptiScaler.ini`.

You should see ReShade's boot notification. The [OptiScaler
wiki](https://github.com/optiscaler/OptiScaler/wiki/Compatibility-with-other-mods-(Reshade,-SpecialK))
has two other arrangements (a `plugins` folder, or Ultimate ASI Loader) if that one does not take in
a particular game.

## Building

Two header sets are needed that are **not** vendored in this repo:

| | |
|---|---|
| ReShade add-on SDK | `reshade.hpp` and friends, from [crosire/reshade](https://github.com/crosire/reshade) — the `include/` folder |
| Dear ImGui | headers only, **docking branch, v1.92.2b** — the version ReShade's overlay is built with |

The ImGui version has to match the one your ReShade build shipped. ReShade answers
`ReShadeGetImGuiFunctionTable` only for version numbers it knows, so a mismatch makes
`register_addon` return false and the add-on quietly fails to load — no crash, no message, just an
absent tab and no Alt+Home. If nothing appears and OptiScaler or RenoDX is definitely running,
suspect this first.

Note the ImGui vendored *inside* OptiScaler is a different copy for a different context, and is not
the one to build against here.

**ImGui is not compiled in.** Only the headers are needed; including `reshade.hpp` *after* `imgui.h`
rebinds every ImGui function to the instance ReShade already created. Linking a second ImGui in would
give the add-on its own context and nothing would draw.

```
msbuild dlssnr_reshade.vcxproj /p:Configuration=Release /p:Platform=x64 ^
        /p:ReShadeSdkDir=C:\src\reshade\include ^
        /p:ImGuiDir=C:\src\imgui
```

Defaults to `external\reshade\include` and `external\imgui-docking` under the solution directory if
you would rather clone them there. Output is `OptiScaler_DlssNr.addon64` in the usual release folder.

## Registered add-on name

Deliberately **not** `"DLSS 5 Neural Rendering"` — RenoDX's own add-on (`renodx-dlss5.addon64`)
already registers under that exact name, and ReShade allows only one add-on per name; the second one
to load fails outright (`Failed to register add-on... already registered`, error 1114), taking its
whole feature down with it. Confirmed the hard way on a real install where this add-on's own earlier
build did exactly that to RenoDX. This add-on registers as `"OptiScaler DLSS 5 Neural Rendering"`
instead — the overlay tab title (`"DLSS 5"`, via `register_overlay`) is a separate string and did not
need to change.

## Where things sit in the frame

Ordering matters and is worth stating, because it explains what each tool can and cannot see:

```
upscaler (DLSS / FSR / XeSS)
    -> DLSS 5 Neural Rendering        <- the pass this add-on's controls drive
        -> frame generation
            -> the game's own HUD and post
                -> ReShade's effects
                    -> present
```

ReShade's effects run on the finished frame, so they see the model's work and shade on top of it.
That is the sane order and needs nothing configured. It does mean a ReShade effect that changes
tone — an HDR mod, say — changes what the pass's white-point measurement is later compared against
by eye, so set the white point first and the effects second.

## What it shows

Everything in `fields.h`'s two tables — general, global controls, model automask, models, cost, how
much of it lands, colour, guide, inspect (including the compare side-by-side/wipe/zoom/split
controls) and experimental — matching the desktop app's own panel field for field. The OptiScaler
route additionally shows live status (running / GPU ms / a failure reason with Retry) above the
fields, since that comes from OptiScaler's own status query and has no RenoDX equivalent.

Not here: OptiScaler's exposure-scan anchoring *workflow* itself (as opposed to its settings, which
are) and frame generation — those live in OptiScaler's own full in-process panel, which can be up at
the same time as this one.

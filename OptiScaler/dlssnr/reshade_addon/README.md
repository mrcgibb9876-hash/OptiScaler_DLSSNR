# DLSS 5 Neural Rendering — ReShade add-on

Draws OptiScaler's DLSS 5 controls inside ReShade's own overlay, so a ReShade user gets them on the
key they already press, in one ImGui context, with no second window to arrange around.

## What it is, and what it deliberately is not

**It does not run the pass.** It drives the one running inside OptiScaler.

That is not laziness, it is where the inputs are. The model needs the game's depth, motion vectors,
MV scale, jitter reset and pre-exposure — labelled, and at the right moment in the frame. OptiScaler
has all of it because it *is* the upscaler interceptor: the game hands it the NGX parameter block
with every input named, and the pass runs on the upscaled colour before frame generation sees it.

ReShade's add-on API hands an add-on draw calls, resources, a swapchain and the native device
handle. It has no idea which resource is motion vectors — the game never tells it — and for depth it
offers a per-game heuristic that needs a UI of its own because it is wrong often enough to matter.
An add-on that ran the model itself would be feeding it guesses and calling the result Neural
Rendering.

So the pass stays where the truth is, and this reaches across to it.

## How it talks to OptiScaler

Through the flat C ABI in [`../DlssNr_Api.h`](../DlssNr_Api.h), resolved by name at runtime with
`GetProcAddress`. Consequences worth knowing:

- The add-on does not link against OptiScaler and cannot crash it by being out of date.
- An OptiScaler without the ABI — upstream, or an older build of this fork — is detected and
  reported in the overlay, not crashed into.
- The ABI is keyed by string, using the ini names under `[DlssNr]`. Settings added to OptiScaler
  later show up as keys this add-on happens not to draw. Nothing breaks.
- A control whose key the running OptiScaler does not have greys itself out and says so, rather than
  pretending to work.

`OptiNr_AbiVersion()` is checked before anything else. A mismatch is fatal on purpose: that is
exactly the case where carrying on produces a panel that looks right and writes to the wrong places.

## Building

Two header sets are needed that are **not** vendored in this repo:

| | |
|---|---|
| ReShade add-on SDK | `reshade.hpp` and friends, from [crosire/reshade](https://github.com/crosire/reshade) — the `include/` folder |
| Dear ImGui | headers only, **docking branch, v1.92.2b** — the version ReShade's overlay is built with |

The ImGui version has to match the one your ReShade build shipped. ReShade answers
`ReShadeGetImGuiFunctionTable` only for version numbers it knows, so a mismatch makes
`register_addon` return false and the add-on quietly fails to load — no crash, no message, just an
absent tab. If the DLSS 5 tab does not appear and OptiScaler is definitely running, suspect this
first.

Note the ImGui vendored *inside* OptiScaler is a different copy for a different context, and is not
the one to build against here.

**ImGui is not compiled in.** Only the headers are needed; including `reshade.hpp` *after* `imgui.h`
rebinds every ImGui function to the instance ReShade already created. Linking a second ImGui would
give the add-on its own context and nothing would draw.

```
msbuild dlssnr_reshade.vcxproj /p:Configuration=Release /p:Platform=x64 ^
        /p:ReShadeSdkDir=C:\src\reshade\include ^
        /p:ImGuiDir=C:\src\imgui
```

Defaults to `external\reshade\include` and `external\imgui-docking` under the solution directory if
you would rather clone them there. Output is `OptiScaler_DlssNr.addon64` in the usual release folder.

## Installing

1. Put `OptiScaler_DlssNr.addon64` next to the game executable, beside ReShade.
2. Make sure you have a ReShade build **with add-on support** — the plain one has add-ons disabled.
3. Both OptiScaler and ReShade have to load, which needs a word of care — see below.
4. In game, open ReShade's overlay. The controls are under the **DLSS 5** tab.

## Getting OptiScaler and ReShade to load together

They both want to be the library the game loads, usually `dxgi.dll`, and only one of them can be.
OptiScaler already knows how to resolve this — it will load ReShade itself once it has the hook:

1. Rename ReShade's DLL to `ReShade64.dll`, next to OptiScaler.
2. Set `LoadReShade=true` under `[Plugins]` in `OptiScaler.ini`.

You should see ReShade's boot notification. The [OptiScaler
wiki](https://github.com/optiscaler/OptiScaler/wiki/Compatibility-with-other-mods-(Reshade,-SpecialK))
has two other arrangements (a `plugins` folder, or Ultimate ASI Loader) if that one does not take in
a particular game.

## Where things sit in the frame

Ordering matters and is worth stating, because it explains what each tool can and cannot see:

```
upscaler (DLSS / FSR / XeSS)
    -> DLSS 5 Neural Rendering        <- the pass this add-on controls
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

A working subset: enable, status and cost, the strengths, the white point and its source, the
highlight guard, the reversible proxy, model resolution, and the compare controls.

Not here: the exposure-scan anchoring workflow, model presets and styles, and frame generation.
Those are in OptiScaler's own panel, which opens on its own key (Alt+Home by default) and can be up at
the same time as ReShade's overlay. The add-on says so rather than leaving you to wonder.

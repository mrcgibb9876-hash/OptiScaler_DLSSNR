# Authorship and licensing

This repository is a fork of [OptiScaler](https://github.com/optiscaler/OptiScaler). It is
distributed under the **GNU General Public License, version 3** — the same licence as the project
it is derived from. See `LICENSE`. Nothing in this file changes that, and nothing in it restricts
any right the GPL grants you.

What this file does is record **who wrote what**, because until now the tree said nothing about it.

## Original to this fork

Copyright (c) 2026 mrcgibb9876-hash.

The DLSS 5 Neural Rendering subsystem is original work written for this fork. Upstream OptiScaler
is an upscaler and contains no part of it: neither `OptiScaler/dlssnr/` nor
`OptiScaler/shaders/dlssnr/` exists upstream at all.

As of 2026-09-24 that is **50 source files, about 21,500 lines**, comprising:

| Area | What it is |
| --- | --- |
| `OptiScaler/dlssnr/` | The NR pass: the D3D12 and Vulkan features, the panel, live stats, exposure scanning, depth tracking, budgeting, the i18n layer |
| `OptiScaler/dlssnr/forwarder/` | The shim that satisfies the model's caller check |
| `OptiScaler/shaders/dlssnr/` | The NR shaders |
| `OptiScaler/shaders/output_scaling/` (fork-only files) | Output scaling added by this fork |
| `OptiScaler/dlssnr/opticalflow/` (fork-only files) | The optical-flow integration, excluding the vendored SDK below |
| `tests/` | The fork's own tests |

Beyond those whole files, this fork has also modified upstream files to integrate the above. Those
changes are contributions to a GPL work and are covered by the same copyright and the same licence.

Each original file now carries a two-line header saying so. Under **GPL-3.0 section 5(a)** anyone
conveying a modified version must keep those notices, and under **section 7(b)** the requirement to
preserve author attributions is an explicitly permitted additional term. Removing them is not
something the licence allows.

## Not original to this fork

These are present in the tree but were written by others, and are **excluded from the claim above**.
They carry their own copyright and their own licences, which continue to apply:

| Component | Owner / licence |
| --- | --- |
| Everything inherited from upstream OptiScaler | its authors, GPL-3.0 |
| `external/nvapi/` | NVIDIA |
| `OptiScaler/include/` (FSR2 headers, `d3dx12.h`) | AMD; Microsoft |
| `OptiScaler/dlssnr/opticalflow/fidelityfx/sdk/` | AMD FidelityFX SDK, MIT |
| `OptiScaler/dlssnr/opticalflow/shaders/optical/` | generated from AMD FidelityFX 1.1.4, MIT |
| `**/precompile/` | compiled shader blobs, build output rather than authored source |
| `OptiScaler/dlssnr/DlssNr_I18n_Tables.cpp` | generated |

That amounts to roughly 98,500 lines of the tree. It is listed here so the claim above is a
narrow and checkable one: a copyright notice that swept in other people's code would be worth
less than no notice at all.

## Verifying this

Nothing here has to be taken on trust. Clone upstream and compare:

```
git clone --depth 1 https://github.com/optiscaler/OptiScaler
ls OptiScaler/OptiScaler/dlssnr           # does not exist upstream
ls OptiScaler/OptiScaler/shaders/dlssnr   # does not exist upstream
```

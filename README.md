# RenoDX DLSS Neural Rendering add-on

Source for the generic RenoDX/ReShade add-on that runs NVIDIA DLSS Neural Rendering
(DLSSNR, NGX feature 18) around a game's existing DLSS evaluation. DLSSNR is a
visual enhancer; it performs no denoising.

This fork contains addon source and a Windows GitHub Actions build workflow.
RenoDX shared utilities and vendor SDKs are fetched from a pinned official
checkout; the ReShade loader and NVIDIA runtime are not bundled. The files are designed to live at `src/addons/dlss5` in a
compatible [RenoDX](https://github.com/clshortfuse/renodx) source checkout. A
compatible full checkout and its toolchain are required to build the `dlss5` target.

| Branch | Source version | Status |
| --- | --- | --- |
| `main` | v7.5.0-rc5 | Experimental v7.5 maintenance source |
| `v7.6` | v7.6.0-rc3 | Backport candidate; qualification failed, not a release |
| `v8.5dev` | v8.5.0-rc10 | Development snapshot, not a release |

These branches are source snapshots. No branch here implies that a binary has
passed field qualification. The add-on's optional **Upload & copy link** action
sends an issue-report archive to a third-party temporary file host only after
the player selects that action.

This source derives from RenoDX and retains its MIT license and original
copyright notice in [LICENSE](LICENSE). Dependencies have their own licenses
and are not included here.

## Experimental automatic Present fallback

This working copy adds `NRAutoPresentFallback` (off by default), intended for
Alan Wake Remastered's Direct3D 12 menus and prerendered cinematics. Keep the
hook point on **Upscaled**, then enable **Present fallback without DLSS**.
Equivalent settings in the existing `[RenoDX.DLSS5]` section of `ReShade.ini`:

```ini
NRHookPoint=0
NRAutoPresentFallback=1
```

Successful intercepted SR/RR evaluations keep the existing Upscaled path,
even if NR declines that evaluation. Once those evaluations stop, fallback
waits until command-list recordings containing inline NR have been reset or
destroyed and their GPU submissions have completed. It then passes through
one complete cycle of swapchain buffers before using the existing Present
path. The first Present NR feature creation also needs its usual warmup.
This is conservative episode switching, **not an exact per-visible-frame
DLSS detector**.

Fallback always uses neutral depth and motion, with no gameplay exposure or
jitter. It uses Present's encoding, HDR-white and HUD-correction settings;
Present's captured-guide and frame-generation settings do not apply to it.
The finished game frame includes menus, HUD and subtitles. ReShade's effects
and overlay still run afterwards.

The next observed successful SR/RR evaluation cancels a pending fallback and
returns to Upscaled immediately. Both transitions reset temporal and
normalization history through the existing reset epoch. They do not change
the saved hook point or request feature recreation. The last gameplay stream
and Present stream are exempt from idle retirement while the option is on;
this retains additional GPU memory. Existing workset caps, resolution/format
contract checks and device teardown still apply.

The initial implementation is deliberately limited:

- Direct3D 12, one presenting swapchain/device, no detected frame generation.
  Multiple observed presentation streams disable fallback until routing is
  reset. Observed frame generation disables it until device teardown.
- The NGX serialization turn and a revision ticket guard the decision,
  including revalidation after releasing locks for a GPU-ring wait. This does
  not map arbitrary worker-produced textures to final visible frames.
- A game that parks a replayable gameplay command list throughout a video
  will keep waiting. The overlay reports “waiting for DLSS command lists to
  retire.” A timer cannot safely prove that recording will never execute again.
- The addon cannot identify a previously enhanced texture that the game
  later copies into another frame. Games presenting cached enhanced images,
  or unobserved third-party frame generation, need additional provenance
  tracking. Alan Wake Remastered's actual ordering still needs a game test.
- Existing NGX serialization has a 750 ms game-call timeout after which it
  allows an unprotected call. This change preserves that existing policy;
  timeout-free operation must be checked during runtime qualification.

`tests/present_fallback_test.cpp` exercises continuous/multiple evaluates,
cinematic and startup-menu entry, pending/replayable work, repeated buffer
indices, resumed evaluation during waits, resize/format changes, ambiguous
streams and buffer-count bounds. Run the portable routing tests with:

```sh
clang++ -std=c++20 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -g tests/present_fallback_test.cpp \
  -o /tmp/renodx-present-fallback-test
/tmp/renodx-present-fallback-test
```

The routing tests have passed on macOS. The Windows workflow also runs them
and Windows COM identity/Detours contention tests before compiling the addon.
A successful CI build does **not** qualify game/GPU behavior.
In-game validation should cover gameplay → cinematic → gameplay, startup
menus, repeated transitions, F6 toggling, F5 comparisons, alt-tab and changes
to output size/HDR. Check both transition log messages and the fallback status
in the overlay. Disabling the option restores the original routing policy.

## Building a Windows `.addon64`

You do not need Windows locally. Open this fork's **Actions** tab, select a
successful **Build Windows addon** run for `aw-remastered-auto-present`, and
download `renodx-dlss5-aw-fallback1-windows-x64` under **Artifacts**. Extract
`renodx-dlss5.addon64` from the ZIP. The workflow runs on pushes to that branch;
GitHub may require enabling Actions on a new fork first. Artifacts expire after
30 days. `build-info.json` records both source revisions and the binary SHA-256.

The workflow stages these sources at `src/addons/dlss5` in official RenoDX
revision `374d07bf0e2fcef112ed1dfee98541bcf16487dd`, initializes dependencies,
installs shader compilers with RenoDX's setup script, runs CPU tests, and builds
the `dlss5` target using clang-cl and the Windows x64 SDK.

The original snapshot calls five shared utilities absent from that RenoDX
revision. This fork supplies scoped equivalents in `native_identity.hpp` and
`detour_transaction.hpp`, without modifying RenoDX shared code:

- COM identity unwraps ReShade through the existing public helper, queries
  canonical IUnknown identity, and balances temporary references. Returned
  pointers are borrowed and require the source object to remain alive.
- All addon hook groups share a transaction mutex. A pending Detours transaction
  causes a failed/retryable begin and increments contention telemetry; it never
  aborts the existing transaction. The lock covers only this addon, not other DLLs.

These follow [COM identity rules](https://learn.microsoft.com/en-us/windows/win32/com/rules-for-implementing-queryinterface)
and [Detours transaction semantics](https://github.com/microsoft/Detours/wiki/DetourTransactionBegin).
`tests/windows_compat_test.cpp` checks distinct interface pointers, proxy/native
identity, reference balance, and a contending caller leaving the owner intact.

For a local Windows build, use an **x64 Visual Studio Developer PowerShell**
with C++ tools, Windows SDK, LLVM/clang-cl, CMake and Ninja. Clone official RenoDX
at the pinned revision with submodules and run its shader-tool setup. Then:

```powershell
.\scripts\build-addon.ps1 -RenoDXRoot C:\src\renodx -Configuration Release
```

The script preserves unrelated existing addon sources and runs the official
CMake presets. Output: `build/Release/renodx-dlss5.addon64` in the RenoDX checkout.
See [RenoDX's build guide](https://github.com/clshortfuse/renodx/blob/374d07bf0e2fcef112ed1dfee98541bcf16487dd/docs/CONTRIBUTING.md)
for toolchain setup and `.github/workflows/build-windows.yml` for the exact CI steps.

When testing, back up your current addon and `ReShade.ini`. Keep only one DLSS5
addon loaded; replace it with the compiled file and retain your working NVIDIA
NR runtime setup. Keep **Upscaled** selected and enable **Present fallback
without DLSS**. Preset controls remain unchanged. Look for the fallback status
in menus/cinematics and its return to Upscaled in gameplay. Report a persistent
“waiting for DLSS command lists to retire” message along with the ReShade log;
that means this conservative routing proof needs further game-specific work.

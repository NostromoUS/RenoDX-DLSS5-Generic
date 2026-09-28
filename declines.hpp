/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The decline taxonomy: every way an evaluate can end without NR running.
//
// The compatibility policy behind it is that the worst failure mode of this
// mod is silently doing nothing.  So every evaluate-path terminal bumps a
// NAMED counter and logs its first occurrence, Shutdown prints the session
// totals, and the verdict line (funnel.hpp) reports the largest one.  A
// terminal with no counter is the bug this table exists to prevent: it makes
// the engagement ratio a guess, because `eligible - injected - sum(declines)`
// stops being zero (T-SILENT).
//
// This header is deliberately free of addon dependencies so test/dlss5 can
// include it and pin the table: the tags are a field contract - they appear
// in telemetry that users paste and tools parse - and an array that falls out
// of step with the enum is a silent mislabelling of every counter after the
// gap.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace renodx::addons::dlss5 {

enum class NrDeclineReason : uint8_t {
  kNgxNotDlssEvaluation = 0,
  kNgxMissingGuides,
  kNgxOutputGeometry,
  kPreSrGeometry,
  kStreamlineCaptureIncomplete,
  kStreamlineResourceStates,
  kStreamlineRuntimeUnavailable,
  // v6 audit issue 23: gates that previously declined (or bypassed) without
  // any accounting - a silent-looking session must be explainable from the
  // counters alone.
  kDuplicateOutputThisPresent,
  kStateShadowUnavailable,
  kPreSrNoStateTarget,
  kWorksetSetupFailed,
  kSlotWarmupMaturation,
  // Renamed from kHardInvalidBypass (v6.8.0-alpha4).  The "hard-invalid
  // evidence state" it was named for is a v5-era concept that no longer
  // exists; the terminal that DOES exist on that code path is the user
  // setting every pass to zero strength, which is an exact passthrough by
  // design.  Renaming costs nothing: neither reason was ever counted, so
  // the tag has never appeared in a log (checked against all 211 archived
  // sessions) and cannot be a field contract.
  kZeroStrengthPassthrough,
  // v6.5.0: frames skipped while a failed slot waits out its retry
  // interval - previously an uncounted `return false`.
  kFailureBackoff,
  // v6.5.2: an enclosing hook of a call chain whose inner hook already
  // injected (module-copy forwarding, C export, Streamline around NGX).
  // By design once per frame per extra hooked layer - split from
  // kDuplicateOutputThisPresent so that one only counts real skips.
  kNestedMirror,
  // v6.8.0-alpha23: the signed NR runtime is not on the machine, so
  // EnsureDirectRuntime fails and the evaluate ends.  Both injection paths
  // returned false here without counting anything, which is the single most
  // likely state for a machine this mod has never worked on - measured by
  // the `no_nr_runtime` lane on its first run: 240 eligible evaluates, one
  // named decline, `unaccounted=239`.  The verdict already said UNAVAILABLE
  // and was right; the funnel underneath it still leaked, and T-SILENT is
  // the promise that it does not.
  kNrRuntimeUnavailable,
  // v6.8.0-alpha34 (item 19): the evaluate wrapper's unnamed early-outs.
  // The disabled early-out is the big one - every evaluate that streamed
  // past a live hook set while NR was off landed in `unaccounted` (7807 of
  // them in the MSFS2024 field log of a healthy session), so its ratio read
  // 0.31 forever.  game_failed and malformed close the rest of the v6.7.4
  // audit's "Uncounted" family (Appendix C).
  kNrDisabledEvaluation,
  kGameEvaluateFailed,
  kMalformedEvaluate,
  // P4 Path A taxonomy (PLAN_DX11_V68.md 4.3/4.4): the evaluate belongs to
  // another DX11 source - under DX11Source=native (B16/P5) a foreign tool's
  // D3D12 evaluate is not ours to inject.  INELIGIBLE: it never competes
  // for the engagement ratio.  Named in the taxonomy in P4 so the build
  // that wires the native path cannot mislabel it; nothing counts it in P4
  // - the shipped 'foreign' default serves every D3D12 evaluate and names
  // no decline (the double-processing ride is the intended Path A behavior;
  // any gate on it is P5 B7, observe-only first).
  kSourceOther,
  // v7.0.0 (A-1): the evaluate's first argument is not a readable object -
  // the Skyrim SE class, a third-party caller's sentinel such as 0xFFFFFFFF
  // (arg_plausibility.hpp).  The wrapper skips every dereference of its own
  // and hands the call to the real export, so the frame is not a usable one:
  // INELIGIBLE, like kMalformedEvaluate.
  kImplausibleArgument,
  // v7.0.0-alpha45, Path B (PLAN_DX11_V68.md 4.4): the Direct3D 11 bridge's
  // terminals.  All ELIGIBLE - each is a real game DLSS frame the bridge
  // could have served and did not, so each counts against the ratio and
  // competes for the top-decline tag.  The frame keeps the game's image.
  //   bridge_down    - the bridge is not up (starting, backing off after a
  //                    failed start, a second immediate context), or a
  //                    surface set backs off after an allocation failure
  //                    (rc11, LIFE-11: no longer latched as bridge_format);
  //   bridge_format  - the frame's surfaces cannot cross to D3D12 (format,
  //                    multisampling, a foreign device);
  //   bridge_busy    - every submission in the ring is still on the GPU;
  //   bridge_timeout - the watchdog released a stalled D3D12 submission;
  //   bridge_lost    - the private device was removed or stalled too often;
  //   d3d11_deferred - DLSS was evaluated on a deferred context.
  kBridgeDown,
  kBridgeFormat,
  kBridgeBusy,
  kBridgeTimeout,
  kBridgeLost,
  kD3D11Deferred,
  // v7.0.0: another Neural Rendering producer created a feature-18 instance
  // in this process (a second NR mod, or a DLSSNR-aware tool), and with
  // ForeignNr=yield this addon stands down so no frame is enhanced twice.
  // INELIGIBLE, like kNrDisabledEvaluation: the stand-down is deliberate.
  kForeignNr,
  // v7.0.0-rc9: the swapchain NR was initialized with was destroyed while
  // recorded NR work had not completed, and this evaluate is on another
  // device.  NR state is released once that work completed, and the next
  // evaluate after the release re-arms; until then NR on the other device
  // would release it early.  An evaluate on the same device resumes instead.
  kTeardownPending,
  // rc11 (NRHookPoint=2, the Present hook point; present_path.hpp).  The
  // units are presents; the game's evaluates only lend their guides.
  // INELIGIBLE:
  //   present_hook      - a game DLSS evaluate: not a unit of NR work, NR
  //                       runs at the present;
  //   present_format    - a present whose back buffer cannot be served
  //                       (colour space, format, or the present ring could
  //                       not be built; v8.5.0-rc5: or frame generation that
  //                       pre-FG cannot serve, present[prefg_upscaled=]); the
  //                       evaluates then run the Upscaled hook point instead,
  //                       so they are the units;
  //   present_repeat    - NRPresentFrames=1 and this present repeats a game
  //                       frame (frame generation): NR ran on the first.
  // ELIGIBLE (the frame keeps the game's image):
  //   present_busy      - every present submission in the ring is still on
  //                       the GPU;
  //   present_no_guides - NRPresentGuides=Required and no guide capture
  //                       matches this present.
  kPresentHook,
  kPresentBusy,
  kPresentFormat,
  kPresentNoGuides,
  kPresentRepeat,
  // v8 (LIFE-12): NR's runtime is initialized on one device face, and this
  // evaluate is recorded in another (EnsureDirectRuntime).  ELIGIBLE: the
  // frame keeps the game's image and the next evaluate retries.
  //   runtime_switch_pending   - a different device, or a face that has
  //                              settled: re-initializing releases every
  //                              feature and workset, so it waits until the
  //                              work NR recorded on the old one is proven
  //                              complete;
  //   runtime_face_alternation - the other face of the same device, which
  //                              has not yet been the only face for 8
  //                              evaluates in a row (two paths alternating
  //                              worlds never switch).
  kRuntimeSwitchPending,
  kRuntimeFaceAlternation,
  // v8.0.0 hardening (P1): the after path's guide-dimension early-out.  The
  // per-evaluate block, the create contract and the Color resource all left
  // the guide dimensions unknown, so there is nothing to run NR at; the
  // terminal set last_result and returned false with a one-shot warning but
  // no counter, which is exactly the unaccounted remainder T-SILENT promises
  // never to leak.  ELIGIBLE, like kNgxOutputGeometry: a real game DLSS frame
  // this addon could have served (EnableHooks=1 is the usual fix).
  kNoGuideDims,
  // v8.0.4 (NRPresentPreFg, present_path.hpp RunPresentPreFg).  INELIGIBLE:
  //   present_prefg - a present of a game frame NR already ran on before
  //                   DLSS-G interpolated it; the unit is that game frame.
  kPresentPreFg,
  // v8.1 (vulkan_path.hpp): the game's VkDevice did not enable a device
  // extension the NR runtime's GetFeatureDeviceExtensionRequirements names
  // for feature 18.  An extension cannot be enabled after vkCreateDevice, so
  // the session keeps the game's image; the log names the extension once.
  // ELIGIBLE: a real DLSS frame NR could not serve.
  kVkExtensionMissing,
  // v8.1: a Vulkan DLSS evaluate arrived before the game's NGX Vulkan Init
  // was observed, so the VkInstance and VkPhysicalDevice NR's runtime Init
  // needs are unknown (ReShade's add-on API exposes neither); the frame
  // keeps the game's image and the next evaluate retries.  ELIGIBLE, so a
  // session whose Init predates the hooks reads NOT_ENGAGED with this tag,
  // never a silent zero.
  kVkNrPending,
  // F5 temporarily reserves an untouched final frame while an inline hook
  // is selected. These presents deliberately pass through until that
  // request's presentation-queue proof exists (or the comparison is
  // reported unavailable), so they are INELIGIBLE like NR-off frames.
  // Normal Present admission failures retain their existing terminals.
  kScreenshotWait,
  // v8.5.0-rc2 (vulkan_path.hpp ProcessNr): a Vulkan DLSS evaluate recorded
  // on a device other than the one NR's runtime, codec and readback belong
  // to.  Continuing would bind the owner's pipelines and buffers in the
  // other device's command buffer (illegal API use), so the frame keeps the
  // game's image; ownership moves when the owner device is destroyed.
  // ELIGIBLE: a real DLSS frame NR could not serve.
  kVkOwnerMismatch,
  kCount,
};
inline std::atomic_uint64_t nr_decline_counts[
    static_cast<size_t>(NrDeclineReason::kCount)] = {};
inline const char* const kNrDeclineNames[
    static_cast<size_t>(NrDeclineReason::kCount)] = {
    "NGX evaluate is not DLSS/DLSSD",
    "NGX DLSS parameter block missing Output/Motion/Depth guides",
    "NGX output geometry unsupported (dimension/array/samples)",
    "pre-SR color geometry unsupported (dimension/mips/array/samples)",
    "Streamline capture missing color/output/motion/depth",
    "Streamline resource states not injectable",
    "Streamline direct runtime unavailable",
    "output already NR-processed this present (mirror/duplicate evaluate)",
    "compute-state restore target incomplete (hooks/heaps/root signature/"
    "root arguments/PSO unobserved)",
    "pre-SR: compute-state restore target incomplete",
    "workset/codec setup failed",
    "slot warm-up (frame-ahead maturation, image passes through)",
    "every pass at zero strength (exact passthrough: the image is left"
    " untouched by design)",
    "failed NR slot waiting out its retry interval",
    "enclosing hook of an evaluate an inner hook already injected (benign)",
    "the signed NR runtime (nvngx_dlssnr.dll) is not available in this"
    " process",
    "NR is switched off; the game's DLSS evaluate passes through untouched",
    "the game's own NGX evaluate returned an error (no frame to enhance)",
    "malformed evaluate call (null handle or parameters)",
    "the evaluate belongs to another DX11 source (DX11Source=native: a"
    " foreign tool's D3D12 evaluate is not ours to inject)",
    "the evaluate's first argument is not a readable object (passed to the"
    " real export untouched; this addon skips it)",
    "the Direct3D 11 bridge is not up (starting, backing off, or a second"
    " immediate context)",
    "the frame's surfaces cannot cross from Direct3D 11 to Direct3D 12"
    " (format, multisampling or device)",
    "every Direct3D 11 bridge submission is still on the GPU",
    "a Direct3D 12 bridge submission stalled and the watchdog released it",
    "the Direct3D 11 bridge was lost (device removed or repeated stalls)",
    "DLSS was evaluated on a deferred Direct3D 11 context",
    "another Neural Rendering producer is active in this process; this addon"
    " stands down (ForeignNr=yield)",
    "NR is waiting for the destroyed swapchain's recorded work to finish"
    " before it releases and re-arms",
    "Present hook point: NR runs on the swapchain at the present; this DLSS"
    " evaluate only lent it its guides",
    "every Present hook point submission is still on the GPU",
    "the swapchain's back buffer cannot be served at the Present hook point"
    " (colour space, format, the present ring, or frame generation NR cannot"
    " reach before it interpolates); the DLSS evaluates run the Upscaled hook"
    " point instead",
    "NRPresentGuides=Required and no DLSS guide capture matches this present",
    "NRPresentFrames=1: this present repeats a game frame (frame generation);"
    " NR ran on the first present of the frame",
    "NR's runtime waits to re-initialize for another device or device face"
    " until the work it recorded on the old one is proven complete",
    "the evaluate is recorded in the other face of NR's device (ReShade's"
    " proxy or the native device); NR switches only after 8 evaluates in a"
    " row there",
    "the game's NGX contract names no guide (input) dimensions to run NR at"
    " (set EnableHooks=1 if the game uses NVIDIA Streamline)",
    "Present hook point: NR ran on this game frame before frame generation"
    " interpolated it (NRPresentPreFg); its presents run none",
    "the game's Vulkan device did not enable a device extension the NR"
    " runtime needs for feature 18 (named once in the log)",
    "a Vulkan DLSS evaluate arrived before the game's NGX Vulkan Init was"
    " observed, so NR's runtime cannot start yet (retried every evaluate)",
    "F5 comparison deliberately passed through this present while waiting"
    " for an untouched final frame with a matching presentation-queue proof",
    "a Vulkan DLSS evaluate on a second device; NR's runtime and codec belong"
    " to the first device until it is destroyed",
};
// Short tags for the periodic telemetry line (same order).
inline const char* const kNrDeclineTags[
    static_cast<size_t>(NrDeclineReason::kCount)] = {
    "not_dlss", "no_guides", "out_geom", "presr_geom", "sl_capture",
    "sl_states", "sl_runtime", "duplicate", "state_target",
    "presr_state_target", "setup_failed", "warmup", "zero_strength",
    "backoff", "mirror", "no_runtime", "nr_off", "game_failed",
    "malformed", "source_other", "implausible_arg", "bridge_down",
    "bridge_format", "bridge_busy", "bridge_timeout", "bridge_lost",
    "d3d11_deferred", "foreign_nr", "teardown_pending", "present_hook",
    "present_busy", "present_format", "present_no_guides", "present_repeat",
    "runtime_switch_pending", "runtime_face_alternation", "no_guide_dims",
    "present_prefg",
    "vk_extension_missing", "vk_nr_pending",
    "screenshot_wait", "vk_owner_mismatch",
};
inline void CountNrDecline(NrDeclineReason reason) {
  nr_decline_counts[static_cast<size_t>(reason)].fetch_add(
      1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Which aspect of the restore target was missing
// ---------------------------------------------------------------------------
//
// "state_target" is the decline that has eaten whole field sessions, and on
// its own it does not say what to fix.  The five aspects below are what an
// injection must be able to put back, and the answer "root_arguments was the
// one we never observed" is the difference between a triage and a guess -
// v6.7.2 declined 100% of AW2 and KCD2 evaluates on exactly one of these.
//
// Counted per decline, not once per session: a session whose missing set
// CHANGES is a different story from one that is missing the same thing all
// along, and the one-shot WARN that shipped until v6.8.0 could not tell them
// apart.
enum class GateAspect : std::uint8_t {
  kHooks = 0,
  kHeaps,
  kRootSignature,
  kRootArguments,
  kPipelineState,
  kCount,
};

inline constexpr std::size_t kGateAspectCount =
    static_cast<std::size_t>(GateAspect::kCount);

// Bare identifiers: these go into the same flat key=value telemetry the
// decline tags do.
inline const char* const kGateAspectTags[kGateAspectCount] = {
    "hooks", "heaps", "root_signature", "root_arguments", "pso",
};

inline std::atomic_uint64_t nr_gate_missing_counts[kGateAspectCount] = {};

// Bit i set = aspect i was NOT observed, so the restore could not write it.
inline void CountGateMissing(std::uint32_t missing_mask) {
  for (std::size_t i = 0; i < kGateAspectCount; ++i) {
    if ((missing_mask & (1u << i)) != 0) {
      nr_gate_missing_counts[i].fetch_add(1, std::memory_order_relaxed);
    }
  }
}

// The aspect missing most often, or null when the gate has never declined.
// This is what the verdict line reports, because it is the one thing a
// reporter can be asked for that actually narrows the cause.
inline const char* TopMissingGateAspect() {
  std::uint64_t best = 0;
  const char* tag = nullptr;
  for (std::size_t i = 0; i < kGateAspectCount; ++i) {
    const std::uint64_t count =
        nr_gate_missing_counts[i].load(std::memory_order_relaxed);
    if (count > best) {
      best = count;
      tag = kGateAspectTags[i];
    }
  }
  return tag;
}

// The number of terminals, for tests and for anyone sizing an array.
inline constexpr std::size_t kNrDeclineCount =
    static_cast<std::size_t>(NrDeclineReason::kCount);

}  // namespace renodx::addons::dlss5

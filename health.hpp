/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The health ledger: every way NR can keep running while doing less than the
// player thinks, as one table (rc11).
//
// The verdict (funnel.hpp) answers "is NR running", and the card
// (ui/state_card.hpp) says it in the player's words.  Neither can say "NR is
// running, and part of it is not": Silent Hill 2's Look stabilizer ran
// without motion vectors on 80.5% of its frames while the card was green,
// because the motion ring was full and the only trace was a `ring_full=`
// count on a telemetry line nobody grades.  Every row below is one such
// degraded path, with the counters it reads, the rule that makes it a
// problem, how long the rule must hold before the player hears about it (and
// how long it must stay clean before the warning goes away), who can act on
// it, and what to tell them.
//
// The table is the single source of truth.  The overlay lists the active
// rows under the status card, the HUD notice names them, the card turns amber
// ("active, with N issues") for a warn row, and the log carries one
// `NR-WARN enter|exit <slug>` line per transition plus a periodic summary.
// tools/field/health_ledger.py parses this file, so log_verdict.py and the
// harness oracle read the same rows the addon evaluates - no second copy.
// That parser reads the `.field = value` lines of kRows literally: keep one
// field per line, in declaration order.
//
// Rules are judged per WINDOW, never on session totals: a window closes after
// at least kWindowMinSeconds AND kWindowMinFrames real presents, or after
// kWindowMaxSeconds whatever the frame count (TIME-11: a count of presents or
// ticks is a different wall time under frame generation, at low fps and in a
// hitch).  Rates read the window's deltas against their own denominator (NR
// evaluates, eligible DLSS frames), never against presents (TIME-05).  A
// window whose denominator is below the row's min_volume is not evidence
// either way: it resets the entry streak and counts toward the exit hold.
//
// Like funnel.hpp and state_card.hpp this header has no addon dependencies
// beyond declines.hpp, so test/dlss5 runs every rule without a device.
//
// Sites the red team listed (LIFE/ENV/TIME/DATA "Silent degradation sites")
// that have no row here, and why - the rest map to a row by slug:
//   LIFE-3/15/16, LIFE-14   owned by rc11-worksets (LIFE-01/13/14/06); their
//                           fix names its counter and it becomes a row.
//   LIFE-5, TIME-13         the governor's prime snaps and floored steps:
//                           rc11-boil (LIFE-10, TIME-15).
//   LIFE-7, TIME-6          tracker/open-generation invariants: harness-graded
//                           zeros; what a player would notice (VRAM held to
//                           teardown) is vram_retained.
//   LIFE-8..11, ENV-3/8-11, rc11-hooks (module slots, Streamline, detours):
//   ENV-16, TIME-8          their hook_surface counters, below.
//   ENV-12                  trace CSV open failures: diagnostics, off by
//                           default (below, with TIME-9/10).
//   LIFE-11, TIME-14/15/17  re-arm, shutdown and lease-at-record: rc11-teardown.
//   TIME-1                  a cancelled F5 capture already logs its own line;
//                           captures are a support tool (TIME-01/03 teardown).
//   TIME-5                  runtime_mutex waits: a cost with no player action,
//                           on the telemetry line (locks[blocked=]); rc11-perf.
//   TIME-9/10               trace and timer drops: diagnostics that are off by
//                           default, graded by the harness.
//   TIME-11                 synthetic lifecycle ticks: since the TIME-11 fix
//                           they need 250 ms AND 8 evaluates without a
//                           present, and their WARN line is the record;
//                           nothing degrades.
//   ENV-1/2                 a value is either repaired as meant ("0,5",
//                           true/false: one info line) or listed by
//                           ini_value_invalid, clamps included.
//   ENV-5/6/7/15            fixed at the source, not warned: the driver line
//                           states the declared minimum and the start result
//                           pairs with it (NrStartAdvice); the runtime is
//                           named reference / custom / unreadable with its
//                           file version; api=other has its own card; a
//                           panel language the font cannot draw falls back
//                           to English with one line.  A Blackwell-only
//                           reference runtime on an older GPU is the card's
//                           GPU text, not a row: NR does not run at all.
//   ENV-13                  the config backup before a migration: one line
//                           per outcome (silent until rc11); no running
//                           state degrades.
//   DATA-01 (feed texel)    rows exposure_feed_guarded and
//                           exposure_feed_unchecked, on rc11-presr's guard
//                           probe (feed[guard_sampled= guard_v1= guard_lost=]).
//   rc11-worksets counters  over_live= is an internal invariant: it logs
//                           its own warning line at the event and the
//                           harness grades it to 0 - no player action, and
//                           reading it would tie this header to that
//                           branch's globals.  late_leases=, recycled=,
//                           idle_retired= are pool
//                           health, not degradation (workset_thrash is the
//                           row).  The LIFE-07 residual (own_mib[unproven=])
//                           is vram_retained.
//   DATA-02/03/07/08/13     rc11-data's guide[...] counters, one row each:
//                           mv_contradiction -> motion_window_contradiction,
//                           mv_scale_invalid -> motion_scale_unusable,
//                           mv_jittered -> motion_jittered, out_rect_clamped
//                           -> output_rect_clamped, exposure_unreadable ->
//                           exposure_texture_unreadable.  mv_display,
//                           mv_scale_absent and out_rect are DLSS's own
//                           contract read as DLSS reads it: no row.
//                           out_declared_disagree (rc2) is the engine's lie
//                           corrected, not a degradation: no row either.
//                           mv_window_outside and depth_window_outside (v8
//                           hardening) are observe-only overrun readouts with
//                           nothing behind them: no clamping, so no state a
//                           row could grade - the motion side's ground is
//                           already motion_window_misfit's, and the depth
//                           side waits for a reproduced field failure before
//                           it becomes anything more than a counter.
//   rc11-hooks hook_surface full + partial -> dlss_calls_bypass (a level:
//                           calls through a skipped copy never reach the
//                           addon), sl_missing -> streamline_unhookable,
//                           overlay_frozen -> overlay_frozen.  stale is a
//                           recovery (the copy is hooked again); foreign and
//                           stranded happen at unhook, after the last tick;
//                           unknown, sl_free and sl_frame_miss are logged and
//                           read by the harness: no row.
//   DATA-04..12 others      rc11-data / rc11-presr / rc11-teardown fix the
//                           contract itself; a fix that leaves a degraded path
//                           counts it, and that counter becomes a row.
//   "SDR classic nearest"   a player choice, not a degradation.

#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "declines.hpp"

namespace renodx::addons::dlss5::health {

// ---------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------

// What a row can read.  Counters are cumulative (the ledger reads per-window
// deltas); gauges are the current value.  Counters come first (IsGauge);
// NR-WARN lines and health_ledger.py name signals, never number them.
enum class Signal : std::uint8_t {
  // Counters.
  kNrFrames = 0,       // NR evaluates that ran, both insertion points
  kEligible,           // eligible game DLSS evaluates (the verdict's)
  kUnaccounted,        // eligible evaluates that named no terminal
  kMotionChecked,      // DLSS evaluates whose motion vectors were looked for
  kMotionMissing,      // ... and were absent
  kMotionMisfit,       // after-path evaluates whose motion window overruns
  kLookStabilized,     // Look passes with Stabilize on
  kLookMotionWanted,   // ... set to Motion
  kLookMotionMissing,  // ... that ran without motion vectors
  kLookRestarts,       // Look history restarts
  kWorksetsEvicted,    // worksets retired because the pool was full
  kFeedTexture,        // frames fed from the game's exposure texture
  kFeedHeld,           // ... that reused an older scale (ring full)
  kHookExceptions,     // exceptions a hook boundary swallowed
  kLoaderUnderLock,    // loader calls made holding runtime_mutex
  kNrDeviceChanges,    // NR runtime re-inits for another device
  kGuardSampled,       // feed-guard probe samples read (rc11-presr, 1 per 8 presents)
  kGuardV1,            // ... on v1's divisor: the game's exposure texel did not fit
  kGuardLost,          // ... samples the probe could not read
  // rc11-data's guide contract (ReportGuideContract), in evaluates:
  kGuideMvContradiction,    // MVLowRes unset, texture too small: render window kept
  kGuideMvScaleInvalid,     // the game's MV scale 0 or non-finite: read as 1
  kGuideOutRectClamped,     // the declared output rect clamped to the resource
  kGuideExposureUnreadable, // an exposure texture the feed cannot view: feed v1
  kGuideMvJittered,         // MVJittered vectors: NR has no jitter key
  kOverlayFrozen,      // presents with the overlay listeners frozen (rc11-hooks)
  // Gauges.
  kVramUsageMib,       // this process's local video memory use
  kVramBudgetMib,      // ... and the OS budget for it
  kIniIssues,          // ReShade.ini values that could not be read as written
  kPeerCopies,         // other copies of this add-on in the process
  kLeaseStarved,       // 1 while the telemetry lease probe reads STARVED
  kMotionScale,        // the smaller |NRMVecScaleX|, |NRMVecScaleY|
  kUnprovenMib,        // addon VRAM retained without a completion proof
  kHookBypass,         // NGX copies refused (slots full) or partly detoured (rc11-hooks)
  kSlUnhookable,       // Streamline loaded but its implementation not hookable
  kCount,
};

inline constexpr std::size_t kSignalCount = static_cast<std::size_t>(Signal::kCount);
inline constexpr std::size_t kFirstGauge = static_cast<std::size_t>(Signal::kVramUsageMib);

inline const char* const kSignalNames[kSignalCount] = {
    "nr_frames",        "eligible",       "unaccounted",      "motion_checked",
    "motion_missing",   "motion_misfit",  "look_stabilized",  "look_motion_wanted",
    "look_motion_missing", "look_restarts", "worksets_evicted", "feed_texture",
    "feed_held",        "hook_exceptions", "loader_under_lock",
    "nr_device_changes", "guard_sampled",  "guard_v1",         "guard_lost",
    "guide_mv_contradiction", "guide_mv_scale_invalid", "guide_out_rect_clamped",
    "guide_exposure_unreadable", "guide_mv_jittered", "overlay_frozen",
    "vram_usage_mib",   "vram_budget_mib", "ini_issues",
    "peer_copies",      "lease_starved",  "motion_scale",     "unproven_mib",
    "hook_bypass",      "sl_unhookable",
};

inline constexpr bool IsGauge(std::size_t signal) { return signal >= kFirstGauge; }

template <typename... Ids>
constexpr std::uint64_t Bits(Ids... ids) {
  return ((std::uint64_t{1} << static_cast<unsigned>(ids)) | ... | std::uint64_t{0});
}

// One observation of everything the rows read.
struct Sample {
  double signals[kSignalCount] = {};
  double declines[kNrDeclineCount] = {};

  double& operator[](Signal signal) { return signals[static_cast<std::size_t>(signal)]; }
  double& operator[](NrDeclineReason reason) {
    return declines[static_cast<std::size_t>(reason)];
  }
};

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

enum class Rule : std::uint8_t {
  kShareMax = 0,  // delta(num) / delta(den) > limit, once delta(den) >= min_volume
  kCountMax,      // delta(num) > limit in a window
  kLevelMax,      // gauge(num) > limit
  kRatioMax,      // gauge(num) / gauge(den) > limit, once gauge(den) > 0
  kLevelMin,      // gauge(num) < limit
};

enum class Tone : std::uint8_t {
  kLog = 0,  // the NR-WARN line only: an internal invariant
  kInfo,     // listed in the panel, gray: nothing the player can change
  kWarn,     // amber: counted on the card, raises the HUD notice
};

enum class Actor : std::uint8_t {
  kPlayer = 0,  // a setting or the machine: the fix is the player's
  kGame,        // the game's own integration or engine
  kMod,         // a defect in this mod: say so, and name what to send
};

inline const char* RuleName(Rule rule) {
  switch (rule) {
    case Rule::kShareMax: return "share_max";
    case Rule::kCountMax: return "count_max";
    case Rule::kLevelMax: return "level_max";
    case Rule::kRatioMax: return "ratio_max";
    case Rule::kLevelMin: return "level_min";
  }
  return "-";
}

inline const char* ToneName(Tone tone) {
  switch (tone) {
    case Tone::kLog: return "log";
    case Tone::kInfo: return "info";
    case Tone::kWarn: return "warn";
  }
  return "-";
}

inline const char* ActorName(Actor actor) {
  switch (actor) {
    case Actor::kPlayer: return "player";
    case Actor::kGame: return "game";
    case Actor::kMod: return "mod";
  }
  return "-";
}

struct Selector {
  std::uint64_t signals = 0;
  std::uint64_t declines = 0;
};

struct Row {
  const char* slug = "";
  Rule rule = Rule::kShareMax;
  Selector num;
  Selector den;
  double limit = 0.0;
  // Share rules: the window's denominator below this is not evidence.
  double min_volume = 0.0;
  // Seconds the rule must hold (at least two windows when non-zero) before
  // the row enters, and stay clean before it exits.
  double enter_s = 0.0;
  double exit_s = 0.0;
  Tone tone = Tone::kLog;
  Actor actor = Actor::kMod;
  const char* title = nullptr;
  const char* body = nullptr;
  const char* fix = nullptr;
  const char* fix2 = nullptr;
  // One %.0f conversion, fed value * measure_scale.
  const char* measure = nullptr;
  double measure_scale = 1.0;
};

// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)

// Who acts, as the panel labels it.
inline const char* ActorLabel(Actor actor) {
  switch (actor) {
    case Actor::kPlayer: return "You can fix this";
    case Actor::kGame: return "A limit of this game";
    case Actor::kMod: return "A defect in this mod";
  }
  return "";
}

inline constexpr const char* kModDefectFix =
    "This is a defect in the mod, not in your setup: please send ReShade.log.";

// The table.  Slugs are a FIELD CONTRACT (NR-WARN lines, lanes, the harness
// oracle): append only, never rename.  Every warn row carries a fix; a mod
// row names what to send.  Nothing here is red: red on the card stays
// "install or replace something", and every row below is a degraded session,
// not one that cannot run.
inline constexpr Row kRows[] = {
    // ---- motion vectors ---------------------------------------------------
    // A registered DLSS evaluate without motion vectors ends as not_dlss
    // (ineligible) on the after path and uncounted on pre-SR, so the
    // engagement ratio cannot see it: counted at every insertion point
    // (after, pre-SR, Streamline, the D3D11 bridge).
    {
        .slug = "motion_vectors_missing",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kMotionMissing)},
        .den = {.signals = Bits(Signal::kMotionChecked)},
        .limit = 0.01,
        .min_volume = 30,
        .enter_s = 2,
        .exit_s = 10,
        .tone = Tone::kWarn,
        .actor = Actor::kGame,
        .title = "The game's DLSS is not passing motion vectors",
        .body = "NR follows motion through the motion vectors the game hands"
                " DLSS. On these frames the game's DLSS call carried none, so"
                " NR skipped them and they keep the game's own DLSS image.",
        .fix = "If the game uses NVIDIA Streamline, set EnableHooks=1 in"
               " [RenoDX.DLSS5] in ReShade.ini and restart the game.",
        .fix2 = "If EnableHooks=1 is already set, the game hands DLSS its"
                " motion vectors in a way NR cannot read: please send"
                " ReShade.log.",
        .measure = "%.0f%% of DLSS frames had no motion vectors",
        .measure_scale = 100,
    },
    // The SH2 case: the Look's motion ring (or any other reason the
    // stabilizer's motion binding did not happen) - counted after every
    // branch, so a rewrite of the ring keeps the count.
    {
        .slug = "look_motion_lost",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kLookMotionMissing)},
        .den = {.signals = Bits(Signal::kLookMotionWanted)},
        .limit = 0.05,
        .min_volume = 30,
        .enter_s = 2,
        .exit_s = 10,
        .tone = Tone::kWarn,
        .actor = Actor::kMod,
        .title = "The motion stabilizer is running without motion vectors",
        .body = "Stabilize is set to Motion, but on these frames it had no"
                " motion vectors to follow, so it held edges in place while"
                " the scene moved: moving edges can smear or shimmer.",
        .fix = "Set Stabilize to Static in the Look section; it does not need"
               " motion vectors.",
        .fix2 = kModDefectFix,
        .measure = "%.0f%% of stabilized frames ran without motion vectors",
        .measure_scale = 100,
    },
    {
        .slug = "motion_scale_setting",  // i18n: skip
        .rule = Rule::kLevelMin,
        .num = {.signals = Bits(Signal::kMotionScale)},
        .limit = 0.05,
        .tone = Tone::kWarn,
        .actor = Actor::kPlayer,
        .title = "Motion scale is set to zero",
        .body = "Motion scale X and Y in the Fixes section multiply the game's"
                " motion vectors before NR and the motion stabilizer read"
                " them. Near zero they see a still scene on every moving"
                " frame, which smears motion.",
        .fix = "Reset Motion scale X and Motion scale Y in the Fixes section"
               " (NRMVecScaleX=1 and NRMVecScaleY=1).",
        .measure = "Motion scale is at %.0f%%",
        .measure_scale = 100,
    },
    // The one-shot "NR contract note: subrect fit" line, counted per
    // evaluate.  Informational: NR reads the part that fits.
    {
        .slug = "motion_window_misfit",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kMotionMisfit)},
        .den = {.signals = Bits(Signal::kMotionChecked)},
        .limit = 0.01,
        .min_volume = 30,
        .enter_s = 5,
        .exit_s = 30,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "The game's motion vectors are cut off",
        .body = "The game tells DLSS to read its motion vectors from a window"
                " larger than the texture that holds them, so NR reads the"
                " part that exists. Nothing to change on your side.",
        .measure = "%.0f%% of DLSS frames",
        .measure_scale = 100,
    },
    // rc11-data's guide contract: each row is a decision NR made because the
    // game's DLSS parameters contradict themselves or name something NR
    // cannot use.  NR still runs on these frames, so all are information.
    // DATA-02: MVLowRes unset, but the texture cannot hold the display window
    // at its base - the render window is kept (the rc10 reading).
    {
        .slug = "motion_window_contradiction",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kGuideMvContradiction)},
        .den = {.signals = Bits(Signal::kMotionChecked)},
        .limit = 0.01,
        .min_volume = 30,
        .enter_s = 5,
        .exit_s = 30,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "The game's motion vectors do not match their declared size",
        .body = "The game tells DLSS its motion vectors are at display"
                " resolution, but the texture holding them is too small for"
                " that, so NR reads them at render resolution.",
        .fix = "If moving edges trail with NR on, please send ReShade.log: it"
               " names the sizes the game declared.",
        .measure = "%.0f%% of DLSS frames",
        .measure_scale = 100,
    },
    // DATA-08: a motion-vector scale of 0, NaN or Inf is read as 1.
    {
        .slug = "motion_scale_unusable",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kGuideMvScaleInvalid)},
        .den = {.signals = Bits(Signal::kMotionChecked)},
        .limit = 0.01,
        .min_volume = 30,
        .enter_s = 5,
        .exit_s = 30,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "The game sent an unusable motion vector scale",
        .body = "The game's DLSS call set the motion vector scale to 0 or to"
                " a value that is not a number, so NR reads it as 1, the"
                " value DLSS itself uses. Nothing to change on your side.",
        .measure = "%.0f%% of DLSS frames",
        .measure_scale = 100,
    },
    // DATA-07: MVJittered vectors reach NR jittered (NR 310.8 has no jitter
    // key); in the e2e host NR's edit moved 0.0067 mean, 10 % of the edit.
    // A create flag, constant for the feature: no transient to hold out.
    {
        .slug = "motion_jittered",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kGuideMvJittered)},
        .den = {.signals = Bits(Signal::kMotionChecked)},
        .limit = 0.01,
        .min_volume = 30,
        .enter_s = 2,
        .exit_s = 30,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "The game's motion vectors include camera jitter",
        .body = "The game marks its motion vectors as jittered. The NR"
                " runtime has no setting for that, so NR follows motion"
                " slightly less precisely in this game. Nothing to change on"
                " your side.",
        .measure = "%.0f%% of DLSS frames",
        .measure_scale = 100,
    },
    // DATA-03: the declared DLSS output rect does not fit the resource; NR
    // works on the clamped rect.
    {
        .slug = "output_rect_clamped",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kGuideOutRectClamped)},
        .den = {.signals = Bits(Signal::kMotionChecked)},
        .limit = 0.01,
        .min_volume = 30,
        .enter_s = 5,
        .exit_s = 30,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "The game's DLSS output area is larger than its image",
        .body = "The game tells DLSS to write an area that does not fit"
                " inside its output image, so NR works on the part that"
                " fits. Nothing to change on your side.",
        .measure = "%.0f%% of DLSS frames",
        .measure_scale = 100,
    },

    // ---- frames NR skipped, by who can act ----------------------------------
    {
        .slug = "frames_skipped_restore",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.declines = Bits(NrDeclineReason::kStateShadowUnavailable,
                                 NrDeclineReason::kPreSrNoStateTarget)},
        .den = {.signals = Bits(Signal::kEligible)},
        .limit = 0.02,
        .min_volume = 60,
        .enter_s = 5,
        .exit_s = 15,
        .tone = Tone::kWarn,
        .actor = Actor::kMod,
        .title = "NR is skipping frames to protect the image",
        .body = "On these frames NR could not put the game's GPU state back"
                " safely after its pass, so it left them alone. They show the"
                " game's own DLSS image, which can look like NR flickering on"
                " and off.",
        .fix = kModDefectFix,
        .measure = "%.0f%% of DLSS frames skipped",
        .measure_scale = 100,
    },
    {
        .slug = "frames_skipped_retry",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.declines = Bits(NrDeclineReason::kWorksetSetupFailed,
                                 NrDeclineReason::kFailureBackoff)},
        .den = {.signals = Bits(Signal::kEligible)},
        .limit = 0.02,
        .min_volume = 60,
        .enter_s = 5,
        .exit_s = 15,
        .tone = Tone::kWarn,
        .actor = Actor::kMod,
        .title = "NR failed on some frames and is retrying",
        .body = "NR's pass failed to start on these frames and waits a moment"
                " before it tries again, so they keep the game's own DLSS"
                " image.",
        .fix = "If you raised Stack passes or Resolution scale, lower them"
               " again: running out of video memory ends here.",
        .fix2 = "Otherwise this is a defect in the mod: please send"
                " ReShade.log, which names the failing step.",
        .measure = "%.0f%% of DLSS frames skipped",
        .measure_scale = 100,
    },
    {
        .slug = "frames_skipped_streamline",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.declines = Bits(NrDeclineReason::kStreamlineCaptureIncomplete,
                                 NrDeclineReason::kStreamlineResourceStates,
                                 NrDeclineReason::kStreamlineRuntimeUnavailable)},
        .den = {.signals = Bits(Signal::kEligible)},
        .limit = 0.02,
        .min_volume = 60,
        .enter_s = 5,
        .exit_s = 15,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "Streamline hands NR incomplete frames",
        .body = "This game runs DLSS through NVIDIA Streamline, and on these"
                " frames Streamline had not provided every input NR needs, so"
                " NR skipped them. Nothing to change on your side.",
        .measure = "%.0f%% of DLSS frames skipped",
        .measure_scale = 100,
    },
    {
        .slug = "presr_frames_skipped",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.declines = Bits(NrDeclineReason::kPreSrGeometry)},
        .den = {.signals = Bits(Signal::kEligible)},
        .limit = 0.02,
        .min_volume = 60,
        .enter_s = 5,
        .exit_s = 15,
        .tone = Tone::kWarn,
        .actor = Actor::kPlayer,
        .title = "NR can't run before upscaling in this game",
        .body = "Before upscaling reads the image the game hands DLSS, and on"
                " these frames that image (or the motion vectors or depth"
                " beside it) has a shape NR cannot use, so they keep the"
                " game's own DLSS image.",
        .fix = "Set Hook point to Upscaled in the Neural Rendering section"
               " (NRHookPoint=0).",
        .measure = "%.0f%% of DLSS frames skipped",
        .measure_scale = 100,
    },
    {
        .slug = "output_unsupported",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.declines = Bits(NrDeclineReason::kNgxOutputGeometry)},
        .den = {.signals = Bits(Signal::kEligible)},
        .limit = 0.02,
        .min_volume = 60,
        .enter_s = 5,
        .exit_s = 15,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "The game's DLSS output has a shape NR can't replace",
        .body = "On these frames the game's DLSS wrote into an output NR cannot"
                " replace (a texture array or a multisampled texture), so they"
                " keep the game's own DLSS image. Nothing to change on your"
                " side.",
        .measure = "%.0f%% of DLSS frames skipped",
        .measure_scale = 100,
    },
    {
        .slug = "bridge_busy",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.declines = Bits(NrDeclineReason::kBridgeBusy)},
        .den = {.signals = Bits(Signal::kEligible)},
        .limit = 0.05,
        .min_volume = 30,
        .enter_s = 3,
        .exit_s = 10,
        .tone = Tone::kWarn,
        .actor = Actor::kPlayer,
        .title = "NR's Direct3D 11 bridge can't keep up",
        .body = "In this Direct3D 11 game NR runs on a second, Direct3D 12"
                " device, and on these frames that device was still busy with"
                " earlier ones, so they keep the game's own DLSS image.",
        .fix = "Lower Resolution scale or Stack passes, or cap the game's frame"
               " rate.",
        .measure = "%.0f%% of DLSS frames skipped",
        .measure_scale = 100,
    },
    {
        .slug = "bridge_stalled",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.declines = Bits(NrDeclineReason::kBridgeDown,
                                 NrDeclineReason::kBridgeTimeout,
                                 NrDeclineReason::kBridgeLost)},
        .den = {.signals = Bits(Signal::kEligible)},
        .limit = 0.02,
        .min_volume = 30,
        .enter_s = 3,
        .exit_s = 15,
        .tone = Tone::kWarn,
        .actor = Actor::kMod,
        .title = "NR's Direct3D 11 bridge stalled",
        .body = "The Direct3D 12 device NR runs on beside this game stopped"
                " answering or is restarting, so frames keep the game's own"
                " DLSS image until it recovers.",
        .fix = "If it keeps happening, restart the game and update the NVIDIA"
               " driver.",
        .fix2 = kModDefectFix,
        .measure = "%.0f%% of DLSS frames skipped",
        .measure_scale = 100,
    },
    {
        .slug = "bridge_surfaces",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.declines = Bits(NrDeclineReason::kBridgeFormat,
                                 NrDeclineReason::kD3D11Deferred)},
        .den = {.signals = Bits(Signal::kEligible)},
        .limit = 0.02,
        .min_volume = 60,
        .enter_s = 5,
        .exit_s = 15,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "Some of this game's DLSS frames can't reach NR",
        .body = "On these frames the game's DLSS surfaces use a format,"
                " multisampling or context the Direct3D 11 bridge cannot"
                " share, so they keep the game's own DLSS image. Nothing to"
                " change on your side.",
        .measure = "%.0f%% of DLSS frames skipped",
        .measure_scale = 100,
    },
    {
        .slug = "teardown_wait",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.declines = Bits(NrDeclineReason::kTeardownPending)},
        .den = {.signals = Bits(Signal::kEligible)},
        .limit = 0.05,
        .min_volume = 30,
        .enter_s = 10,
        .exit_s = 10,
        .tone = Tone::kWarn,
        .actor = Actor::kMod,
        .title = "NR is stuck waiting after a display change",
        .body = "After the game rebuilt its swapchain (a resolution, window or"
                " display change), NR waits for its old work to finish before"
                " it restarts. It has waited unusually long, and frames keep"
                " the game's own DLSS image meanwhile.",
        .fix = "Restart the game.",
        .fix2 = kModDefectFix,
        .measure = "%.0f%% of DLSS frames skipped",
        .measure_scale = 100,
    },
    {
        .slug = "frames_unaccounted",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kUnaccounted)},
        .den = {.signals = Bits(Signal::kEligible)},
        .limit = 0.01,
        .min_volume = 60,
        .enter_s = 5,
        .exit_s = 15,
        .tone = Tone::kWarn,
        .actor = Actor::kMod,
        .title = "NR skipped frames without saying why",
        .body = "Every frame NR leaves alone should have a named reason, and"
                " on these frames none was recorded.",
        .fix = kModDefectFix,
        .measure = "%.0f%% of DLSS frames",
        .measure_scale = 100,
    },

    // ---- cost and resources --------------------------------------------------
    // TIME-05: evictions per NR evaluate in the window, not per present over
    // the session (blind under frame generation and to late-session thrash).
    {
        .slug = "workset_thrash",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kWorksetsEvicted)},
        .den = {.signals = Bits(Signal::kNrFrames)},
        .limit = 0.02,
        .min_volume = 60,
        .enter_s = 5,
        .exit_s = 15,
        .tone = Tone::kWarn,
        .actor = Actor::kPlayer,
        .title = "NR keeps rebuilding its working surfaces",
        .body = "The game rotates its DLSS output through more images than NR"
                " keeps ready, so NR throws its surfaces away and builds new"
                " ones over and over. That costs frame rate.",
        .fix = "Set NRMaxWorksets=8 in [RenoDX.DLSS5] in ReShade.ini and"
               " restart the game.",
        .fix2 = "If it is already 8, please send ReShade.log.",
        .measure = "%.0f rebuilds per 100 enhanced frames",
        .measure_scale = 100,
    },
    // LIFE-09: the exposure-view ring saturates into a held scale.
    {
        .slug = "exposure_feed_held",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kFeedHeld)},
        .den = {.signals = Bits(Signal::kFeedTexture)},
        .limit = 0.05,
        .min_volume = 30,
        .enter_s = 3,
        .exit_s = 15,
        .tone = Tone::kWarn,
        .actor = Actor::kMod,
        .title = "NR's brightness reading is stuck",
        .body = "NR reads the game's exposure to set the brightness it feeds"
                " the model. On these frames it had no free slot to read it"
                " and reused an older value, so brightness changes can lag or"
                " pump.",
        .fix = "Set Brightness source to v1 (frame meter) in the Fixes section"
               " (NRFeedMode=1).",
        .fix2 = kModDefectFix,
        .measure = "%.0f%% of frames reused an old exposure",
        .measure_scale = 100,
    },
    // DATA-01 (rc11-presr's guard): the game's exposure texel did not fit
    // the frame it came with, and the guard fed NR the frame's own meter.
    // The image is protected - the guard IS the conservative path - so this
    // is information, not a warning: the game's exposure data is wrong.
    {
        .slug = "exposure_feed_guarded",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kGuardV1)},
        .den = {.signals = Bits(Signal::kGuardSampled)},
        .limit = 0,
        // The probe reads 1 frame in 8: two samples is a 1 s window at
        // 16 fps, so the row is judged at any playable frame rate.
        .min_volume = 2,
        .enter_s = 2,
        .exit_s = 15,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "The game's exposure data looks wrong",
        .body = "On these frames the exposure value the game hands DLSS did"
                " not fit the image, so NR measures the brightness itself"
                " with its built-in meter instead.",
        .measure = "%.0f%% of checked frames used the built-in meter",
        .measure_scale = 100,
    },
    // The guard's probe could not read its samples (lapped, no proof, no
    // readback): whether the game's exposure data fits is unknown.
    {
        .slug = "exposure_feed_unchecked",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kGuardLost)},
        .den = {.signals = Bits(Signal::kGuardSampled, Signal::kGuardLost)},
        .limit = 0.1,
        .min_volume = 2,
        .enter_s = 5,
        .exit_s = 15,
        .tone = Tone::kInfo,
        .actor = Actor::kMod,
        .title = "NR couldn't check the game's exposure data",
        .body = "NR checks a sample of frames to confirm the game's exposure"
                " value fits the image; on these frames the check could not"
                " be read, so a wrong value would go unnoticed.",
        .measure = "%.0f%% of checks could not be read",
        .measure_scale = 100,
    },
    // DATA-13: an exposure texture the feed cannot view (shape or format):
    // feed v1, NR's own meter, on those frames.
    {
        .slug = "exposure_texture_unreadable",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kGuideExposureUnreadable)},
        .den = {.signals = Bits(Signal::kMotionChecked)},
        .limit = 0.01,
        .min_volume = 30,
        .enter_s = 5,
        .exit_s = 30,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "NR can't read the game's exposure texture",
        .body = "The game hands DLSS an exposure texture in a format or shape"
                " NR cannot read, so NR measures the brightness itself with"
                " its built-in meter. Nothing to change on your side.",
        .measure = "%.0f%% of DLSS frames",
        .measure_scale = 100,
    },
    // ENV-13.
    {
        .slug = "vram_over_budget",  // i18n: skip
        .rule = Rule::kRatioMax,
        .num = {.signals = Bits(Signal::kVramUsageMib)},
        .den = {.signals = Bits(Signal::kVramBudgetMib)},
        .limit = 0.95,
        .enter_s = 10,
        .exit_s = 30,
        .tone = Tone::kWarn,
        .actor = Actor::kPlayer,
        .title = "The GPU is short on video memory",
        .body = "The game and NR together use more video memory than Windows"
                " grants the game, so Windows moves some of it to system"
                " memory. Expect stutter and a lower frame rate.",
        .fix = "Lower Resolution scale or Stack passes in NR, or texture"
               " quality in the game.",
        .measure = "%.0f%% of the video memory budget in use",
        .measure_scale = 100,
    },
    // ENV-01: the panel lists each key, what it holds and the value used.
    {
        .slug = "ini_value_invalid",  // i18n: skip
        .rule = Rule::kLevelMax,
        .num = {.signals = Bits(Signal::kIniIssues)},
        .limit = 0,
        .tone = Tone::kWarn,
        .actor = Actor::kPlayer,
        .title = "Some ReShade.ini values could not be used as written",
        .body = "NR used the value shown instead of these values in"
                " [RenoDX.DLSS5]:",
        .fix = "Write each value as a plain number (a switch as 0 or 1) inside"
               " its range, or reset it in this panel, then restart the game.",
        .measure = "%.0f value(s)",
    },
    // ENV-03: another copy of this add-on (a second version, or the Debug
    // build beside the Release one) hooks the same evaluates.  Observe-only:
    // no copy stands down until a field log shows the double pass.
    {
        .slug = "duplicate_addon",  // i18n: skip
        .rule = Rule::kLevelMax,
        .num = {.signals = Bits(Signal::kPeerCopies)},
        .limit = 0,
        .tone = Tone::kWarn,
        .actor = Actor::kPlayer,
        .title = "This add-on is loaded twice",
        .body = "Another copy of this add-on runs in the same game, so Neural"
                " Rendering can be applied twice on each frame: stronger halos"
                " and darker edges.",
        .fix = "Keep one DLSS5 .addon64 file in the game's folder, remove the"
               " others, and restart the game.",
        .measure = "%.0f other copy loaded",
    },
    {
        .slug = "hook_exception",  // i18n: skip
        .rule = Rule::kCountMax,
        .num = {.signals = Bits(Signal::kHookExceptions)},
        .limit = 0,
        .exit_s = 60,
        .tone = Tone::kWarn,
        .actor = Actor::kMod,
        .title = "Something failed inside the mod and was contained",
        .body = "An error inside one of the mod's hooks was caught before it"
                " could reach the game. The frame it happened on may have kept"
                " the game's own image.",
        .fix = kModDefectFix,
        .measure = "%.0f error(s) caught",
    },
    // rc11-hooks' hook_surface: an NGX copy refused because every detour
    // slot of its API is taken, or one Detours attached only in part.  Calls
    // through it never reach the addon, so no decline can count them: the
    // session-long level is the only witness.
    {
        .slug = "dlss_calls_bypass",  // i18n: skip
        .rule = Rule::kLevelMax,
        .num = {.signals = Bits(Signal::kHookBypass)},
        .limit = 0,
        .tone = Tone::kWarn,
        .actor = Actor::kMod,
        .title = "Some of the game's DLSS calls bypass NR",
        .body = "NR could not attach to every copy of NVIDIA's DLSS library"
                " the game loaded (too many copies, or functions it could not"
                " hook), so DLSS calls through those copies run without NR.",
        .fix = kModDefectFix,
        .measure = "%.0f library copies not fully hooked",
    },
    // sl_missing: sl.common is loaded but not Streamline 2 or without an
    // evaluate behind its gateway; NR reaches the title through NGX only,
    // and EnableHooks=1 advice is pointless.
    {
        .slug = "streamline_unhookable",  // i18n: skip
        .rule = Rule::kLevelMax,
        .num = {.signals = Bits(Signal::kSlUnhookable)},
        .limit = 0,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "NR can't hook this game's Streamline version",
        .body = "The game loads NVIDIA Streamline, but not a version NR can"
                " attach to, so NR reaches the game's DLSS through NVIDIA's"
                " NGX library only. EnableHooks=1 does not change this.",
        .measure = "%.0f Streamline module(s) not hooked",
    },
    // overlay_frozen: a second ReShade effect runtime is live, so the
    // overlay listeners stay as they are (TIME-13).
    {
        .slug = "overlay_frozen",  // i18n: skip
        .rule = Rule::kCountMax,
        .num = {.signals = Bits(Signal::kOverlayFrozen)},
        .limit = 0,
        .enter_s = 2,
        .exit_s = 5,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "NR's overlay controls are paused",
        .body = "The game has a second window or swap chain open, so the"
                " status HUD, the overlay hotkeys and the window dock keep"
                " their current state until it closes.",
        .measure = "%.0f frames with the controls paused",
    },
    {
        .slug = "stabilizer_restarting",  // i18n: skip
        .rule = Rule::kShareMax,
        .num = {.signals = Bits(Signal::kLookRestarts)},
        .den = {.signals = Bits(Signal::kLookStabilized)},
        .limit = 0.1,
        .min_volume = 30,
        .enter_s = 5,
        .exit_s = 15,
        .tone = Tone::kInfo,
        .actor = Actor::kGame,
        .title = "The stabilizer keeps restarting",
        .body = "Stabilize starts over whenever the game resets DLSS (a camera"
                " cut) or a frame takes very long. That happens often here, so"
                " Stabilize has little effect. Nothing to change on your side.",
        .measure = "%.0f%% of stabilized frames restarted",
        .measure_scale = 100,
    },
    // LIFE-07's residual (rc11-worksets): with no queue fence to prove the
    // GPU finished (the queue hook never installed), a retirement is held to
    // teardown.  Correct, and memory the game cannot have back: listed for
    // the player, not amber, since nothing they change releases it.
    {
        .slug = "vram_retained",  // i18n: skip
        .rule = Rule::kLevelMax,
        .num = {.signals = Bits(Signal::kUnprovenMib)},
        .limit = 0,
        .enter_s = 60,
        .exit_s = 10,
        .tone = Tone::kInfo,
        .actor = Actor::kMod,
        .title = "NR is holding video memory until the game closes",
        .body = "The GPU never confirmed it had finished with memory NR"
                " retired, so NR keeps that memory until the game exits"
                " rather than risk freeing it too early.",
        .measure = "%.0f MiB held",
    },

    // ---- internal invariants: the NR-WARN line only ------------------------
    // TIME-03: an idle queue the every-queue lease waits on forever.
    {
        .slug = "lease_starved",  // i18n: skip
        .rule = Rule::kLevelMax,
        .num = {.signals = Bits(Signal::kLeaseStarved)},
        .limit = 0,
        .tone = Tone::kLog,
        .actor = Actor::kMod,
        .title = "GPU lease probe STARVED: a tracked queue went idle; lease-proven"  // i18n: skip
                 " releases and F5 captures wait",
    },
    // LEDGER-01: the DllMain-deadlock shape.
    {
        .slug = "loader_under_lock",  // i18n: skip
        .rule = Rule::kCountMax,
        .num = {.signals = Bits(Signal::kLoaderUnderLock)},
        .limit = 0,
        .exit_s = 60,
        .tone = Tone::kLog,
        .actor = Actor::kMod,
        .title = "a Windows loader call was made holding runtime_mutex"  // i18n: skip
                 " (locks[site=] names it)",
    },
    // LIFE-12: a device alternation re-initializes the runtime per flip.
    {
        .slug = "nr_runtime_restarts",  // i18n: skip
        .rule = Rule::kCountMax,
        .num = {.signals = Bits(Signal::kNrDeviceChanges)},
        .limit = 1,
        .enter_s = 5,
        .exit_s = 15,
        .tone = Tone::kLog,
        .actor = Actor::kMod,
        .title = "the NR runtime keeps re-initializing for another device"  // i18n: skip
    },
};

// i18n: end

inline constexpr std::size_t kRowCount = std::size(kRows);
static_assert(kRowCount <= 64, "the active mask is one 64-bit word");

inline bool Counted(const Row& row) { return row.tone == Tone::kWarn; }

// ---------------------------------------------------------------------------
// Judging a window
// ---------------------------------------------------------------------------

struct Judgement {
  bool judged = false;  // the window was evidence either way
  bool breach = false;
  double value = 0.0;
  double num = 0.0;
  double den = 0.0;
};

// The selected signals' and declines' sum: per-window deltas (clamped at
// zero: they are read across threads without a snapshot) or current gauges.
inline double Select(const Selector& selector, const Sample& base, const Sample& now,
                     bool delta) {
  double sum = 0.0;
  for (std::size_t i = 0; i < kSignalCount; ++i) {
    if ((selector.signals & (std::uint64_t{1} << i)) == 0) continue;
    const double d = delta ? now.signals[i] - base.signals[i] : now.signals[i];
    sum += d > 0.0 ? d : 0.0;
  }
  for (std::size_t i = 0; i < kNrDeclineCount; ++i) {
    if ((selector.declines & (std::uint64_t{1} << i)) == 0) continue;
    const double d = delta ? now.declines[i] - base.declines[i] : now.declines[i];
    sum += d > 0.0 ? d : 0.0;
  }
  return sum;
}

// `rates`: false in the starting grace, when a share rule is not judged
// (the first evaluates of a session decline by design).
inline Judgement Judge(const Row& row, const Sample& base, const Sample& now, bool rates) {
  Judgement j;
  const bool delta = row.rule == Rule::kShareMax || row.rule == Rule::kCountMax;
  j.num = Select(row.num, base, now, delta);
  j.den = Select(row.den, base, now, delta);
  switch (row.rule) {
    case Rule::kShareMax:
      if (!rates || j.den < row.min_volume || j.den <= 0.0) return j;
      j.value = j.num / j.den;
      break;
    case Rule::kCountMax:
    case Rule::kLevelMax:
    case Rule::kLevelMin:
      j.value = j.num;
      break;
    case Rule::kRatioMax:
      if (j.den <= 0.0) return j;
      j.value = j.num / j.den;
      break;
  }
  j.judged = true;
  j.breach = row.rule == Rule::kLevelMin ? j.value < row.limit : j.value > row.limit;
  return j;
}

// ---------------------------------------------------------------------------
// Windows and holds
// ---------------------------------------------------------------------------

inline constexpr double kWindowMinSeconds = 1.0;
inline constexpr std::uint32_t kWindowMinFrames = 8;
inline constexpr double kWindowMaxSeconds = 4.0;

inline bool WindowCloses(double elapsed_s, std::uint32_t frames) {
  return (elapsed_s >= kWindowMinSeconds && frames >= kWindowMinFrames)
         || elapsed_s >= kWindowMaxSeconds;
}

struct RowState {
  bool active = false;
  double breach_s = 0.0;
  std::uint32_t breach_windows = 0;
  double clean_s = 0.0;
  double active_since_s = 0.0;
  // The last judged window: what the panel shows and the exit line prints.
  double value = 0.0;
  double num = 0.0;
  double den = 0.0;
  std::uint32_t enters = 0;
};

enum class Transition : std::uint8_t { kNone = 0, kEnter, kExit };

inline Transition Step(const Row& row, const Judgement& j, double window_s,
                       double now_s, RowState* state) {
  if (j.judged) {
    state->value = j.value;
    state->num = j.num;
    state->den = j.den;
  }
  if (j.judged && j.breach) {
    state->clean_s = 0.0;
    state->breach_s += window_s;
    ++state->breach_windows;
    const std::uint32_t windows = row.enter_s > 0.0 ? 2u : 1u;
    if (!state->active && state->breach_s >= row.enter_s
        && state->breach_windows >= windows) {
      state->active = true;
      state->active_since_s = now_s;
      ++state->enters;
      return Transition::kEnter;
    }
    return Transition::kNone;
  }
  // Clean, or no evidence: either way the streak toward entering is over.
  state->breach_s = 0.0;
  state->breach_windows = 0;
  if (!state->active) return Transition::kNone;
  state->clean_s += window_s;
  if (state->clean_s < row.exit_s) return Transition::kNone;
  state->active = false;
  state->clean_s = 0.0;
  return Transition::kExit;
}

struct Event {
  std::uint8_t row = 0;
  Transition transition = Transition::kNone;
  double window_s = 0.0;
  double held_s = 0.0;  // enter: the breach streak; exit: time active
};

// The ledger: one window at a time over a stream of samples.  Owned by the
// lifecycle tick (runtime_mutex); the overlay reads the published mask.
class Ledger {
 public:
  // One tick.  `frame`: a real present (synthetic ticks advance time only).
  // Returns the number of events written, all from the window that closed on
  // this tick (0 when none closed).
  std::size_t Tick(const Sample& now, double now_s, bool frame, bool rates,
                   Event* events, std::size_t capacity) {
    // The first window's counter deltas run from zero, not from the first
    // sample: what happened before the first tick (a hook exception at
    // load, a loader call during the first device init) is in the first
    // window.  Seeding the base from the first sample hid the
    // hook-exception lane's three contained throws (dlss5_e2e_hook_throw,
    // rc11).  Counters are process totals, so zero is their true start.
    if (!started_) {
      started_ = true;
      base_ = Sample{};
      window_start_s_ = now_s;
      frames_ = 0;
      return 0;
    }
    if (frame) ++frames_;
    const double window_s = now_s - window_start_s_;
    if (!WindowCloses(window_s, frames_)) return 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < kRowCount; ++i) {
      const Row& row = kRows[i];
      RowState& state = states_[i];
      const double active_for = now_s - state.active_since_s;
      const Transition t = Step(row, Judge(row, base_, now, rates), window_s, now_s, &state);
      if (t == Transition::kNone) continue;
      if (t == Transition::kEnter) ++entered_;
      if (count < capacity) {
        events[count++] = {static_cast<std::uint8_t>(i), t, window_s,
                           t == Transition::kEnter ? state.breach_s : active_for};
      }
    }
    base_ = now;
    window_start_s_ = now_s;
    frames_ = 0;
    ++windows_;
    return count;
  }

  std::uint64_t ActiveMask() const {
    std::uint64_t mask = 0;
    for (std::size_t i = 0; i < kRowCount; ++i) {
      if (states_[i].active) mask |= std::uint64_t{1} << i;
    }
    return mask;
  }
  const RowState& State(std::size_t row) const { return states_[row]; }
  std::uint32_t Entered() const { return entered_; }
  std::uint32_t Windows() const { return windows_; }

 private:
  bool started_ = false;
  Sample base_;
  double window_start_s_ = 0.0;
  std::uint32_t frames_ = 0;
  std::uint32_t windows_ = 0;
  std::uint32_t entered_ = 0;
  RowState states_[kRowCount];
};

// Rows the card counts ("active, with N issues"), from a published mask.
inline std::uint32_t CountedIssues(std::uint64_t mask) {
  std::uint32_t issues = 0;
  for (std::size_t i = 0; i < kRowCount; ++i) {
    if ((mask & (std::uint64_t{1} << i)) != 0 && Counted(kRows[i])) ++issues;
  }
  return issues;
}

// ---------------------------------------------------------------------------
// The log lines: a CONTRACT like NR-VERDICT and NR-CARD.  test/dlss5 pins
// them, tools/field/log_verdict.py parses them, the harness grades them.
// ---------------------------------------------------------------------------

inline int FormatEnterLine(char* buffer, std::size_t size, const Event& event,
                           const RowState& state) {
  const Row& row = kRows[event.row];
  return std::snprintf(
      buffer, size,
      "NR-WARN enter %s tone=%s actor=%s rule=%s value=%.4f limit=%.4f num=%.0f"
      " den=%.0f window_s=%.2f held_s=%.2f",
      row.slug, ToneName(row.tone), ActorName(row.actor), RuleName(row.rule),
      state.value, row.limit, state.num, state.den, event.window_s, event.held_s);
}

inline int FormatExitLine(char* buffer, std::size_t size, const Event& event,
                          const RowState& state) {
  return std::snprintf(buffer, size, "NR-WARN exit %s active_s=%.2f value=%.4f",
                       kRows[event.row].slug, event.held_s, state.value);
}

// `slugs`: the active slugs joined with ',' ("-" for none).
inline int FormatSummaryLine(char* buffer, std::size_t size, std::uint64_t mask,
                             std::uint32_t entered) {
  char slugs[1024] = {};
  std::size_t used = 0;
  std::uint32_t active = 0;
  for (std::size_t i = 0; i < kRowCount; ++i) {
    if ((mask & (std::uint64_t{1} << i)) == 0) continue;
    ++active;
    const int wrote = std::snprintf(slugs + used, sizeof(slugs) - used, "%s%s",
                                    used == 0 ? "" : ",", kRows[i].slug);
    if (wrote > 0) used = (std::min)(sizeof(slugs) - 1, used + static_cast<std::size_t>(wrote));
  }
  return std::snprintf(buffer, size, "NR-WARN summary active=%u slugs=%s entered=%u",
                       active, active == 0 ? "-" : slugs, entered);
}

// ---------------------------------------------------------------------------
// ENV-01: ReShade.ini values as the player wrote them
// ---------------------------------------------------------------------------
//
// ReShade splits a value on every single ',' and hands the elements back
// joined with '\0'; its typed get_config_value then parses with from_chars,
// which calls a partial parse a success and reads a bool as an int.  So
// `NRIntensity=0,5` (a comma-decimal locale: six of the panel's languages)
// silently became 0 and `true` silently kept the default.  This reads the
// raw elements first: a comma decimal with exactly two numeric elements and
// the words true/false/yes/no/on/off are unambiguous and accepted
// (kRepaired, with the value to write back); anything else that is not a
// whole number is kInvalid, and the player is told which key.

enum class IniKind : std::uint8_t { kFloat = 0, kUint, kBool };
enum class IniRead : std::uint8_t { kAbsent = 0, kClean, kRepaired, kInvalid };

struct IniValue {
  IniRead read = IniRead::kAbsent;
  double value = 0.0;
};

namespace ini_internal {

inline bool Space(char c) { return c == ' ' || c == '\t'; }

// [begin, end) trimmed of spaces.
inline void Trim(const char** begin, const char** end) {
  while (*begin < *end && Space(**begin)) ++*begin;
  while (*end > *begin && Space(*(*end - 1))) --*end;
}

inline bool WholeNumber(const char* begin, const char* end, double* out) {
  if (begin == end) return false;
  const auto result = std::from_chars(begin, end, *out);
  return result.ec == std::errc{} && result.ptr == end && std::isfinite(*out);
}

inline bool Digits(const char* begin, const char* end, bool allow_sign) {
  if (allow_sign && begin < end && (*begin == '-' || *begin == '+')) ++begin;
  if (begin == end) return false;
  for (const char* c = begin; c < end; ++c) {
    if (*c < '0' || *c > '9') return false;
  }
  return true;
}

inline bool Word(const char* begin, const char* end, const char* word) {
  const std::size_t length = std::strlen(word);
  if (static_cast<std::size_t>(end - begin) != length) return false;
  for (std::size_t i = 0; i < length; ++i) {
    const char c = begin[i];
    const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (lower != word[i]) return false;
  }
  return true;
}

}  // namespace ini_internal

// `raw`/`size`: ReShade's elements, each followed by '\0' (size counts every
// byte it wrote, trailing terminators included).
inline IniValue ReadIniValue(const char* raw, std::size_t size, IniKind kind) {
  using namespace ini_internal;
  IniValue out;
  const char* element_begin[3] = {};
  const char* element_end[3] = {};
  std::size_t elements = 0;
  const char* start = raw;
  for (std::size_t i = 0; i <= size && elements < 3; ++i) {
    if (i < size && raw[i] != '\0') continue;
    element_begin[elements] = start;
    element_end[elements] = raw + i;
    ++elements;
    start = raw + i + 1;
  }
  // Trailing empty elements are terminators, not values.
  while (elements > 0 && element_begin[elements - 1] == element_end[elements - 1]) --elements;
  if (elements == 0) return out;
  for (std::size_t i = 0; i < elements; ++i) Trim(&element_begin[i], &element_end[i]);
  out.read = IniRead::kInvalid;
  if (elements == 1) {
    const char* b = element_begin[0];
    const char* e = element_end[0];
    double number = 0.0;
    if (WholeNumber(b, e, &number)) {
      const bool integral = number == std::floor(number);
      if (kind == IniKind::kFloat || integral) {
        if (kind == IniKind::kUint && number < 0.0) return out;
        out.read = IniRead::kClean;
        out.value = number;
      }
      return out;
    }
    if (kind == IniKind::kBool) {
      for (const char* yes : {"true", "yes", "on"}) {
        if (Word(b, e, yes)) {
          out.read = IniRead::kRepaired;
          out.value = 1.0;
        }
      }
      for (const char* no : {"false", "no", "off"}) {
        if (Word(b, e, no)) {
          out.read = IniRead::kRepaired;
          out.value = 0.0;
        }
      }
    }
    return out;
  }
  // "0,5": exactly two elements, digits both sides, and a float key.
  if (elements == 2 && kind == IniKind::kFloat
      && Digits(element_begin[0], element_end[0], true)
      && Digits(element_begin[1], element_end[1], false)) {
    char joined[64] = {};
    const std::size_t a = static_cast<std::size_t>(element_end[0] - element_begin[0]);
    const std::size_t b = static_cast<std::size_t>(element_end[1] - element_begin[1]);
    if (a + b + 1 < sizeof(joined)) {
      std::memcpy(joined, element_begin[0], a);
      joined[a] = '.';
      std::memcpy(joined + a + 1, element_begin[1], b);
      double number = 0.0;
      if (WholeNumber(joined, joined + a + 1 + b, &number)) {
        out.read = IniRead::kRepaired;
        out.value = number;
      }
    }
  }
  return out;
}

}  // namespace renodx::addons::dlss5::health

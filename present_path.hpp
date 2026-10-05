/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The Present hook point (NRHookPoint=2, rc11).
//
// Included by dlssnr.hpp INSIDE namespace internal, just after
// d3d11_bridge.hpp, so it sees the whole D3D12 pipeline it drives.  It is not
// a standalone header and must never be included anywhere else.
//
// Why.  The Upscaled hook point enhances the game's DLSS output before the
// game's own post-processing, tone map and UI; field users compared it with
// a mod that runs NR on the swapchain and asked for that look.  Here NR runs
// on the back buffer at ReShade's present event: after the game has drawn
// everything, before ReShade's effects and overlay (dxgi_swapchain.cpp
// on_present invokes the `present` add-on event, then present_effect_runtime,
// then flushes the immediate command list; the queue's lock is held
// throughout).
//
// What runs, per present:
//
//   1. the back buffer's encoding comes from the swapchain's colour space
//      (NRPresentEncoding overrides it): sRGB is SDR (the legacy codec, as a
//      DLSS output in an 8/10-bit format), scRGB is linear with 80 nits = 1.0,
//      HDR10 is PQ with BT.2020 primaries (the v6 linearize converts to
//      BT.709 and the commit converts back, shader Encoding 4).  HLG or an
//      unknown space is not served: the present declines present_format and
//      the DLSS evaluates run the Upscaled hook point instead;
//   2. the guides are the game's DLSS motion vectors and depth, COPIED at its
//      evaluate (CapturePresentGuides, on the game's list, into addon-owned
//      clones) because by the present the game may have reused them.  A
//      capture carries a generation stamp (serial), its present clock, its
//      contract, and a tracker marker; the present takes the newest capture
//      whose recording was submitted and whose copies it can order itself
//      after without deadlocking (the tracker's exact (fence, value) proofs,
//      submission::UseSubmissionTokens): a proof on the present queue's own
//      fence becomes a queue Wait, a proof on another queue's fence must
//      already be complete (see "Cross-queue captures" below).
//      NRPresentGuides=Never, or Optional
//      when no capture matches (no DLSS, a stale or differently shaped one),
//      uses neutral guides instead: zero motion and depth 0 with
//      DepthInverted - the whole frame static and at infinity;
//   3. the after path itself - ProcessInline, the codec, the look stage, the
//      working resolution - records into the addon's own list from a ring of
//      eight (kPresentRing), reading a parameter block the addon fills (Color = Output = the
//      back buffer, the guides above), with PresentTarget saying what differs:
//      the back buffer rests in PRESENT, its workset is keyed by the
//      swapchain (the buffers rotate), its units are the colour space's;
//   4. the list executes on the present queue (unwrapped: ReShade's queue
//      proxy is not reachable from the event, and the queue detour tracks the
//      native submit the same way), after ReShade's immediate list is
//      flushed so earlier add-on work keeps its order, and a ring fence
//      proves each slot's completion before the slot is reused (a present
//      that finds its slot busy waits for it, bounded: kPresentSlotWaitMs).
//
// Frame generation.  DLSS-G can present several frames per game frame
// (Silent Hill 2 field log: 10987 presents against 2533 evaluates), and at
// ReShade's present a generated frame and a real one carry no marker.  What
// the path can use is DLSS-G's declared MultiFrameCount + 1, when observed
// for this stream, with the capture cadence as a fallback. A present that
// finds a capture it has not used yet follows a new game frame; others repeat it.
// NRPresentFrames=0 (default) runs NR on every present - motion scaled by
// 1/multiplier once the cadence says frame generation - and 1 runs it on the
// first present after each new capture only (present_repeat), so the
// generated frames are shown without NR.  The cadence counts every present
// while a fresh capture exists, over windows of at least one second and
// 16 game frames. Two consistent windows establish a new multiplier;
// a hitch discards the partial window. NRPresentFgSource=2 keeps the old
// timing rule. Which policy runs, the multiplier and the measured
// cadence are logged when frame generation starts, stops or changes.
//
// Stability under frame generation (v8.0.3, FH6 2026-09-25: "frame
// generation now causes massive flickering with present mode").  Under
// NRPresentFrames=0 the player must never see NR switch on and off between
// presents, and NR's history must not restart on a transient gap.  Four
// v8.0.2 causes, ranked by FH6's log: the ring of three declined 17 % of
// all presents (25-45 % while DLSS-G ran) and sent them out raw between NR
// presents (kPresentRing); those declines were left out of the cadence,
// which flipped the frame generation verdict and the motion scale every
// second or two (the old timing rule); a capture could overwrite the only
// complete one while newer ones were in flight, and the present fell to
// neutral guides and back with a history reset each way
// (NextPresentCaptureSet); and a fixed 8-present age made the last complete
// capture stale under high multipliers (kPresentGuideMaxAge).
// present[gaps=] counts what is left.
//
// Cross-queue captures (v8.0.2, FH6 dump 2026-09-25).  Streamline DLSS-G
// presents on its own thread and on an extra high-priority direct queue it
// creates, while the game evaluates DLSS - where the capture copies are
// recorded - on its own queue, one frame ahead of the present.  The game's
// queue already waits on the GPU for DLSS-G's queue to consume the previous
// frame's inputs, and DLSS-G signals that only after the present.  A queue
// Wait on the present queue for the newest capture's token therefore closes
// a cycle: the present queue waits for the game's queue, which waits for the
// present queue.  Both stop at the first Present frame, the GPU idles, and
// the game freezes or its hang watchdog kills it (FH6: hang55, 22 s; every
// field "Present mode crash" was a frame-generation title).  So a token on
// another queue's fence is never waited for on the GPU: its capture is taken
// only once the fence reports it complete on the CPU, else the newest older
// complete capture is (neutral guides, or present_no_guides under Required,
// when none is), counted in present[in_flight=] and logged once.  Under
// DLSS-G that pairs the present of game frame N-1 with frame N-1's guides,
// which is also the right pairing.  A token on the present queue's own
// tracker fence keeps the queue Wait: the queue's order already puts the
// capture's Signal ahead of it, so it can never wait for work behind itself
// (a single-queue game, KCD2).
//
// Starvation.  When ReShade stops delivering presents while NGX keeps
// evaluating (synthetic_tick_active: 250 ms without a present), nothing would
// run NR at all; once kPresentStarvedEvaluates evaluates in a row have seen
// no present, the evaluate hooks run the Upscaled hook point instead,
// counted in present[starved=] and logged once per episode.  The present
// event restamps the present clock when this path returns (dlssnr.hpp
// OnPresent), so the path's own NR feature create is never a gap.
//
// Direct3D 11 (the native route, DX11Source).  The same path runs through
// the Direct3D 11 bridge (d3d11_bridge.hpp RunBridge): the parameter block
// above stands in for the game's DLSS block, the back buffer crosses to the
// bridge's private D3D12 device through a shared twin as a DLSS output does
// (its twin rests in UAV, which the bridge tells the PresentTarget), and NR's
// result is copied back into the back buffer before ReShade's effects.  The
// guides are copied at the game's D3D11 evaluate (CapturePresentGuides11) on
// its immediate context, the context that also runs the present's bridge
// copies, so the context orders every capture before the presents that read
// it: no marker and no queue wait.  The foreign route (a third-party tool's
// D3D12 evaluates) and DX11Source=off are not served here: the first keeps
// the Upscaled hook point for the tool's evaluates, the second keeps the
// addon inert, and either is logged once.

inline constexpr uint32_t kPresentGuidesOptional = 0;
inline constexpr uint32_t kPresentGuidesRequired = 1;
inline constexpr uint32_t kPresentGuidesNever = 2;
// Present command lists in flight at once (v8.0.3; 3 through v8.0.2).  A
// present whose slot the GPU has not finished cannot run NR, and under frame
// generation the present thread runs ahead of a GPU busy with NR on every
// present: FH6 (DLSS-G 2x, 4K, NR ~6 ms per present) sent 1789 of 10264
// presents to the screen raw, 25-45 % in the windows DLSS-G ran, interleaved
// with NR presents - the "massive flicker".  The harness shape (4 presents per
// game frame on DLSS-G's queue shape, 4K) declined every 4th present with a
// ring of 3.  Eight slots cover DXGI's frame latency plus 4x frame
// generation's burst; past that the present waits for its slot (below).
inline constexpr uint32_t kPresentRing = 8;
// The bounded CPU wait for the next slot, on the present thread under
// ReShade's queue lock.  The slot was submitted kPresentRing presents earlier
// on this queue, which executes in order, so it completes as soon as the GPU
// is within kPresentRing - 1 presents of the CPU; DXGI's own frame-latency
// throttle blocks the same thread in the real Present for the same reason, so
// the wait moves that throttle rather than adding one.  100 ms (six 60 Hz
// frames) means the queue is not progressing (a wait on a CPU signal this
// thread owes): that present skips NR as before (present_busy,
// present[slot_timeouts=]), and presents stop waiting until a slot is free
// on its own, so a stuck queue costs one timeout, not one per present.
inline constexpr DWORD kPresentSlotWaitMs = 100;
// Four capture sets: the newest (the next present's), one the game may be
// copying into, one a present on the GPU may still read, and the one a
// present read last, which a capture never overwrites (NextPresentCaptureSet)
// so a present always has a complete capture to fall back on.
inline constexpr uint32_t kPresentGuideSets = 4;
// A capture older than this many game frames (presents times the stream's
// frame generation multiplier) AND older than kPresentGuideMaxAgeNs no longer
// describes the image (a menu or a loading screen without DLSS).  Through
// v8.0.2 it was 8 presents whatever the cadence: under 4x frame generation,
// with the newest capture in flight on the game's queue, the last complete
// one is 8-12 presents old, and before the cadence says frame generation
// (its first full second) the multiplier is still 1.  The harness at 12
// presents per game frame ran 573 of 1448 presents without guides that way.
inline constexpr uint64_t kPresentGuideMaxAge = 8;
inline constexpr int64_t kPresentGuideMaxAgeNs = 250'000'000;
inline constexpr uint32_t kPresentLogCap = 16;

inline std::atomic_uint64_t present_frames_seen{0};
// v8.5.0-rc4: pre-FG's NR passes (RunPresentPreFg), each a unit of the
// verdict's `seen` like an evaluate, ending in exactly one terminal
// (ProcessInline's success or decline).  Every other unit of a pre-FG
// session is ineligible - the presents (kPresentPreFg), the DLSS-G
// evaluates (kNgxNotDlssEvaluation), the game's DLSS (kPresentHook) - so
// until rc4 its verdict read eligible=0 and its card "Starting..." while NR
// ran (Resonance v8.5.0-rc1: 3773 evaluates, eligible 0).
inline std::atomic_uint64_t present_prefg_attempts{0};
inline std::atomic_uint64_t present_guides_real{0};
inline std::atomic_uint64_t present_guides_neutral{0};
inline std::atomic_uint64_t present_captures{0};
inline std::atomic_uint64_t present_capture_busy{0};
inline std::atomic_uint64_t present_guide_waits{0};
// Presents that passed over a newer capture another queue had not finished
// (Cross-queue captures above).
inline std::atomic_uint64_t present_guides_in_flight{0};
inline std::atomic_uint64_t present_starved_fallbacks{0};
inline std::atomic_uint64_t present_format_fallbacks{0};
inline std::atomic<float> present_cadence{1.f};
// The flicker a player sees (v8.0.3): presents of a stream that already ran
// NR which reached the screen without it, whatever the reason.
inline std::atomic_uint64_t present_gaps{0};
// Presents that waited on the CPU for their ring slot, and waits that timed
// out (kPresentSlotWaitMs).
inline std::atomic_uint64_t present_slot_waits{0};
inline std::atomic_uint64_t present_slot_timeouts{0};
// Temporal history restarts the path asked for (a guide source switch), and
// frame generation verdict changes.
inline std::atomic_uint64_t present_history_resets{0};
inline std::atomic_uint64_t present_fg_flips{0};
// Presents per game frame the motion scale divides by (1 = no frame
// generation).
inline std::atomic_uint32_t present_fg_multiplier{1};
// The latest DLSS-G declaration, observed even when NRPresentPreFg=0 or
// pre-FG processing falls back. Identity pointers are compared only and are
// never dereferenced. Shape/device matching is used only for an unambiguous
// presenting stream; another matching swapchain keeps timing as fallback.
struct PresentFgDeclaration {
  const void* device = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t multiplier = 0;
  int64_t observed_ns = 0;
  uint64_t observed_present = 0;
};
inline PresentFgDeclaration present_fg_declaration;
// Set by a slot wait that timed out, cleared by a present that found its
// slot free: a queue that stopped costs one timeout.  Under runtime_mutex.
inline bool present_slot_wait_off = false;

// Before frame generation (v8.1.0, RunPresentPreFg below).  NRPresentPreFg:
// 1 (default) Auto - NR runs on DLSS-G's input whenever a DLSS-G evaluate is
// seen and can be served, once per game frame with the HUD composite - 2 the
// same with two NR passes whenever a HUD-less frame is tagged (no composite),
// and 0 every present, as through v8.0.3.
inline constexpr uint32_t kPresentPreFgOff = 0;
// How long a served pre-FG frame keeps the presents from running NR: 0.1 s
// or two game frames' presents at the SERVED frame's multiplier, whichever
// lasts longer. A host hitch over 0.1 s must not enhance a frame twice.
// Latch the multiplier at the successful evaluate: timing-only overrides
// may still say 1x on the first served 4x frame, and later timing changes
// must not shrink that frame's hold. At 4x, a stopped DLSS-G hands NR back
// after max(0.1 s, 8 presents), counted at its new presentation cadence.
inline constexpr int64_t kPresentPreFgHoldNs = 100'000'000;
inline constexpr uint64_t kPresentPreFgHoldFrames = 2;
// The original direct-list contract. On a compute list NGX's shader inputs
// use NON_PIXEL_SHADER_RESOURCE alone: PIXEL_SHADER_RESOURCE is illegal on
// that queue. This still relies on the integration handing NGX shader-read
// inputs; the harness checks both contracts, not a real DLSS-G implementation.
inline constexpr D3D12_RESOURCE_STATES kPresentPreFgInputState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
// Game frames NR ran on before DLSS-G, its NR passes there (back buffer and
// HUD-less stand-in), DLSS-G evaluates seen, and the ones it could not serve.
inline std::atomic_uint64_t present_prefg_frames{0};
inline std::atomic_uint64_t present_prefg_passes{0};
inline std::atomic_uint64_t present_prefg_evaluates{0};
inline std::atomic_uint64_t present_prefg_fallbacks{0};
// v8.5.0-rc2, observe-only: served game frames whose HUD-less had another
// storage format than the frame, that were recorded on a compute list, and
// that were served while other D3D12 swapchains presented. Each is a shape
// v8.5 serves without a field log behind it.
inline std::atomic_uint64_t present_prefg_mixed_format{0};
inline std::atomic_uint64_t present_prefg_compute{0};
inline std::atomic_uint64_t present_prefg_multi_stream{0};
// v8.5.0-rc5: served game frames whose swapchain had another device face than
// DLSS-G's frame (RunPresentPreFg's matcher), and the DLSS evaluates that ran
// the Upscaled hook point because pre-FG could not serve frame generation.
inline std::atomic_uint64_t present_prefg_foreign_face{0};
inline std::atomic_uint64_t present_prefg_upscaled{0};
// v8.5.0-rc5.  While DLSS-G interpolates, the presents cannot stand in for
// pre-FG: a game whose generated frames never pass ReShade's swapchain showed
// NR on one displayed frame in three (Wuthering Waves v8.5.0-rc1: 398 presents
// seen, all real, at 3x; pre-FG fell back on every game frame).  After this
// many game frames in a row pre-FG could not serve for a reason of the frame's
// shape (kPresentPreFgMisses; not NR's own declines, which the Upscaled path
// meets too), the DLSS evaluates run the Upscaled hook point - v7's path, which
// DLSS-G interpolates from on every frame it makes - and the presents run none,
// until pre-FG could serve this many in a row again.  NRPresentPreFg=0 keeps
// the presents: the user chose them.  Under runtime_mutex, except the flag.
inline constexpr uint32_t kPresentPreFgMissFrames = 30;
inline constexpr uint32_t kPresentPreFgMisses = 2 | 8 | 16 | 32 | 128 | 1024;
inline uint32_t present_prefg_miss_streak = 0;
inline uint32_t present_prefg_clear_streak = 0;
inline uint32_t present_prefg_episodes_logged = 0;
inline std::atomic_bool present_prefg_upscaled_active{false};
// Under runtime_mutex: the last successful pre-FG frame's clocks and
// multiplier, plus the swapchain's target and size from its last present.
inline int64_t present_prefg_served_ns = 0;
inline uint64_t present_prefg_served_presents = 0;
inline uint32_t present_prefg_served_multiplier = 1;
inline PresentTarget present_prefg_target;
inline uint32_t present_prefg_target_multiplier = 1;
inline uint32_t present_prefg_width = 0;
inline uint32_t present_prefg_height = 0;
inline const void* present_prefg_served_swapchain = nullptr;
// The two pre-FG NR streams' keys in present_streams (never dereferenced).
inline const uint8_t present_prefg_keys[2] = {};
// Under runtime_mutex: NR's copy of DLSS-G's HUDLess (at rest in the selected
// input state), and the game's HUDLess it stands in for this
// game frame, so the frame's later evaluates (MultiFrameIndex 2..n) get it
// too.
inline ID3D12Resource* present_prefg_hudless = nullptr;
inline D3D12_RESOURCE_STATES present_prefg_hudless_state = kPresentPreFgInputState;
inline ID3D12Resource* present_prefg_hudless_source = nullptr;
inline const void* present_prefg_hudless_device = nullptr;
// v8.1.0, the one-pass HUD composite (shaders/prefg_compose.cs_5_1.hlsl):
// with a HUD-less frame NR runs once, on the stand-in, and the real frame is
// recomposed from it with the game's HUD kept.  NRPresentPreFg=2 forces the
// two-pass path (NR on the stand-in and on the back buffer).  3 (v8.5.0-rc4)
// is Auto as it was through rc3: a frame whose mask is not a HUD (above
// kPresentPreFgHudShare) runs the two passes.  Auto (1) gives such a frame
// the residual composite instead, one pass, when it differs from its HUD-less
// frame by grain everywhere but a HUD-sized share; a frame that differs by
// more over a larger share (a menu over most of the screen, a tone map after
// the UI) is ambiguous and runs the two passes, as through rc3
// (shaders/prefg_compose.cs_5_1.hlsl).
inline constexpr uint32_t kPresentPreFgTwoPass = 2;
inline constexpr uint32_t kPresentPreFgHudTwoPass = 3;
// Served frames the GPU measured as grain above the HUD share under Auto,
// which took the residual (read late, like prefg_hud_permille).
inline std::atomic_uint64_t present_prefg_residual{0};
// The difference mask's tolerance in the encoded domain: half an 8-bit step
// (1 / 510), so a HUD pixel one 8-bit step off the scene is HUD while 10-bit
// and FP16 rounding (under 1 / 1023, relative above 1) is not.
inline constexpr float kPresentPreFgEpsilon = 1.f / 510.f;
// Above this share of HUD pixels the mask is not a HUD: a HUD covers a few
// percent of the screen (the harness rectangle is 4 %), while grain or a tone
// map after the UI makes nearly every pixel differ (the harness grain arm:
// all of them).  25 % sits far from both; a frame above it runs two passes.
inline constexpr float kPresentPreFgHudShare = 0.25f;
// Measurements at or below the share, in a row, before the mask path is
// trusted again after one above it (which leaves at once).  Before the first
// measurement arrives (a few game frames) the path is two-pass, the
// conservative one; the first clean measurement trusts it.
inline constexpr uint32_t kPresentPreFgCleanFrames = 30;
// Descriptor sets and stats slots in rotation. Reuse requires the tracked
// recording to be discarded and its submissions complete; queue depth or
// elapsed frames are not lifetime proof. Every use writes fresh views, so
// recycled game-resource addresses never retain a stale descriptor.
inline constexpr uint32_t kPresentPreFgComposeSets = 16;
inline constexpr uint32_t kPresentPreFgComposeDescriptors = 6;
// Per slot: [HUD pixels, serial, grain-sized pixels] (the shader's Stats).
inline constexpr uint32_t kPresentPreFgStatsWords = 3;
// Under runtime_mutex.  The composite's objects, made in the world (device
// face) of the list that first ran it; a list of another world makes a new
// heap and keeps the old one until shutdown (it may still be in flight).
struct PreFgCompose {
  const ID3D12Device* device = nullptr;  // compared only
  ID3D12DescriptorHeap* heap = nullptr;
  uint32_t descriptor_size = 0;
  ID3D12Resource* output = nullptr;    // the recomposed frame, UAV at rest
  ID3D12Resource* stats = nullptr;     // kPresentPreFgStatsWords per slot, COMMON at rest
  ID3D12Resource* zero = nullptr;      // upload: one slot's zero words
  ID3D12Resource* readback = nullptr;  // persistently mapped
  const uint32_t* readback_data = nullptr;
  uint32_t serial = 0;       // game frames composed
  uint32_t read_serial = 0;  // the newest measurement read
  uint32_t clean_frames = 0;
  uint8_t slot_mode[kPresentPreFgComposeSets] = {};  // the shader mode each slot measured
  bool one_pass = false;     // the mask path is trusted
  bool exceeded = false;     // a mask measurement was ever above the share
  bool unavailable = false;  // the PSO could not be made: two passes for the session
};
inline PreFgCompose present_prefg_compose;
// Without submission proofs, old heaps stay alive until present-path teardown.
inline std::vector<ID3D12DescriptorHeap*> present_prefg_old_heaps;
inline ID3D12PipelineState* present_prefg_compose_pso = nullptr;
// Game frames served on the two-pass path (a HUD-less frame whose composite
// could not run or whose mask read as more than HUD), and the latest HUD
// share the composite measured (mask or UI alpha), in 1/1000.
inline std::atomic_uint64_t present_prefg_two_pass{0};
inline std::atomic_uint32_t present_prefg_hud_permille{0};
// The last present ran no NR because a pre-FG frame served it (the status
// card's "NR once per game frame, before frame generation").
inline std::atomic_bool present_prefg_active{false};
// Evaluates since the last present (PresentTakesEvaluate).
inline std::atomic_uint32_t present_unpresented_evaluates{0};
inline constexpr uint32_t kPresentStarvedEvaluates = 3;
// Raised by a present whose swapchain cannot be served, lowered by one that
// can; while it is up the evaluates run the Upscaled hook point.
inline std::atomic_bool present_unusable{false};
// The steady clock of the last present the path was selected for, 0 once
// RetireIdlePresentPath released what it held.
inline std::atomic<int64_t> present_served_ns{0};
// The presenting display's Windows SDR white level in nits (0 = unknown),
// polled about once a second outside runtime_mutex.
inline std::atomic<float> present_sdr_white{0.f};
inline std::atomic<int64_t> present_sdr_white_polled_ns{0};
// v8.5.0-rc8 (PLAN_SETUP_MATRIX.md F4): an upstream RenoDX mod's Game
// Brightness in nits (0 = none), polled with the Windows level.  A RenoDX
// HDR mod puts the game's diffuse white at its ToneMapGameNits; the Windows
// SDR level is the white of SDR content under HDR, which such a frame never
// uses.  Onimusha rc5 at Present: 223 nits from the Windows slider against
// the mod's 100, NR's input a stop and more too dark (the black-level
// report).  Every RenoDX mod stores it in ReShade.ini [renodx-preset1]: the
// mod starts each session on preset 1 and does not store a switch
// (utils/settings.hpp preset_index).  Taken only while a RenoDX module
// other than this add-on is loaded (renodx_mod_loaded, the module scan in
// InstallHooks: an uninstalled mod leaves its ini section behind), with its
// tone mapper on (ToneMapType 0 is the game's own) and the value in
// 48-10000; any signal absent, the Windows level stays the source.
// NRPresentWhiteSource=1 keeps the Windows level; NRPresentWhiteNits > 0
// outranks both.
inline std::atomic<float> present_mod_white{0.f};
inline std::atomic_bool renodx_mod_loaded{false};
// The white source the last HDR present logged, and its nits.
inline std::atomic<const char*> present_white_logged{nullptr};
inline std::atomic<float> present_white_logged_nits{0.f};

struct PresentGuideSet {
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;
  const void* device_identity = nullptr;
  const NVSDK_NGX_Handle* source_handle = nullptr;  // compared only
  // 0 = never captured.
  uint64_t serial = 0;
  uint64_t comparison_request = 0;  // F5 request whose inline pass was bypassed
  // present_generation and the steady clock at the capture.
  uint64_t captured_at = 0;
  int64_t captured_ns = 0;
  // The ring fence value of the last present that read it.
  uint64_t read_value = 0;
  // A tracker use no other object has: its submissions are this capture's.
  const void* marker = nullptr;
  // F5 still needs queue identity after a completed recording is pruned.
  // Keep one exact fence/value; multiple producing fences are ambiguous.
  // This remains fixed size even if the game replays a recording repeatedly.
  FenceRef completed_fence;
  uint64_t completed_value = 0;
  bool completed_multiple_queues = false;
  // The capture's guide contract (dims, subrects, motion scale, flags, the
  // game's Reset) and the DLSS output it describes.
  FeatureState contract;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  // A Direct3D 11 capture (CapturePresentGuides11) fills these instead of
  // the D3D12 clones, and only a present of its own API reads it.
  ID3D11Texture2D* motion11 = nullptr;
  ID3D11Texture2D* depth11 = nullptr;
  bool d3d11 = false;
};

struct PresentSlot {
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  uint64_t value = 0;
};

// One ring per device face (NGX records in the world of its list, so the
// lists are made on the face NR's runtime was initialized on).
struct PresentRing {
  ID3D12Device* device = nullptr;
  ID3D12Fence* fence = nullptr;
  uint64_t submitted = 0;
  PresentSlot slots[kPresentRing];
};

// One NR stream per swapchain: its temporal history is the NR feature
// registered under Handle(), an address no NGX handle can have (the
// Streamline path keys its synthetic streams the same way; never
// dereferenced).
struct PresentStream {
  uint8_t handle_key = 0;
  const NVSDK_NGX_Handle* Handle() const {
    return reinterpret_cast<const NVSDK_NGX_Handle*>(&handle_key);
  }
  bridge::OwnedParameters parameters;
  ID3D12Resource* neutral_motion = nullptr;
  ID3D12Resource* neutral_depth = nullptr;
  ID3D11Texture2D* neutral_motion11 = nullptr;
  ID3D11Texture2D* neutral_depth11 = nullptr;
  uint64_t consumed_serial = 0;
  bool real_guides = false;
  ::renodx::dlss5::PresentCadence fg_cadence;
  const void* device_identity = nullptr;
  // The device behind a Streamline proxy that ReShade took for the native
  // device (StreamlineBaseIdentity), or null.  Compared only.
  const void* streamline_identity = nullptr;
  uint32_t backbuffer_width = 0;
  uint32_t backbuffer_height = 0;
  DXGI_FORMAT backbuffer_format = DXGI_FORMAT_UNKNOWN;
  PresentTarget prefg_target;
  bool prefg_target_valid = false;
  bool d3d11 = false;
  uint64_t cadence_captures = 0;
  const NVSDK_NGX_Handle* cadence_source = nullptr;  // compared only
  int64_t cadence_source_ns = 0;
  int64_t cadence_ambiguous_until_ns = 0;
  // Whole-number presentation multiplier, not a noisy instantaneous ratio.
  uint32_t fg_multiplier = 1;
  // Presents this stream ran NR on (present[gaps=] counts the ones after
  // the first that did not).
  uint64_t nr_presents = 0;
};

struct PresentRetired {
  IUnknown* object = nullptr;
  const void* key = nullptr;
  GpuLease lease;
  bool tracker_proof = false;
};

inline PresentRing present_ring;
inline PresentGuideSet present_guide_sets[kPresentGuideSets];
inline uint64_t present_capture_serial = 0;
inline std::unordered_map<const void*, PresentStream> present_streams;
// One observed FG-only stream. Interleaved identities restart admission;
// equal-size views remain ambiguous until exactly one swapchain matches.
// Only identity comparisons use these pointers, under runtime_mutex.
struct AutoFgObservation {
  const void* device = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  // Concrete, as the pre-FG owner matcher compares it: a same-size window
  // in another format (HDR10 beside SDR) is not this frame's stream.
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint64_t evaluates = 0;
  int64_t last_ns = 0;
  const void* swapchain = nullptr;
};
inline AutoFgObservation auto_fg_observation;
// Sticky until device teardown: SR absence is not a safe ownership signal
// when generated frames can still carry earlier inline NR.
inline bool auto_present_frame_generation_seen = false;
// Last admitted owner for a nonblocking Present observation. It is compared
// only; RunPresentPath revalidates the locked state before recording work.
inline std::atomic<const void*> auto_fg_fallback_swapchain{nullptr};
inline std::vector<PresentRetired> present_retired;
// Under runtime_mutex: an evaluate fallback episode is logged once.
inline bool present_fallback_episode = false;
// Inline fallback may already be recorded for a future presented image.
// Resuming presents or clearing the starvation counter does not prove that
// image is untouched. F5 must establish a new bypassed frame first.
inline bool present_inline_fallback_pending = false;
inline uint32_t present_logged_lines = 0;

inline void LogPresent(reshade::log::level level, const std::string& message) {
  if (present_logged_lines >= kPresentLogCap) return;
  ++present_logged_lines;
  Log(level, "Present hook point: " + message
                 + (present_logged_lines == kPresentLogCap
                        ? " (further Present hook point lines are not logged)"
                        : ""));
}

// Caller holds runtime_mutex.  Retirement, like every other addon object:
// released once the tracker proves no recording can reach it.
template <typename T>
inline void RetirePresentResource(T** resource) {
  if (*resource == nullptr) return;
  present_retired.push_back(
      {*resource, *resource, AcquireGpuLease(),
       queue_tracking_active.load(std::memory_order_relaxed)});
  *resource = nullptr;
}

inline void DrainPresentRetired() {
  for (auto it = present_retired.begin(); it != present_retired.end();) {
    if (it->tracker_proof ? submission::ResourceReleasable(it->key)
                          : !it->lease.empty()
                                && QueryGpuLease(it->lease) != GpuLeaseState::kPending) {
      it->object->Release();
      it = present_retired.erase(it);
    } else {
      ++it;
    }
  }
}

// Our own queue signal follows every submission, including any guide waits.
// A removed device cannot execute the recording again. Caller holds
// runtime_mutex, which keeps the ring and its submitted value together.
inline bool PresentRingComplete() {
  return present_ring.fence == nullptr
      ? present_ring.submitted == 0
      : present_ring.fence->GetCompletedValue() >= present_ring.submitted;
}

// Poll only: a device-face switch may run here while another queue is busy.
// The former bounded wait released list allocators after timeout, beneath
// their GPU work. Keep the ring AND guide read values intact until complete;
// EnsurePresentRing retries then, so captures cannot overwrite guides still
// read by the old ring. Caller holds runtime_mutex.
inline bool ReleasePresentRing() {
  if (!PresentRingComplete()) return false;
  for (PresentSlot& slot : present_ring.slots) {
    if (slot.list != nullptr) submission::OnCommandListDestroyed(slot.list);
    ReleaseCom(slot.list);
    ReleaseCom(slot.allocator);
  }
  ReleaseCom(present_ring.fence);
  ReleaseCom(present_ring.device);
  present_ring = {};
  // The fence the sets' read values name is gone with it.
  for (PresentGuideSet& set : present_guide_sets) set.read_value = 0;
  return true;
}

// Caller holds runtime_mutex.
inline bool EnsurePresentRing(ID3D12Device* face) {
  if (present_ring.device == face && present_ring.fence != nullptr) return true;
  if (!ReleasePresentRing()) return false;
  // RENODX_NR_TEST_FAIL_PRESENT_RING=<n>: the first n builds fail, so the
  // lane that covers the fallback (present_switch) can produce it.
  static uint32_t test_failures_left = [] {
    char buffer[16] = {};
    size_t length = 0;
    if (getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_FAIL_PRESENT_RING") != 0
        || length == 0) {
      return 0u;
    }
    return static_cast<uint32_t>(strtoul(buffer, nullptr, 10));
  }();
  bool built = test_failures_left == 0;
  if (!built) --test_failures_left;
  built = built
      && SUCCEEDED(face->CreateFence(
          0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&present_ring.fence)));
  for (PresentSlot& slot : present_ring.slots) {
    built = built
        && SUCCEEDED(face->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))
        && SUCCEEDED(face->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, nullptr,
            IID_PPV_ARGS(&slot.list)))
        && SUCCEEDED(slot.list->Close());
  }
  if (!built) {
    ReleasePresentRing();
    LogPresent(reshade::log::level::error,
               "the present command list ring could not be created");
    return false;
  }
  face->AddRef();
  present_ring.device = face;
  // A rebuild waits for the old ring, so more than one per device is a cost
  // worth seeing in a field log.
  static uint32_t builds = 0;
  LogPresent(reshade::log::level::info,
             "present command list ring built (" + std::to_string(++builds) + ")");
  return true;
}

// The stream's history restarts: its guides changed source (captured <->
// neutral), so the model's previous frame is not the one they describe.
inline void ResetPresentStreamHistory(PresentStream* stream) {
  present_history_resets.fetch_add(1, std::memory_order_relaxed);
  if (const auto feature = features.find(stream->Handle()); feature != features.end()) {
    for (NrFeatureSlot& slot : feature->second.slots) slot.pending_reset = true;
  }
}

// Caller holds runtime_mutex.
inline void RetirePresentStream(const void* swapchain) {
  const auto it = present_streams.find(swapchain);
  if (it == present_streams.end()) return;
  PresentStream& stream = it->second;
  if (auto_present_state.owner.swapchain == swapchain) {
    SetAutoPresentFallbackActive(false);
    auto_present_state.Reset();
    auto_present_handle = nullptr;
  }
  if (const auto feature = features.find(stream.Handle()); feature != features.end()) {
    ReleaseAllNrSlots(feature->second, true);
    features.erase(feature);
  }
  RetireWorkset(
      {stream.Handle(), reinterpret_cast<const ID3D12Resource*>(&stream)});
  RetirePresentResource(&stream.neutral_motion);
  RetirePresentResource(&stream.neutral_depth);
  // Direct3D 11: the stream's bridge twins, and its neutral guides (the
  // D3D11 runtime defers their destruction until its GPU work is done).
  RetireBridgeSetLocked(stream.Handle());
  ReleaseCom(stream.neutral_motion11);
  ReleaseCom(stream.neutral_depth11);
  present_streams.erase(it);
  if (present_prefg_served_swapchain == swapchain) {
    present_prefg_served_swapchain = nullptr;
    present_prefg_served_ns = 0;
    present_prefg_hudless_source = nullptr;
  }
  if (auto_fg_observation.swapchain == swapchain) {
    auto_fg_observation = {};
    auto_fg_fallback_active.store(false, std::memory_order_relaxed);
  }
}

// A fresh session's routing state (ReleasePresentPath, RetireIdlePresentPath).
// Caller holds runtime_mutex.
inline void ResetPresentPathState() {
  present_unusable.store(false, std::memory_order_relaxed);
  present_fallback_episode = false;
  present_inline_fallback_pending = false;
  present_slot_wait_off = false;
  present_prefg_served_ns = 0;
  present_prefg_served_presents = 0;
  present_prefg_served_multiplier = 1;
  present_prefg_target_multiplier = 1;
  present_prefg_served_swapchain = nullptr;
  workset_idle_exempt_handle = nullptr;
  present_prefg_active.store(false, std::memory_order_relaxed);
  present_prefg_width = 0;
  present_prefg_height = 0;
  present_prefg_hudless_source = nullptr;
  present_prefg_hudless_device = nullptr;
  present_prefg_miss_streak = 0;
  present_prefg_clear_streak = 0;
  present_prefg_upscaled_active.store(false, std::memory_order_relaxed);
}

// The composite's objects retire like the path's others; without submission
// proofs its heap waits for the path's teardown.  Caller holds runtime_mutex.
inline void RetirePreFgCompose() {
  PreFgCompose& compose = present_prefg_compose;
  if (queue_tracking_active.load(std::memory_order_relaxed)) {
    RetirePresentResource(&compose.heap);
  } else if (compose.heap != nullptr) {
    present_prefg_old_heaps.push_back(compose.heap);
    compose.heap = nullptr;
  }
  if (compose.readback_data != nullptr) compose.readback->Unmap(0, nullptr);
  for (ID3D12Resource** resource :
       {&compose.output, &compose.stats, &compose.zero, &compose.readback}) {
    RetirePresentResource(resource);
  }
  compose = PreFgCompose{};
}

// v8.5.0-rc5: the path's memory, once it has served nothing for
// kWorksetIdleRetireNs - after an F5 comparison at an inline hook point, or a
// switch away from Present.  Through rc4 its guide captures (four motion and
// depth pairs at render size: 112-253 MiB at 4K), pre-FG surfaces and ring
// stayed to the game's exit unless NRDisableRelease=1 released them at F6
// OFF, and rc4 made 0 the default; v7 never allocated them.  Its streams'
// features and worksets go too.  Everything retires on the tracker's proof
// like the path's other objects, the ring once its fence passed.  Caller holds
// runtime_mutex; true once all of it is released.
inline bool RetireIdlePresentPath() {
  if (!ReleasePresentRing()) return false;
  while (!present_streams.empty()) RetirePresentStream(present_streams.begin()->first);
  for (PresentGuideSet& set : present_guide_sets) {
    RetirePresentResource(&set.motion);
    RetirePresentResource(&set.depth);
    ReleaseCom(set.motion11);
    ReleaseCom(set.depth11);
    set = {};
  }
  RetirePresentResource(&present_prefg_hudless);
  RetirePreFgCompose();
  ResetPresentPathState();
  DrainPresentRetired();
  return present_retired.empty();
}

// Shutdown, after the tracked queues were waited for and the feature states
// released.  Caller holds runtime_mutex.
inline bool ReleasePresentPath() {
  if (!ReleasePresentRing()) return false;
  for (auto& [_, stream] : present_streams) {
    ReleaseCom(stream.neutral_motion);
    ReleaseCom(stream.neutral_depth);
    ReleaseCom(stream.neutral_motion11);
    ReleaseCom(stream.neutral_depth11);
  }
  present_streams.clear();
  SetAutoPresentFallbackActive(false);
  auto_present_state.Reset();
  auto_present_inline_handle = nullptr;
  auto_present_handle = nullptr;
  auto_present_frame_generation_seen = false;
  present_fg_declaration = {};
  auto_fg_observation = {};
  auto_fg_fallback_active.store(false, std::memory_order_relaxed);
  for (PresentGuideSet& set : present_guide_sets) {
    ReleaseCom(set.motion);
    ReleaseCom(set.depth);
    ReleaseCom(set.motion11);
    ReleaseCom(set.depth11);
    set = {};
  }
  for (const PresentRetired& retired : present_retired) retired.object->Release();
  present_retired.clear();
  ResetPresentPathState();
  ReleaseCom(present_prefg_hudless);
  PreFgCompose& compose = present_prefg_compose;
  if (compose.readback != nullptr && compose.readback_data != nullptr) {
    compose.readback->Unmap(0, nullptr);
  }
  ReleaseCom(compose.heap);
  ReleaseCom(compose.output);
  ReleaseCom(compose.stats);
  ReleaseCom(compose.zero);
  ReleaseCom(compose.readback);
  compose = {};
  for (ID3D12DescriptorHeap* heap : present_prefg_old_heaps) heap->Release();
  present_prefg_old_heaps.clear();
  ReleaseCom(present_prefg_compose_pso);
  return true;
}

// Called by every evaluate hook, under runtime_mutex, after the game's real
// evaluate.  False at another hook point, in a session the path does not
// serve (PresentHookServes: a Direct3D 11 session off the native route, where
// the D3D12 evaluates are a third-party tool's and no present would run NR
// for them), and while the present path cannot serve (presents starved, the
// swapchain unusable): the evaluate then runs the Upscaled hook point, the
// last two counted and logged once per episode.
//
// Starved takes the lifecycle's flag (250 ms without a present) AND
// kPresentStarvedEvaluates evaluates in a row with no present between them.
// The flag alone rises on the first evaluate after any 250 ms gap, and a
// game's first DLSS frame after a load that presented nothing is exactly
// that: measured, the first Required frame-generation run fell back on its
// very first evaluate and built an Upscaled NR feature and workset (1.5 s of
// CPU) for four frames the present path then served anyway.
inline bool PresentTakesEvaluate() {
  // Auto-FG serves a stream with no DLSS evaluates. A real DLSS evaluate
  // (including an independent view) keeps its selected inline insertion.
  if (!PresentHookServes(false)) return false;
  if (screenshot::WantsFinalPresent()) {
    // F5 temporarily reserves this unenhanced game frame for a final-display
    // comparison. Inline fallback must not enhance it first.
    return true;
  }
  const bool starved =
      present_unpresented_evaluates.fetch_add(1, std::memory_order_relaxed) + 1
          >= kPresentStarvedEvaluates
      && synthetic_tick_active.load(std::memory_order_relaxed);
  const bool unusable = present_unusable.load(std::memory_order_relaxed);
  if (!starved && !unusable && !present_prefg_upscaled_active.load(std::memory_order_relaxed)) {
    present_fallback_episode = false;
    return true;
  }
  (starved ? present_starved_fallbacks : unusable ? present_format_fallbacks : present_prefg_upscaled)
      .fetch_add(1, std::memory_order_relaxed);
  present_inline_fallback_pending = true;
  // Frame generation's episode is logged where it starts (RunPresentPreFg).
  if (!present_fallback_episode && (starved || unusable)) {
    present_fallback_episode = true;
    LogPresent(reshade::log::level::warning,
               starved ? "present events are starved; the DLSS evaluates run the"
                         " Upscaled hook point until presents resume"
                       : "the swapchain cannot be served; the DLSS evaluates run"
                         " the Upscaled hook point until it can");
  }
  return false;
}

// The guide contract a capture carries, by ProcessInline's rules: the
// evaluate's values (`contract`, DeriveFeatureState of its block), the create
// contract where the evaluate sends none, the Color's geometry last.  False
// when no input size can be found.
inline bool CompletePresentContract(const NVSDK_NGX_Handle* handle,
                                    uint32_t color_width,
                                    uint32_t color_height,
                                    FeatureState* contract) {
  if (const auto known = features.find(handle); known != features.end()) {
    const FeatureState& feature = known->second;
    if (contract->input_width == 0) {
      contract->input_width = feature.create_input_width != 0
          ? feature.create_input_width : feature.input_width;
    }
    if (contract->input_height == 0) {
      contract->input_height = feature.create_input_height != 0
          ? feature.create_input_height : feature.input_height;
    }
    if (contract->motion_x == 0 && contract->motion_y == 0) {
      contract->motion_x = feature.motion_x;
      contract->motion_y = feature.motion_y;
    }
    if (contract->depth_x == 0 && contract->depth_y == 0) {
      contract->depth_x = feature.depth_x;
      contract->depth_y = feature.depth_y;
    }
    if (contract->create_flags == 0) contract->create_flags = feature.create_flags;
  }
  if (contract->input_width == 0) contract->input_width = color_width;
  if (contract->input_height == 0) contract->input_height = color_height;
  if (contract->input_width == 0 || contract->input_height == 0) return false;
  // An absent MV scale is 1, as DLSS reads it (ResolveMotionScale); the flag
  // stays unset so FillPresentBlock passes the absence on.  Through
  // v8.5.0-rc6 the Present path made it the render size - every vector
  // render-width times too long, the rc10 defect ProcessInline had already
  // shed - so NR's history and Detail stability reprojected off-screen
  // whenever the camera moved, in any game that sends no MV_Scale.
  if (!contract->has_motion_scale_x) contract->motion_scale_x = 1.f;
  if (!contract->has_motion_scale_y) contract->motion_scale_y = 1.f;
  return true;
}

// The set a capture overwrites: the oldest one no present on the GPU still
// reads (a Direct3D 11 set has no read value; the immediate context orders
// it), and never the one a present read last.  That one was proven complete
// (or ordered on the present queue) when it was read, so while every newer
// capture is still in flight on the game's queue (DLSS-G, a GPU-bound frame)
// the next present still has it; through v8.0.2 a game two captures ahead
// could overwrite it, and the present fell to neutral guides and back, a
// history reset each way (FH6: 62 neutral presents).  Null, counted in
// present_capture_busy, while every set is read.
inline PresentGuideSet* NextPresentCaptureSet() {
  const uint64_t done = present_ring.fence != nullptr
      ? present_ring.fence->GetCompletedValue() : UINT64_MAX;
  const PresentGuideSet* last_read = nullptr;
  for (const PresentGuideSet& set : present_guide_sets) {
    if (set.read_value != 0 && (last_read == nullptr || set.read_value > last_read->read_value)) {
      last_read = &set;
    }
  }
  PresentGuideSet* target = nullptr;
  for (PresentGuideSet& set : present_guide_sets) {
    if (&set == last_read || (done != UINT64_MAX && set.read_value > done)) continue;
    if (target == nullptr || set.serial < target->serial) target = &set;
  }
  if (target == nullptr) present_capture_busy.fetch_add(1, std::memory_order_relaxed);
  return target;
}

inline bool PresentAspectMatches(uint32_t source_width, uint32_t source_height,
                                 uint32_t target_width, uint32_t target_height) {
  return source_width != 0 && source_height != 0 && target_width != 0 && target_height != 0
         && std::abs(static_cast<double>(source_width) * target_height
                     - static_cast<double>(target_width) * source_height)
                <= 0.01 * static_cast<double>(target_width) * source_height;
}

inline void StampPresentCapture(PresentGuideSet* set, const FeatureState& contract,
                                uint32_t output_width, uint32_t output_height,
                                bool d3d11) {
  set->serial = ++present_capture_serial;
  set->captured_at = present_generation;
  set->captured_ns = SteadyNowNs();
  set->contract = contract;
  set->output_width = output_width;
  set->output_height = output_height;
  set->d3d11 = d3d11;
  set->comparison_request = screenshot::evaluation_request;
  set->completed_fence = {};
  set->completed_value = 0;
  set->completed_multiple_queues = false;
  present_captures.fetch_add(1, std::memory_order_relaxed);
  // Global capture totals include other devices and viewports. Only this
  // stream's API/device/output shape can be its cadence denominator. Two
  // feature handles actively feeding the same shape are ambiguous: retain
  // its current multiplier rather than reading two renders as half the FG.
  for (auto& [_, stream] : present_streams) {
    if (stream.d3d11 != d3d11 || stream.device_identity != set->device_identity
        || !PresentAspectMatches(output_width, output_height,
                                 stream.backbuffer_width, stream.backbuffer_height)) {
      continue;
    }
    if (stream.cadence_source != nullptr && stream.cadence_source != set->source_handle
        && set->captured_ns - stream.cadence_source_ns <= kPresentGuideMaxAgeNs) {
      stream.cadence_ambiguous_until_ns = set->captured_ns + 1'000'000'000;
    }
    stream.cadence_source = set->source_handle;
    stream.cadence_source_ns = set->captured_ns;
    ++stream.cadence_captures;
  }
}

// Called by retirement with runtime_mutex held, inside the tracker's prune
// lock. Copy before erasure so a concurrently completed submission cannot
// fall between a separate token snapshot and pruning. A discarded unsubmitted
// recording has no tokens and must never become a same-queue proof.
inline void PreservePresentCaptureProof(
    const void* marker, const std::vector<submission::GenerationSubmission>& submissions) {
  if (submissions.empty()) return;
  for (PresentGuideSet& set : present_guide_sets) {
    if (set.serial == 0 || set.marker != marker || set.comparison_request == 0) continue;
    for (const submission::GenerationSubmission& proof : submissions) {
      if (set.completed_fence.empty()) set.completed_fence = proof.fence;
      if (set.completed_fence.fence != proof.fence.fence) {
        set.completed_multiple_queues = true;
      } else {
        set.completed_value = std::max(set.completed_value, proof.value);
      }
    }
    break;
  }
}

inline void CapturePresentGuides(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,
    D3D12_RESOURCE_STATES motion_state,
    D3D12_RESOURCE_STATES depth_state) {
  ID3D12Resource* const output = GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Output);
  // One capture per evaluate chain, admitted like an after-path pass: a real
  // NGX entry owns the stream, so the same slEvaluateFeature's Streamline
  // fallback stays out, and the outer entry of a plugin->core chain is the
  // inner one's mirror.  Through v8.0.1 neither held here: every Streamline
  // title's evaluate also ran the fallback (a second capture, or a "capture
  // lacks" warning), and every nested chain captured twice per frame, which
  // halved the frame generation cadence.  The evaluate's terminal is counted
  // here, so the mirror is not also counted as lending its guides.
  if (output != nullptr) {
    if (!AdmitOutputPass(output, handle)) return;
    RecordOutputPass(output, handle);
  }
  CountNrDecline(NrDeclineReason::kPresentHook);
  if (present_guides.load(std::memory_order_relaxed) == kPresentGuidesNever
      && !screenshot::WantsFinalPresent()) return;
  ID3D12Resource* const motion =
      GetD3D12Resource(parameters, NVSDK_NGX_Parameter_MotionVectors);
  ID3D12Resource* const depth = GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Depth);
  if (motion == nullptr || depth == nullptr || output == nullptr) return;
  const D3D12_RESOURCE_DESC motion_desc = motion->GetDesc();
  const D3D12_RESOURCE_DESC depth_desc = depth->GetDesc();
  const D3D12_RESOURCE_DESC output_desc = output->GetDesc();
  for (const D3D12_RESOURCE_DESC* desc : {&motion_desc, &depth_desc}) {
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || desc->DepthOrArraySize != 1 || desc->SampleDesc.Count != 1) {
      return;
    }
  }
  FeatureState contract = DeriveFeatureState(parameters);
  D3D12_RESOURCE_DESC color_desc = {};
  if (ID3D12Resource* const color = GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Color);
      color != nullptr) {
    color_desc = color->GetDesc();
  }
  if (!CompletePresentContract(handle, static_cast<uint32_t>(color_desc.Width),
                               color_desc.Height, &contract)) {
    return;
  }
  PresentGuideSet* const target = NextPresentCaptureSet();
  if (target == nullptr) return;
  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return;
  const void* const device_identity = renodx::addons::dlss5::native_identity::Get(device);
  // Reallocation may succeed for only one guide. Until both copies are
  // recorded and StampPresentCapture publishes them, this slot is not a
  // readable capture, even if it previously held a completed older frame.
  target->serial = 0;
  // Same shape as the game's guide, mip 0 only, resting in the evaluate's
  // read state. A shape or native-device change retires the old clone;
  // matching descriptions do not make resources cross-device copyable.
  const auto ensure_clone = [&](ID3D12Resource** clone, const D3D12_RESOURCE_DESC& source,
                                const wchar_t* name) {
    if (*clone != nullptr) {
      const D3D12_RESOURCE_DESC have = (*clone)->GetDesc();
      if (target->device_identity == device_identity
          && have.Width == source.Width && have.Height == source.Height
          && have.Format == source.Format && have.Flags == source.Flags) {
        return true;
      }
      RetirePresentResource(clone);
    }
    D3D12_RESOURCE_DESC desc = source;
    desc.MipLevels = 1;
    desc.Alignment = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(clone)))
        || *clone == nullptr) {
      *clone = nullptr;
      LogPresent(reshade::log::level::error,
                 "a DLSS guide capture surface could not be created (format "
                     + std::to_string(static_cast<uint32_t>(source.Format)) + ' '
                     + std::to_string(source.Width) + 'x'
                     + std::to_string(source.Height) + ')');
      return false;
    }
    (*clone)->SetName(name);
    return true;
  };
  const bool cloned =
      ensure_clone(&target->motion, motion_desc, L"DLSS5 Generic present motion capture")
      && ensure_clone(&target->depth, depth_desc, L"DLSS5 Generic present depth capture");
  if (cloned) {
    target->device_identity = device_identity;
    target->source_handle = handle;
  }
  device->Release();
  if (!cloned) return;
  {
    // DLSS guide 3.4: the inputs are in NON_PIXEL_SHADER_RESOURCE at the
    // evaluate and DLSS leaves them there; Streamline tags declare theirs.
    // Subresource 0 only, as the after path treats the output.
    const InjectedCommandScope injected;
    const std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES> sources[] = {
        {motion, motion_state}, {depth, depth_state}};
    ID3D12Resource* const clones[] = {target->motion, target->depth};
    for (size_t i = 0; i < 2; ++i) {
      Transition(command_list, sources[i].first, sources[i].second,
                 D3D12_RESOURCE_STATE_COPY_SOURCE, 0u);
      Transition(command_list, clones[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_COPY_DEST);
      CopyMip0(command_list, clones[i], sources[i].first);
      Transition(command_list, clones[i], D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      Transition(command_list, sources[i].first, D3D12_RESOURCE_STATE_COPY_SOURCE,
                 sources[i].second, 0u);
    }
  }
  StampPresentCapture(target, contract, static_cast<uint32_t>(output_desc.Width),
                      output_desc.Height, false);
  target->marker = reinterpret_cast<const void*>(
      0xFFFF'0000'0000'0000ull | target->serial);
  submission::TrackUse(command_list, target->motion);
  submission::TrackUse(command_list, target->depth);
  submission::TrackUse(command_list, target->marker);
}

// The Direct3D 11 capture: the bridge's evaluate hook (Ngx11Evaluate) calls
// it under runtime_mutex after the game's real evaluate.  Same contract and
// sets as CapturePresentGuides.  The clones live on the game's native device
// and keep the source's format and binds, so a depth-stencil capture still
// takes the bridge's depth conversion at the present.  The copies run on the
// game's immediate context, which also runs the present's bridge copies, so
// the context orders each capture before every present that reads it.  A
// deferred context would order nothing and captures nothing (Optional then
// runs the present on neutral guides, Required declines it).
inline void CapturePresentGuides11(
    ID3D11DeviceContext* context,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters) {
  if (present_guides.load(std::memory_order_relaxed) == kPresentGuidesNever
      && !screenshot::WantsFinalPresent()) return;
  if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) {
    static bool said = false;  // runtime_mutex
    if (!std::exchange(said, true)) {
      LogPresent(reshade::log::level::warning,
                 "a Direct3D 11 DLSS evaluate on a deferred context lends the"
                 " present no guides");
    }
    return;
  }
  const auto texture = [parameters](const char* key) -> ID3D11Texture2D* {
    ID3D11Resource* const resource = bridge::GetD3D11Resource(parameters, key);
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (resource != nullptr) resource->GetType(&dimension);
    return dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D
               ? static_cast<ID3D11Texture2D*>(resource)
               : nullptr;
  };
  ID3D11Texture2D* const motion = texture(NVSDK_NGX_Parameter_MotionVectors);
  ID3D11Texture2D* const depth = texture(NVSDK_NGX_Parameter_Depth);
  ID3D11Texture2D* const output = texture(NVSDK_NGX_Parameter_Output);
  if (motion == nullptr || depth == nullptr || output == nullptr) return;
  D3D11_TEXTURE2D_DESC motion_desc = {};
  D3D11_TEXTURE2D_DESC depth_desc = {};
  D3D11_TEXTURE2D_DESC output_desc = {};
  D3D11_TEXTURE2D_DESC color_desc = {};
  motion->GetDesc(&motion_desc);
  depth->GetDesc(&depth_desc);
  output->GetDesc(&output_desc);
  if (ID3D11Texture2D* const color = texture(NVSDK_NGX_Parameter_Color); color != nullptr) {
    color->GetDesc(&color_desc);
  }
  for (const D3D11_TEXTURE2D_DESC* desc : {&motion_desc, &depth_desc}) {
    if (desc->ArraySize != 1 || desc->SampleDesc.Count != 1) return;
  }
  // The block holds D3D11 pointers, so its contract is read through a view
  // that answers no resource at all.
  const bridge::ParameterView view(parameters, nullptr);
  FeatureState contract = DeriveFeatureState(&view);
  if (!CompletePresentContract(handle, color_desc.Width, color_desc.Height, &contract)) {
    return;
  }
  PresentGuideSet* const target = NextPresentCaptureSet();
  if (target == nullptr) return;
  ID3D11DeviceContext* native = context;
  renodx::utils::directx::NativeFromReShadeProxy(&native);
  ID3D11Device* device = nullptr;
  native->GetDevice(&device);
  if (device == nullptr) return;
  const void* const device_identity = renodx::addons::dlss5::native_identity::Get(device);
  target->serial = 0;
  const auto ensure_clone = [&](ID3D11Texture2D** clone, const D3D11_TEXTURE2D_DESC& source) {
    if (*clone != nullptr) {
      D3D11_TEXTURE2D_DESC have = {};
      (*clone)->GetDesc(&have);
      if (target->device_identity == device_identity
          && have.Width == source.Width && have.Height == source.Height
          && have.Format == source.Format && have.BindFlags == source.BindFlags) {
        return true;
      }
      ReleaseCom(*clone);
    }
    D3D11_TEXTURE2D_DESC desc = source;
    desc.MipLevels = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, clone)) && *clone != nullptr) {
      return true;
    }
    *clone = nullptr;
    LogPresent(reshade::log::level::error,
               "a Direct3D 11 DLSS guide capture surface could not be created (format "
                   + std::to_string(static_cast<uint32_t>(source.Format)) + ' '
                   + std::to_string(source.Width) + 'x' + std::to_string(source.Height)
                   + ')');
    return false;
  };
  const bool cloned = ensure_clone(&target->motion11, motion_desc)
                      && ensure_clone(&target->depth11, depth_desc);
  if (cloned) {
    target->device_identity = device_identity;
    target->source_handle = handle;
  }
  device->Release();
  if (!cloned) return;
  native->CopySubresourceRegion(target->motion11, 0, 0, 0, 0, motion, 0, nullptr);
  native->CopySubresourceRegion(target->depth11, 0, 0, 0, 0, depth, 0, nullptr);
  StampPresentCapture(target, contract, output_desc.Width, output_desc.Height, true);
}

// The Windows SDR white level of the display the swapchain presents to, in
// nits (DISPLAYCONFIG_SDR_WHITE_LEVEL: 1000 = 80 nits; 200 reads as the
// BT.2408 203, as the RenoDX swapchain utilities do), or 0.  Only while the
// display runs in HDR: with HDR off Windows reports 1000 (80 nits) whatever
// the player chose, which says nothing about where an HDR game put its
// white, so the fallback (NRDiffuseWhiteNits) decides instead.
inline float QueryPresentSdrWhite(reshade::api::swapchain* swapchain) {
  auto* const native = reinterpret_cast<IDXGISwapChain*>(swapchain->get_native());
  IDXGIOutput* output = nullptr;
  if (native == nullptr || FAILED(native->GetContainingOutput(&output))
      || output == nullptr) {
    return 0.f;
  }
  DXGI_OUTPUT_DESC1 output_desc{};
  IDXGIOutput6* output6 = nullptr;
  const bool described = SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6)))
                         && SUCCEEDED(output6->GetDesc1(&output_desc));
  ReleaseCom(output6);
  output->Release();
  if (!described || output_desc.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
    return 0.f;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  LONG result = ERROR_INSUFFICIENT_BUFFER;
  UINT32 path_count = 0;
  while (result == ERROR_INSUFFICIENT_BUFFER) {
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count)
        != ERROR_SUCCESS) {
      return 0.f;
    }
    paths.resize(path_count);
    modes.resize(mode_count);
    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                                &mode_count, modes.data(), nullptr);
  }
  if (result != ERROR_SUCCESS) return 0.f;
  for (UINT32 i = 0; i < path_count; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS
        || wcscmp(source.viewGdiDeviceName, output_desc.DeviceName) != 0) {
      continue;
    }
    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    white.header.size = sizeof(white);
    white.header.adapterId = paths[i].targetInfo.adapterId;
    white.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) return 0.f;
    const float nits = static_cast<float>(white.SDRWhiteLevel) / 1000.f * 80.f;
    return nits == 200.f ? 203.f : nits;
  }
  return 0.f;
}

// The parameter block ProcessInline reads for a stream (on Direct3D 11 the
// bridge reads it first, as it reads a game's D3D11 DLSS block): Color =
// Output = the frame, the guides and their contract, the motion share of a
// game frame this frame moves, and the Reset.  Caller holds runtime_mutex.
inline bridge::OwnedParameters& FillPresentBlock(
    PresentStream* stream, void* frame, void* motion, void* depth,
    const FeatureState& contract, uint32_t width, uint32_t height,
    float motion_share, int reset) {
  bridge::OwnedParameters& block = stream->parameters;
  block.Reset();
  block.Set(NVSDK_NGX_Parameter_Color, frame);
  block.Set(NVSDK_NGX_Parameter_Output, frame);
  block.Set(NVSDK_NGX_Parameter_MotionVectors, motion);
  block.Set(NVSDK_NGX_Parameter_Depth, depth);
  block.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, contract.input_width);
  block.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, contract.input_height);
  block.Set(NVSDK_NGX_Parameter_Width, contract.input_width);
  block.Set(NVSDK_NGX_Parameter_Height, contract.input_height);
  block.Set(NVSDK_NGX_Parameter_OutWidth, width);
  block.Set(NVSDK_NGX_Parameter_OutHeight, height);
  block.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, contract.motion_x);
  block.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, contract.motion_y);
  block.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, contract.depth_x);
  block.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, contract.depth_y);
  // A game's absent scale stays absent unless the motion share rides on it:
  // NR's evaluate then reads it as DLSS does (ResolveMotionScale), counts
  // it, and the guide contract line says absent->1.  Contracts the add-on
  // builds itself (neutral guides, pre-FG) always carry theirs.
  if (contract.has_motion_scale_x || motion_share != 1.f) {
    block.Set(NVSDK_NGX_Parameter_MV_Scale_X, contract.motion_scale_x * motion_share);
  }
  if (contract.has_motion_scale_y || motion_share != 1.f) {
    block.Set(NVSDK_NGX_Parameter_MV_Scale_Y, contract.motion_scale_y * motion_share);
  }
  block.Set(NVSDK_NGX_Parameter_Reset, reset);
  // The guides' flags travel with them: MVLowRes and MVJittered describe the
  // captured vectors (ResolveMotionWindow takes the render window from
  // MVLowRes), DepthInverted the captured depth.  Through v8.0.1 only
  // DepthInverted crossed, so every MVLowRes game's render-size vectors
  // "contradicted" the display window (motion_window_contradiction blamed
  // the game on every present: KCD2 0x4b, Alan Wake 2, Control) and vectors
  // in a texture larger than the render window were read at the display
  // window.  IsHDR and AutoExposure describe the game's DLSS input, not the
  // back buffer, whose encoding the swapchain's colour space decides.
  block.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
            static_cast<int>(contract.create_flags
                             & (NVSDK_NGX_DLSS_Feature_Flags_DepthInverted
                                | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                                | NVSDK_NGX_DLSS_Feature_Flags_MVJittered)));
  // Registered up front, so ProcessInline never takes the sentinel for a
  // game feature created before the hooks (FindOrRegisterFeature's warning).
  // Read through a view with no resources: on Direct3D 11 the block holds
  // D3D11 pointers, which DeriveFeatureState would call as D3D12 ones.
  if (features.find(stream->Handle()) == features.end()) {
    const bridge::ParameterView view(&block, nullptr);
    FeatureState state = DeriveFeatureState(&view);
    state.source_feature = kFeatureDlss;
    features.emplace(stream->Handle(), state);
  }
  return block;
}

// ---- Before frame generation (v8.0.4, NRPresentPreFg) ---------------------
//
// Why.  With DLSS-G the Present path ran NR on every present, generated ones
// included: FH6 at 4x on v8.0.3 paid 1.9-6.9 ms of GPU per present, four
// times per game frame, 26-45 % of the GPU frame ("the performance hit is
// brutal with Present").  DLSS-G builds its frames from the game's real
// frame, so NR on that frame before DLSS-G reads it reaches every frame DLSS-G
// shows - its generated frames interpolate NR's output - at one NR pass per
// game frame.
//
// Where.  Streamline hands the game's frame to DLSS-G as an NGX evaluate of
// feature 11 (FrameGeneration) on DLSS-G's own list, which the NGX evaluate
// detours already see ("skipping NR on NGX evaluate: feature 11" in every
// DLSS-G field log).  Its parameter block names the inputs (strings of
// nvngx_dlssg.dll, HFW and RDR2 builds): DLSSG.Backbuffer (the game's final
// frame, which Streamline intercepted at the game's Present to its proxy
// swapchain, HUD included), DLSSG.HUDLess, DLSSG.UI, DLSSG.MVecs, DLSSG.Depth,
// DLSSG.MvecScaleX/Y (Streamline's normalized scale), DLSSG.DepthInverted,
// DLSSG.MvecJittered, DLSSG.Reset and DLSSG.MultiFrameIndex (multi-frame
// generation evaluates once per generated frame, index 1..n-1).  NR runs in
// place on DLSSG.Backbuffer, recorded into that evaluate's list BEFORE the
// real evaluate - the list DLSS-G reads it in, so the order is the list's -
// with the evaluate's own depth and motion: the same frame's guides, no
// capture, no in-flight skew.  The Streamline proxy swapchain's Present was
// the other candidate; nothing in the process hands the addon that proxy
// (ReShade wraps the native swapchain under it), and at its Present the
// frame's DLSS-G inputs are not yet known.
//
// Hudless.  DLSS-G reads DLSSG.HUDLess (the scene before the HUD) to keep the
// HUD steady, and with UI recomposition it interpolates the scene from it and
// composes the UI over.  A raw HUDLess beside an NR back buffer would mix raw
// and NR scene content in the generated frames only - the on/off alternation
// this mode exists to end.  So a tagged HUDLess is copied into an addon
// stand-in, NR runs on the stand-in (same guides, no UI correction: it has no
// HUD), and the stand-in replaces DLSSG.HUDLess in the block for every
// evaluate of that game frame; RestorePresentPreFg puts the game's pointer
// back after each real evaluate, as pre-SR does with Color, and the game's
// HUDLess is never written.  Copying the NR back buffer over HUDLess is
// wrong: it puts the HUD into HUDLess, which DLSS-G then interpolates as
// scene.
//
// One pass (v8.1.0, ComposePresentPreFg).  The real frame is not run through
// NR a second time: it is recomposed from the NR stand-in with the game's HUD
// kept, by DLSS-G's own UI blend where DLSSG.UIAlpha or DLSSG.UI is tagged
// (Final = Back + (1 - a) * (NR(HUDLess) - HUDLess), derived in
// shaders/prefg_compose.cs_5_1.hlsl) and by a Backbuffer / HUDLess difference
// mask where only HUDLess is: one NR pass per game frame.  The mask counts
// its HUD pixels on the GPU; a frame read above kPresentPreFgHudShare (grain
// or a tone map after the UI make every pixel differ) and every frame before
// the first measurement run the two passes above, counted in
// present[prefg_two_pass=] and logged once per reason, as does
// NRPresentPreFg=2.  Two-pass frames are served frames: prefg_fallbacks
// stays "the presents ran NR".
//
// The real frame.  Whether DLSS-G shows it from DLSSG.Backbuffer itself or
// copies it into DLSSG.OutputReal (the probe line says which a title sets),
// NR on Backbuffer in place is in it.
//
// Presents. While a pre-FG pass ran within kPresentPreFgHoldNs or two served
// game frames' presents (the longer), ReShade's
// present event runs no NR (present_prefg: every present, real and
// generated, shows frames DLSS-G built from NR's output).  Fallback: a
// DLSS-G evaluate the mode cannot serve (NRPresentPreFg=0; no present served
// yet, so no encoding; DLSSG.NotRenderingGameFrames; no depth or motion; a
// frame not the swapchain's shape; a list NR cannot record in; the restore
// target incomplete; NR declined) is counted in present[prefg_fallbacks=],
// its reason logged once, and it ends the hold: the next present runs NR as
// through v8.0.3 (the constants and counters are at the top of this file).

// After the real evaluate, outside runtime_mutex: the game's DLSSG.HUDLess
// back in its block (RunPresentPreFg's stand-in was there for real()).
inline void RestorePresentPreFg(const NVSDK_NGX_Parameter* parameters, ID3D12Resource* hudless) {
  if (hudless == nullptr) return;
  NVSDK_NGX_Parameter_SetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(parameters),
                                     NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless);
}

// Why a HUD-less frame runs two NR passes instead of the composite: the bits
// RunPresentPreFg logs once each (the last two silently: they are every
// HUD-less stream's first few game frames).  kPreFgStandInWarming: NR's pass
// on the stand-in was its stream's warm-up frame (the image passes through,
// slot_warmup), so a composite from it would show the real frame without NR.
inline constexpr uint32_t kPreFgTwoPassForced = 1;
inline constexpr uint32_t kPreFgTwoPassUnavailable = 2;
inline constexpr uint32_t kPreFgTwoPassHudShare = 4;
inline constexpr uint32_t kPreFgUnmeasured = 8;
inline constexpr uint32_t kPreFgStandInWarming = 16;
inline constexpr uint32_t kPreFgComposeBusy = 32;

// Under runtime_mutex, after NR's pass on the HUD-less stand-in (at rest in
// the selected input state, NR(HUDLess)): the one-pass HUD composite
// (shaders/prefg_compose.cs_5_1.hlsl).  Records the composite and its HUD
// pixel count, reads the newest count the GPU finished (no wait), and when
// the composite is trusted copies it over DLSSG.Backbuffer.  Returns 0 then,
// else the two-pass reason; the back buffer is untouched in that case.
inline uint32_t ComposePresentPreFg(ID3D12GraphicsCommandList* command_list,
                                    const NVSDK_NGX_Parameter* parameters,
                                    ID3D12Resource* backbuffer, ID3D12Resource* hudless,
                                    const D3D12_RESOURCE_DESC& back_desc,
                                    D3D12_RESOURCE_STATES input_state) {
  const uint32_t pre_fg_mode = present_pre_fg.load(std::memory_order_relaxed);
  if (pre_fg_mode == kPresentPreFgTwoPass) return kPreFgTwoPassForced;
  const bool residual = pre_fg_mode != kPresentPreFgHudTwoPass;
  PreFgCompose& compose = present_prefg_compose;
  // The composite reads the back buffer and HUD-less frame as textures.
  if (compose.unavailable
      || ((back_desc.Flags | hudless->GetDesc().Flags)
          & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
             != 0) {
    return kPreFgTwoPassUnavailable;
  }
  // The list's own world (ReShade proxies descriptor heaps): the heap and
  // views are made on the device the list reports.
  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    return kPreFgTwoPassUnavailable;
  }
  device->Release();  // the list keeps it alive
  if (!EnsureCodecRootSignature(device)
      || !CreateCodecPipelineState(device, __prefg_compose, &present_prefg_compose_pso,
                                   L"DLSS5 Generic pre-FG compose", "pre-FG compose")) {
    compose.unavailable = true;
    return kPreFgTwoPassUnavailable;
  }
  if (compose.device != device) {
    RetirePreFgCompose();
    compose.device = device;
  }
  const DXGI_FORMAT format = ConcreteResourceFormat(back_desc.Format);
  if (compose.output != nullptr) {
    const D3D12_RESOURCE_DESC have = compose.output->GetDesc();
    if (have.Width != back_desc.Width || have.Height != back_desc.Height || have.Format != format) {
      RetirePresentResource(&compose.output);
    }
  }
  // The upload and readback buffers (the stats buffer is a UAV scratch).
  const auto host_buffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                               ID3D12Resource** resource, const wchar_t* name) {
    if (*resource != nullptr) return true;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(uint32_t) * kPresentPreFgStatsWords * kPresentPreFgComposeSets;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(resource)))) {
      *resource = nullptr;
      return false;
    }
    (*resource)->SetName(name);
    return true;
  };
  if (compose.heap == nullptr) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = kPresentPreFgComposeSets * kPresentPreFgComposeDescriptors;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&compose.heap)))) {
      compose.heap = nullptr;
    } else {
      compose.heap->SetName(L"DLSS5 Generic pre-FG compose descriptors");
      compose.descriptor_size =
          device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
  }
  if (compose.heap == nullptr || !SupportsCodecFormat(device, format)
      || (compose.output == nullptr
          && !CreateScratchTexture(device, static_cast<uint32_t>(back_desc.Width),
                                   back_desc.Height, format,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &compose.output,
                                   L"DLSS5 Generic pre-FG composite"))
      || (compose.stats == nullptr
          && !CreateScratchBuffer(device,
                                  sizeof(uint32_t) * kPresentPreFgStatsWords
                                      * kPresentPreFgComposeSets,
                                  &compose.stats, L"DLSS5 Generic pre-FG HUD counts"))
      || !host_buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &compose.zero,
                      L"DLSS5 Generic pre-FG zero")
      || !host_buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                      &compose.readback, L"DLSS5 Generic pre-FG HUD readback")) {
    return kPreFgTwoPassUnavailable;
  }
  if (compose.readback_data == nullptr) {
    void* data = nullptr;
    void* zero = nullptr;
    const D3D12_RANGE nothing{0, 0};
    if (FAILED(compose.readback->Map(0, nullptr, &data)) || data == nullptr
        || FAILED(compose.zero->Map(0, &nothing, &zero)) || zero == nullptr) {
      if (data != nullptr) compose.readback->Unmap(0, nullptr);
      return kPreFgTwoPassUnavailable;
    }
    std::memset(zero, 0, sizeof(uint32_t) * kPresentPreFgStatsWords);
    compose.zero->Unmap(0, nullptr);
    compose.readback_data = static_cast<const uint32_t*>(data);
  }

  // The blend: DLSSG.UIAlpha before DLSSG.UI (the guide: with both tagged
  // DLSS-G reads only UI alpha), each only at the frame's size and in a
  // format a shader can load; neither usable is the difference mask.  A
  // single-channel UIAlpha holds alpha in .r, any other in .a (UNPROVEN: no
  // DLSS-G title's UI buffers have been logged; the probe line names them).
  ID3D12Resource* ui = nullptr;
  DXGI_FORMAT ui_format = DXGI_FORMAT_UNKNOWN;
  uint32_t mode = 2;
  for (const bool alpha : {true, false}) {
    ID3D12Resource* const resource = PreFgResource(
        parameters, alpha ? NVSDK_NGX_DLSSG_Parameter_UIAlpha : NVSDK_NGX_DLSSG_Parameter_UI);
    if (resource == nullptr) continue;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{CodecViewFormat(ConcreteResourceFormat(desc.Format))};
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != back_desc.Width
        || desc.Height != back_desc.Height || desc.DepthOrArraySize != 1
        || desc.SampleDesc.Count != 1
        || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                              sizeof(support)))
        || (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) == 0) {
      continue;
    }
    ui = resource;
    ui_format = support.Format;
    const bool red = ui_format == DXGI_FORMAT_R8_UNORM || ui_format == DXGI_FORMAT_R16_UNORM
                     || ui_format == DXGI_FORMAT_R16_FLOAT || ui_format == DXGI_FORMAT_R32_FLOAT;
    mode = alpha && red ? 0u : 1u;
    break;
  }
  static uint32_t said_modes = 0;
  if ((std::exchange(said_modes, said_modes | (1u << mode)) & (1u << mode)) == 0) {
    LogPresent(reshade::log::level::info,
               std::string("frame generation: the real frame is recomposed from NR's HUD-less"
                           " frame with the game's HUD kept, by ")
                   + (mode == 2 ? "a back buffer / HUD-less difference mask (no usable DLSSG.UI"
                                  " or DLSSG.UIAlpha)"
                      : mode == 0 ? "DLSS-G's UI blend with DLSSG.UIAlpha (.r)"
                                  : "DLSS-G's UI blend with the UI alpha (.a)")
                   + "; one NR pass per game frame while the composite can be trusted"
                     " (present[prefg_two_pass=] counts the frames that ran two)");
  }

  // The newest measurement the GPU finished, if any: the serial is read on
  // both sides of the count so a torn read is not taken.
  uint32_t newest = compose.read_serial;
  uint32_t newest_slot = kPresentPreFgComposeSets;
  for (uint32_t slot = 0; slot < kPresentPreFgComposeSets; ++slot) {
    const uint32_t serial = compose.readback_data[slot * kPresentPreFgStatsWords + 1];
    if (serial > newest && serial <= compose.serial) {
      newest = serial;
      newest_slot = slot;
    }
  }
  if (newest_slot != kPresentPreFgComposeSets) {
    const uint32_t* const words = compose.readback_data + newest_slot * kPresentPreFgStatsWords;
    const uint32_t count = words[0];
    const uint32_t coarse = words[2];
    if (words[1] == newest) {
      compose.read_serial = newest;
      const float area = static_cast<float>(back_desc.Width * back_desc.Height);
      const float share = static_cast<float>(count) / area;
      present_prefg_hud_permille.store(static_cast<uint32_t>(std::min(share, 1.f) * 1000.f),
                                       std::memory_order_relaxed);
      if (compose.slot_mode[newest_slot] == 2) {
        // Under Auto, a frame above the share whose grain-sized count is
        // within it took the residual in the shader: one pass, as a HUD frame.
        const bool took_residual = residual && share > kPresentPreFgHudShare
                                   && static_cast<float>(coarse) / area <= kPresentPreFgHudShare;
        if (took_residual && present_prefg_residual.fetch_add(1, std::memory_order_relaxed) == 0) {
          LogPresent(reshade::log::level::info,
                     "frame generation: the back buffer differs from the HUD-less frame by"
                     " grain in more than 25 % of its pixels (grain after the UI): the real"
                     " frame takes NR's change to the HUD-less frame on top of the back"
                     " buffer, except the pixels that differ by more (the UI), one NR pass"
                     " (present[prefg_residual=]; NRPresentPreFg=3 runs two passes instead)");
        }
        if (share > kPresentPreFgHudShare && !took_residual) {
          compose.one_pass = false;
          compose.exceeded = true;
          compose.clean_frames = 0;
        } else if (!compose.one_pass
                   && ++compose.clean_frames >= (compose.exceeded ? kPresentPreFgCleanFrames : 1u)) {
          compose.one_pass = true;
        }
      }
    }
  }

  // A recorded command list can be submitted again until Reset. Rewriting
  // descriptors while such a list still names this set is illegal even when
  // the GPU finished its last submission.
  uint32_t set = kPresentPreFgComposeSets;
  for (uint32_t offset = 1; offset <= kPresentPreFgComposeSets; ++offset) {
    const uint32_t candidate = (compose.serial + offset) % kPresentPreFgComposeSets;
    if (submission::ResourceReleasable(&compose.slot_mode[candidate])) {
      set = candidate;
      break;
    }
  }
  // A retained recording can pin one set indefinitely. It must not keep
  // every later frame on the fallback while other sets are free.
  if (set == kPresentPreFgComposeSets) return kPreFgComposeBusy;
  const uint32_t serial = ++compose.serial;
  compose.slot_mode[set] = static_cast<uint8_t>(mode);
  submission::TrackUse(command_list, &compose.slot_mode[set]);
  for (const void* object :
       {static_cast<const void*>(compose.heap),
        static_cast<const void*>(compose.output),
        static_cast<const void*>(compose.stats),
        static_cast<const void*>(compose.zero),
        static_cast<const void*>(compose.readback)}) {
    submission::TrackUse(command_list, object);
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE cpu_start = compose.heap->GetCPUDescriptorHandleForHeapStart();
  const uint32_t base = set * kPresentPreFgComposeDescriptors;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = CodecViewFormat(format);
  const DXGI_FORMAT hudless_format = CodecViewFormat(ConcreteResourceFormat(hudless->GetDesc().Format));
  const std::pair<ID3D12Resource*, DXGI_FORMAT> views[4] = {
      {backbuffer, srv.Format},
      {hudless, hudless_format},
      {present_prefg_hudless, hudless_format},
      {ui != nullptr ? ui : backbuffer, ui != nullptr ? ui_format : srv.Format}};
  for (uint32_t i = 0; i < 4; ++i) {
    srv.Format = views[i].second;
    device->CreateShaderResourceView(views[i].first, &srv,
                                     DescriptorHeapSlot(cpu_start, base + i, compose.descriptor_size));
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav.Format = CodecViewFormat(format);
  device->CreateUnorderedAccessView(compose.output, nullptr, &uav,
                                    DescriptorHeapSlot(cpu_start, base + 4, compose.descriptor_size));
  uav = {};
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.Buffer.NumElements = kPresentPreFgStatsWords * kPresentPreFgComposeSets;
  uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(compose.stats, nullptr, &uav,
                                    DescriptorHeapSlot(cpu_start, base + 5, compose.descriptor_size));

  const uint64_t stats_offset = sizeof(uint32_t) * kPresentPreFgStatsWords * set;
  Transition(command_list, compose.stats, D3D12_RESOURCE_STATE_COMMON,
             D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyBufferRegion(compose.stats, stats_offset, compose.zero, 0,
                                 sizeof(uint32_t) * kPresentPreFgStatsWords);
  Transition(command_list, compose.stats, D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap* heaps[] = {compose.heap};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(codec_pipeline.root_signature);
  command_list->SetPipelineState(present_prefg_compose_pso);
  const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start = compose.heap->GetGPUDescriptorHandleForHeapStart();
  command_list->SetComputeRootDescriptorTable(
      0, DescriptorHeapSlot(gpu_start, base, compose.descriptor_size));
  command_list->SetComputeRootDescriptorTable(
      1, DescriptorHeapSlot(gpu_start, base + 4, compose.descriptor_size));
  // The shader's ComposeConstants (9 of the codec signature's 28 dwords).
  // Stage 0 counts, stage 1 composes against this frame's own count (see the
  // shader: the frame a mask stops being a HUD, before the addon reads it).
  struct {
    uint32_t width, height, mode, stats_offset, serial;
    float epsilon;
    uint32_t pass, hud_limit, residual;
  } constants{static_cast<uint32_t>(back_desc.Width), back_desc.Height, mode,
              static_cast<uint32_t>(stats_offset), serial, kPresentPreFgEpsilon, 0u,
              static_cast<uint32_t>(kPresentPreFgHudShare * static_cast<float>(back_desc.Width)
                                    * static_cast<float>(back_desc.Height)),
              residual ? 1u : 0u};
  static_assert(sizeof(constants) == 9 * sizeof(uint32_t));
  for (const uint32_t pass : {0u, 1u}) {
    constants.pass = pass;
    command_list->SetComputeRoot32BitConstants(2, 9, &constants, 0);
    command_list->Dispatch((static_cast<uint32_t>(back_desc.Width) + 15) / 16,
                           (back_desc.Height + 15) / 16, 1);
    if (pass == 0) UavBarrier(command_list, compose.stats);
  }
  Transition(command_list, compose.stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_list->CopyBufferRegion(compose.readback, stats_offset, compose.stats, stats_offset,
                                 sizeof(uint32_t) * kPresentPreFgStatsWords);
  Transition(command_list, compose.stats, D3D12_RESOURCE_STATE_COPY_SOURCE,
             D3D12_RESOURCE_STATE_COMMON);

  if (mode == 2 && !compose.one_pass) {
    return compose.exceeded ? kPreFgTwoPassHudShare : kPreFgUnmeasured;
  }
  Transition(command_list, compose.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(command_list, backbuffer, input_state, D3D12_RESOURCE_STATE_COPY_DEST, 0u);
  CopyMip0(command_list, backbuffer, compose.output);
  Transition(command_list, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, input_state, 0u);
  Transition(command_list, compose.output, D3D12_RESOURCE_STATE_COPY_SOURCE,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  return 0;
}

// The device behind a Streamline proxy, or null.  A Streamline proxy that
// ReShade wraps is ReShade's "native" device, and it answers IID_IUnknown with
// itself (sl.interposer d3d12Device.cpp, checkAndUpgradeInterface), so
// NativeIdentity stops there, while DLSS-G's resources report the device
// beneath it.  Only its private IID reaches the base (sl.api internal.h,
// StreamlineRetrieveBaseInterface).  Kept apart from NativeIdentity: NR
// records in the face its lists report, so faces must stay distinct there.
inline const void* StreamlineBaseIdentity(IUnknown* device) {
  static constexpr GUID kStreamlineRetrieveBaseInterface = {
      0xADEC44E2, 0x61F0, 0x45C3, {0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF}};
  IUnknown* base = nullptr;
  if (device == nullptr
      || FAILED(device->QueryInterface(kStreamlineRetrieveBaseInterface,
                                       reinterpret_cast<void**>(&base)))
      || base == nullptr) {
    return nullptr;
  }
  const void* const identity = renodx::addons::dlss5::native_identity::Get(base);
  base->Release();
  return identity;
}

// Record every D3D12 presenting stream, including ones not yet served, so
// auto-FG never selects a different device or guesses between equal views.
inline bool UpdateAutoFgFallback(
    reshade::api::command_queue* queue, reshade::api::swapchain* swapchain) {
  if (!enabled.load(std::memory_order_relaxed)
      || (auto_fg_fallback.load(std::memory_order_relaxed) == 0
          && hook_point.load(std::memory_order_relaxed) != kHookPresent)) {
    auto_fg_fallback_active.store(false, std::memory_order_relaxed);
    return false;
  }
  if (queue == nullptr || swapchain == nullptr || queue->get_device() == nullptr
      || queue->get_device()->get_api() != reshade::api::device_api::d3d12) {
    return false;
  }
  auto* const backbuffer = reinterpret_cast<ID3D12Resource*>(
      static_cast<uintptr_t>(swapchain->get_current_back_buffer().handle));
  if (backbuffer == nullptr) return false;
  const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
  // ReShade owns its presenting queue lock here. Never wait behind an NGX
  // create/evaluate that may need that same queue; retry the observation.
  RuntimeTryLock lock(runtime_mutex);
  if (!lock.owns_lock()) {
    return auto_fg_fallback_active.load(std::memory_order_relaxed)
        && auto_fg_fallback_swapchain.load(std::memory_order_relaxed) == swapchain;
  }
  PresentStream& stream = present_streams[swapchain];
  auto* const native_device = reinterpret_cast<IUnknown*>(queue->get_device()->get_native());
  if (const void* const identity = renodx::addons::dlss5::native_identity::Get(native_device);
      stream.device_identity != identity) {
    stream.device_identity = identity;
    stream.streamline_identity = StreamlineBaseIdentity(native_device);
  }
  stream.backbuffer_width = static_cast<uint32_t>(desc.Width);
  stream.backbuffer_height = desc.Height;
  stream.backbuffer_format = desc.Format;
  stream.d3d11 = false;
  const void* owner = nullptr;
  uint32_t matching = 0;
  for (const auto& [key, candidate] : present_streams) {
    if (!candidate.d3d11 && candidate.device_identity == auto_fg_observation.device
        && candidate.backbuffer_width == auto_fg_observation.width
        && candidate.backbuffer_height == auto_fg_observation.height
        && ConcreteResourceFormat(candidate.backbuffer_format) == auto_fg_observation.format) {
      owner = key;
      ++matching;
    }
  }
  const bool active = auto_fg_fallback.load(std::memory_order_relaxed) != 0
      && hook_point.load(std::memory_order_relaxed) != kHookPresent
      && enabled.load(std::memory_order_relaxed)
      && matching == 1 && auto_fg_observation.last_ns > 0
      && SteadyNowNs() - auto_fg_observation.last_ns < 1'000'000'000LL
      && auto_fg_observation.evaluates >= 60;
  auto_fg_observation.swapchain = active ? owner : nullptr;
  auto_fg_fallback_swapchain.store(auto_fg_observation.swapchain, std::memory_order_relaxed);
  if (auto_fg_fallback_active.exchange(active, std::memory_order_relaxed) != active) {
    Log(reshade::log::level::info,
        active ? "DLSS/RR evaluations stopped while frame generation continues;"
                 " NR is using its matching Present path until DLSS/RR resumes"
               : "DLSS/RR or frame-generation stream changed; NR returned to the"
                 " selected inline hook point");
  }
  return active && owner == swapchain;
}

// A different device's DLSS must not disable a proven FG-only owner.
// On the same device, output size alone cannot distinguish a separate view
// from this frame's atlas/subrect or final scaling. Keep the existing inline
// path for that ambiguity. Called after a successful registered DLSS evaluate,
// under runtime_mutex.
inline void ObservePresentDlssEvaluate(const NVSDK_NGX_Parameter* parameters) {
  ID3D12Resource* const output = GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Output);
  if (output == nullptr || auto_fg_observation.device == nullptr) return;
  ID3D12Device* device = nullptr;
  if (FAILED(output->GetDevice(IID_PPV_ARGS(&device)))) return;
  if (renodx::addons::dlss5::native_identity::Get(device) == auto_fg_observation.device) {
    auto_fg_observation.evaluates = 0;
    auto_fg_observation.swapchain = nullptr;
    // Every return is logged, like every switch: rc1 logs showed back-to-back
    // "stopped" lines with no return between them.
    if (auto_fg_fallback_active.exchange(false, std::memory_order_relaxed)) {
      Log(reshade::log::level::info,
          "DLSS/RR evaluations resumed on the frame-generation device; NR"
          " returned to the selected inline hook point");
    }
  }
  ReleaseCom(device);
}

// Read-only observation before injection admission: an unsupported command
// list or an explicit pre-FG opt-out still declares its rate. Both outermost
// NGX evaluate wrappers call this under runtime_mutex.
inline void ObservePresentFrameGeneration(const NVSDK_NGX_Parameter* parameters) {
  auto_present_frame_generation_seen = true;
  ObserveAutoPresentEvaluate();
  ID3D12Resource* const backbuffer =
      PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_Backbuffer);
  if (backbuffer == nullptr) return;
  ID3D12Device* device = nullptr;
  if (FAILED(backbuffer->GetDevice(IID_PPV_ARGS(&device)))) return;
  const void* const identity = renodx::addons::dlss5::native_identity::Get(device);
  ReleaseCom(device);
  const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
  const DXGI_FORMAT format = ConcreteResourceFormat(desc.Format);
  if (auto_fg_observation.device != identity || auto_fg_observation.width != desc.Width
      || auto_fg_observation.height != desc.Height || auto_fg_observation.format != format) {
    auto_fg_observation = {identity, static_cast<uint32_t>(desc.Width), desc.Height, format};
    auto_fg_fallback_active.store(false, std::memory_order_relaxed);
  }
  auto_fg_observation.last_ns = SteadyNowNs();
  // Only evaluates NR could serve count toward the 60: the DLSS/RR evaluate
  // that resets the count (ObservePresentDlssEvaluate) is observed only while
  // NR is on, so counting through an NR-off stretch fired a false fallback at
  // the first present after every re-enable (KCD2, GoW, MSFS 2024 and
  // Resonance v8.5.0-rc1 logs).
  if (enabled.load(std::memory_order_relaxed)) {
    ++auto_fg_observation.evaluates;
  } else {
    auto_fg_observation.evaluates = 0;
  }
  // NVIDIA's NGX contract counts GENERATED frames: 3 means 4x, 1 means
  // 2x. An absent optional key is not a declaration. Paused rendering says
  // nothing about a new rate: leave the last observation to expire.
  unsigned int generated = 0;
  if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,
                                       &generated))
      && generated >= 1 && generated < 8
      && GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames) == 0) {
    present_fg_declaration = {
        identity, static_cast<uint32_t>(desc.Width), desc.Height, generated + 1,
        auto_fg_observation.last_ns, present_frames_seen.load(std::memory_order_relaxed)};
  }
}

// Called by the NGX evaluate hooks under runtime_mutex, in the outermost
// wrapper, BEFORE the real evaluate, for every DLSS-G evaluate.  Returns the
// game's DLSSG.HUDLess when the stand-in replaced it (RestorePresentPreFg).
inline ID3D12Resource* RunPresentPreFg(ID3D12GraphicsCommandList* command_list,
                                       const NVSDK_NGX_Parameter* parameters) {
  if ((!PresentHookServes(false) && !auto_fg_fallback_active.load(std::memory_order_relaxed))
      || command_list == nullptr) return nullptr;
  if (screenshot::WantsFinalPresent()) {
    // F5 compares one final image; do not enhance the FG input or HUD-less
    // image first, then mislabel that already-enhanced source as NR off.
    present_prefg_served_ns = 0;
    present_prefg_hudless_source = nullptr;
    return nullptr;
  }
  ID3D12Resource* const backbuffer =
      PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_Backbuffer);
  if (backbuffer == nullptr) return nullptr;
  present_prefg_evaluates.fetch_add(1, std::memory_order_relaxed);
  ID3D12Resource* const hudless = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess);
  // The block already carries this path's stand-in: an enclosing evaluate
  // put it there for its real() (the chain's prefg_ran covers that on one
  // thread).  NR on it again would copy the stand-in onto itself.
  if (hudless != nullptr && hudless == present_prefg_hudless) return nullptr;
  const uint32_t index = GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1);
  // Observe-only, once per session: what this title's DLSS-G evaluate
  // carries (the list, the frame index, which inputs are tagged at what size
  // and format, and whether it names an OutputReal).
  static bool probed = false;
  if (!std::exchange(probed, true)) {
    std::string line = "frame generation evaluate (observe-only, first one): list type "
                       + std::to_string(static_cast<int>(command_list->GetType()))
                       + " MultiFrameIndex " + std::to_string(index) + " MultiFrameCount "
                       + std::to_string(GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount))
                       + " NotRenderingGameFrames "
                       + std::to_string(GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames));
    for (const char* key :
         {NVSDK_NGX_DLSSG_Parameter_Backbuffer, NVSDK_NGX_DLSSG_Parameter_HUDLess,
          NVSDK_NGX_DLSSG_Parameter_UI, NVSDK_NGX_DLSSG_Parameter_UIAlpha,
          NVSDK_NGX_DLSSG_Parameter_Depth, NVSDK_NGX_DLSSG_Parameter_MVecs,
          NVSDK_NGX_DLSSG_Parameter_OutputReal, NVSDK_NGX_DLSSG_Parameter_OutputInterpolated}) {
      line += std::string(" ") + (key + std::strlen("DLSSG.")) + '=';
      ID3D12Resource* const resource = PreFgResource(parameters, key);
      if (resource == nullptr) {
        line += '-';
        continue;
      }
      const D3D12_RESOURCE_DESC desc = resource->GetDesc();
      line += std::to_string(desc.Width) + 'x' + std::to_string(desc.Height) + '/'
              + std::to_string(static_cast<uint32_t>(desc.Format));
    }
    LogPresent(reshade::log::level::info, line);
  }
  // The frame's later evaluates: the stand-in NR made at index 1.
  if (index > 1) {
    if (hudless == nullptr || hudless != present_prefg_hudless_source
        || present_prefg_hudless == nullptr) {
      return nullptr;
    }
    // Each generated-frame evaluate may record on a different list. Index
    // 1's use cannot keep the stand-in alive for these later recordings.
    submission::TrackUse(command_list, present_prefg_hudless);
    NVSDK_NGX_Parameter_SetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(parameters),
                                       NVSDK_NGX_DLSSG_Parameter_HUDLess, present_prefg_hudless);
    return hudless;
  }
  present_prefg_hudless_source = nullptr;
  const D3D12_RESOURCE_DESC back_desc = backbuffer->GetDesc();
  ID3D12Device* frame_device = nullptr;
  const void* frame_identity = nullptr;
  if (SUCCEEDED(backbuffer->GetDevice(IID_PPV_ARGS(&frame_device)))) {
    frame_identity = renodx::addons::dlss5::native_identity::Get(frame_device);
    ReleaseCom(frame_device);
  }
  const void* owner = nullptr;
  const PresentStream* owner_stream = nullptr;
  const void* shaped_owner = nullptr;
  const PresentStream* shaped_stream = nullptr;
  uint32_t d3d12_streams = 0;
  uint32_t matching = 0;
  uint32_t shaped = 0;
  for (const auto& [key, stream] : present_streams) {
    if (stream.d3d11 || stream.device_identity == nullptr) continue;
    ++d3d12_streams;
    if (stream.backbuffer_width != back_desc.Width || stream.backbuffer_height != back_desc.Height
        || ConcreteResourceFormat(stream.backbuffer_format) != ConcreteResourceFormat(back_desc.Format)) {
      continue;
    }
    shaped_owner = key;
    shaped_stream = &stream;
    ++shaped;
    if (frame_identity != nullptr
        && (stream.device_identity == frame_identity || stream.streamline_identity == frame_identity)) {
      owner = key;
      owner_stream = &stream;
      ++matching;
    }
  }
  // A proxy under ReShade that answers IID_IUnknown with itself gives the
  // swapchain a device face DLSS-G's frame never reports (Streamline's base
  // is compared above; another wrapper's is not reachable).  One swapchain of
  // the frame's size and format is still unambiguous: the stream lends only
  // its encoding and size, and NR records in the evaluate's own list.
  // Wuthering Waves v8.5.0-rc1 never matched: prefg=0 in 2315 evaluates.
  if (matching == 0 && shaped == 1) {
    owner = shaped_owner;
    owner_stream = shaped_stream;
    matching = 1;
  }
  const auto fall_back = [&](uint32_t bit, const std::string& why) -> ID3D12Resource* {
    present_prefg_fallbacks.fetch_add(1, std::memory_order_relaxed);
    if (matching == 1 && owner == present_prefg_served_swapchain) {
      present_prefg_served_ns = 0;
    }
    static uint32_t said = 0;
    if ((std::exchange(said, said | bit) & bit) == 0) {
      LogPresent(reshade::log::level::warning,
                 "a frame generation evaluate cannot take NR before it interpolates (" + why
                     + "); NR runs on its presents instead (present[prefg_fallbacks=])");
    }
    if (bit == 1) present_prefg_upscaled_active.store(false, std::memory_order_relaxed);
    if ((bit & kPresentPreFgMisses) != 0) {
      present_prefg_clear_streak = 0;
      if (++present_prefg_miss_streak >= kPresentPreFgMissFrames
          && !present_prefg_upscaled_active.exchange(true, std::memory_order_relaxed)
          && present_prefg_episodes_logged++ < 16) {
        Log(reshade::log::level::warning,
            "Present hook point: frame generation: NR cannot run before DLSS-G interpolates ("
                + why + ") for " + std::to_string(kPresentPreFgMissFrames)
                + " game frames in a row, and the presents cannot reach the frames DLSS-G"
                  " makes; the DLSS evaluates run the Upscaled hook point and the presents"
                  " run none until it can (present[prefg_upscaled=])");
      }
    }
    return nullptr;
  };
  if (present_pre_fg.load(std::memory_order_relaxed) == kPresentPreFgOff) {
    return fall_back(1, "NRPresentPreFg=0");
  }
  // The evaluates run the Upscaled hook point while the swapchain cannot be
  // served (PresentTakesEvaluate); NR here as well would run twice on the
  // frame.  Code Vein II v8.5.0-rc1 did, alternating two encodings every game
  // frame for 12 s: the flicker.
  if (present_unusable.load(std::memory_order_relaxed)) {
    return fall_back(2048, "the swapchain cannot be served; the DLSS evaluates run the Upscaled"
                           " hook point");
  }
  if (matching != 1 || owner_stream == nullptr || !owner_stream->prefg_target_valid) {
    return fall_back(2, shaped == 0 ? "no presenting swapchain has the frame's size and format"
                        : matching == 0 ? std::to_string(shaped)
                                              + " swapchains of the frame's size and format"
                                                " present, none on its device"
                        : matching > 1 ? std::to_string(matching)
                                             + " swapchains on the frame's device match it"
                                       : "no present was served yet, so the frame's encoding"
                                         " is unknown");
  }
  // The stand-in and composite are one stream, and exactly one presenting
  // stream matched this frame above. Another D3D12 swapchain (a second
  // window, a launcher) never reaches the stand-in; frame generation
  // interleaved on two of them is unproven, so it is counted, not declined.
  if (d3d12_streams != 1) {
    static bool said = false;
    if (!std::exchange(said, true)) {
      LogPresent(reshade::log::level::info,
                 "frame generation: " + std::to_string(d3d12_streams)
                     + " D3D12 swapchains present; NR serves the one matching this frame"
                       " before it interpolates (observe-only, present[prefg_multi_stream=])");
    }
  }
  if (!PresentHookServes(false) && auto_fg_observation.swapchain != owner) return nullptr;
  present_prefg_target = owner_stream->prefg_target;
  present_prefg_width = owner_stream->backbuffer_width;
  present_prefg_height = owner_stream->backbuffer_height;
  present_prefg_target_multiplier = owner_stream->fg_multiplier;
  if (GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames) != 0) {
    return fall_back(4, "DLSSG.NotRenderingGameFrames: the game says it renders no game frame");
  }
  ID3D12Resource* const motion = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_MVecs);
  ID3D12Resource* const depth = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_Depth);
  if (motion == nullptr || depth == nullptr) {
    return fall_back(8, "its parameter block names no motion vectors or depth");
  }
  const auto served_shape = [&](const D3D12_RESOURCE_DESC& desc) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1
           && desc.SampleDesc.Count == 1 && desc.Width == present_prefg_width
           && desc.Height == present_prefg_height;
  };
  const D3D12_RESOURCE_DESC hudless_desc =
      hudless != nullptr && hudless != backbuffer ? hudless->GetDesc() : back_desc;
  if (!served_shape(back_desc) || !served_shape(hudless_desc)) {
    return fall_back(16, "its frame or HUD-less is not the swapchain's size and shape");
  }
  const bool legacy_contract = present_pre_fg_contract.load(std::memory_order_relaxed) == 1;
  // Streamline's DLSS-G guide 5.1 requires HUDLess and Backbuffer to share
  // color space, not storage format. Cyberpunk reports FP16 HUDLess beside
  // R10G10B10A2 PQ. Keep the served PresentTarget encoding for BOTH; inferring
  // linear/scRGB from FP16 would decode those PQ values incorrectly.
  if (legacy_contract
      && ConcreteResourceFormat(hudless_desc.Format) != ConcreteResourceFormat(back_desc.Format)) {
    return fall_back(1024, "NRPresentPreFgContract=1 requires matching frame and HUD-less formats");
  }
  if (!IsSupportedInjectionCommandList(command_list)) {
    return fall_back(32, "NR cannot record in its list (type "
                             + std::to_string(static_cast<int>(command_list->GetType())) + ")");
  }
  const bool compute_list = command_list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE;
  if (legacy_contract && compute_list) {
    return fall_back(32, "NRPresentPreFgContract=1 requires a direct list");
  }
  const D3D12_RESOURCE_STATES input_state = compute_list
      ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : kPresentPreFgInputState;
  ComputeStateEnvelope envelope(command_list);
  envelope.StopCapture();
  if (!envelope.CanInject()) {
    return fall_back(64, "the list's compute-state restore target is incomplete; retried"
                         " every game frame");
  }
  // The frame's shape can be served.  While the evaluates run the Upscaled
  // hook point (kPresentPreFgMissFrames) this game frame's DLSS output already
  // took NR: serve none, and hand back only after as many servable frames.
  present_prefg_miss_streak = 0;
  if (present_prefg_upscaled_active.load(std::memory_order_relaxed)) {
    if (++present_prefg_clear_streak >= kPresentPreFgMissFrames) {
      present_prefg_upscaled_active.store(false, std::memory_order_relaxed);
      if (present_prefg_episodes_logged++ < 16) {
        Log(reshade::log::level::info,
            "Present hook point: frame generation: NR can run before DLSS-G interpolates"
            " again; the DLSS evaluates return to the Present hook point");
      }
    }
    return nullptr;
  }
  // The HUD-less stand-in, the HUDLess's twin at rest in the input state.
  ID3D12Resource* const standing = hudless != nullptr && hudless != backbuffer ? hudless : nullptr;
  if (standing != nullptr) {
    if (present_prefg_hudless != nullptr) {
      const D3D12_RESOURCE_DESC have = present_prefg_hudless->GetDesc();
      if (present_prefg_hudless_device != frame_identity
          || have.Width != hudless_desc.Width || have.Height != hudless_desc.Height
          || have.Format != hudless_desc.Format || present_prefg_hudless_state != input_state) {
        RetirePresentResource(&present_prefg_hudless);
      }
    }
    if (present_prefg_hudless == nullptr) {
      ID3D12Device* device = nullptr;
      D3D12_RESOURCE_DESC desc = hudless_desc;
      desc.MipLevels = 1;
      desc.Alignment = 0;
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      desc.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                      | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device)))
          || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    input_state, nullptr,
                                                    IID_PPV_ARGS(&present_prefg_hudless)))) {
        present_prefg_hudless = nullptr;
      }
      ReleaseCom(device);
      if (present_prefg_hudless == nullptr) {
        return fall_back(128, "its HUD-less stand-in could not be created (format "
                                  + std::to_string(static_cast<uint32_t>(hudless_desc.Format))
                                  + ")");
      }
      present_prefg_hudless->SetName(L"DLSS5 Generic pre-FG HUD-less");
      present_prefg_hudless_device = frame_identity;
      present_prefg_hudless_state = input_state;
    }
  }
  gate_ever_opened.store(true, std::memory_order_relaxed);
  // DLSS-G integrations use both normalized and pixel motion-scale units.
  // MSFS 2024 supplied (3838,1675) for a 3838x1675 window; multiplying
  // again sent roughly (14.7 million,2.8 million) to NR. Only an obvious
  // pixel-sized pair selects that mode automatically. Ambiguous values keep
  // the previous normalized rule; NRPresentMVecUnits can force either one.
  const D3D12_RESOURCE_DESC motion_desc = motion->GetDesc();
  FeatureState contract;
  contract.input_width =
      GetUInt(parameters, "DLSSG.MVecsSubrectWidth", static_cast<uint32_t>(motion_desc.Width));
  contract.input_height = GetUInt(parameters, "DLSSG.MVecsSubrectHeight", motion_desc.Height);
  contract.motion_x = GetUInt(parameters, "DLSSG.MVecsSubrectBaseX");
  contract.motion_y = GetUInt(parameters, "DLSSG.MVecsSubrectBaseY");
  contract.depth_x = GetUInt(parameters, "DLSSG.DepthSubrectBaseX");
  contract.depth_y = GetUInt(parameters, "DLSSG.DepthSubrectBaseY");
  const float declared_x = GetFloat(parameters, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.f);
  const float declared_y = GetFloat(parameters, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.f);
  const uint32_t units = present_mvec_units.load(std::memory_order_relaxed);
  const bool pixels = units == 2 || (units == 0
      && std::isfinite(declared_x) && std::isfinite(declared_y)
      && std::fabs(declared_x) >= 4.f && std::fabs(declared_y) >= 4.f
      && std::fabs(declared_x) <= 4.f * static_cast<float>(contract.input_width)
      && std::fabs(declared_y) <= 4.f * static_cast<float>(contract.input_height));
  contract.has_motion_scale_x = true;
  contract.has_motion_scale_y = true;
  contract.motion_scale_x = declared_x * (pixels ? 1.f : static_cast<float>(contract.input_width));
  contract.motion_scale_y = declared_y * (pixels ? 1.f : static_cast<float>(contract.input_height));
  if (pixels && units == 0) {
    static std::atomic_bool logged_pixel_units{false};
    if (!logged_pixel_units.exchange(true)) {
      LogPresent(reshade::log::level::info,
                 "DLSS-G motion scale is already in pixels; keeping the declared scale"
                 " (NRPresentMVecUnits=0 auto)");
    }
  }
  contract.create_flags =
      (GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_DepthInverted) != 0
           ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted
           : 0)
      | (GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MvecJittered) != 0
             ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered
             : 0)
      | (contract.input_width < back_desc.Width ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0);
  const int reset = static_cast<int>(GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_Reset));
  if (standing != nullptr) {
    submission::TrackUse(command_list, present_prefg_hudless);
    Transition(command_list, standing, input_state, D3D12_RESOURCE_STATE_COPY_SOURCE,
               0u);
    Transition(command_list, present_prefg_hudless, input_state,
               D3D12_RESOURCE_STATE_COPY_DEST);
    CopyMip0(command_list, present_prefg_hudless, standing);
    Transition(command_list, standing, D3D12_RESOURCE_STATE_COPY_SOURCE, input_state,
               0u);
    Transition(command_list, present_prefg_hudless, D3D12_RESOURCE_STATE_COPY_DEST,
               input_state);
  }
  // The HUD-less stand-in first, then the back buffer - recomposed from it
  // (ComposePresentPreFg), else its own NR pass - stopping at the first pass
  // NR declines: both or neither, since an NR back buffer beside a raw
  // HUD-less is the mix this mode exists to end.  A declined stand-in never
  // enters the block, and the back buffer is untouched then.
  bool touched = false;
  uint32_t passes = 0;
  uint32_t two_pass = 0;
  // The stand-in's warm-up frame (NR passed the image through): see
  // kPreFgStandInWarming.
  auto& warmups =
      nr_decline_counts[static_cast<size_t>(NrDeclineReason::kSlotWarmupMaturation)];
  uint64_t warmups_before = 0;
  for (const size_t i : {size_t{1}, size_t{0}}) {
    if (i == 1 && standing == nullptr) continue;
    if (i == 1) warmups_before = warmups.load(std::memory_order_relaxed);
    if (i == 0 && standing != nullptr) {
      if (warmups.load(std::memory_order_relaxed) != warmups_before) {
        two_pass = kPreFgStandInWarming;
      } else {
        two_pass = ComposePresentPreFg(command_list, parameters, backbuffer, hudless, back_desc,
                                      input_state);
        touched |= two_pass != kPreFgTwoPassForced
            && two_pass != kPreFgTwoPassUnavailable
            && two_pass != kPreFgComposeBusy;
        if (two_pass == 0) break;
      }
    }
    ID3D12Resource* const frame = i == 0 ? backbuffer : present_prefg_hudless;
    PresentStream& stream = present_streams[&present_prefg_keys[i]];
    FillPresentBlock(&stream, frame, motion, depth, contract,
                     static_cast<uint32_t>(back_desc.Width), back_desc.Height, 1.f, reset);
    PresentTarget target = present_prefg_target;
    target.capture_display = false;
    target.output_state = input_state;
    target.workset_key = reinterpret_cast<const ID3D12Resource*>(&stream);
    // HUDLess has no HUD to correct.
    target.ui_correction = i == 0 && target.ui_correction;
    bool pass_touched = false;
    const int64_t started_ns = SteadyNowNs();
    // In flight until its terminal, so a verdict emitted meanwhile does not
    // read it as unaccounted.
    const EvaluateInFlightScope attempt_in_flight(true);
    present_prefg_attempts.fetch_add(1, std::memory_order_relaxed);
    const bool ran = ProcessInline(command_list, stream.Handle(), &stream.parameters,
                                   &pass_touched, &target);
    RecordInjectionCpu(started_ns);
    touched |= pass_touched;
    if (!ran) break;
    ++passes;
  }
  if (touched) envelope.MarkInjectionCommandsRecorded();
  if (passes != (standing != nullptr && two_pass != 0 ? 2u : 1u)) {
    return fall_back(256, "NR declined the frame (see the declines)");
  }
  present_prefg_passes.fetch_add(passes, std::memory_order_relaxed);
  if (two_pass != 0) {
    present_prefg_two_pass.fetch_add(1, std::memory_order_relaxed);
    static uint32_t said = kPreFgUnmeasured | kPreFgStandInWarming;
    if ((std::exchange(said, said | two_pass) & two_pass) == 0) {
      LogPresent(two_pass == kPreFgTwoPassForced ? reshade::log::level::info
                                                 : reshade::log::level::warning,
                 std::string("frame generation: NR runs twice this game frame, on the HUD-less"
                             " frame and on the back buffer, instead of recomposing the real"
                             " frame (")
                     + (two_pass == kPreFgTwoPassForced ? "NRPresentPreFg=2"
                         : two_pass == kPreFgTwoPassUnavailable
                             ? "the composite's pipeline, views or surfaces could not be made"
                         : two_pass == kPreFgComposeBusy
                             ? "its descriptor set is still referenced by a command list"
                             : "the back buffer differs from the HUD-less frame in more than 25 %"
                              " of its pixels: not a HUD (grain, a tone map after the UI);"
                              " re-checked every game frame")
                     + "; present[prefg_two_pass=])");
    }
  }
  if (present_prefg_frames.fetch_add(1, std::memory_order_relaxed) == 0) {
    LogPresent(reshade::log::level::info,
               std::string("frame generation: NR runs once per game frame on DLSS-G's"
                           " input, before it interpolates (")
                   + (standing != nullptr ? "a HUD-less stand-in, the real frame recomposed from it"
                                            " or, see present[prefg_two_pass=], run too"
                                          : "back buffer")
                   + "); the presents run none (NRPresentPreFg, present[prefg=])");
  }
  if (ConcreteResourceFormat(hudless_desc.Format) != ConcreteResourceFormat(back_desc.Format)) {
    present_prefg_mixed_format.fetch_add(1, std::memory_order_relaxed);
  }
  if (compute_list) present_prefg_compute.fetch_add(1, std::memory_order_relaxed);
  if (d3d12_streams != 1) present_prefg_multi_stream.fetch_add(1, std::memory_order_relaxed);
  if (owner_stream->device_identity != frame_identity
      && present_prefg_foreign_face.fetch_add(1, std::memory_order_relaxed) == 0) {
    std::ostringstream message;
    message << "frame generation: DLSS-G's frame is on device " << frame_identity
            << ", the swapchain's device face is " << owner_stream->device_identity
            << " (Streamline base " << owner_stream->streamline_identity
            << "); the one swapchain of the frame's size and format serves it"
               " (present[prefg_foreign_face=])";
    LogPresent(reshade::log::level::info, message.str());
  }
  present_prefg_served_ns = SteadyNowNs();
  present_prefg_served_swapchain = owner;
  present_prefg_served_presents = present_frames_seen.load(std::memory_order_relaxed);
  // The completed frame's declared count is independent of the user's
  // motion detection override. Missing counts keep the last target rate.
  unsigned int generated = 0;
  present_prefg_served_multiplier =
      NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, &generated))
              && generated >= 1 && generated < 8
          ? generated + 1 : present_prefg_target_multiplier;
  // The back-buffer stream's workset stays while this path serves, however
  // long the composite keeps it idle (RetireSupersededWorksets).
  workset_idle_exempt_handle = present_streams[&present_prefg_keys[0]].Handle();
  workset_idle_exempt_ns = present_prefg_served_ns;
  if (standing == nullptr) return nullptr;
  present_prefg_hudless_source = hudless;
  NVSDK_NGX_Parameter_SetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(parameters),
                                     NVSDK_NGX_DLSSG_Parameter_HUDLess, present_prefg_hudless);
  return hudless;
}

inline void RunPresentPath(
    reshade::api::command_queue* queue, reshade::api::swapchain* swapchain,
    bool auto_fallback = false) {
  // Resolve unsupported requests before the route's early return: Vulkan
  // and the D3D11 foreign/off routes never satisfy PresentHookServes().
  // A helper's small control-window swapchain is not its evaluated game
  // image. Model diagnostics keep the active evaluate route unchanged.
  if (screenshot::IsArmed() && swapchain != nullptr) {
    auto* const capture_device = swapchain->get_device();
    if (!enabled.load(std::memory_order_relaxed)) {
      screenshot::FailRequest("nr_disabled", "enable NR before requesting a comparison");
    } else if (NrYieldsToForeign()) {
      screenshot::FailRequest("foreign_nr", "another NR implementation owns this frame");
    } else if (capture_device != nullptr) {
      const auto capture_api = capture_device->get_api();
      // As for model buffers below: a mixed-API session's unrelated window
      // must not cancel a final-present request a D3D12 swapchain, or the
      // Direct3D 11 bridge's, can still satisfy. Unanswered requests expire
      // after kArmTimeoutPresents.
      const bool d3d12_stream = d3d12_swapchain_seen.load(std::memory_order_relaxed);
      if (screenshot::WantsFinalPresent()
          && capture_api != reshade::api::device_api::d3d11
          && capture_api != reshade::api::device_api::d3d12
          && !d3d12_stream && !bridge_device_live.load(std::memory_order_acquire)) {
        screenshot::FailRequest(
            "unsupported_present",
            "this API has no supported capture readback path yet");
      } else if (screenshot::WantsEvaluatedBuffer()
                 && capture_api == reshade::api::device_api::vulkan
                 && !d3d12_device_seen.load(std::memory_order_relaxed)
                 && !bridge_device_live.load(std::memory_order_acquire)
                 && !ngx_dlss_evaluate_seen.load(std::memory_order_relaxed)) {
        // Vulkan-only model readback is not implemented. A mixed-API
        // session may still supply a supported D3D12 model pair: an
        // unrelated Vulkan window must not cancel that request.
        screenshot::FailRequest(
            "unsupported_diagnostic", "Vulkan model-buffer readback is not implemented; "
            "use the game's screenshot function for the displayed image");
      } else if (screenshot::WantsFinalPresent()
                 && (screenshot::separate_presentation.load(std::memory_order_relaxed)
                     || (capture_api == reshade::api::device_api::d3d11 && !Dx11NativeRoute()
                         && !d3d12_stream))) {
        screenshot::FailRequest(
            "unsupported_present", "this presentation window does not carry the enhanced game image; "
            "select Evaluated model buffers (NRScreenshotSource=2) for a diagnostic pair");
      }
    }
  }
  bool sr_fallback = false;
  PresentFallback::Surface fallback_surface;
  uint64_t fallback_ticket = 0;
  {
    // ReShade owns the presenting queue lock. A missed observation must not
    // block behind an evaluate that could need that queue.
    RuntimeTryLock fallback_lock(runtime_mutex);
    if (fallback_lock.owns_lock()) {
      if (!AutoPresentFallbackSelected()) {
        SetAutoPresentFallbackActive(false);
        auto_present_state.Reset();
      } else if (queue != nullptr && swapchain != nullptr
                 && swapchain->get_device() != nullptr
                 && swapchain->get_device()->get_api() == reshade::api::device_api::d3d12) {
        auto* const resource = reinterpret_cast<ID3D12Resource*>(
            static_cast<uintptr_t>(swapchain->get_current_back_buffer().handle));
        if (resource != nullptr) {
          const auto desc = resource->GetDesc();
          fallback_surface = {
              .swapchain = swapchain,
              .device = renodx::addons::dlss5::native_identity::Get(
                  reinterpret_cast<IUnknown*>(swapchain->get_device()->get_native())),
              .width = static_cast<uint32_t>(desc.Width),
              .height = desc.Height,
              .format = static_cast<uint32_t>(desc.Format),
              .buffers = swapchain->get_back_buffer_count(),
          };
          uint32_t index = fallback_surface.buffers;
          for (uint32_t i = 0; i < fallback_surface.buffers && i < 64; ++i) {
            if (swapchain->get_back_buffer(i).handle == swapchain->get_current_back_buffer().handle) {
              index = i;
              break;
            }
          }
          if (!(auto_present_state.owner == fallback_surface)) SetAutoPresentFallbackActive(false);
          const bool recordings_complete = submission::ResourceReleasable(&auto_present_state);
          const bool allowed = hooks_enabled.load(std::memory_order_relaxed)
              && !auto_fallback && !screenshot::IsArmed() && !NrYieldsToForeign()
              && !teardown_pending.load(std::memory_order_acquire);
          sr_fallback = auto_present_state.ObservePresent(
              fallback_surface, index, allowed && recordings_complete && !auto_present_frame_generation_seen);
          const auto reason = (auto_present_state.ambiguous ? AutoPresentWait::kAmbiguous
              : auto_present_frame_generation_seen ? AutoPresentWait::kFrameGeneration
              : !allowed ? AutoPresentWait::kPaused
              : !recordings_complete ? AutoPresentWait::kRecordings : AutoPresentWait::kBuffers);
          auto_present_wait.store(reason, std::memory_order_relaxed);
          fallback_ticket = auto_present_state.revision;
        }
      }
    }
  }
  if (!PresentHookServes(false) && !auto_fallback && !sr_fallback) {
    if (screenshot::HasPending()
        || screenshot::internal::retained_sets.load(std::memory_order_relaxed) != 0) {
      // The one-shot comparison restored the inline hook after recording
      // its pair. No future Present pass will recycle its private list, so
      // retire the completed ring explicitly: a fence alone cannot close
      // the tracker's replayable recording or release the PNG readbacks.
      // Never wait here while ReShade owns the presenting queue's lock.
      RuntimeTryLock lock(runtime_mutex);
      if (lock.owns_lock()) ReleasePresentRing();
    } else if (const int64_t served = present_served_ns.load(std::memory_order_relaxed);
               !AutoPresentFallbackSelected()
               && served != 0 && SteadyNowNs() - served >= kWorksetIdleRetireNs) {
      RuntimeTryLock lock(runtime_mutex);
      if (lock.owns_lock() && RetireIdlePresentPath()) {
        present_served_ns.store(0, std::memory_order_relaxed);
        LogPresent(reshade::log::level::info,
                   "idle for " + std::to_string(kWorksetIdleRetireNs / 1'000'000'000)
                       + " s: its guide captures, pre-FG surfaces, NR streams and command"
                         " ring are released");
      }
    }
    return;
  }
  if (queue == nullptr || swapchain == nullptr) return;
  present_served_ns.store(SteadyNowNs(), std::memory_order_relaxed);
  present_unpresented_evaluates.store(0, std::memory_order_relaxed);
  reshade::api::device* const api_device = swapchain->get_device();
  if (api_device == nullptr) return;
  const bool d3d11 = api_device->get_api() == reshade::api::device_api::d3d11;
  if (!d3d11 && api_device->get_api() != reshade::api::device_api::d3d12) {
    screenshot::FailRequest("unsupported_present",
                            "this API has no final-presentation comparison path yet;"
                            " no pre-tonemap substitute was saved");
    return;
  }
  if (d3d11 && !Dx11NativeRoute()) {
    screenshot::FailRequest("unsupported_present",
                            "the Direct3D 11 foreign/off route has no final-presentation comparison");
    // Off the native route no Direct3D 11 back buffer is served
    // (PresentHookServes): the foreign route keeps a third-party tool's
    // D3D12 evaluates at the Upscaled hook point, DX11Source=off keeps the
    // addon inert.  Said once, so the setting is never silently inert; an
    // undecided route is waiting for its first module scan.
    static std::atomic_bool said{false};
    const bool off = dx11_source.load(std::memory_order_relaxed) == kDx11SourceOff;
    if ((off || dx11_route.load(std::memory_order_acquire) == kDx11RouteForeign)
        && !said.exchange(true, std::memory_order_relaxed)) {
      RuntimeLock lock(runtime_mutex);
      LogPresent(reshade::log::level::warning,
                 std::string("the Direct3D 11 back buffer is not served: ")
                     + (off ? "DX11Source=off keeps the addon inert"
                            : "the foreign route (DX11Source) serves a third-party"
                              " tool's Direct3D 12 evaluates, which keep the"
                              " Upscaled hook point"));
    }
    return;
  }
  // Outside runtime_mutex: DisplayConfig can block.
  if (const int64_t now_ns = SteadyNowNs();
      now_ns - present_sdr_white_polled_ns.load(std::memory_order_relaxed)
      >= 1'000'000'000) {
    present_sdr_white_polled_ns.store(now_ns, std::memory_order_relaxed);
    present_sdr_white.store(QueryPresentSdrWhite(swapchain), std::memory_order_relaxed);
    float tone_mapper = 0.f;
    float game_nits = 0.f;
    const bool mod_white =
        renodx_mod_loaded.load(std::memory_order_relaxed)
        && reshade::get_config_value(nullptr, "renodx-preset1", "ToneMapType", tone_mapper)
        && tone_mapper != 0.f
        && reshade::get_config_value(nullptr, "renodx-preset1", "ToneMapGameNits", game_nits)
        && game_nits >= 48.f && game_nits <= 10000.f;
    present_mod_white.store(mod_white ? game_nits : 0.f, std::memory_order_relaxed);
  }
  const CallbackScope callback_scope;
  if (!callback_scope) return;
  // From here every present names one terminal.
  present_frames_seen.fetch_add(1, std::memory_order_relaxed);
  if (!enabled.load()) {
    screenshot::FailRequest("nr_disabled", "enable NR before requesting a comparison");
    CountNrDecline(NrDeclineReason::kNrDisabledEvaluation);
    return;
  }
  if (NrYieldsToForeign()) {
    screenshot::FailRequest("foreign_nr", "another NR implementation owns this frame");
    CountNrDecline(NrDeclineReason::kForeignNr);
    return;
  }
  if (teardown_pending.load(std::memory_order_acquire)) {
    CountNrDecline(NrDeclineReason::kTeardownPending);
    return;
  }
  const uintptr_t back_handle =
      static_cast<uintptr_t>(swapchain->get_current_back_buffer().handle);
  ID3D12Resource* back = nullptr;
  ID3D11Texture2D* back11 = nullptr;
  uint32_t back_width = 0;
  uint32_t back_height = 0;
  DXGI_FORMAT back_format = DXGI_FORMAT_UNKNOWN;
  bool usable = false;
  if (d3d11) {
    // ReShade does not wrap D3D11 resources: this is the swapchain's buffer
    // 0, which is always the one being presented.
    auto* const resource = reinterpret_cast<ID3D11Resource*>(back_handle);
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (resource != nullptr) resource->GetType(&dimension);
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      back11 = static_cast<ID3D11Texture2D*>(resource);
      D3D11_TEXTURE2D_DESC desc = {};
      back11->GetDesc(&desc);
      back_width = desc.Width;
      back_height = desc.Height;
      back_format = desc.Format;
      usable = desc.ArraySize == 1 && desc.SampleDesc.Count == 1;
    }
  } else {
    back = reinterpret_cast<ID3D12Resource*>(back_handle);
    if (back != nullptr) {
      const D3D12_RESOURCE_DESC desc = back->GetDesc();
      back_width = static_cast<uint32_t>(desc.Width);
      back_height = desc.Height;
      back_format = desc.Format;
      usable = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
               && desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1;
    }
  }
  const DXGI_FORMAT format = ConcreteResourceFormat(back_format);
  const bool float_format = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  const reshade::api::color_space color_space = swapchain->get_color_space();
  const uint32_t automatic = color_space == reshade::api::color_space::srgb       ? 1u
                             : color_space == reshade::api::color_space::hdr10_pq ? 2u
                             : color_space == reshade::api::color_space::scrgb    ? 3u
                                                                                  : 0u;
  uint32_t encoding = present_encoding.load(std::memory_order_relaxed);
  // A forced encoding this back buffer cannot carry (scRGB needs FP16, HDR10
  // 10-bit or FP16) takes Auto's for this swapchain instead of switching the
  // Present hook point off: Code Vein II v8.5.0-rc1 set scRGB on an HDR10
  // R10G10B10A2 swapchain, and the evaluates fell back to Upscaled.
  if (encoding != 0 && automatic != 0
      && ((encoding == 2 && format != DXGI_FORMAT_R10G10B10A2_UNORM && !float_format)
          || (encoding == 3 && !float_format))) {
    static std::atomic_uint64_t said{0};
    if (const uint64_t shape = (uint64_t{encoding} << 32) | static_cast<uint32_t>(back_format);
        said.exchange(shape, std::memory_order_relaxed) != shape) {
      RuntimeLock lock(runtime_mutex);
      LogPresent(reshade::log::level::warning,
                 "NRPresentEncoding " + std::to_string(encoding)
                     + " cannot be carried by this back buffer (format "
                     + std::to_string(static_cast<uint32_t>(back_format))
                     + "); Auto's encoding " + std::to_string(automatic) + " (colour space "
                     + std::to_string(static_cast<uint32_t>(color_space)) + ") serves it");
    }
    encoding = automatic;
  }
  if (encoding == 0) encoding = automatic;
  PresentTarget target;
  target.capture_display = true;
  if (encoding == 2) {
    target.hdr_mode = 2;
    target.units = {codec::Encoding::Pq, true, 10000.f, true};
    usable = usable && (format == DXGI_FORMAT_R10G10B10A2_UNORM || float_format);
  } else if (encoding == 3) {
    target.hdr_mode = 1;
    target.units = {codec::Encoding::Linear, true, 80.f, false};
    usable = usable && float_format;
  } else {
    usable = usable && encoding == 1;
  }
  if (!usable) {
    screenshot::FailRequest("unsupported_present", "the swapchain format or color space is unsupported");
    CountNrDecline(NrDeclineReason::kPresentFormat);
    if (!present_unusable.exchange(true, std::memory_order_relaxed)) {
      RuntimeLock lock(runtime_mutex);
      LogPresent(reshade::log::level::warning,
                 "the swapchain cannot be served (colour space "
                     + std::to_string(static_cast<uint32_t>(color_space)) + ", format "
                     + std::to_string(static_cast<uint32_t>(back_format))
                     + ", NRPresentEncoding "
                     + std::to_string(present_encoding.load()) + ")");
    }
    return;
  }
  const float mod_white =
      present_white_source.load() == 0u ? present_mod_white.load(std::memory_order_relaxed) : 0.f;
  const char* const white_source = present_white_nits.load() > 0.f ? "NRPresentWhiteNits"
                                   : mod_white > 0.f
                                       ? "the RenoDX mod's Game Brightness ([renodx-preset1]"
                                         " ToneMapGameNits)"
                                   : present_sdr_white.load() > 0.f
                                       ? "the Windows SDR content brightness"
                                       : "NRDiffuseWhiteNits (no Windows SDR level was read)";
  const float white = present_white_nits.load() > 0.f ? present_white_nits.load()
                      : mod_white > 0.f                ? mod_white
                      : present_sdr_white.load() > 0.f ? present_sdr_white.load()
                                                        : diffuse_white_nits.load();
  if (target.hdr_mode != 0) {
    // Both latches move on every HDR present; a line per change of either.
    const bool source_changed =
        present_white_logged.exchange(white_source, std::memory_order_relaxed) != white_source;
    const bool nits_changed =
        present_white_logged_nits.exchange(white, std::memory_order_relaxed) != white;
    if (source_changed || nits_changed) {
      char line[160];
      snprintf(line, sizeof(line), "Present hook point: HDR diffuse white %.0f nits from %s",
               white, white_source);
      Log(reshade::log::level::info, line);
    }
  }
  const uint32_t mode = codec_mode.load();
  // Anchored (1) and Display (3) keep their own anchors (FrameCodecDivisor).
  target.divisor = target.hdr_mode != 0 && mode != 1u && mode != 3u
      ? codec::PresentDivisor(target.units, white, paper_white_scale.load(),
                              pq_calibration.load())
      : 0.f;
  target.ui_correction = present_ui_correction.load();

  // A D3D11 immediate context may be protected by the game's recursive
  // device section. The order on both the game and present paths is device
  // section -> NGX turn -> runtime_mutex, including neutral-guide clears.
  std::optional<BridgeDeviceLock> device_lock;
  if (d3d11) {
    PrimeNgxLoaderSymbols(nullptr);
    ID3D11DeviceContext* context = nullptr;
    reinterpret_cast<ID3D11Device*>(api_device->get_native())->GetImmediateContext(&context);
    const NrDeclineReason bridge_ready = EnsureBridgeUp(context);
    if (bridge_ready != kBridgeReady) {
      context->Release();
      CountNrDecline(bridge_ready);
      return;
    }
    device_lock.emplace(context);
    context->Release();
    // A teardown may have finished while Enter waited. Do not initialize
    // DLLs or a replacement device while holding the game's device section.
    if (bridge_state.load(std::memory_order_acquire) != BridgeState::kUp) {
      CountNrDecline(NrDeclineReason::kBridgeDown);
      return;
    }
  }
  // NGX detours take their serial turn before runtime_mutex. Keep that order
  // here too: holding runtime_mutex while waiting for a game's NGX call makes
  // that call wait for us, and the bounded timeout sends an unenhanced frame
  // between enhanced ones (GoWR v8.0.3: gaps=113, ngx_busy=113, slot_timeouts=0).
  std::optional<ngx_serial::PresentTurn> ngx_turn(std::in_place, sr_fallback);
  // Optional because Direct3D 11 hands the frame to the bridge, which takes
  // runtime_mutex itself and may bring the bridge up outside it.
  std::optional<RuntimeLock> lock(std::in_place, runtime_mutex);
  // A DLSS evaluate can resume while this present waits for its NGX turn.
  // The selected inline path owns that frame again; do not enhance it twice.
  const auto auto_owner_current = [&] {
    if (sr_fallback) {
      return AutoPresentFallbackSelected() && !auto_present_frame_generation_seen
          && !screenshot::IsArmed()
          && auto_present_state.IsCurrent(fallback_surface, fallback_ticket)
          && submission::ResourceReleasable(&auto_present_state);
    }
    return !auto_fallback || PresentHookServes(false)
        || (auto_fg_fallback_active.load(std::memory_order_relaxed)
            && auto_fg_observation.swapchain == swapchain);
  };
  if (!auto_owner_current()) {
    CountNrDecline(NrDeclineReason::kPresentHook);
    return;
  }
  if (screenshot::WantsFinalPresent()
      && (hook_point.load(std::memory_order_relaxed) != kHookPresent
          || present_prefg_active.load(std::memory_order_relaxed)
          || present_inline_fallback_pending)) {
    // Read fallback provenance under the evaluate's lock, before a resumed
    // present clears present_unusable. The queued image may still have been
    // enhanced by Upscaled fallback even though the selected hook is Present.
    screenshot::internal::requires_passthrough_frame.store(true, std::memory_order_release);
    if (present_fg_multiplier.load(std::memory_order_relaxed) > 1
        || present_prefg_active.load(std::memory_order_relaxed)) {
      // A completed game-frame guide copy does not identify DLSS-G's
      // currently presented frame. It may still contain earlier inline NR.
      // Never call that image NR_OFF or guess at a number of drain frames.
      screenshot::FailRequest("unproven_baseline",
                              "exact comparison unavailable with this frame-generation route;"
                              " disable frame generation for F5");
      if (hook_point.load(std::memory_order_relaxed) != kHookPresent) {
        CountNrDecline(NrDeclineReason::kScreenshotWait);
        return;
      }
    }
  }
  DrainPresentRetired();
  if (!d3d11) {
    // NGX records in the world of its list: the face NR's runtime holds, or
    // the one ReShade hands resources' GetDevice callers (its device proxy)
    // when the runtime is not up yet - the world the game's own lists are in.
    ID3D12Device* face = nullptr;
    if (direct_device != nullptr
        && renodx::addons::dlss5::native_identity::Same(
            direct_device, reinterpret_cast<IUnknown*>(api_device->get_native()))) {
      face = direct_device;
      face->AddRef();
    } else if (FAILED(back->GetDevice(IID_PPV_ARGS(&face)))) {
      face = nullptr;
    }
    const bool ring_ready = face != nullptr && EnsurePresentRing(face);
    ReleaseCom(face);
    if (!ring_ready) {
      // A present without its ring cannot run NR, like one whose swapchain
      // cannot be served, and falls back the same way: the evaluates run the
      // Upscaled hook point until a present builds it (PresentTakesEvaluate).
      // Through v8.0.1 it only counted present_format, so NR silently did
      // nothing while the decline text said the evaluates took over.
      CountNrDecline(NrDeclineReason::kPresentFormat);
      present_unusable.store(true, std::memory_order_relaxed);
      return;
    }
  }
  if (present_unusable.exchange(false, std::memory_order_relaxed)) {
    LogPresent(reshade::log::level::info, "the swapchain can be served again");
  }
  PresentStream& stream = present_streams[swapchain];
  // Every return below that runs no NR on a stream that already ran it sends
  // a raw frame to the screen between NR frames: the flicker a player sees,
  // counted in present[gaps=].  NRPresentFrames=1's repeats are raw by
  // choice and set `ran` too.  Destroyed before `lock`.
  struct GapCount {
    bool had_nr = false;
    bool ran = false;
    ~GapCount() {
      if (!ran && had_nr) {
        present_gaps.fetch_add(1, std::memory_order_relaxed);
      }
    }
  } gap{stream.nr_presents != 0};

  // A matching DLSS-G declaration is authoritative. When absent, timing
  // needs two complete, consistent windows; hitches do not change NR's
  // motion contract. Count before ring readiness so busy NR does not lower
  // the measured number of presents per game frame.
  const int64_t now_ns = SteadyNowNs();
  const auto fresh = [&](const PresentGuideSet& set) {
    return set.serial != 0 && set.d3d11 == d3d11
           && set.device_identity == stream.device_identity
           && PresentAspectMatches(set.output_width, set.output_height, back_width, back_height)
           && (present_generation - set.captured_at <= kPresentGuideMaxAge * stream.fg_multiplier
               || now_ns - set.captured_ns <= kPresentGuideMaxAgeNs);
  };
  if (stream.device_identity == nullptr) {
    auto* const native_device = reinterpret_cast<IUnknown*>(api_device->get_native());
    stream.device_identity = renodx::addons::dlss5::native_identity::Get(native_device);
    stream.streamline_identity = StreamlineBaseIdentity(native_device);
  }
  stream.backbuffer_width = back_width;
  stream.backbuffer_height = back_height;
  stream.backbuffer_format = back_format;
  stream.d3d11 = d3d11;
  // The target is retained with its owner; another swapchain's last present
  // cannot supply this frame's encoding or dimensions to RunPresentPreFg.
  if (!d3d11) {
    stream.prefg_target = target;
    stream.prefg_target_valid = true;
  }
  uint32_t matching_streams = 0;
  uint32_t matching_cadence_streams = 0;
  for (const auto& entry : present_streams) {
    const PresentStream& other = entry.second;
    if (other.device_identity != stream.device_identity || other.d3d11 != d3d11) continue;
    matching_streams += other.backbuffer_width == back_width && other.backbuffer_height == back_height;
    matching_cadence_streams += PresentAspectMatches(
        other.backbuffer_width, other.backbuffer_height, back_width, back_height);
  }
  uint32_t declared_multiplier = 0;
  if (!d3d11 && present_fg_declaration.multiplier != 0
      && present_fg_declaration.device == stream.device_identity
      && present_fg_declaration.width == back_width
      && present_fg_declaration.height == back_height
      && (now_ns - present_fg_declaration.observed_ns <= 1'000'000'000
          || present_frames_seen.load(std::memory_order_relaxed)
                 - present_fg_declaration.observed_present
             <= 8 * present_fg_declaration.multiplier)) {
    if (matching_streams == 1) declared_multiplier = present_fg_declaration.multiplier;
  }
  const bool changed = stream.fg_cadence.Observe(
      now_ns, stream.cadence_captures,
      matching_cadence_streams == 1 && now_ns >= stream.cadence_ambiguous_until_ns
          && std::ranges::any_of(present_guide_sets, fresh), declared_multiplier,
      static_cast<::renodx::dlss5::PresentFgSource>(present_fg_source.load(std::memory_order_relaxed)));
  present_cadence.store(stream.fg_cadence.cadence, std::memory_order_relaxed);
  if (changed) {
    const uint32_t multiplier = stream.fg_cadence.multiplier;
    const bool flipped = (multiplier == 1) != (stream.fg_multiplier == 1);
    if (flipped) present_fg_flips.fetch_add(1, std::memory_order_relaxed);
    stream.fg_multiplier = multiplier;
    present_fg_multiplier.store(multiplier, std::memory_order_relaxed);
    std::ostringstream message;
    message.precision(3);
    message << stream.fg_cadence.cadence << " presents per game frame: frame generation "
            << (!flipped         ? "now " + std::to_string(multiplier) + "x"
                : multiplier > 1 ? "detected (" + std::to_string(multiplier) + "x)"
                                 : std::string("no longer detected"))
            << "; source=" << (stream.fg_cadence.declared ? "DLSSG.MultiFrameCount"
                : stream.fg_cadence.source == ::renodx::dlss5::PresentFgSource::kLegacyTiming
                    ? "legacy timing" : "stable timing")
            << "; "
            << (present_prefg_served_ns != 0
                    ? "NR runs once per game frame before frame generation, the"
                      " presents none (NRPresentPreFg)"
                : present_frames.load() == 1
                    ? "NR runs on the first present after each game frame"
                      " (NRPresentFrames=1)"
                : multiplier > 1
                    ? "NR runs on every present with motion scaled by 1/"
                          + std::to_string(multiplier) + " (NRPresentFrames=0)"
                    : "NR runs on every present (NRPresentFrames=0)");
    LogPresent(reshade::log::level::info, message.str());
  }

  // Before frame generation (RunPresentPreFg): the frame DLSS-G presents,
  // generated or real, was built from NR's output already, so no NR here and
  // no gap.  After the cadence, so the multiplier stays current for the
  // presents that take over when pre-FG falls back.
  const bool prefg = !d3d11 && present_prefg_served_swapchain == swapchain
      && present_prefg_served_ns != 0
      && (now_ns - present_prefg_served_ns <= kPresentPreFgHoldNs
          || present_frames_seen.load(std::memory_order_relaxed) - present_prefg_served_presents
                 <= kPresentPreFgHoldFrames * present_prefg_served_multiplier);
  present_prefg_active.store(prefg, std::memory_order_relaxed);
  if (prefg) {
    gap.ran = true;
    CountNrDecline(NrDeclineReason::kPresentPreFg);
    return;
  }
  // Pre-FG cannot serve frame generation (kPresentPreFgMissFrames): the
  // evaluates run the Upscaled hook point, which DLSS-G interpolates from, so
  // NR here would enhance the real frames twice and the generated ones never.
  if (!d3d11 && present_prefg_upscaled_active.load(std::memory_order_relaxed)) {
    gap.ran = true;
    CountNrDecline(NrDeclineReason::kPresentFormat);
    return;
  }

  if (!ngx_turn->owns()) {
    CountNrDecline(NrDeclineReason::kPresentBusy);
    return;
  }

  // Direct3D 12 records on the path's own ring; Direct3D 11 on the bridge's.
  // A busy slot is waited for, bounded (kPresentSlotWaitMs), outside
  // runtime_mutex so the game's evaluates never wait behind the GPU.
  PresentSlot* slot = nullptr;
  if (!d3d11) {
    slot = &present_ring.slots[(present_ring.submitted + 1) % kPresentRing];
    uint64_t done = present_ring.fence->GetCompletedValue();
    if (done != UINT64_MAX && done >= slot->value) {
      present_slot_wait_off = false;
    } else if (done != UINT64_MAX && !present_slot_wait_off) {
      present_slot_waits.fetch_add(1, std::memory_order_relaxed);
      // Keep the waited ring's identity alive while other swapchains can
      // rebuild it. Their lists must never be submitted on this queue.
      const FenceRef waited_fence(present_ring.fence);
      bool completed = false;
      if (HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        if (SUCCEEDED(present_ring.fence->SetEventOnCompletion(slot->value, event))) {
          lock.reset();
          ngx_turn.reset();
          completed = WaitForSingleObject(event, kPresentSlotWaitMs) == WAIT_OBJECT_0;
          ngx_turn.emplace(sr_fallback);
          lock.emplace(runtime_mutex);
        }
        CloseHandle(event);
      }
      if (!completed) {
        present_slot_wait_off = true;
        if (present_slot_timeouts.fetch_add(1, std::memory_order_relaxed) == 0) {
          LogPresent(reshade::log::level::warning,
                     "a present command list slot was still on the GPU after "
                         + std::to_string(kPresentSlotWaitMs)
                         + " ms; that present keeps the game's image, and presents"
                           " stop waiting until a slot frees on its own"
                           " (present[slot_timeouts=])");
        }
      }
      // Another present (another swapchain's thread) or a teardown may have
      // moved the ring meanwhile.
      const auto current_stream = present_streams.find(swapchain);
      if (!ngx_turn->owns() || !auto_owner_current()
          || present_ring.fence != waited_fence.fence
          || current_stream == present_streams.end() || &current_stream->second != &stream
          || teardown_pending.load(std::memory_order_acquire) || !enabled.load()) {
        CountNrDecline(NrDeclineReason::kPresentBusy);
        return;
      }
      slot = &present_ring.slots[(present_ring.submitted + 1) % kPresentRing];
      done = present_ring.fence->GetCompletedValue();
    }
    if (done == UINT64_MAX || done < slot->value) {
      CountNrDecline(NrDeclineReason::kPresentBusy);
      return;
    }
  }

  // The newest capture that describes this present: of its API, submitted
  // (D3D12; the immediate context orders a D3D11 one), fresh, and of the
  // back buffer's aspect (an output drawn into a letterbox or composed
  // beside other views would misplace every guide), and orderable: its
  // tokens on the present queue's own tracker fence become queue Waits, its
  // tokens on any other queue's fence must already be complete (Cross-queue
  // captures above).  A queue the tracker has not seen submit yet has no
  // fence, so every token is another queue's.
  auto* const native_queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
  const bool comparing_clean_frame = screenshot::WantsFinalPresent()
      && screenshot::internal::requires_passthrough_frame.load(std::memory_order_acquire);
  // SR inactivity must never reuse gameplay captures (Optional allows them
  // for up to 250 ms). Videos have no corresponding depth, motion or jitter.
  const uint32_t guides_mode = sr_fallback ? kPresentGuidesNever
      : (comparing_clean_frame ? kPresentGuidesRequired : present_guides.load());
  uint64_t comparison_serial = 0;
  if (comparing_clean_frame) {
    for (const PresentGuideSet& set : present_guide_sets) {
      if (fresh(set)
          && set.comparison_request
                 == screenshot::internal::request_id.load(std::memory_order_acquire)) {
        comparison_serial = std::max(comparison_serial, set.serial);
      }
    }
  }
  PresentGuideSet* guides = nullptr;
  std::vector<std::pair<FenceRef, uint64_t>> waits;
  uint64_t in_flight_serial = 0;
  if (guides_mode != kPresentGuidesNever) {
    const ID3D12Fence* own_fence = nullptr;
    if (!d3d11) {
      std::shared_lock map(queue_completion_mutex);
      if (const auto it = queue_completions.find(native_queue); it != queue_completions.end()) {
        own_fence = it->second.fence;
      }
    }
    std::vector<std::pair<FenceRef, uint64_t>> tokens;
    for (PresentGuideSet& set : present_guide_sets) {
      if (!fresh(set) || (guides != nullptr && set.serial < guides->serial)
          || (comparing_clean_frame
              && (comparison_serial == 0 || set.serial != comparison_serial))) {
        continue;
      }
      if (!d3d11) {
        if (submission::UseSubmissionTokens(set.marker, &tokens)
            == submission::UseSubmission::kUnsubmitted) continue;
        if (comparing_clean_frame && !set.completed_fence.empty()) {
          tokens.emplace_back(set.completed_fence, set.completed_value);
        }
      }
      bool in_flight = false;
      bool foreign_queue = comparing_clean_frame && set.completed_multiple_queues;
      std::erase_if(tokens, [&](const std::pair<FenceRef, uint64_t>& token) {
        if (token.first.fence == own_fence) return false;
        foreign_queue = true;
        in_flight |= token.first->GetCompletedValue() < token.second;
        return true;
      });
      if (comparing_clean_frame && !d3d11
          && (tokens.empty() || own_fence == nullptr) && !foreign_queue) {
        // Neither a live token nor a retained completion identifies a
        // producer queue. A recording reset without submitting is also
        // "complete" to the lifetime tracker, but cannot prove this image.
        // Keep F5 armed for a newer frame with actual submission evidence.
        continue;
      }
      if (comparing_clean_frame && !d3d11
          && foreign_queue) {
        screenshot::FailRequest("unproven_baseline",
                                "the game frame has no matching presentation-queue proof, so an untouched"
                                " final frame cannot be identified; disable frame generation for F5");
        gap.ran = true;
        CountNrDecline(NrDeclineReason::kScreenshotWait);
        return;
      }
      if (in_flight) {
        in_flight_serial = std::max(in_flight_serial, set.serial);
        continue;
      }
      guides = &set;
      waits = std::move(tokens);
    }
  }
  if (in_flight_serial > (guides != nullptr ? guides->serial : 0)
      && present_guides_in_flight.fetch_add(1, std::memory_order_relaxed) == 0) {
    LogPresent(reshade::log::level::info,
               "the newest guide capture is still in flight on another queue (frame"
               " generation presents on its own queue); presents take the newest"
               " complete capture instead of waiting for it on the GPU, which would"
               " deadlock both queues (present[in_flight=])");
  }
  if (guides == nullptr && guides_mode == kPresentGuidesRequired) {
    if (comparing_clean_frame) {
      // In flight on a game's other queue is not proof of an untouched
      // presented source. Keep the request armed until that raw frame's
      // guide copy is ordered, without ever waiting on another queue here.
      gap.ran = true;
      CountNrDecline(NrDeclineReason::kScreenshotWait);
      return;
    }
    CountNrDecline(NrDeclineReason::kPresentNoGuides);
    return;
  }
  if (comparing_clean_frame && guides != nullptr) {
    // Only the accepted request-stamped, presentation-ordered passthrough
    // frame retires the uncertainty from an earlier inline fallback.
    present_inline_fallback_pending = false;
  }
  const bool new_frame = guides != nullptr && guides->serial != stream.consumed_serial;
  if (present_frames.load() == 1 && guides != nullptr && !new_frame) {
    gap.ran = true;
    CountNrDecline(NrDeclineReason::kPresentRepeat);
    return;
  }

  if (guides == nullptr && d3d11) {
    // The same neutral guides on the swapchain's native D3D11 device,
    // cleared to zero once (D3D11 leaves a texture created without data
    // undefined); the bridge copies them into its twins like a capture.
    auto* const device11 = reinterpret_cast<ID3D11Device*>(api_device->get_native());
    for (auto [surface, surface_format] :
         {std::pair{&stream.neutral_motion11, DXGI_FORMAT_R16G16_FLOAT},
          std::pair{&stream.neutral_depth11, DXGI_FORMAT_R32_FLOAT}}) {
      if (*surface != nullptr) {
        D3D11_TEXTURE2D_DESC have = {};
        (*surface)->GetDesc(&have);
        if (have.Width != back_width || have.Height != back_height) ReleaseCom(*surface);
      }
      if (*surface != nullptr) continue;
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = back_width;
      desc.Height = back_height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = surface_format;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      ID3D11RenderTargetView* view = nullptr;
      if (FAILED(device11->CreateTexture2D(&desc, nullptr, surface)) || *surface == nullptr
          || FAILED(device11->CreateRenderTargetView(*surface, nullptr, &view))) {
        ReleaseCom(*surface);
        CountNrDecline(NrDeclineReason::kWorksetSetupFailed);
        return;
      }
      ID3D11DeviceContext* context = nullptr;
      device11->GetImmediateContext(&context);
      constexpr float kZero[4] = {};
      context->ClearRenderTargetView(view, kZero);
      context->Release();
      view->Release();
    }
  } else if (guides == nullptr) {
    // Neutral guides at the back buffer's size: zero-initialized committed
    // textures (no render-target or depth flags), so zero motion and depth
    // 0, which DepthInverted below reads as the far plane.
    for (auto [surface, surface_format, name] :
         {std::tuple{&stream.neutral_motion, DXGI_FORMAT_R16G16_FLOAT,
                     L"DLSS5 Generic present neutral motion"},
          std::tuple{&stream.neutral_depth, DXGI_FORMAT_R32_FLOAT,
                     L"DLSS5 Generic present neutral depth"}}) {
      if (*surface != nullptr
          && ((*surface)->GetDesc().Width != back_width
              || (*surface)->GetDesc().Height != back_height)) {
        RetirePresentResource(surface);
      }
      if (*surface == nullptr
          && !CreateScratchTexture(present_ring.device, back_width, back_height,
                                   surface_format, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   surface, name)) {
        CountNrDecline(NrDeclineReason::kWorksetSetupFailed);
        return;
      }
    }
  }
  if ((guides != nullptr) != stream.real_guides) {
    stream.real_guides = guides != nullptr;
    ResetPresentStreamHistory(&stream);
  }

  ID3D12Resource* const motion = guides != nullptr ? guides->motion : stream.neutral_motion;
  ID3D12Resource* const depth = guides != nullptr ? guides->depth : stream.neutral_depth;
  ID3D11Texture2D* const motion11 =
      guides != nullptr ? guides->motion11 : stream.neutral_motion11;
  ID3D11Texture2D* const depth11 = guides != nullptr ? guides->depth11 : stream.neutral_depth11;
  const FeatureState neutral_contract = [&] {
    FeatureState contract;
    contract.input_width = back_width;
    contract.input_height = back_height;
    contract.has_motion_scale_x = true;
    contract.has_motion_scale_y = true;
    contract.motion_scale_x = static_cast<float>(back_width);
    contract.motion_scale_y = static_cast<float>(back_height);
    contract.create_flags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    return contract;
  }();
  const FeatureState& contract = guides != nullptr ? guides->contract : neutral_contract;
  // Every present of a generated run moves 1/multiplier of a game frame.
  const float motion_share = guides != nullptr && present_frames.load() == 0
      ? 1.f / static_cast<float>(stream.fg_multiplier)
      : 1.f;
  // The game's Reset rides the first present of its frame only.
  bridge::OwnedParameters& block = FillPresentBlock(
      &stream, d3d11 ? static_cast<void*>(back11) : back,
      d3d11 ? static_cast<void*>(motion11) : motion,
      d3d11 ? static_cast<void*>(depth11) : depth, contract, back_width, back_height,
      motion_share, new_frame ? contract.frame_reset : 0);
  target.workset_key = reinterpret_cast<const ID3D12Resource*>(&stream);

  // The serial turn was acquired before runtime_mutex, and is held through
  // recording and submission. A GPU slot wait releases both locks so the
  // game's NGX calls remain able to make progress.

  if (d3d11) {
    // The bridge's own run path, with this block for the game's: it takes
    // runtime_mutex itself and may bring the bridge up (DLLs, the Detours
    // transaction), so the lock goes first.  The stream and its block stay
    // put meanwhile: only this swapchain's destroy erases them, never during
    // its present.  The bridge names the terminal from here, and its
    // declines (bridge_down, bridge_busy, bridge_format, ...) are counted
    // and logged as for a game's evaluate.
    const uint64_t serial = guides != nullptr ? guides->serial : 0;
    const NVSDK_NGX_Handle* const handle = stream.Handle();
    lock.reset();
    ID3D11DeviceContext* context = nullptr;
    reinterpret_cast<ID3D11Device*>(api_device->get_native())->GetImmediateContext(&context);
    bool delivered = false;
    {
      const EvaluateChainScope chain_scope;
      delivered = RunBridge(context, handle, &block, nullptr, &target);
    }
    context->Release();
    if (!delivered) return;
    lock.emplace(runtime_mutex);
    gap.ran = true;
    ++stream.nr_presents;
    if (serial != 0) stream.consumed_serial = serial;
    (serial != 0 ? present_guides_real : present_guides_neutral)
        .fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (FAILED(slot->allocator->Reset())
      || FAILED(slot->list->Reset(slot->allocator, nullptr))) {
    CountNrDecline(NrDeclineReason::kPresentBusy);
    LogPresent(reshade::log::level::error, "a present command list could not be reset");
    return;
  }
  submission::OnCommandListReset(slot->list);
  // Our own list has no host state for the restore-target gate to wait on.
  gate_ever_opened.store(true, std::memory_order_relaxed);
  bool touched = false;
  if (sr_fallback) {
    // Rechecked after every unlocked wait above; the NGX turn and runtime
    // lock remain held through recording AND queue submission.
    SetAutoPresentFallbackActive(true);
    auto_present_handle = stream.Handle();
  }
  {
    const EvaluateChainScope chain_scope;
    const int64_t started_ns = SteadyNowNs();
    // From here ProcessInline names the terminal.
    ProcessInline(slot->list, stream.Handle(), &block, &touched, &target);
    RecordInjectionCpu(started_ns);
  }
  submission::TrackUse(slot->list, motion);
  submission::TrackUse(slot->list, depth);
  const bool closed = SUCCEEDED(slot->list->Close());
  if (!touched) return;
  if (!closed) {
    LogPresent(reshade::log::level::error,
               "a present command list could not be closed; this frame keeps"
               " the game's image");
    return;
  }
  // After the capture's copies: only this queue's own tokens are left here,
  // the others were proven complete above.
  for (const auto& [fence, value] : waits) {
    native_queue->Wait(fence.fence, value);
    present_guide_waits.fetch_add(1, std::memory_order_relaxed);
  }
  // Add-on work recorded earlier through ReShade's immediate list keeps its
  // place ahead of NR.
  queue->flush_immediate_command_list();
  ID3D12CommandList* list = slot->list;
  renodx::utils::directx::NativeFromReShadeProxy(&list);
  native_queue->ExecuteCommandLists(1, &list);
  slot->value = ++present_ring.submitted;
  native_queue->Signal(present_ring.fence, slot->value);
  gap.ran = true;
  ++stream.nr_presents;
  if (guides != nullptr) {
    guides->read_value = slot->value;
    stream.consumed_serial = guides->serial;
    present_guides_real.fetch_add(1, std::memory_order_relaxed);
  } else {
    present_guides_neutral.fetch_add(1, std::memory_order_relaxed);
  }
}

/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <vector>

namespace renodx::addons::dlss5::evaluation {

// Observations only: this ledger never admits, rejects, or waits for GPU work.
enum class Outcome : uint8_t {
  kUnexplained, kCompleted, kDeclined, kMissingInput, kListUnsupported,
  kHooksUnavailable, kDeviceUnavailable, kNoGuideSize, kNrFailed,
  kRouteUnavailable, kLifecycleInactive, kDisabled, kExcluded, kRetry,
  kGameFailed, kMalformed, kPassthrough, kTransportFailed, kCount
};
inline constexpr const char* kOutcomeNames[] = {
    "unexplained", "nr_chain_completed", "declined", "missing_input",
    "command_list_unsupported", "state_hooks_unavailable", "device_unavailable",
    "guide_size_unavailable", "nr_failed", "route_unavailable", "lifecycle_inactive",
    "disabled", "excluded_feature_or_source", "retry_pending", "game_failed",
    "malformed_evaluate", "passthrough", "transport_failed"};
enum class Route : uint8_t { kUnknown, kAfter, kBefore, kStreamline, kBridgeAfter, kBridgeBefore };
inline constexpr const char* kRouteNames[] = {
    "unknown", "after", "before", "streamline_after", "dx11_after", "dx11_before"};
struct Scalar {
  double value = 0;
  uint32_t result = 0;
  bool present = false;
};
inline constexpr const char* kScalarNames[] = {
    "width", "height", "out_width", "out_height", "render_width", "render_height",
    "color_x", "color_y", "motion_x", "motion_y", "depth_x", "depth_y",
    "output_x", "output_y", "flags", "output_subrects", "scale_x", "scale_y",
    "jitter_x", "jitter_y", "pre_exposure", "exposure_scale", "reset"};
struct Resource {
  uint64_t width = 0;
  uint32_t height = 0, format = 0, flags = 0;
  uint16_t mips = 0, layers = 0, samples = 0, dimension = 0;
};
struct TransportSteps {
  uint32_t calls = 0, failures = 0, first_hr = 0, last_hr = 0, format = 0, bind = 0;
  uint32_t support1 = 0, support2 = 0, required1 = 0, required2 = 0;
  const char* condition = "none";
  const char* first_step = "none";
  const char* last_step = "none";
};
struct Record {
  uint64_t sequence = 0, handle = 0, generation = 0, device_generation = 0;
  int64_t completed_ns = 0;
  uint32_t feature = ~0u, game_result = 0, nr_result = 0;
  uint32_t missing_inputs = 0, decline = ~0u, nr_calls = 0;
  // Mask bits mark actual Close, signal-in, wait-in, signal-out, signal-done, wait-out returns.
  uint32_t queue_mask = 0;
  std::array<uint32_t, 6> queue_results{};
  uint64_t bridge_submission = 0, bridge_completed_before = 0;
  bool bridge_submitted = false;
  bool before_selected = false, enabled = false, create_seen = false;
  bool game_returned = false, recorded = false, attempted = false, raw_available = false;
  bool bridge_reused = false;
  uint8_t source_api = 0;  // 1 D3D12, 2 D3D11, 3 Streamline/D3D12
  uint8_t eligibility = 0;  // 0 unknown, 1 DLSS/RR, 2 excluded
  std::array<uint32_t, 3> modes{};  // guide, output rectangle, Color rectangle
  Route route = Route::kUnknown;
  Outcome outcome = Outcome::kUnexplained;
  std::array<Scalar, std::size(kScalarNames)> raw{}, captured{};
  std::array<Resource, 5> source{}, transport{};
  std::array<TransportSteps, 5> bridge{};
  // render, motion, output and Color windows; scale values; flags and policy.
  std::array<uint32_t, 24> resolved{};
  float scale_x = 0, scale_y = 0;
  uint32_t bridge_role = ~0u, bridge_hr = 0;
  const char* bridge_step = "none";
};
inline thread_local Record* active = nullptr;
// One scratch record per evaluating thread, rather than a large stack object
// in every nested export and each of the addon's own stack-pass callbacks.
inline thread_local Record current_record;
inline constexpr size_t kCapacity = 256;
struct Entry {
  std::atomic_flag busy = ATOMIC_FLAG_INIT;
  Record record;
};
inline std::array<Entry, kCapacity> history;
inline std::atomic_uint64_t started{0}, finished{0}, in_flight{0}, revision{0};
inline std::atomic_uint32_t writers{0};
inline std::array<std::atomic_uint64_t, static_cast<size_t>(Outcome::kCount)> totals{};
inline std::atomic_uint64_t overwritten{0}, detail_dropped{0}, latest_sequence{0}, latest_relevant_sequence{0};
inline std::array<std::atomic_uint64_t, 3> eligibility_totals{};
inline std::atomic_uint64_t raw_entries{0}, nested_entries{0}, internal_entries{0};
static_assert(sizeof(history) < 1024 * 1024);

struct Totals {
  uint64_t started = 0, finished = 0, in_flight = 0, overwritten = 0, dropped = 0;
  std::array<uint64_t, static_cast<size_t>(Outcome::kCount)> outcomes{};
  std::array<uint64_t, 3> eligibility{};
  bool coherent = false;
};
// Concurrent writers never wait. Readers disclose an unstable snapshot instead
// of using saturated arithmetic to pretend that incomplete counters reconcile.
inline Totals ReadTotals() {
  Totals result;
  for (unsigned attempt = 0; attempt != 3; ++attempt) {
    const uint64_t before = revision.load();
    const bool quiet = writers.load() == 0;
    result.started = started.load();
    result.finished = finished.load();
    result.in_flight = in_flight.load();
    result.overwritten = overwritten.load();
    result.dropped = detail_dropped.load();
    for (size_t i = 0; i < result.outcomes.size(); ++i) result.outcomes[i] = totals[i].load();
    for (size_t i = 0; i < 3; ++i) result.eligibility[i] = eligibility_totals[i].load();
    result.coherent = quiet && writers.load() == 0 && before == revision.load();
    if (result.coherent) break;
  }
  return result;
}

inline void Note(Outcome outcome) {
  if (active != nullptr && active->outcome != Outcome::kCompleted
      && active->outcome == Outcome::kUnexplained) active->outcome = outcome;
}
inline void Decline(uint32_t reason, Outcome outcome = Outcome::kDeclined) {
  if (active != nullptr && active->outcome == Outcome::kUnexplained) {
    active->outcome = outcome;
    active->decline = reason;
  }
}
inline void Completed(Route route) {
  if (active == nullptr) return;
  active->outcome = Outcome::kCompleted;
  active->route = route;
  active->recorded = true;
}
// Actual HRESULTs only; caller records void copies/dispatches as recorded/submitted.
inline void BridgeApi(uint32_t role, const char* step, int32_t hr, uint32_t format = 0, uint32_t bind = 0) {
  if (active == nullptr || role >= active->bridge.size()) return;
  auto& d = active->bridge[role];
  ++d.calls;
  d.last_hr = static_cast<uint32_t>(hr);
  d.last_step = step;
  if (format != 0) { d.format = format; d.bind = bind; }
  if (hr < 0) {
    if (d.failures++ == 0) { d.first_hr = static_cast<uint32_t>(hr); d.first_step = step; }
    if (active->bridge_role == ~0u) {
      active->bridge_role = role;
      active->bridge_step = step;
      active->bridge_hr = static_cast<uint32_t>(hr);
    }
  }
}
// A structural/capability observation is not an HRESULT from a graphics API.
inline void BridgeCondition(uint32_t role, const char* condition) {
  if (active != nullptr && role < active->bridge.size()
      && active->bridge[role].condition == std::string_view("none"))
    active->bridge[role].condition = condition;
}
inline void GameReturned(uint32_t result) {
  if (active == nullptr) return;
  active->game_result = result;
  active->game_returned = true;
}
template <typename T>
inline T Return(T result, bool game = true) {
  if (game) GameReturned(static_cast<uint32_t>(result));
  return result;
}

// Nested wrappers share a record. Direct NR re-entry is explicitly excluded by
// the caller; its return value must never replace the game's return value.
class Scope {
 public:
  Scope(bool game, const void* handle, bool enabled, bool before, Route route, std::array<uint32_t, 3> modes = {})
      : owner_(game && active == nullptr) {
    raw_entries.fetch_add(1, std::memory_order_relaxed);
    if (!game) internal_entries.fetch_add(1, std::memory_order_relaxed);
    else if (!owner_) nested_entries.fetch_add(1, std::memory_order_relaxed);
    if (!owner_) return;
    record_ = &current_record;
    *record_ = {};
    record_->handle = reinterpret_cast<uintptr_t>(handle);
    record_->enabled = enabled;
    record_->before_selected = before;
    record_->route = route;
    record_->modes = modes;
    writers.fetch_add(1);
    record_->sequence = started.fetch_add(1) + 1;
    in_flight.fetch_add(1);
    revision.fetch_add(1);
    writers.fetch_sub(1);
    active = record_;
  }
  ~Scope() {
    if (!owner_) return;
    active = nullptr;
    record_->completed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    writers.fetch_add(1);
    Entry& entry = history[(record_->sequence - 1) % kCapacity];
    if (!entry.busy.test_and_set(std::memory_order_acquire)) {
      // A slow old evaluation must not displace a newer completed record.
      if (entry.record.sequence < record_->sequence) {
        if (entry.record.sequence != 0) overwritten.fetch_add(1);
        entry.record = *record_;
        uint64_t latest = latest_sequence.load(std::memory_order_relaxed);
        while (latest < record_->sequence && !latest_sequence.compare_exchange_weak(
            latest, record_->sequence, std::memory_order_relaxed)) {}
        if (record_->eligibility == 1 || record_->attempted) {
          latest = latest_relevant_sequence.load(std::memory_order_relaxed);
          while (latest < record_->sequence && !latest_relevant_sequence.compare_exchange_weak(
              latest, record_->sequence, std::memory_order_relaxed)) {}
        }
      } else {
        detail_dropped.fetch_add(1);
      }
      entry.busy.clear(std::memory_order_release);
    } else {
      detail_dropped.fetch_add(1);
    }
    totals[static_cast<size_t>(record_->outcome)].fetch_add(1);
    eligibility_totals[record_->eligibility].fetch_add(1);
    finished.fetch_add(1);
    in_flight.fetch_sub(1);
    revision.fetch_add(1);
    writers.fetch_sub(1);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
 private:
  bool owner_;
  Record* record_ = nullptr;
};

// One slot, one try: a contended or overwritten recent record is unavailable.
inline bool ReadLatest(Record* result, bool relevant = true) {
  const uint64_t sequence = (relevant ? latest_relevant_sequence : latest_sequence).load(std::memory_order_relaxed);
  if (sequence == 0) return false;
  Entry& entry = history[(sequence - 1) % kCapacity];
  if (entry.busy.test_and_set(std::memory_order_acquire)) return false;
  const bool matches = entry.record.sequence == sequence;
  if (matches) *result = entry.record;
  entry.busy.clear(std::memory_order_release);
  return matches;
}

inline std::string TotalsJson(const Totals& value) {
  std::ostringstream out;
  out << "{\"started\":" << value.started << ",\"finished\":" << value.finished
      << ",\"in_flight\":" << value.in_flight << ",\"coherent\":" << (value.coherent ? "true" : "false")
      << ",\"overwritten\":" << value.overwritten << ",\"detail_dropped\":" << value.dropped
      << ",\"eligibility\":{\"unknown\":" << value.eligibility[0] << ",\"dlss_rr\":" << value.eligibility[1]
      << ",\"excluded\":" << value.eligibility[2] << "},\"outcomes\":{";
  for (size_t i = 0; i < value.outcomes.size(); ++i) {
    if (i) out << ',';
    out << '"' << kOutcomeNames[i] << "\":" << value.outcomes[i];
  }
  return out.str() + "}}";
}

// Called by the report worker, never the render path. A busy detail slot is
// reported as omitted; it does not affect the accounting totals above.
inline std::string HistoryJson() {
  std::vector<Record> records;
  records.reserve(kCapacity);
  uint32_t busy = 0;
  for (Entry& entry : history) {
    if (entry.busy.test_and_set(std::memory_order_acquire)) { ++busy; continue; }
    if (entry.record.sequence != 0) records.push_back(entry.record);
    entry.busy.clear(std::memory_order_release);
  }
  std::ostringstream out;
  out << "{\"schema\":1,\"capacity\":" << kCapacity << ",\"busy_slots\":" << busy
      << ",\"snapshot_atomic\":false,\"raw_entries\":" << raw_entries.load()
      << ",\"nested_entries\":" << nested_entries.load() << ",\"internal_entries\":" << internal_entries.load()
      << ",\"totals\":" << TotalsJson(ReadTotals()) << ",\"records\":[";
  bool first = true;
  for (const Record& r : records) {
    if (!first) out << ',';
    first = false;
    out << "{\"sequence\":" << r.sequence << ",\"handle\":" << r.handle
        << ",\"generation\":" << r.generation << ",\"device_generation\":" << r.device_generation
        << ",\"completed_ns\":" << r.completed_ns << ",\"feature\":" << r.feature
        << ",\"source_api\":" << unsigned(r.source_api) << ",\"eligibility\":" << unsigned(r.eligibility) << ",\"attempted\":" << r.attempted
        << ",\"modes\":[" << r.modes[0] << "," << r.modes[1] << "," << r.modes[2] << "]"
        << ",\"create_seen\":" << r.create_seen << ",\"before_selected\":" << r.before_selected
        << ",\"enabled\":" << r.enabled << ",\"route\":\"" << kRouteNames[static_cast<size_t>(r.route)]
        << "\",\"outcome\":\"" << kOutcomeNames[static_cast<size_t>(r.outcome)]
        << "\",\"decline_id\":" << r.decline << ",\"missing_inputs\":" << r.missing_inputs
        << ",\"game_returned\":" << r.game_returned << ",\"game_result\":" << r.game_result
        << ",\"nr_result\":" << r.nr_result << ",\"commands_recorded\":" << r.recorded
        << ",\"bridge_reused\":" << r.bridge_reused
        << ",\"nr_calls\":" << r.nr_calls
        << ",\"bridge_submitted\":" << r.bridge_submitted
        << ",\"bridge_submission\":" << r.bridge_submission
        << ",\"bridge_completed_before\":" << r.bridge_completed_before
        << ",\"bridge_role\":" << r.bridge_role << ",\"bridge_hr\":" << r.bridge_hr
        << ",\"bridge_step\":\"" << r.bridge_step << "\"";
    out << ",\"queue_mask\":" << r.queue_mask << ",\"queue_results\":[";
    for (size_t i = 0; i < r.queue_results.size(); ++i) { if (i) out << ","; out << r.queue_results[i]; }
    out << "]";
    for (unsigned evidence = 0; evidence < 2; ++evidence) {
    out << (evidence ? ",\"captured\":{" : ",\"raw\":{");
    const auto& scalars = evidence ? r.captured : r.raw;
    for (size_t i = 0; i < r.raw.size(); ++i) {
      if (i) out << ',';
      out << '"' << kScalarNames[i] << "\":{\"present\":" << scalars[i].present
          << ",\"query_result\":" << scalars[i].result << ",\"finite\":" << std::isfinite(scalars[i].value)
          << ",\"value\":";
      if (std::isfinite(scalars[i].value)) out << scalars[i].value; else out << "null";
      out << '}';
    }
    out << "}";
    }
    out << ",\"resolved\":[";
    for (size_t i = 0; i < r.resolved.size(); ++i) { if (i) out << ','; out << r.resolved[i]; }
    out << "],\"motion_scale\":[";
    if (std::isfinite(r.scale_x)) out << r.scale_x; else out << "null";
    out << ',';
    if (std::isfinite(r.scale_y)) out << r.scale_y; else out << "null";
    out << "],\"bridge_steps\":[";
    for (size_t i = 0; i < r.bridge.size(); ++i) {
      if (i) out << ',';
      const auto& d = r.bridge[i];
      out << "{\"calls\":" << d.calls << ",\"failures\":" << d.failures
          << ",\"first_hr\":" << d.first_hr << ",\"last_hr\":" << d.last_hr
          << ",\"format\":" << d.format << ",\"bind\":" << d.bind
          << ",\"support1\":" << d.support1 << ",\"support2\":" << d.support2
          << ",\"required1\":" << d.required1 << ",\"required2\":" << d.required2
          << ",\"condition\":\"" << d.condition << "\""
          << ",\"first_step\":\"" << d.first_step << "\",\"last_step\":\"" << d.last_step << "\"}";
    }
    out << ']';
    for (unsigned group = 0; group < 2; ++group) {
      out << (group ? ",\"transport\":[" : ",\"source\":[");
      const auto& resources = group ? r.transport : r.source;
      for (size_t i = 0; i < resources.size(); ++i) {
        if (i) out << ',';
        const Resource& d = resources[i];
        out << "{\"width\":" << d.width << ",\"height\":" << d.height << ",\"format\":" << d.format
            << ",\"flags\":" << d.flags << ",\"mips\":" << d.mips << ",\"layers\":" << d.layers
            << ",\"samples\":" << d.samples << ",\"dimension\":" << d.dimension << '}';
      }
      out << ']';
    }
    out << '}';
  }
  return out.str() + "]}";
}
}  // namespace renodx::addons::dlss5::evaluation

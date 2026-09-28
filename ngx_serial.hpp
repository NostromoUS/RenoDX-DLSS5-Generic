/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// One NGX call at a time, process-wide, while the Present hook point serves.
//
// nvsdk_ngx.h:18-19: "IMPORTANT: Methods in this library are NOT thread
// safe. It is up to the client to ensure that thread safety is enforced as
// needed."  On the Upscaled and Render hook points NR's NGX calls run inside
// the game's evaluate, on the game's thread, so they never race the game's.
// The Present hook point creates, evaluates and releases NR on the present
// thread while the game's own NGX calls (its DLSS evaluate, Streamline's,
// its creates and releases) run on others.  Code Vein II (v8.0.1) created
// feature 18 on present thread 4996 while render thread 18248 was inside
// its evaluate, and the process died within milliseconds.
//
// Every NGX call that passes through the addon's detours (the game's and the
// addon's own) opens a Call; the Present path takes a PresentTurn around its
// NGX work.  Both lock one recursive timed mutex, so a plugin->core chain or
// Streamline's evaluate that nests on one thread re-enters freely.  Calls
// lock while the Present hook point or a retirement turn is armed. Upscaled
// and Render otherwise run exactly as before, unlocked.
//
// Every wait is bounded, so no lock order can deadlock.  The present side
// waits kPresentWait and skips NR for that present: ReShade holds its present
// queue's lock during the present event (TIME-06), and a DLSS-G evaluate may
// itself wait on the present thread.  The game side waits kGameWait, long
// enough to cover NR's feature create (the Code Vein moment), and then runs
// unserialized, counted.
//
// Dependency-free on purpose (only the standard library), like
// direct_call.hpp.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

namespace renodx::addons::dlss5::ngx_serial {

// The present side: a present's NR work runs about 0.3 ms of CPU (the
// evaluate's recording, telemetry nr[cpu_us_avg]); 4 ms covers a game
// evaluate in flight on another thread several times over and stays under a
// quarter of a 60 Hz frame, which is what a skipped present may cost.
inline constexpr auto kPresentWait = std::chrono::milliseconds(4);
// The game side: NR's feature create on the present thread took 358 ms in
// the harness's first Present frame (telemetry nr[cpu_us_max=358507]); 750 ms
// covers it with margin, and is still a bounded hitch if a wait order ever
// closes a cycle.
inline constexpr auto kGameWait = std::chrono::milliseconds(750);

inline std::recursive_timed_mutex mutex;
// The Present hook point is selected: game calls lock.
inline std::atomic_bool armed{false};
// An explicit off cleanup also calls NGX from the present thread. Keep its
// reservation independent from the selected hook point until cleanup drains.
inline std::atomic_bool retirement_armed{false};
// Transient retirement turns compose with each other and both persistent
// arms: one failed/finished turn cannot disarm another thread's cleanup.
inline std::atomic_uint32_t forced_turns{0};
// Game calls not yet protected by the lock. Raised BEFORE `armed` is read,
// and held through a failed lock attempt until the call leaves, so a present
// that reads 0 after arming has seen every unserialized call.
inline std::atomic_uint32_t unlocked_in_flight{0};
// Threads inside an NGX call right now.
inline std::atomic_uint32_t inside{0};
// Outermost NGX calls that began while another thread was inside one.
inline std::atomic_uint64_t overlap{0};
// Game calls that waited kGameWait and ran unserialized.
inline std::atomic_uint64_t serial_timeouts{0};
// Presents that skipped NR: the lock stayed held kPresentWait, or a call
// that entered unlocked had not drained.
inline std::atomic_uint64_t present_busy{0};
// Presents whose turn released the lock at least once for a reservation
// and retried (v8.5), whether or not they then owned it.
inline std::atomic_uint64_t present_retried{0};

inline thread_local uint32_t depth = 0;

// Around one NGX call that passes through a detour.  Nested calls on the
// same thread (a chain, the addon's own call inside a Present turn) only
// count depth.
class Call {
 public:
  Call() noexcept {
    if (depth++ != 0) return;
    outermost_ = true;
    unlocked_in_flight.fetch_add(1);
    unlocked_ = true;
    if (armed.load() || retirement_armed.load() || forced_turns.load() != 0) {
      locked_ = mutex.try_lock_for(kGameWait);
      if (locked_) {
        unlocked_in_flight.fetch_sub(1);
        unlocked_ = false;
      } else {
        // Keep the reservation until destruction. Adding it only AFTER a
        // timeout would leave a window for PresentTurn to observe zero and
        // enter NGX alongside the call that just gave up waiting.
        serial_timeouts.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (inside.fetch_add(1) != 0) overlap.fetch_add(1, std::memory_order_relaxed);
  }
  ~Call() {
    --depth;
    if (!outermost_) return;
    inside.fetch_sub(1);
    if (locked_) mutex.unlock();
    if (unlocked_) unlocked_in_flight.fetch_sub(1);
  }
  Call(const Call&) = delete;
  Call& operator=(const Call&) = delete;

 private:
  bool outermost_ = false;
  bool locked_ = false;
  bool unlocked_ = false;
};

// A detour's pass-through to the real export, called like it, inside a Call.
template <typename Fn>
auto Serialized(Fn* real) noexcept {
  return [real](auto&&... arguments) {
    const Call call;
    return real(std::forward<decltype(arguments)>(arguments)...);
  };
}

// The Present path's hold around its NGX work.  owns() false: skip NR for
// this present (counted in present_busy); the next present tries again.
// A forced turn arms game-call serialization for its entire lifetime, even
// if it cannot acquire the lock or is waiting for unprotected calls to drain.
class PresentTurn {
 public:
  explicit PresentTurn(bool force = false) noexcept : forced_(force) {
    if (forced_) forced_turns.fetch_add(1);
    const auto deadline = std::chrono::steady_clock::now() + kPresentWait;
    bool retried = false;
    do {
      if (!mutex.try_lock_until(deadline)) break;
      if (unlocked_in_flight.load() == 0) {
        owns_ = true;
        if (retried) present_retried.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      // A reservation can also belong to an armed game call waiting for
      // this mutex. Release it so that call can enter and clear its
      // reservation, then retry within the SAME total wait budget. Failing
      // immediately here turned normal serialized contention into raw
      // presents. Truly unprotected calls still have to drain first, and
      // their reservation remains intact through the game's timeout.
      mutex.unlock();
      retried = true;
      std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    if (retried) present_retried.fetch_add(1, std::memory_order_relaxed);
    present_busy.fetch_add(1, std::memory_order_relaxed);
  }
  ~PresentTurn() {
    if (owns_) mutex.unlock();
    if (forced_) forced_turns.fetch_sub(1);
  }
  PresentTurn(const PresentTurn&) = delete;
  PresentTurn& operator=(const PresentTurn&) = delete;
  bool owns() const noexcept { return owns_; }

 private:
  bool owns_ = false;
  const bool forced_;
};

}  // namespace renodx::addons::dlss5::ngx_serial

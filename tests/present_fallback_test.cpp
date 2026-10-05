#include <cassert>
#include <cstdint>
#include <iostream>
#include <latch>
#include <optional>
#include <thread>

#include "../ngx_serial.hpp"
#include "../present_fallback.hpp"

using renodx::addons::dlss5::PresentFallback;

int main() {
  int swapchain = 0;
  int device = 0;
  const PresentFallback::Surface surface{
      .swapchain = &swapchain,
      .device = &device,
      .width = 3840,
      .height = 2160,
      .format = 28,
      .buffers = 3,
  };

  // Continuous SR and multiple/nested evaluations never admit Present.
  PresentFallback state;
  for (uint32_t frame = 0; frame < 120; ++frame) {
    state.ObserveEvaluate();
    state.ObserveEvaluate();
    assert(!state.ObservePresent(surface, frame % 3, true));
  }

  // Cinematic entry cannot advance while an inline recording remains
  // unsubmitted, executing, or replayable. Elapsed time does not bypass it.
  for (uint32_t frame = 0; frame < 120; ++frame) {
    assert(!state.ObservePresent(surface, frame % 3, false));
  }
  assert(!state.ObservePresent(surface, 0, true));
  for (uint32_t repeat = 0; repeat < 10; ++repeat) {
    assert(!state.ObservePresent(surface, 0, true));
  }
  assert(!state.ObservePresent(surface, 1, true));
  assert(!state.ObservePresent(surface, 2, true));
  assert(state.ObservePresent(surface, 0, true));
  const auto cinematic_ticket = state.revision;
  assert(state.IsCurrent(surface, cinematic_ticket));
  state.active = true;
  for (uint32_t frame = 0; frame < 120; ++frame) {
    assert(state.ObservePresent(surface, frame % 3, true));
    assert(state.IsCurrent(surface, cinematic_ticket));
  }

  // SR resuming during the initial NGX-turn wait invalidates the ticket and
  // requests one history transition, including when inline NR then declines.
  assert(state.ObserveEvaluate());
  assert(!state.IsCurrent(surface, cinematic_ticket));
  assert(!state.active);
  assert(!state.ObserveEvaluate());
  assert(!state.ObservePresent(surface, 1, true));
  assert(!state.ObservePresent(surface, 2, true));
  assert(!state.ObservePresent(surface, 0, true));
  assert(state.ObservePresent(surface, 1, true));

  // A second check after releasing locks for a GPU slot wait must also
  // reject an intervening evaluate, rather than reuse the initial decision.
  const auto slot_wait_ticket = state.revision;
  state.ObserveEvaluate();
  assert(!state.IsCurrent(surface, slot_wait_ticket));

  // Startup in a menu works without any earlier SR evaluation.
  PresentFallback menu;
  assert(!menu.ObservePresent(surface, 0, true));
  assert(!menu.ObservePresent(surface, 1, true));
  assert(!menu.ObservePresent(surface, 2, true));
  assert(menu.ObservePresent(surface, 0, true));
  const auto old_surface_ticket = menu.revision;

  // Resize, format and buffer-count changes require a fresh drain cycle.
  auto resized = surface;
  resized.width = 1920;
  resized.height = 1080;
  resized.format = 24;
  resized.buffers = 2;
  assert(!menu.ObservePresent(resized, 0, true));
  assert(!menu.IsCurrent(surface, old_surface_ticket));
  assert(!menu.ObservePresent(resized, 1, true));
  assert(menu.ObservePresent(resized, 0, true));
  const auto resized_ticket = menu.revision;
  menu.active = true;
  assert(menu.Reset());
  assert(!menu.IsCurrent(resized, resized_ticket));
  assert(!menu.Reset());

  // Multiple windows/devices are ambiguous, not a last-present-wins choice.
  assert(!menu.ObservePresent(surface, 0, true));
  int another_swapchain = 0;
  auto other = surface;
  other.swapchain = &another_swapchain;
  assert(!menu.ObservePresent(other, 1, true));
  for (uint32_t frame = 0; frame < 120; ++frame) {
    assert(!menu.ObservePresent(surface, frame % 3, true));
  }
  menu.Reset();
  assert(!menu.ObservePresent(surface, 0, true));
  int another_device = 0;
  other = surface;
  other.device = &another_device;
  assert(!menu.ObservePresent(other, 1, true));
  assert(menu.ambiguous);

  // Bounds must fail closed and avoid undefined shifts, including 64 buffers.
  PresentFallback bounds;
  auto invalid = surface;
  invalid.buffers = 0;
  assert(!bounds.ObservePresent(invalid, 0, true));
  invalid.buffers = 65;
  assert(!bounds.ObservePresent(invalid, 0, true));
  invalid.buffers = 64;
  assert(!bounds.ObservePresent(invalid, 64, true));
  for (uint32_t index = 0; index < 64; ++index) {
    assert(!bounds.ObservePresent(invalid, index, true));
  }
  assert(bounds.ObservePresent(invalid, 0, true));
  assert(!bounds.ObservePresent(invalid, 0, false));
  assert(!bounds.ObservePresent(invalid, 0, true));

  // Exercise the actual serial guard used by RunPresentPath: a forced turn
  // excludes a resumed game call even before the normal present arm is set.
  namespace serial = renodx::addons::dlss5::ngx_serial;
  serial::armed.store(false);
  std::optional<serial::PresentTurn> turn(std::in_place, true);
  assert(turn->owns());
  const auto before_wait = state.revision;
  std::latch started(1);
  std::latch completed(1);
  std::thread evaluate([&] {
    started.count_down();
    const serial::Call call;
    state.ObserveEvaluate();
    completed.count_down();
  });
  started.wait();
  assert(!completed.try_wait());
  // RunPresentPath releases both locks while waiting for its GPU ring slot.
  turn.reset();
  completed.wait();
  evaluate.join();
  turn.emplace(true);
  assert(turn->owns());
  assert(!state.IsCurrent(surface, before_wait));
  turn.reset();

  // A call already running without serialization must drain first. The
  // bounded Present wait skips rather than entering NGX concurrently.
  std::latch entered(1);
  std::latch leave(1);
  std::thread unarmed_evaluate([&] {
    const serial::Call call;
    entered.count_down();
    leave.wait();
  });
  entered.wait();
  turn.emplace(true);
  assert(!turn->owns());
  turn.reset();
  leave.count_down();
  unarmed_evaluate.join();
  turn.emplace(true);
  assert(turn->owns());
  turn.reset();
  assert(serial::forced_turns.load() == 0);
  assert(serial::unlocked_in_flight.load() == 0);

  std::cout << "Present fallback routing tests passed\n";
}

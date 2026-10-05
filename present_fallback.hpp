/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace renodx::addons::dlss5 {

// CPU routing only. The caller holds runtime_mutex and supplies an exact
// closed-recording/GPU-completion proof before advancing the drain cycle.
// This does not infer which displayed image an arbitrary worker evaluate
// produced. A cached enhanced image copied again later is not detectable here.
struct PresentFallback {
  struct Surface {
    const void* swapchain = nullptr;
    const void* device = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t buffers = 0;
    bool operator==(const Surface&) const = default;
  };

  Surface owner;
  uint64_t revision = 1;
  uint64_t drained_buffers = 0;
  bool ambiguous = false;
  bool active = false;

  void Invalidate() {
    ++revision;
    drained_buffers = 0;
  }

  // Return whether a temporal path change needs a history reset.
  bool ObserveEvaluate() {
    Invalidate();
    const bool switched = active;
    active = false;
    return switched;
  }

  bool Reset() {
    const bool switched = ObserveEvaluate();
    owner = {};
    ambiguous = false;
    return switched;
  }

  bool ObservePresent(const Surface& surface, uint32_t index, bool recordings_complete) {
    if (owner.swapchain != nullptr
        && (owner.swapchain != surface.swapchain || owner.device != surface.device)) {
      // No output-to-swapchain provenance: never guess between windows/devices.
      ambiguous = true;
    }
    if (!(owner == surface)) {
      owner = surface;
      Invalidate();
    }
    if (ambiguous || surface.swapchain == nullptr || surface.device == nullptr
        || surface.width == 0 || surface.height == 0
        || surface.buffers == 0 || surface.buffers > 64 || index >= surface.buffers
        || !recordings_complete) {
      Invalidate();
      return false;
    }
    const uint64_t all = (surface.buffers == 64 ? UINT64_MAX : ((uint64_t{1} << surface.buffers) - 1));
    if (drained_buffers == all) return true;
    drained_buffers |= uint64_t{1} << index;
    // The final buffer in the drain also passes through unchanged.
    return false;
  }

  bool IsCurrent(const Surface& surface, uint64_t ticket) const {
    return !ambiguous && owner == surface && revision == ticket;
  }
};

}  // namespace renodx::addons::dlss5

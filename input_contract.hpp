/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <nvsdk_ngx.h>

#include "evaluation_ledger.hpp"

namespace renodx::addons::dlss5::contract {

enum Key : size_t {
  Width, Height, OutWidth, OutHeight, RenderWidth, RenderHeight,
  ColorX, ColorY, MotionX, MotionY, DepthX, DepthY, OutputX, OutputY,
  Flags, OutputSubrects, ScaleX, ScaleY, JitterX, JitterY,
  PreExposure, ExposureScale, Reset, Count
};
using Evidence = std::array<evaluation::Scalar, Count>;
inline constexpr const char* kKeys[] = {
    NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
    NVSDK_NGX_Parameter_OutWidth, NVSDK_NGX_Parameter_OutHeight,
    NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
    NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
    NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
    NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
    NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
    NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
    NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
    NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
    NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
    NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y,
    NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
    NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,
    NVSDK_NGX_Parameter_MV_Scale_X, NVSDK_NGX_Parameter_MV_Scale_Y,
    NVSDK_NGX_Parameter_Jitter_Offset_X, NVSDK_NGX_Parameter_Jitter_Offset_Y,
    NVSDK_NGX_Parameter_DLSS_Pre_Exposure, NVSDK_NGX_Parameter_DLSS_Exposure_Scale,
    NVSDK_NGX_Parameter_Reset};
static_assert(Count == std::size(evaluation::kScalarNames));

inline Evidence Read(const NVSDK_NGX_Parameter* parameters) {
  Evidence result{};
  if (parameters == nullptr) return result;
  for (size_t i = 0; i != Count; ++i) {
    NVSDK_NGX_Result status;
    if (i >= ScaleX && i <= ExposureScale) {
      float value = 0;
      status = parameters->Get(kKeys[i], &value);
      result[i].value = value;
    } else if (i == Flags || i == OutputSubrects || i == Reset) {
      int value = 0;
      status = parameters->Get(kKeys[i], &value);
      result[i].value = static_cast<uint32_t>(value);
    } else {
      unsigned value = 0;
      status = parameters->Get(kKeys[i], &value);
      result[i].value = value;
    }
    result[i].present = NVSDK_NGX_SUCCEED(status);
    result[i].result = static_cast<uint32_t>(status);
  }
  return result;
}
inline uint32_t UInt(const Evidence& source, Key key) {
  return static_cast<uint32_t>(source[key].value);
}
struct Rect {
  uint32_t x = 0, y = 0, width = 0, height = 0;
  bool Fits(uint64_t w, uint32_t h) const {
    return width != 0 && height != 0 && x <= w && y <= h
        && width <= w - x && height <= h - y;
  }
  bool operator==(const Rect&) const = default;
};
struct Resolved {
  uint32_t width = 0, height = 0, flags = 0;
  uint32_t motion_x = 0, motion_y = 0, depth_x = 0, depth_y = 0;
  uint32_t motion_width = 0, motion_height = 0;
  float scale_x = 0, scale_y = 0;
  Rect output, color;
  // Observations, not new admission gates: invalid output/Color, MV mismatch,
  // nonfinite scale, and unknown flags respectively.
  uint32_t notes = 0, flags_source = 0, scale_sources = 0;
  uint32_t size_sources = 0, base_sources = 0, output_source = 0, color_source = 0;
};

// Guide mode: 0 Auto, 1 Legacy, 2 Declared.
// Rect modes: 0 Auto, 1 Declared, 2 Resource (the v7.5 interpretation).
// The public NGX helper changes zero scale arguments to 1. Missing raw keys
// do not prove units, so Auto retains the legacy dimensional default for them.
// Feature 18 consumes the resulting scale in pixels of its MVec window; it
// must not be scaled again with the independent NR working resolution.
inline Resolved Resolve(const Evidence& eval, const Evidence& captured, bool create_seen,
                        const Resolved& legacy, const evaluation::Resource& color,
                        const evaluation::Resource& output, const evaluation::Resource& motion,
                        uint32_t guide_mode, uint32_t output_mode, uint32_t color_mode) {
  Resolved r = legacy;
  const bool corrected = guide_mode != 1;
  for (unsigned axis = 0; axis != 2; ++axis) {
    uint32_t& extent = axis ? r.height : r.width;
    const auto render = static_cast<Key>(RenderWidth + axis);
    const auto size = static_cast<Key>(Width + axis);
    uint32_t size_source = 0;
    if (eval[render].present && UInt(eval, render) != 0) { extent = UInt(eval, render); size_source = 1; }
    else if (eval[size].present && UInt(eval, size) != 0) { extent = UInt(eval, size); size_source = 2; }
    else if (captured[size].present && UInt(captured, size) != 0) { extent = UInt(captured, size); size_source = create_seen ? 3 : 4; }
    else if (captured[render].present && UInt(captured, render) != 0) { extent = UInt(captured, render); size_source = create_seen ? 3 : 4; }
    if (extent == 0) { extent = axis ? color.height : static_cast<uint32_t>(color.width); size_source = 5; }
    r.size_sources |= size_source << (axis * 4);
    if (corrected) {
      for (unsigned guide = 0; guide != 2; ++guide) {
        const auto base = static_cast<Key>(MotionX + guide * 2 + axis);
        uint32_t& value = guide ? (axis ? r.depth_y : r.depth_x) : (axis ? r.motion_y : r.motion_x);
        uint32_t source = 0;
        if (eval[base].present) { value = UInt(eval, base); source = 1; }
        else if (captured[base].present) { value = UInt(captured, base); source = create_seen ? 2 : 3; }
        r.base_sources |= source << ((guide * 2 + axis) * 2);
      }
    }
    const auto scale = static_cast<Key>(ScaleX + axis);
    float& value = axis ? r.scale_y : r.scale_x;
    uint32_t source = 0;  // 0 legacy fallback, 1 supplied, 2 unit default
    if (eval[scale].present) {
      value = static_cast<float>(eval[scale].value);
      source = 1;
      if (!std::isfinite(value)) r.notes |= 8;
      if (corrected && (!std::isfinite(value) || value == 0.f)) { value = 1.f; source = 2; }
    } else {
      value = guide_mode == 2 ? 1.f : static_cast<float>(extent);
      source = guide_mode == 2 ? 2 : 0;
    }
    r.scale_sources |= source << (axis * 2);
  }
  bool flags_known = false;
  if (corrected && create_seen && captured[Flags].present) {
    r.flags = UInt(captured, Flags);
    r.flags_source = 1;
    flags_known = true;
  } else if (eval[Flags].present) {
    r.flags = UInt(eval, Flags);
    r.flags_source = 2;
    flags_known = true;
  }
  if (!flags_known) r.notes |= 16;
  r.output = {0, 0, static_cast<uint32_t>(output.width), output.height};
  const bool subrects = create_seen && captured[OutputSubrects].present
      ? UInt(captured, OutputSubrects) != 0
      : eval[OutputSubrects].present && UInt(eval, OutputSubrects) != 0;
  if (output_mode != 2 && (subrects || output_mode == 1)) {
    Rect rect{};
    uint32_t source = 0;
    rect.x = eval[OutputX].present ? UInt(eval, OutputX) : 0;
    rect.y = eval[OutputY].present ? UInt(eval, OutputY) : 0;
    if (output_mode == 0 && create_seen && captured[OutWidth].present && captured[OutHeight].present) {
      rect.width = UInt(captured, OutWidth);
      rect.height = UInt(captured, OutHeight);
      source = 1;
    } else if (eval[OutWidth].present && eval[OutHeight].present) {
      rect.width = UInt(eval, OutWidth);
      rect.height = UInt(eval, OutHeight);
      source = 2;
    }
    if (rect.Fits(output.width, output.height)) { r.output = rect; r.output_source = source; }
    else r.notes |= 1;
  }
  r.color = {0, 0, static_cast<uint32_t>(color.width), color.height};
  if (color_mode != 2 && ((eval[RenderWidth].present && eval[RenderHeight].present) || color_mode == 1)) {
    const Rect rect{eval[ColorX].present ? UInt(eval, ColorX) : 0,
                    eval[ColorY].present ? UInt(eval, ColorY) : 0,
                    eval[RenderWidth].present ? UInt(eval, RenderWidth) : r.width,
                    eval[RenderHeight].present ? UInt(eval, RenderHeight) : r.height};
    if (rect.Fits(color.width, color.height)) { r.color = rect; r.color_source = 1; }
    else r.notes |= 2;
  }
  r.motion_width = r.width;
  r.motion_height = r.height;
  if (corrected && flags_known && (r.flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) == 0) {
    const Rect window{r.motion_x, r.motion_y, r.output.width, r.output.height};
    if (window.Fits(motion.width, motion.height)) {
      r.motion_width = window.width;
      r.motion_height = window.height;
    } else {
      r.notes |= 4;  // Keep the existing render grid; observe the contradiction.
    }
  }
  return r;
}
}  // namespace renodx::addons::dlss5::contract

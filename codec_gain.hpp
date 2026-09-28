#pragma once

// Host-side constants for the DLSS5 proxy codec.  Keep this file limited to
// source-unit interpretation and deterministic divisor math.  Relative-HDR
// scene statistics live on the GPU in v6_autoscale.cs_5_1.hlsl; there is no
// CPU normalization/health state machine.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace renodx::dlss5::codec {

inline constexpr float kProxyShoulder = 0.75f;
inline constexpr float kRelativeProxyMedian = 0.30f;
inline constexpr float kLegacyPaperWhite = 2.5375f;

enum class Encoding : uint8_t {
  Sdr = 0,
  Linear = 1,
  Pq = 2,
};

struct SourceUnits {
  Encoding encoding = Encoding::Linear;
  bool absolute = false;
  float unit_nits = 0.f;
};

// An explicit content override selects the pipeline as well as its units.
// Otherwise preserve the existing format + NGX HDR-flag inference, including
// ten-bit SDR. Merely changing SourceUnits after allocation left forced PQ on
// the SDR codec and forced SDR on the HDR codec (v7.6 field/settings audit).
inline uint8_t SourceHdrMode(uint8_t format_mode, bool hdr_declared, uint32_t override_encoding) {
  switch (override_encoding) {
    case 1: return 0;  // SDR
    case 2: return 1;  // Linear
    case 3: return 2;  // PQ
    default: return hdr_declared ? std::max<uint8_t>(format_mode, 1)
                                : format_mode == 2 ? 0 : format_mode;
  }
}

// Stored modes 0/1 retain v7.5's Auto/Always behavior. New modes: 2 Off,
// 3 Auto (v7.6), 4 On with independent strength. A positive block-mean lift
// is not proof that every dark pixel needs subtraction: the PQ GPU case
// creates black pixels from unchanged 0.1-nit originals. Auto skips that
// operation on proven PQ units; ambiguous/other sources keep the old rule.
inline bool PedestalAllowed(uint32_t mode, bool display, const SourceUnits& units) {
  if (mode == 2) return false;
  if (mode == 1 || mode == 4) return true;
  if (mode == 3 && units.encoding == Encoding::Pq && units.absolute) return false;
  return !display || (units.absolute && units.unit_nits > 0.f);
}

inline float ClassicLinearDivisor(float paper_white_scale) {
  return std::max(0.0001f, paper_white_scale);
}

inline float ClassicPqDivisor(
    float diffuse_white_nits,
    float paper_white_scale,
    float pq_calibration) {
  const float legacy_ratio = paper_white_scale / kLegacyPaperWhite;
  return std::max(
      1e-8f,
      (std::max(1.f, diffuse_white_nits) / 10000.f)
          * std::max(0.005f, legacy_ratio)
          * std::max(0.005f, pq_calibration));
}

inline float AnchoredDivisor(float anchor_nits, const SourceUnits& units) {
  if (!units.absolute || !(units.unit_nits > 0.f)) return 0.f;
  return std::max(
      1e-8f,
      std::max(0.02f, anchor_nits) / (kProxyShoulder * units.unit_nits));
}

// CPU reference for the one-line scale contract implemented by the autoscale
// shader.  The shader obtains median_linear from the current frame itself.
inline float RelativeFrameDivisor(float median_linear) {
  if (!(median_linear > 0.f) || !std::isfinite(median_linear)) return 1.f;
  return std::max(1e-8f, median_linear / kRelativeProxyMedian);
}

inline float BlackPedestalCap(const SourceUnits& units) {
  if (units.absolute && units.unit_nits > 0.f) return 2.f / units.unit_nits;
  return 0.01f;
}

inline float DisplayAnchorLinear(
    const SourceUnits& units,
    float anchor_nits,
    float diffuse_white_nits) {
  if (units.encoding == Encoding::Pq) {
    return std::max(1e-6f, std::max(1.f, diffuse_white_nits) / 10000.f);
  }
  if (units.absolute && units.unit_nits > 0.f) {
    return std::max(
        1e-6f, std::max(0.02f, anchor_nits) / units.unit_nits);
  }
  return 1.f;
}

// Raw NGX exposure evidence is diagnostic metadata only.  It is deliberately
// not a normalization input in v6; relative HDR derives scale on the GPU from
// the exact frame entering NR.
struct ExposureEvidence {
  bool auto_exposure = false;
  bool has_texture = false;
  uint32_t texture_format = 0;
  uint32_t texture_width = 0;
  uint32_t texture_height = 0;
  bool has_scale = false;
  float scale = 1.f;
  bool has_pre_exposure = false;
  float pre_exposure = 1.f;
};

}  // namespace renodx::dlss5::codec

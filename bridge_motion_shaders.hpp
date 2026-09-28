/* Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace renodx::addons::dlss5 {
// The RGBA32F route keeps ordinary finite R32G32_FLOAT motion components at
// binary32 precision; shader float load/store does not promise bitwise NaN
// payload or denormal preservation.
inline constexpr char kBridgeMotionToTransportShader[] = R"HLSL(
Texture2D<float2> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  uint width, height;
  Destination.GetDimensions(width, height);
  if (dispatch_id.x >= width || dispatch_id.y >= height) return;
  Destination[dispatch_id.xy] = float4(Source.Load(int3(dispatch_id.xy, 0)), 0.0, 0.0);
}
)HLSL";

inline constexpr char kBridgeMotionFromTransportShader[] = R"HLSL(
Texture2D<float4> Source : register(t0);
RWTexture2D<float2> Destination : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  uint width, height;
  Destination.GetDimensions(width, height);
  if (dispatch_id.x >= width || dispatch_id.y >= height) return;
  Destination[dispatch_id.xy] = Source.Load(int3(dispatch_id.xy, 0)).xy;
}
)HLSL";

}  // namespace renodx::addons::dlss5

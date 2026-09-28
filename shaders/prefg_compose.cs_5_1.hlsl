// The Present hook point before frame generation (present_path.hpp,
// RunPresentPreFg, v8.1.0): the real frame DLSS-G shows, recomposed from
// NR's HUD-less image with the game's HUD kept bit for bit, so one NR pass
// per game frame serves both the frame DLSS-G interpolates from (the HUD-less
// stand-in) and the real frame (this output, copied over DLSSG.Backbuffer).
// Every value is in the buffers' encoded domain, where the game composed its
// UI and where DLSS-G applies the same blend.
//
// UI alpha (Mode 0: a single-channel DLSSG.UIAlpha, .r) or alpha in .a
// (Mode 1: DLSSG.UI's colour and alpha, or a UIAlpha with more channels).  DLSS-G's contract (Streamline ProgrammingGuideDLSS_G.md,
// 5.1): Back.rgb = UI.rgb + (1 - a) * Hudless.rgb, with UI.rgb premultiplied
// by a, and DLSS-G itself recomposes Final = UI.rgb + (1 - a) * Hudless'.
// With NR's HUD-less E in place of Hudless:
//   Final = UI.rgb + (1 - a) * E
//         = (Back.rgb - (1 - a) * Hudless.rgb) + (1 - a) * E
//         = Back.rgb + (1 - a) * (E - Hudless.rgb).
// The last form needs no UI colour, so UI alpha alone suffices (the guide:
// with both tagged DLSS-G reads only UI alpha), it keeps Back exactly where
// a = 1 (the HUD bit for bit), and it does not depend on UI.rgb's precision
// (the guide warns against R10G10B10A2 UI buffers).
//
// Difference mask (Mode 2: HUD-less alone).  Where Back equals Hudless
// within Epsilon (relative above 1, for float encodings) the pixel is scene
// and takes E; elsewhere it is HUD and keeps Back.  The HUD pixels are
// counted per frame into Stats, which the addon reads a few frames late: a
// game whose post-UI effects (grain, a tone map after the UI) touch every
// pixel reads as nearly all HUD, and the addon then runs its two-pass path.
//
// Residual (Auto, v8.5.0-rc4): the same pixels are counted a second time at
// a grain-sized tolerance (kGrainTolerance, Stats + 8).  A frame above the
// HUD share whose grain-sized count is within it differs from its HUD-less
// frame by grain everywhere but a HUD, and takes NR's change to the HUD-less
// frame on top of the back buffer, the UI-alpha form with a = 0,
//   Final = Back.rgb + (E - Hudless.rgb),
// except where the difference is larger than grain: that pixel is UI and
// keeps Back.  One NR pass whose delta the real and the generated frames
// share (two passes gave the real frame NR(Back), which grain makes differ
// from NR(Hudless): MSFS 2024 and Resonance v8.5.0-rc1, 2x the NR cost).  A
// frame above the share at the grain tolerance too (a menu over most of the
// screen, a tone map after the UI) is ambiguous and runs two passes, as
// through rc3: the residual would lay the scene's NR change over the UI.

Texture2D<float4> Back : register(t0);
Texture2D<float4> Hudless : register(t1);
Texture2D<float4> Enhanced : register(t2);
Texture2D<float4> Ui : register(t3);
RWTexture2D<float4> Output : register(u0);
RWByteAddressBuffer Stats : register(u1);

// Two dispatches per frame.  Stage 0 counts the HUD pixels into Stats; stage 1
// composes.  The addon reads the count a frame or more late, so the frame
// that switches from a HUD to grain would still be composed as a mask - every
// pixel "HUD", the back buffer kept, no NR on the real frame.  Stage 1 reads
// this frame's own counts instead: above HudLimit the mask is not a HUD, and
// the frame takes the residual (Residual, grain-sized count within HudLimit)
// or NR's HUD-less frame whole (the HUD-less content, but NR) until the
// addon's two-pass path takes over.
cbuffer ComposeConstants : register(b0) {
  uint2 Size;
  uint Mode;
  uint StatsOffset;
  uint Serial;
  float Epsilon;
  uint Stage;
  uint HudLimit;
  uint Residual;
};

// Grain-sized, in the encoded domain (relative above 1, as Epsilon): 16
// 8-bit steps.
static const float kGrainTolerance = 1.0 / 16.0;

groupshared uint hud_pixels;
groupshared uint coarse_pixels;

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID, uint group_index : SV_GroupIndex) {
  if (group_index == 0) {
    hud_pixels = 0;
    coarse_pixels = 0;
  }
  GroupMemoryBarrierWithGroupSync();
  const uint2 pixel = dispatch_id.xy;
  if (all(pixel < Size)) {
    const float4 back = Back.Load(int3(pixel, 0));
    const float4 hudless = Hudless.Load(int3(pixel, 0));
    const float4 enhanced = Enhanced.Load(int3(pixel, 0));
    bool hud;
    bool coarse = false;
    float4 result;
    if (Mode == 2) {
      const float3 scale = max(1.0, abs(hudless.rgb));
      const float3 difference = abs(back.rgb - hudless.rgb);
      hud = any(difference > Epsilon * scale);
      coarse = any(difference > kGrainTolerance * scale);
      if (Stage == 1 && Stats.Load(StatsOffset) > HudLimit) {
        if (Residual != 0 && Stats.Load(StatsOffset + 8) <= HudLimit) {
          result = coarse ? back : float4(back.rgb + (enhanced.rgb - hudless.rgb), back.a);
        } else {
          result = float4(enhanced.rgb, back.a);
        }
      } else {
        result = hud ? back : float4(enhanced.rgb, back.a);
      }
    } else {
      const float4 ui = Ui.Load(int3(pixel, 0));
      const float alpha = saturate(Mode == 0 ? ui.r : ui.a);
      hud = alpha > 0.0;
      result = alpha >= 1.0
          ? back
          : float4(back.rgb + (1.0 - alpha) * (enhanced.rgb - hudless.rgb), back.a);
    }
    if (Stage == 1) Output[pixel] = result;
    if (hud) InterlockedAdd(hud_pixels, 1u);
    if (coarse) InterlockedAdd(coarse_pixels, 1u);
  }
  GroupMemoryBarrierWithGroupSync();
  if (Stage == 1) return;
  if (group_index == 0 && hud_pixels != 0) Stats.InterlockedAdd(StatsOffset, hud_pixels);
  if (group_index == 0 && coarse_pixels != 0) Stats.InterlockedAdd(StatsOffset + 8, coarse_pixels);
  if (all(dispatch_id.xy == 0)) Stats.Store(StatsOffset + 4, Serial);
}

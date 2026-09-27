// Depth resolve: copies a region of a host depth-stencil surface (D24S8) into
// resolve textures at an arbitrary destination point (e.g. the 1024x1024
// shadow map tiles of the 4096x2048 shadow atlas). The viewport/scissor select
// the destination rectangle.
//   target 0: R32_FLOAT depth (fetched as k_24_8 by the game)
//   target 1: the raw Xenos D24S8 word as RGBA8 (the game also fetches depth
//             resolves as k_8_8_8_8 to read the stencil byte): with the 8in32
//             endian swap, R = stencil, G/B/A = depth bits 0-7/8-15/16-23.
#ifdef MSAA
// Multisampled source: Xenos depth resolves read one sample (FRAGMENT0).
Texture2DMS<float> g_Depth : register(t0);
Texture2DMS<uint2> g_Stencil : register(t1);
#define LOAD(t, p) t.Load(p.xy, 0)
#else
Texture2D<float> g_Depth : register(t0);
Texture2D<uint2> g_Stencil : register(t1);
#define LOAD(t, p) t.Load(p)
#endif
cbuffer Constants : register(b0) {
  int2 g_SourceOffset;  // source texel = destination pixel + offset
};

struct Output {
  float depth : SV_Target0;
  float4 raw : SV_Target1;
};

void VSMain(uint id : SV_VertexID, out float4 pos : SV_Position) {
  float2 uv = float2((id << 1) & 2, id & 2);
  pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

Output PSMain(float4 pos : SV_Position) {
  int3 p = int3(int2(pos.xy) + g_SourceOffset, 0);
  Output o;
  o.depth = LOAD(g_Depth, p);
  uint stencil = LOAD(g_Stencil, p).g;
  uint d24 = uint(round(saturate(o.depth) * 16777215.0));
  o.raw = float4(stencil, d24 & 0xFF, (d24 >> 8) & 0xFF, d24 >> 16) / 255.0;
  return o;
}

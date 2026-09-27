// Screen-space ambient occlusion for the native renderer (graphics option,
// EXP-045). Runs after the game's opaque scene (Opaque / Character / Skybox)
// on the scene depth, in world space: the camera comes from the game's own
// g_mProjectionToWorld (inverse view-projection, row-vector convention:
// world = clip.x * M[0] + clip.y * M[1] + clip.z * M[2] + clip.w * M[3]).
//   AOMain:    AO term into an R8 target (one value per pixel)
//   ApplyMain: depth-aware 4x4 blur of the AO, multiplied into the HDR scene
//              target (blend: dest * src)
// Compiled with tools/dxc (see compile lines at the end of this comment):
//   dxc -T ps_6_0 -E AOMain -D MSAA_DEPTH=1 -Fh ssao_ms_ps.h -Vn kSsaoMsPS ssao.hlsl
//   dxc -T ps_6_0 -E AOMain -D MSAA_DEPTH=0 -Fh ssao_ps.h -Vn kSsaoPS ssao.hlsl
//   dxc -T ps_6_0 -E ApplyMain -D MSAA_DEPTH=1 -Fh ssao_apply_ms_ps.h -Vn kSsaoApplyMsPS ssao.hlsl
//   dxc -T ps_6_0 -E ApplyMain -D MSAA_DEPTH=0 -Fh ssao_apply_ps.h -Vn kSsaoApplyPS ssao.hlsl
#if MSAA_DEPTH
Texture2DMS<float> g_Depth : register(t0);
#define LOAD_DEPTH(p) g_Depth.Load(int2(p), 0)
#else
Texture2D<float> g_Depth : register(t0);
#define LOAD_DEPTH(p) g_Depth.Load(int3(int2(p), 0))
#endif
Texture2D<float> g_Ao : register(t1);

cbuffer SsaoConstants : register(b0) {
  float4 g_InvViewProj[4];  // clip -> world rows (game layout)
  float4 g_ViewProj[4];     // world -> clip rows (inverse of the above)
  float2 g_Size;            // target size in pixels
  float g_Radius;           // world units
  float g_Intensity;
  float g_FadeDistance;     // world units: AO fades out towards this distance
  float3 g_Pad;
};

float3 WorldPos(float2 uv, float z) {
  float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
  float4 w = clip.x * g_InvViewProj[0] + clip.y * g_InvViewProj[1] + clip.z * g_InvViewProj[2] +
             g_InvViewProj[3];
  return w.xyz / w.w;
}

float4 Project(float3 p) {
  return p.x * g_ViewProj[0] + p.y * g_ViewProj[1] + p.z * g_ViewProj[2] + g_ViewProj[3];
}

float DepthAt(float2 uv) {
  int2 p = clamp(int2(uv * g_Size), int2(0, 0), int2(g_Size) - 1);
  return LOAD_DEPTH(p);
}

static const int kSamples = 12;
// Hemisphere kernel (z = along the normal), lengths spread towards the center.
static const float3 kKernel[kSamples] = {
    float3(0.53, 0.18, 0.40), float3(-0.30, 0.55, 0.35), float3(-0.46, -0.37, 0.30),
    float3(0.22, -0.60, 0.42), float3(0.12, 0.17, 0.78), float3(-0.20, -0.08, 0.62),
    float3(0.72, -0.30, 0.25), float3(-0.66, 0.22, 0.45), float3(0.05, 0.84, 0.30),
    float3(0.28, 0.08, 0.28), float3(-0.10, -0.22, 0.20), float3(0.02, -0.34, 0.86)};

float AOMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float z = LOAD_DEPTH(int2(pos.xy));
  if (z >= 0.99999) return 1.0;  // sky / cleared
  float3 p = WorldPos(uv, z);
  float3 eye = WorldPos(uv, 0.0);
  float dist = length(p - eye);
  if (dist > g_FadeDistance) return 1.0;
  // Normal from the depth neighbours (the smaller step on each axis avoids
  // bleeding across depth discontinuities).
  float2 texel = 1.0 / g_Size;
  float3 pr = WorldPos(uv + float2(texel.x, 0), DepthAt(uv + float2(texel.x, 0)));
  float3 pl = WorldPos(uv - float2(texel.x, 0), DepthAt(uv - float2(texel.x, 0)));
  float3 pd = WorldPos(uv + float2(0, texel.y), DepthAt(uv + float2(0, texel.y)));
  float3 pu = WorldPos(uv - float2(0, texel.y), DepthAt(uv - float2(0, texel.y)));
  float3 dx = length(pr - p) < length(p - pl) ? pr - p : p - pl;
  float3 dy = length(pd - p) < length(p - pu) ? pd - p : p - pu;
  float3 n = normalize(cross(dx, dy));
  if (dot(n, eye - p) < 0.0) n = -n;
  // Per-pixel rotation (interleaved gradient noise), smoothed by the blur.
  float angle = 6.2831853 * frac(52.9829189 * frac(dot(pos.xy, float2(0.06711056, 0.00583715))));
  float3 up = abs(n.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
  float3 t = normalize(cross(up, n));
  float3 b = cross(n, t);
  float ca = cos(angle), sa = sin(angle);
  float3 tr = t * ca + b * sa, br = b * ca - t * sa;
  float occlusion = 0.0;
  [unroll] for (int i = 0; i < kSamples; ++i) {
    float3 k = kKernel[i];
    float3 s = p + (tr * k.x + br * k.y + n * k.z) * g_Radius;
    float4 c = Project(s);
    if (c.w <= 0.0) continue;
    float3 ndc = c.xyz / c.w;
    float2 suv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(suv < 0.0) || any(suv > 1.0)) continue;
    float zb = DepthAt(suv);
    if (zb >= 0.99999) continue;
    // Occluded when the visible surface there is in front of the sample point
    // (compared in world space: hardware depth has little precision far out).
    float3 q = WorldPos(suv, zb);
    if (length(q - eye) < length(s - eye) - 0.05 * g_Radius) {
      float range = saturate(g_Radius / max(length(q - p), 1e-4));
      occlusion += range * range;
    }
  }
  float ao = 1.0 - occlusion / kSamples;
  ao = lerp(ao, 1.0, saturate((dist / g_FadeDistance - 0.6) / 0.4));
  return saturate(ao);
}

float4 ApplyMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  int2 center = int2(pos.xy);
  float zc = LOAD_DEPTH(center);
  float3 pc = WorldPos(uv, zc);
  float sum = 0.0, weight = 0.0;
  [unroll] for (int y = -2; y < 2; ++y) {
    [unroll] for (int x = -2; x < 2; ++x) {
      int2 q = clamp(center + int2(x, y), int2(0, 0), int2(g_Size) - 1);
      float zq = LOAD_DEPTH(q);
      float3 pq = WorldPos((float2(q) + 0.5) / g_Size, zq);
      float w = saturate(1.0 - length(pq - pc) / (g_Radius * 0.5));
      sum += g_Ao.Load(int3(q, 0)) * w;
      weight += w;
    }
  }
  float ao = weight > 0.0 ? sum / weight : 1.0;
  ao = lerp(1.0, ao, g_Intensity);
  return float4(ao, ao, ao, 1.0);
}

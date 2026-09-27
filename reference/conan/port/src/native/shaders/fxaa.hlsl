// FXAA (fast approximate antialiasing, after Timothy Lottes' FXAA 3.11 quality
// algorithm) applied to the game's final 3D image (the "Upscale" pass target)
// before the HUD is drawn - graphics option, EXP-045. Input is the display
// (gamma) space LDR image; luma from the green-weighted RGB.
// Compiled with tools/dxc: dxc -T ps_6_0 -E PSMain -Fh fxaa_ps.h -Vn kFxaaPS fxaa.hlsl
Texture2D<float4> g_Source : register(t0);
SamplerState g_Linear : register(s0);
cbuffer FxaaConstants : register(b0) {
  float2 g_RcpFrame;  // 1 / target size in pixels
};

static const float kEdgeThreshold = 0.125;     // minimum local contrast (relative)
static const float kEdgeThresholdMin = 0.0312;  // ignore dark areas below this contrast
static const float kSubpixel = 0.75;            // sub-pixel aliasing removal amount
static const int kSearchSteps = 10;
static const float kSearchStep[kSearchSteps] = {1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float LumaAt(float2 uv) { return Luma(g_Source.SampleLevel(g_Linear, uv, 0.0).rgb); }
float LumaOff(float2 uv, int2 o) { return Luma(g_Source.SampleLevel(g_Linear, uv, 0.0, o).rgb); }

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float4 center = g_Source.SampleLevel(g_Linear, uv, 0.0);
  float lumaM = Luma(center.rgb);
  float lumaN = LumaOff(uv, int2(0, -1));
  float lumaS = LumaOff(uv, int2(0, 1));
  float lumaW = LumaOff(uv, int2(-1, 0));
  float lumaE = LumaOff(uv, int2(1, 0));
  float rangeMax = max(max(max(lumaN, lumaS), max(lumaW, lumaE)), lumaM);
  float rangeMin = min(min(min(lumaN, lumaS), min(lumaW, lumaE)), lumaM);
  float range = rangeMax - rangeMin;
  if (range < max(kEdgeThresholdMin, rangeMax * kEdgeThreshold)) {
    return center;
  }
  float lumaNW = LumaOff(uv, int2(-1, -1));
  float lumaNE = LumaOff(uv, int2(1, -1));
  float lumaSW = LumaOff(uv, int2(-1, 1));
  float lumaSE = LumaOff(uv, int2(1, 1));

  // Sub-pixel blend factor from the 3x3 low-pass contrast.
  float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
  float average = (2.0 * (lumaNS + lumaWE) + (lumaNW + lumaNE + lumaSW + lumaSE)) / 12.0;
  float subpixel = saturate(abs(average - lumaM) / range);
  subpixel = smoothstep(0.0, 1.0, subpixel);
  subpixel = subpixel * subpixel * kSubpixel;

  // Edge orientation.
  float edgeH = abs(lumaNW + lumaNE - 2.0 * lumaN) + 2.0 * abs(lumaW + lumaE - 2.0 * lumaM) +
                abs(lumaSW + lumaSE - 2.0 * lumaS);
  float edgeV = abs(lumaNW + lumaSW - 2.0 * lumaW) + 2.0 * abs(lumaN + lumaS - 2.0 * lumaM) +
                abs(lumaNE + lumaSE - 2.0 * lumaE);
  bool horizontal = edgeH >= edgeV;
  float lumaP = horizontal ? lumaN : lumaW;  // negative side
  float lumaQ = horizontal ? lumaS : lumaE;  // positive side
  float gradP = abs(lumaP - lumaM), gradQ = abs(lumaQ - lumaM);
  float stepLength = horizontal ? g_RcpFrame.y : g_RcpFrame.x;
  float lumaLocal, gradient;
  if (gradP >= gradQ) {
    stepLength = -stepLength;
    lumaLocal = 0.5 * (lumaP + lumaM);
    gradient = gradP;
  } else {
    lumaLocal = 0.5 * (lumaQ + lumaM);
    gradient = gradQ;
  }
  float2 edgeUv = uv;
  if (horizontal) {
    edgeUv.y += 0.5 * stepLength;
  } else {
    edgeUv.x += 0.5 * stepLength;
  }
  float2 edgeDir = horizontal ? float2(g_RcpFrame.x, 0.0) : float2(0.0, g_RcpFrame.y);
  float gradientScaled = gradient * 0.25;

  // Walk both ways along the edge until its contrast ends.
  float2 uvN = edgeUv - edgeDir * kSearchStep[0];
  float2 uvP = edgeUv + edgeDir * kSearchStep[0];
  float endN = LumaAt(uvN) - lumaLocal;
  float endP = LumaAt(uvP) - lumaLocal;
  bool doneN = abs(endN) >= gradientScaled, doneP = abs(endP) >= gradientScaled;
  [unroll] for (int i = 1; i < kSearchSteps; ++i) {
    if (doneN && doneP) break;
    if (!doneN) {
      uvN -= edgeDir * kSearchStep[i];
      endN = LumaAt(uvN) - lumaLocal;
      doneN = abs(endN) >= gradientScaled;
    }
    if (!doneP) {
      uvP += edgeDir * kSearchStep[i];
      endP = LumaAt(uvP) - lumaLocal;
      doneP = abs(endP) >= gradientScaled;
    }
  }
  float distN = horizontal ? (uv.x - uvN.x) : (uv.y - uvN.y);
  float distP = horizontal ? (uvP.x - uv.x) : (uvP.y - uv.y);
  bool nearN = distN < distP;
  float dist = min(distN, distP);
  float edgeLength = distN + distP;
  bool lumaMSmaller = (lumaM - lumaLocal) < 0.0;
  bool correct = ((nearN ? endN : endP) < 0.0) != lumaMSmaller;
  float edgeOffset = correct ? (0.5 - dist / edgeLength) : 0.0;
  float offset = max(edgeOffset, subpixel);
  float2 finalUv = uv;
  if (horizontal) {
    finalUv.y += offset * stepLength;
  } else {
    finalUv.x += offset * stepLength;
  }
  return float4(g_Source.SampleLevel(g_Linear, finalUv, 0.0).rgb, center.a);
}

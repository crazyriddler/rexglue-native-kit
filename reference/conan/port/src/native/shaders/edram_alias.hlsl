// EDRAM aliasing: a color surface bound at the same EDRAM base (same size and
// sample count) as another surface of a different 32bpp format reads that
// surface's bits. Packs the source texel to its Xenos EDRAM encoding and
// unpacks it as the destination format.
// Kinds: 0 = 8_8_8_8, 1 = 2_10_10_10 (unorm), 2 = 2_10_10_10_FLOAT (7e3 RGB).
#ifdef MSAA
Texture2DMS<float4> g_Source : register(t0);
#else
Texture2D<float4> g_Source : register(t0);
#endif
cbuffer Constants : register(b0) {
  uint g_SourceKind;
  uint g_DestKind;
};

void VSMain(uint id : SV_VertexID, out float4 pos : SV_Position) {
  float2 uv = float2((id << 1) & 2, id & 2);
  pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

uint Float32To7e3(float f) {
  f = clamp(f, 0.0, 31.875);
  uint u = asuint(f);
  uint biased;
  if (u < 0x3E800000u) {
    uint shift = min(125u - (u >> 23), 24u);
    biased = ((u & 0x7FFFFFu) | 0x800000u) >> shift;
  } else {
    biased = u + 0xC2000000u;
  }
  return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
}

float Float7e3To32(uint f10) {
  f10 &= 0x3FFu;
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7;
  if (exponent == 0u) {
    return float(mantissa) / 512.0;  // denormal: m / 128 * 2^-2
  }
  return (1.0 + float(mantissa) / 128.0) * exp2(float(exponent) - 3.0);
}

uint Pack(float4 c, uint kind) {
  if (kind == 0u) {
    uint4 b = uint4(round(saturate(c) * 255.0));
    return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
  }
  if (kind == 1u) {
    uint4 b = uint4(round(saturate(c) * float4(1023.0, 1023.0, 1023.0, 3.0)));
    return b.x | (b.y << 10) | (b.z << 20) | (b.w << 30);
  }
  return Float32To7e3(c.r) | (Float32To7e3(c.g) << 10) | (Float32To7e3(c.b) << 20) |
         (uint(round(saturate(c.a) * 3.0)) << 30);
}

float4 Unpack(uint d, uint kind) {
  if (kind == 0u) {
    return float4(d & 0xFFu, (d >> 8) & 0xFFu, (d >> 16) & 0xFFu, d >> 24) / 255.0;
  }
  if (kind == 1u) {
    return float4(float(d & 0x3FFu) / 1023.0, float((d >> 10) & 0x3FFu) / 1023.0,
                  float((d >> 20) & 0x3FFu) / 1023.0, float(d >> 30) / 3.0);
  }
  return float4(Float7e3To32(d), Float7e3To32(d >> 10), Float7e3To32(d >> 20),
                float(d >> 30) / 3.0);
}

#ifdef MSAA
// Per-sample: the aliased surfaces share the sample layout.
float4 PSMain(float4 pos : SV_Position, uint sample_index : SV_SampleIndex) : SV_Target {
  float4 c = g_Source.Load(int2(pos.xy), sample_index);
#else
float4 PSMain(float4 pos : SV_Position) : SV_Target {
  float4 c = g_Source.Load(int3(int2(pos.xy), 0));
#endif
  return Unpack(Pack(c, g_SourceKind), g_DestKind);
}

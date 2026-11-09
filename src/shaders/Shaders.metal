#include <metal_stdlib>
using namespace metal;

struct Uniforms {
  float2 srcSize;      // emulated framebuffer size in pixels (256 x 224/239)
  float2 outSize;      // size of the game viewport in drawable pixels
  float2 ambientScale; // uv scale used for the ambient background fill
  float  time;
  int    filter;       // see VideoFilter order in Settings.swift
  float  bloom;
  float  vibrance;
  int    scanStyle;    // 0 off, 1 sharp lines, 2 soft beam
  float  scanIntensity;
  float  scanThickness;
  int    maskType;     // 0 none, 1 aperture grille, 2 slot, 3 shadow
  float  maskStrength;
  float  scanBoost;
};

struct VOut {
  float4 pos [[position]];
  float2 uv;
};

vertex VOut vtx_main(uint vid [[vertex_id]]) {
  float2 p = float2((vid << 1) & 2, vid & 2);
  VOut o;
  o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
  o.uv = float2(p.x, 1.0 - p.y);
  return o;
}

constexpr sampler s_near(filter::nearest, address::clamp_to_edge);
constexpr sampler s_lin(filter::linear, mip_filter::linear, address::clamp_to_edge);

static inline float3 texel(texture2d<float> t, int2 c) {
  c = clamp(c, int2(0), int2(t.get_width() - 1, t.get_height() - 1));
  return t.read(uint2(c), 0).rgb;
}

// ------------------------------------------------------------------ filters
static float3 sharp_bilinear(texture2d<float> t, float2 uv, float2 src, float2 out) {
  float2 scale = max(floor(out / src), float2(1.0));
  float2 tx = uv * src;
  float2 tf = floor(tx);
  float2 cd = fract(tx) - 0.5;
  float2 range = 0.5 - 0.5 / scale;
  float2 f = (cd - clamp(cd, -range, range)) * scale + 0.5;
  return t.sample(s_lin, (tf + f) / src, level(0)).rgb;
}

static float4 catmull_w(float x) {
  float x2 = x * x, x3 = x2 * x;
  return float4(-0.5 * x3 + x2 - 0.5 * x,
                1.5 * x3 - 2.5 * x2 + 1.0,
                -1.5 * x3 + 2.0 * x2 + 0.5 * x,
                0.5 * x3 - 0.5 * x2);
}

static float lanczos2(float x) {
  x = abs(x);
  if (x < 1e-4) return 1.0;
  if (x >= 2.0) return 0.0;
  float px = M_PI_F * x;
  return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}

static float3 kernel4x4(texture2d<float> t, float2 uv, float2 src, bool lanczos) {
  float2 p = uv * src - 0.5;
  int2 b = int2(floor(p));
  float2 f = p - floor(p);
  float4 wx, wy;
  if (lanczos) {
    wx = float4(lanczos2(1 + f.x), lanczos2(f.x), lanczos2(1 - f.x), lanczos2(2 - f.x));
    wy = float4(lanczos2(1 + f.y), lanczos2(f.y), lanczos2(1 - f.y), lanczos2(2 - f.y));
    wx /= dot(wx, 1.0); wy /= dot(wy, 1.0);
  } else {
    wx = catmull_w(f.x); wy = catmull_w(f.y);
  }
  float3 acc = 0;
  for (int j = 0; j < 4; j++) {
    float3 row = 0;
    for (int i = 0; i < 4; i++) row += texel(t, b + int2(i - 1, j - 1)) * wx[i];
    acc += row * wy[j];
  }
  return clamp(acc, 0.0, 1.0);
}

static inline bool same(float3 a, float3 b) { return distance(a, b) < 0.075; }

// Continuous, anti-aliased Scale2x/EPX rule: rounds staircase corners at any scale.
static float3 smooth_edges(texture2d<float> t, float2 uv, float2 src, float2 out) {
  float2 p = uv * src;
  int2 c = int2(floor(p));
  float2 o = fract(p) - 0.5;
  float3 E = texel(t, c);
  float3 B = texel(t, c + int2(0, -1)), H = texel(t, c + int2(0, 1));
  float3 D = texel(t, c + int2(-1, 0)), F = texel(t, c + int2(1, 0));
  bool left = o.x < 0.0, up = o.y < 0.0;
  float3 NX = left ? D : F, OX = left ? F : D;
  float3 NY = up ? B : H, OY = up ? H : B;
  float aa = 0.5 * src.x / max(out.x, 1.0) + 0.02;
  float3 col = E;
  if (same(NX, NY) && !same(NX, E) && !same(NY, OX) && !same(NX, OY)) {
    float w = smoothstep(0.5 - aa, 0.5 + aa, abs(o.x) + abs(o.y));
    col = mix(E, (NX + NY) * 0.5, w);
  }
  float2 edge = smoothstep(0.5 - aa, 0.5, abs(o));
  col = mix(col, NX, edge.x * 0.3 * float(!same(NX, E)));
  col = mix(col, NY, edge.y * 0.3 * float(!same(NY, E)));
  return col;
}

static float3 lcd_grid(texture2d<float> t, float2 uv, float2 src, float2 out) {
  float3 c = t.sample(s_near, uv).rgb;
  float2 f = fract(uv * src);
  float px = src.x / max(out.x, 1.0);              // one output pixel in source units
  float gap = max(px, 0.08);
  float2 g = smoothstep(0.0, gap, f) * (1.0 - smoothstep(1.0 - gap, 1.0, f));
  float grid = mix(0.72, 1.0, g.x * g.y);
  return min(c * grid * 1.08, 1.0);
}

static float3 composite(texture2d<float> t, float2 uv, float2 src) {
  // NTSC-ish: luma moderately soft, chroma smeared wider
  const float3x3 toYIQ = float3x3(float3(0.299, 0.596, 0.211), float3(0.587, -0.274, -0.523), float3(0.114, -0.322, 0.312));
  const float3x3 toRGB = float3x3(float3(1.0, 1.0, 1.0), float3(0.956, -0.272, -1.106), float3(0.621, -0.647, 1.703));
  float dx = 1.0 / src.x;
  float3 yiq = 0;
  const float wl[7] = {0.02, 0.08, 0.2, 0.4, 0.2, 0.08, 0.02};
  const float wc[7] = {0.1, 0.14, 0.17, 0.18, 0.17, 0.14, 0.1};
  for (int i = 0; i < 7; i++) {
    float3 s = toYIQ * t.sample(s_lin, uv + float2((i - 3) * dx * 0.75, 0), level(0)).rgb;
    yiq.x += s.x * wl[i];
    yiq.yz += s.yz * wc[i];
  }
  yiq.yz *= 1.1;
  return clamp(toRGB * yiq, 0.0, 1.0);
}

static float3 run_filter(int f, texture2d<float> tex, float2 uv, float2 src, float2 out) {
  switch (f) {
    case 0: return tex.sample(s_near, uv).rgb;
    case 1: return tex.sample(s_lin, uv, level(0)).rgb;
    case 2: return sharp_bilinear(tex, uv, src, out);
    case 3: return kernel4x4(tex, uv, src, false);
    case 4: return kernel4x4(tex, uv, src, true);
    case 5: return smooth_edges(tex, uv, src, out);
    case 6: return lcd_grid(tex, uv, src, out);
    default: return composite(tex, uv, src);
  }
}

// ------------------------------------------------------------------ scanlines + mask
static float3 rgb_triad(int k, float lo) {
  return k == 0 ? float3(1, lo, lo) : k == 1 ? float3(lo, 1, lo) : float3(lo, lo, 1);
}

static float3 crt_post(float3 c, float2 uv, float2 pos, constant Uniforms& u) {
  if (u.scanStyle == 0 && u.maskType == 0) return c;
  float3 lin = pow(c, 2.2);
  if (u.scanStyle != 0) {
    float f = fract(uv.y * u.srcSize.y);
    float aa = u.srcSize.y / max(u.outSize.y, 1.0);
    float w;
    if (u.scanStyle == 1) {
      float gap = mix(0.15, 0.6, u.scanThickness);   // dark portion of each line
      w = 1.0 - smoothstep(1.0 - gap - aa, 1.0 - gap + aa, f);
    } else {
      // gaussian beam; bright pixels bloom wider like a real electron beam
      float luma = dot(lin, float3(0.299, 0.587, 0.114));
      float sigma = mix(0.42, 0.18, u.scanThickness) * (1.0 + 0.45 * luma);
      float d = f - 0.5;
      w = exp(-d * d / (2.0 * sigma * sigma));
    }
    lin *= mix(1.0, w, u.scanIntensity);
  }
  if (u.maskType != 0) {
    int2 p = int2(pos);
    float lo = 1.0 - u.maskStrength;
    float3 m;
    if (u.maskType == 1) {
      m = rgb_triad(p.x % 3, lo);
    } else if (u.maskType == 2) {
      m = rgb_triad(p.x % 3, lo);
      int cell = (p.x / 3) & 1;
      if (((p.y + cell * 2) % 4) == 3) m *= lo;
    } else {
      m = rgb_triad((p.x + (p.y & 1) * 2) % 3, lo);
    }
    lin *= m;
  }
  float boost = 1.0 + u.scanBoost * (0.6 * u.scanIntensity * float(u.scanStyle != 0) +
                                     0.8 * u.maskStrength * float(u.maskType != 0));
  lin *= boost;
  return pow(clamp(lin, 0.0, 1.0), 1.0 / 2.2);
}

// ------------------------------------------------------------------ classic
fragment float4 frag_classic(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                             constant Uniforms& u [[buffer(0)]]) {
  float3 c = run_filter(u.filter, tex, in.uv, u.srcSize, u.outSize);
  return float4(crt_post(c, in.uv, in.pos.xy, u), 1.0);
}

// ------------------------------------------------------------------ modern 2026
static float3 grade(float3 c, float vibrance) {
  float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
  float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
  c = mix(float3(luma), c, 1.0 + vibrance * (1.0 - (mx - mn)));
  c = clamp(c, 0.0, 1.0);
  c = mix(c, c * c * (3.0 - 2.0 * c), 0.22);
  c *= mix(float3(1.0), float3(1.03, 1.0, 0.97), smoothstep(0.5, 1.0, luma));
  return c;
}

fragment float4 frag_modern(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                            constant Uniforms& u [[buffer(0)]]) {
  float2 uv = in.uv;
  float3 col = smooth_edges(tex, uv, u.srcSize, u.outSize);
  float3 glow = tex.sample(s_lin, uv, level(2.0)).rgb * 0.4
              + tex.sample(s_lin, uv, level(3.0)).rgb * 0.35
              + tex.sample(s_lin, uv, level(4.0)).rgb * 0.25;
  col += max(glow - 0.72, 0.0) * u.bloom * 1.1;
  float lumaE = dot(col, float3(0.299, 0.587, 0.114));
  float lumaB = dot(glow, float3(0.299, 0.587, 0.114));
  col *= 1.0 - clamp((lumaB - lumaE) * 0.12, 0.0, 0.08);
  col = grade(col, u.vibrance);
  float2 v = uv - 0.5;
  col *= 1.0 - dot(v, v) * 0.22;
  return float4(crt_post(col, uv, in.pos.xy, u), 1.0);
}

fragment float4 frag_ambient(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                             constant Uniforms& u [[buffer(0)]]) {
  float2 uv = (in.uv - 0.5) * u.ambientScale + 0.5;
  float3 c = tex.sample(s_lin, uv, level(5.0)).rgb * 0.5
           + tex.sample(s_lin, uv + float2(0.03, 0.02), level(6.0)).rgb * 0.25
           + tex.sample(s_lin, uv - float2(0.03, 0.02), level(6.0)).rgb * 0.25;
  float luma = dot(c, float3(0.299, 0.587, 0.114));
  c = mix(float3(luma), c, 0.75) * 0.32;
  float2 v = in.uv - 0.5;
  c *= 1.0 - dot(v, v) * 1.1;
  return float4(max(c, 0.0), 1.0);
}

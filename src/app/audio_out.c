// Audio output path: lock-free ring buffer (32 kHz from the S-DSP) -> adaptive
// cubic resampler -> optional "Modern 2026" mastering chain -> device rate.
#include "audio_out.h"
#include <stdatomic.h>
#include <math.h>
#include <string.h>

#define RING 16384
#define IN_RATE 32000.0
#define TARGET_FILL 1600

static float ring_l[RING], ring_r[RING];
static _Atomic int wpos, rpos;
static double out_rate = 48000.0;
static double frac;
static float hist_l[4], hist_r[4];

static _Atomic int p_modern, p_muted;
static _Atomic float p_volume = 0.8f, p_reverb = 0.25f, p_width = 0.35f;

// ---------------------------------------------------------------- biquads
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } Biquad;
static Biquad low_l, low_r, high_l, high_r, dc_l, dc_r;

static void shelf(Biquad *q, int high, double f0, double gain_db) {
  double A = pow(10.0, gain_db / 40.0), w0 = 2 * M_PI * f0 / out_rate;
  double cw = cos(w0), sw = sin(w0), S = 1.0;
  double alpha = sw / 2 * sqrt((A + 1 / A) * (1 / S - 1) + 2);
  double b0, b1, b2, a0, a1, a2, sa = 2 * sqrt(A) * alpha;
  if (!high) {
    b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sa);
    a0 = (A + 1) + (A - 1) * cw + sa; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sa;
  } else {
    b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sa);
    a0 = (A + 1) - (A - 1) * cw + sa; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sa;
  }
  q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0; q->a1 = a1 / a0; q->a2 = a2 / a0;
  q->z1 = q->z2 = 0;
}
static void highpass(Biquad *q, double f0) {
  double w0 = 2 * M_PI * f0 / out_rate, cw = cos(w0), alpha = sin(w0) / (2 * 0.7071);
  double a0 = 1 + alpha;
  q->b0 = (1 + cw) / 2 / a0; q->b1 = -(1 + cw) / a0; q->b2 = (1 + cw) / 2 / a0;
  q->a1 = -2 * cw / a0; q->a2 = (1 - alpha) / a0; q->z1 = q->z2 = 0;
}
static inline float bq(Biquad *q, float x) {
  float y = q->b0 * x + q->z1;
  q->z1 = q->b1 * x - q->a1 * y + q->z2;
  q->z2 = q->b2 * x - q->a2 * y;
  return y;
}

// ---------------------------------------------------------------- reverb (Freeverb-style)
#define NCOMB 4
#define NAP 2
static const int comb_len[NCOMB] = {1116, 1188, 1277, 1356};
static const int ap_len[NAP] = {556, 441};
typedef struct { float buf[4096]; int len, pos; float store; } Comb;
typedef struct { float buf[2048]; int len, pos; } Allpass;
static Comb combs[2][NCOMB];
static Allpass aps[2][NAP];

static void reverb_init(void) {
  double s = out_rate / 44100.0;
  for (int c = 0; c < 2; c++) {
    for (int i = 0; i < NCOMB; i++) {
      memset(&combs[c][i], 0, sizeof(Comb));
      int l = (int)((comb_len[i] + c * 23) * s); combs[c][i].len = l > 4095 ? 4095 : l;
    }
    for (int i = 0; i < NAP; i++) {
      memset(&aps[c][i], 0, sizeof(Allpass));
      int l = (int)((ap_len[i] + c * 23) * s); aps[c][i].len = l > 2047 ? 2047 : l;
    }
  }
}

static inline float reverb_ch(int c, float in) {
  float out = 0;
  for (int i = 0; i < NCOMB; i++) {
    Comb *cb = &combs[c][i];
    float y = cb->buf[cb->pos];
    cb->store = y * 0.6f + cb->store * 0.4f;   // damping
    cb->buf[cb->pos] = in + cb->store * 0.78f; // room size
    if (++cb->pos >= cb->len) cb->pos = 0;
    out += y;
  }
  for (int i = 0; i < NAP; i++) {
    Allpass *ap = &aps[c][i];
    float b = ap->buf[ap->pos];
    float y = -out + b;
    ap->buf[ap->pos] = out + b * 0.5f;
    if (++ap->pos >= ap->len) ap->pos = 0;
    out = y;
  }
  return out;
}

// ---------------------------------------------------------------- API
void ao_init(double rate) {
  out_rate = rate > 8000 ? rate : 48000;
  shelf(&low_l, 0, 140, 3.0); shelf(&low_r, 0, 140, 3.0);
  shelf(&high_l, 1, 7000, 2.5); shelf(&high_r, 1, 7000, 2.5);
  highpass(&dc_l, 18); highpass(&dc_r, 18);
  reverb_init();
  ao_clear();
}

void ao_clear(void) {
  atomic_store(&wpos, 0); atomic_store(&rpos, 0);
  frac = 0;
  memset(hist_l, 0, sizeof hist_l); memset(hist_r, 0, sizeof hist_r);
}

int ao_fill(void) { return (atomic_load(&wpos) - atomic_load(&rpos) + RING) % RING; }

void ao_set_params(int modern, float volume, float reverb, float width, int muted) {
  atomic_store(&p_modern, modern); atomic_store(&p_volume, volume);
  atomic_store(&p_reverb, reverb); atomic_store(&p_width, width); atomic_store(&p_muted, muted);
}

void ao_push(const int16_t *s, int frames) {
  int w = atomic_load(&wpos);
  int r = atomic_load_explicit(&rpos, memory_order_acquire);
  for (int i = 0; i < frames; i++) {
    int nw = (w + 1) % RING;
    if (nw == r) break;  // full: drop
    ring_l[w] = s[i * 2] / 32768.0f;
    ring_r[w] = s[i * 2 + 1] / 32768.0f;
    w = nw;
  }
  atomic_store_explicit(&wpos, w, memory_order_release);
}

static inline float hermite(const float *h, float t) {
  float c0 = h[1], c1 = 0.5f * (h[2] - h[0]);
  float c2 = h[0] - 2.5f * h[1] + 2.0f * h[2] - 0.5f * h[3];
  float c3 = 0.5f * (h[3] - h[0]) + 1.5f * (h[1] - h[2]);
  return ((c3 * t + c2) * t + c1) * t + c0;
}

void ao_render(float *L, float *R, int frames) {
  int r = atomic_load(&rpos);
  int w = atomic_load_explicit(&wpos, memory_order_acquire);
  int fill = (w - r + RING) % RING;
  double adjust = 1.0 + ((double)fill - TARGET_FILL) / TARGET_FILL * 0.006;
  if (adjust < 0.99) adjust = 0.99; if (adjust > 1.01) adjust = 1.01;
  double step = IN_RATE / out_rate * adjust;
  int modern = atomic_load(&p_modern), muted = atomic_load(&p_muted);
  float vol = muted ? 0.0f : atomic_load(&p_volume);
  float rv = atomic_load(&p_reverb), width = atomic_load(&p_width);

  for (int i = 0; i < frames; i++) {
    frac += step;
    while (frac >= 1.0) {
      frac -= 1.0;
      float nl = hist_l[3], nr = hist_r[3];
      if (r != w) { nl = ring_l[r]; nr = ring_r[r]; r = (r + 1) % RING; }
      else { nl *= 0.995f; nr *= 0.995f; }  // underrun: decay smoothly
      memmove(hist_l, hist_l + 1, 3 * sizeof(float)); hist_l[3] = nl;
      memmove(hist_r, hist_r + 1, 3 * sizeof(float)); hist_r[3] = nr;
    }
    float l = hermite(hist_l, (float)frac), rr = hermite(hist_r, (float)frac);
    if (modern) {
      l = bq(&dc_l, l); rr = bq(&dc_r, rr);
      l = bq(&low_l, l); rr = bq(&low_r, rr);
      l = bq(&high_l, l); rr = bq(&high_r, rr);
      float m = (l + rr) * 0.5f, s = (l - rr) * 0.5f * (1.0f + width);
      l = m + s; rr = m - s;
      float in = (l + rr) * 0.5f * 0.12f;
      float wl = reverb_ch(0, in), wr = reverb_ch(1, in);
      l = l * (1.0f - rv * 0.25f) + wl * rv;
      rr = rr * (1.0f - rv * 0.25f) + wr * rv;
      l *= vol * 1.1f; rr *= vol * 1.1f;
      // soft knee limiter
      l = l / (1.0f + fabsf(l) * 0.35f) * 1.18f;
      rr = rr / (1.0f + fabsf(rr) * 0.35f) * 1.18f;
    } else {
      l *= vol; rr *= vol;
    }
    if (l > 1) l = 1; if (l < -1) l = -1; if (rr > 1) rr = 1; if (rr < -1) rr = -1;
    L[i] = l; R[i] = rr;
  }
  atomic_store_explicit(&rpos, r, memory_order_release);
}

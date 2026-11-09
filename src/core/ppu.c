// S-PPU1/S-PPU2: registers + scanline renderer (modes 0-7, sprites, windows,
// color math, mosaic, brightness).
#include "snes.h"
#include <string.h>

#define P (g->ppu)

void ppu_reset(void) {
  memset(&P, 0, sizeof(P));
  P.inidisp = 0x80;
  g->fb_height = 224;
}

static inline uint16_t vram_remap(uint16_t a) {
  switch ((P.vmain >> 2) & 3) {
    case 1: return (a & 0xFF00) | ((a & 0x001F) << 3) | ((a >> 5) & 7);
    case 2: return (a & 0xFE00) | ((a & 0x003F) << 3) | ((a >> 6) & 7);
    case 3: return (a & 0xFC00) | ((a & 0x007F) << 3) | ((a >> 7) & 7);
  }
  return a;
}
static inline int vram_inc(void) { static const int t[4] = {1, 32, 128, 128}; return t[P.vmain & 3]; }

void ppu_latch_counters(void) {
  P.hlatch = g->hpos >> 2;
  P.vlatch = g->vpos;
  P.counter_latched = true;
}

void ppu_vblank_start(void) {
  if (!(P.inidisp & 0x80)) P.oam_addr = P.oam_addr_reload << 1;
}

uint8_t ppu_read(uint16_t addr) {
  switch (addr) {
    case 0x2134: case 0x2135: case 0x2136: {
      int32_t r = (int32_t)P.m7a * (int8_t)(P.m7b >> 8);
      P.ppu1_mdr = (r >> ((addr - 0x2134) * 8)) & 0xFF;
      return P.ppu1_mdr;
    }
    case 0x2137: if (g->wrio & 0x80) ppu_latch_counters(); return g->mdr;
    case 0x2138: {
      uint16_t a = P.oam_addr;
      uint8_t v = a < 0x200 ? P.oam[a] : P.oam[0x200 | (a & 0x1F)];
      P.oam_addr = (a + 1) & 0x3FF;
      return P.ppu1_mdr = v;
    }
    case 0x2139: {
      uint8_t v = P.vram_latch & 0xFF;
      if (!(P.vmain & 0x80)) { P.vram_latch = P.vram[vram_remap(P.vram_addr) & 0x7FFF]; P.vram_addr += vram_inc(); }
      return P.ppu1_mdr = v;
    }
    case 0x213A: {
      uint8_t v = P.vram_latch >> 8;
      if (P.vmain & 0x80) { P.vram_latch = P.vram[vram_remap(P.vram_addr) & 0x7FFF]; P.vram_addr += vram_inc(); }
      return P.ppu1_mdr = v;
    }
    case 0x213B: {
      uint8_t v;
      if (!P.cg_flip) v = P.cgram[P.cg_addr] & 0xFF;
      else { v = ((P.cgram[P.cg_addr] >> 8) & 0x7F) | (P.ppu2_mdr & 0x80); P.cg_addr++; }
      P.cg_flip = !P.cg_flip;
      return P.ppu2_mdr = v;
    }
    case 0x213C: {
      uint8_t v = P.hlatch_flip ? ((P.hlatch >> 8) & 1) | (P.ppu2_mdr & 0xFE) : P.hlatch & 0xFF;
      P.hlatch_flip = !P.hlatch_flip;
      return P.ppu2_mdr = v;
    }
    case 0x213D: {
      uint8_t v = P.vlatch_flip ? ((P.vlatch >> 8) & 1) | (P.ppu2_mdr & 0xFE) : P.vlatch & 0xFF;
      P.vlatch_flip = !P.vlatch_flip;
      return P.ppu2_mdr = v;
    }
    case 0x213E: return P.ppu1_mdr = (P.stat77 & 0xC0) | (P.ppu1_mdr & 0x10) | 0x01;
    case 0x213F: {
      uint8_t v = (P.counter_latched ? 0x40 : 0) | (P.ppu2_mdr & 0x20) | 0x03 | ((g->frames & 1) ? 0x80 : 0);
      if (g->wrio & 0x80) P.counter_latched = false;
      P.hlatch_flip = P.vlatch_flip = false;
      return P.ppu2_mdr = v;
    }
  }
  return g->mdr;
}

static inline void set_hofs(BgLayer *b, uint8_t v) {
  b->hofs = ((v << 8) | (P.bgofs_latch & ~7) | (P.bghofs_latch & 7)) & 0x3FF;
  P.bgofs_latch = v; P.bghofs_latch = v;
}
static inline void set_vofs(BgLayer *b, uint8_t v) {
  b->vofs = ((v << 8) | P.bgofs_latch) & 0x3FF;
  P.bgofs_latch = v;
}
static inline int16_t m7_word(uint8_t v) { int16_t r = (int16_t)((v << 8) | P.m7_latch); P.m7_latch = v; return r; }
static inline int16_t sext13(int v) { v &= 0x1FFF; return (v & 0x1000) ? (int16_t)(v | ~0x1FFF) : (int16_t)v; }

void ppu_write(uint16_t addr, uint8_t v) {
  switch (addr) {
    case 0x2100:
      if ((P.inidisp & 0x80) && !(v & 0x80) && g->vpos == g->vdisp + 1) P.oam_addr = P.oam_addr_reload << 1;
      P.inidisp = v; break;
    case 0x2101: P.obsel = v; break;
    case 0x2102: P.oam_addr_reload = (P.oam_addr_reload & 0x100) | v; P.oam_addr = P.oam_addr_reload << 1; break;
    case 0x2103: P.oam_addr_reload = (P.oam_addr_reload & 0xFF) | ((v & 1) << 8); P.oam_prio_rot = v & 0x80;
      P.oam_addr = P.oam_addr_reload << 1; break;
    case 0x2104: {
      uint16_t a = P.oam_addr;
      if (a < 0x200) {
        if (!(a & 1)) P.oam_latch = v;
        else { P.oam[a - 1] = P.oam_latch; P.oam[a] = v; }
      } else P.oam[0x200 | (a & 0x1F)] = v;
      P.oam_addr = (a + 1) & 0x3FF;
      break;
    }
    case 0x2105:
      P.bgmode = v;
      for (int i = 0; i < 4; i++) P.bg[i].big_tiles = v & (0x10 << i);
      break;
    case 0x2106: P.mosaic = v; break;
    case 0x2107: case 0x2108: case 0x2109: case 0x210A: {
      BgLayer *b = &P.bg[addr - 0x2107];
      b->tilemap_base = (v & 0xFC) << 8; b->tilemap_size = v & 3; break;
    }
    case 0x210B: P.bg[0].chr_base = (v & 0x0F) << 12; P.bg[1].chr_base = (v >> 4) << 12; break;
    case 0x210C: P.bg[2].chr_base = (v & 0x0F) << 12; P.bg[3].chr_base = (v >> 4) << 12; break;
    case 0x210D: P.m7hofs = sext13(m7_word(v)); set_hofs(&P.bg[0], v); break;
    case 0x210E: P.m7vofs = sext13(m7_word(v)); set_vofs(&P.bg[0], v); break;
    case 0x210F: set_hofs(&P.bg[1], v); break;
    case 0x2110: set_vofs(&P.bg[1], v); break;
    case 0x2111: set_hofs(&P.bg[2], v); break;
    case 0x2112: set_vofs(&P.bg[2], v); break;
    case 0x2113: set_hofs(&P.bg[3], v); break;
    case 0x2114: set_vofs(&P.bg[3], v); break;
    case 0x2115: P.vmain = v; break;
    case 0x2116: P.vram_addr = (P.vram_addr & 0xFF00) | v; P.vram_latch = P.vram[vram_remap(P.vram_addr) & 0x7FFF]; break;
    case 0x2117: P.vram_addr = (P.vram_addr & 0x00FF) | (v << 8); P.vram_latch = P.vram[vram_remap(P.vram_addr) & 0x7FFF]; break;
    case 0x2118: {
      uint16_t a = vram_remap(P.vram_addr) & 0x7FFF;
      P.vram[a] = (P.vram[a] & 0xFF00) | v;
      if (!(P.vmain & 0x80)) P.vram_addr += vram_inc();
      break;
    }
    case 0x2119: {
      uint16_t a = vram_remap(P.vram_addr) & 0x7FFF;
      P.vram[a] = (P.vram[a] & 0x00FF) | (v << 8);
      if (P.vmain & 0x80) P.vram_addr += vram_inc();
      break;
    }
    case 0x211A: P.m7sel = v; break;
    case 0x211B: P.m7a = m7_word(v); break;
    case 0x211C: P.m7b = m7_word(v); break;
    case 0x211D: P.m7c = m7_word(v); break;
    case 0x211E: P.m7d = m7_word(v); break;
    case 0x211F: P.m7x = sext13(m7_word(v)); break;
    case 0x2120: P.m7y = sext13(m7_word(v)); break;
    case 0x2121: P.cg_addr = v; P.cg_flip = false; break;
    case 0x2122:
      if (!P.cg_flip) P.cg_latch = v;
      else { P.cgram[P.cg_addr] = ((v & 0x7F) << 8) | P.cg_latch; P.cg_addr++; }
      P.cg_flip = !P.cg_flip;
      break;
    case 0x2123: P.w12sel = v; break;
    case 0x2124: P.w34sel = v; break;
    case 0x2125: P.wobjsel = v; break;
    case 0x2126: P.wh0 = v; break;
    case 0x2127: P.wh1 = v; break;
    case 0x2128: P.wh2 = v; break;
    case 0x2129: P.wh3 = v; break;
    case 0x212A: P.wbglog = v; break;
    case 0x212B: P.wobjlog = v; break;
    case 0x212C: P.tm = v; break;
    case 0x212D: P.ts = v; break;
    case 0x212E: P.tmw = v; break;
    case 0x212F: P.tsw = v; break;
    case 0x2130: P.cgwsel = v; break;
    case 0x2131: P.cgadsub = v; break;
    case 0x2132: {
      uint16_t c = v & 0x1F;
      if (v & 0x20) P.fixed_color = (P.fixed_color & ~0x001F) | c;
      if (v & 0x40) P.fixed_color = (P.fixed_color & ~0x03E0) | (c << 5);
      if (v & 0x80) P.fixed_color = (P.fixed_color & ~0x7C00) | (c << 10);
      break;
    }
    case 0x2133: P.setini = v; break;
  }
}

// ======================================================================= render
typedef struct { uint16_t col; uint8_t z, layer; } Px;
static Px mainl[256], subl[256];
static uint16_t lcol[256];
static uint8_t lz[256], lmath[256];
static bool wmask[256];

static void calc_window(uint8_t sel, uint8_t logic, bool *m) {
  bool e1 = sel & 2, i1 = sel & 1, e2 = sel & 8, i2 = sel & 4;
  if (!e1 && !e2) { memset(m, 0, 256); return; }
  for (int x = 0; x < 256; x++) {
    bool a = x >= P.wh0 && x <= P.wh1; if (i1) a = !a;
    bool b = x >= P.wh2 && x <= P.wh3; if (i2) b = !b;
    bool r;
    if (e1 && !e2) r = a;
    else if (!e1) r = b;
    else switch (logic & 3) { case 0: r = a || b; break; case 1: r = a && b; break; case 2: r = a != b; break; default: r = a == b; }
    m[x] = r;
  }
}

static inline uint16_t direct_color(uint8_t pix, uint8_t pal) {
  uint16_t r = ((pix & 7) << 2) | ((pal & 1) << 1);
  uint16_t gg = (((pix >> 3) & 7) << 2) | (pal & 2);
  uint16_t b = (((pix >> 6) & 3) << 3) | (pal & 4);
  return r | (gg << 5) | (b << 10);
}

static inline uint16_t tm_addr(const BgLayer *b, int tx, int ty) {
  uint16_t a = b->tilemap_base + ((ty & 31) << 5) + (tx & 31);
  if ((tx & 32) && (b->tilemap_size & 1)) a += 0x400;
  if ((ty & 32) && (b->tilemap_size & 2)) a += (b->tilemap_size & 1) ? 0x800 : 0x400;
  return a & 0x7FFF;
}

static void render_bg(int i, int bpp, int line, uint8_t zlo, uint8_t zhi, bool hires) {
  const BgLayer *b = &P.bg[i];
  int msize = (P.mosaic >> 4) + 1;
  bool mos = (P.mosaic & (1 << i)) && msize > 1;
  int y = mos ? 1 + ((line - 1) / msize) * msize : line;
  int tw = (b->big_tiles || hires) ? 16 : 8;
  int th = b->big_tiles ? 16 : 8;
  int vy = (y + b->vofs) & 0x3FF;
  int ty = vy / th, fy = vy % th;
  int words = bpp * 4;
  bool mode0 = (P.bgmode & 7) == 0;
  bool direct = bpp == 8 && (P.cgwsel & 1);
  int last_tx = -1;
  uint16_t entry = 0;
  for (int x = 0; x < 256; x++) {
    int sx = mos ? x - (x % msize) : x;
    int hx = hires ? ((sx * 2 + b->hofs) & 0x3FF) : ((sx + b->hofs) & 0x3FF);
    int tx = hx / tw, fx = hx % tw;
    if (tx != last_tx) { entry = P.vram[tm_addr(b, tx, ty)]; last_tx = tx; }
    int tile = entry & 0x3FF;
    int px = (entry & 0x4000) ? tw - 1 - fx : fx;
    int py = (entry & 0x8000) ? th - 1 - fy : fy;
    if (tw == 16 && px >= 8) tile += 1;
    if (th == 16 && py >= 8) tile += 16;
    tile &= 0x3FF;
    px &= 7; py &= 7;
    uint16_t addr = (b->chr_base + tile * words + py) & 0x7FFF;
    int bit = 7 - px;
    uint16_t w = P.vram[addr];
    int pix = ((w >> bit) & 1) | (((w >> (8 + bit)) & 1) << 1);
    if (bpp >= 4) {
      w = P.vram[(addr + 8) & 0x7FFF];
      pix |= (((w >> bit) & 1) << 2) | (((w >> (8 + bit)) & 1) << 3);
      if (bpp == 8) {
        w = P.vram[(addr + 16) & 0x7FFF];
        pix |= (((w >> bit) & 1) << 4) | (((w >> (8 + bit)) & 1) << 5);
        w = P.vram[(addr + 24) & 0x7FFF];
        pix |= (((w >> bit) & 1) << 6) | (((w >> (8 + bit)) & 1) << 7);
      }
    }
    if (!pix) { lz[x] = 0; continue; }
    int pal = (entry >> 10) & 7;
    uint16_t c;
    if (bpp == 2) c = P.cgram[(mode0 ? i * 32 : 0) + pal * 4 + pix];
    else if (bpp == 4) c = P.cgram[pal * 16 + pix];
    else c = direct ? direct_color(pix, pal) : P.cgram[pix];
    lcol[x] = c;
    lz[x] = (entry & 0x2000) ? zhi : zlo;
  }
}

static void render_mode7(int line, bool extbg, uint8_t zlo, uint8_t zhi) {
  int a = P.m7a, b = P.m7b, c = P.m7c, d = P.m7d;
  int cx = P.m7x, cy = P.m7y;
  int hofs = P.m7hofs, vofs = P.m7vofs;
  int msize = (P.mosaic >> 4) + 1;
  bool mos = (P.mosaic & (extbg ? 2 : 1)) && msize > 1;
  int yy = mos ? 1 + ((line - 1) / msize) * msize : line;
  int y = (P.m7sel & 2) ? 255 - yy : yy;
#define CLIP(n) (((n) & 0x2000) ? ((n) | ~1023) : ((n) & 1023))
  int hc = CLIP(hofs - cx), vc = CLIP(vofs - cy);
#undef CLIP
  int psx = ((a * hc) & ~63) + ((b * vc) & ~63) + ((b * y) & ~63) + (cx << 8);
  int psy = ((c * hc) & ~63) + ((d * vc) & ~63) + ((d * y) & ~63) + (cy << 8);
  int over = P.m7sel >> 6;
  bool direct = !extbg && (P.cgwsel & 1);
  for (int x = 0; x < 256; x++) {
    int sx = mos ? x - (x % msize) : x;
    int tx = (P.m7sel & 1) ? 255 - sx : sx;
    int px = (psx + a * tx) >> 8, py = (psy + c * tx) >> 8;
    uint8_t pix;
    if ((px | py) & ~1023) {
      if (over == 2) { lz[x] = 0; continue; }
      if (over == 3) { pix = P.vram[((py & 7) << 3) | (px & 7)] >> 8; goto have; }
    }
    px &= 1023; py &= 1023;
    {
      uint8_t tile = P.vram[((py >> 3) << 7) | (px >> 3)] & 0xFF;
      pix = P.vram[(tile << 6) | ((py & 7) << 3) | (px & 7)] >> 8;
    }
  have:
    if (extbg) {
      if (!(pix & 0x7F)) { lz[x] = 0; continue; }
      lcol[x] = P.cgram[pix & 0x7F];
      lz[x] = (pix & 0x80) ? zhi : zlo;
    } else {
      if (!pix) { lz[x] = 0; continue; }
      lcol[x] = direct ? direct_color(pix, 0) : P.cgram[pix];
      lz[x] = zlo;
    }
  }
}

static const uint8_t obj_dims[8][2][2] = {
  {{8,8},{16,16}}, {{8,8},{32,32}}, {{8,8},{64,64}}, {{16,16},{32,32}},
  {{16,16},{64,64}}, {{32,32},{64,64}}, {{16,32},{32,64}}, {{16,32},{32,32}}};

static void render_obj(int line, const uint8_t *zobj) {
  memset(lz, 0, sizeof(lz));
  uint8_t list[32];
  int count = 0;
  int first = P.oam_prio_rot ? ((P.oam_addr_reload >> 1) & 0x7F) : 0;
  int sel = P.obsel >> 5;
  for (int i = 0; i < 128; i++) {
    int n = (first + i) & 127;
    const uint8_t *o = &P.oam[n * 4];
    uint8_t hi = (P.oam[0x200 + (n >> 2)] >> ((n & 3) * 2)) & 3;
    int x = o[0] | ((hi & 1) << 8);
    int w = obj_dims[sel][hi >> 1][0], h = obj_dims[sel][hi >> 1][1];
    if (x > 256 && (x + w - 1) < 512) continue;
    int row = (line - 1 - o[1]) & 0xFF;
    if (row >= h) continue;
    if (count == 32) { P.stat77 |= 0x40; break; }
    list[count++] = n;
  }
  uint16_t base = (P.obsel & 7) << 13;
  uint16_t name_off = (((P.obsel >> 3) & 3) + 1) << 12;
  int tiles = 0;
  // fetch in reverse order; later (higher priority) sprites overwrite earlier ones
  static uint16_t ocol[256]; static uint8_t oprio[256], opal[256], oset[256];
  memset(oset, 0, sizeof(oset));
  for (int k = count - 1; k >= 0; k--) {
    int n = list[k];
    const uint8_t *o = &P.oam[n * 4];
    uint8_t hi = (P.oam[0x200 + (n >> 2)] >> ((n & 3) * 2)) & 3;
    int x = o[0] | ((hi & 1) << 8);
    if (x >= 256) x -= 512;
    int w = obj_dims[sel][hi >> 1][0], h = obj_dims[sel][hi >> 1][1];
    uint8_t attr = o[3];
    int row = (line - 1 - o[1]) & 0xFF;
    if (attr & 0x80) row = h - 1 - row;
    int tyy = row >> 3, fy = row & 7;
    int pal = (attr >> 1) & 7, prio = (attr >> 4) & 3;
    bool hf = attr & 0x40;
    int ncols = w >> 3;
    for (int cidx = 0; cidx < ncols; cidx++) {
      int sx = x + cidx * 8;
      if (sx <= -8 || sx >= 256) continue;
      if (++tiles > 34) { P.stat77 |= 0x80; goto done; }
      int col = hf ? ncols - 1 - cidx : cidx;
      int ch = o[2];
      int tile = ((((ch >> 4) + tyy) & 0xF) << 4) | (((ch & 0xF) + col) & 0xF);
      uint16_t addr = (base + ((attr & 1) ? name_off : 0) + tile * 16 + fy) & 0x7FFF;
      uint16_t w0 = P.vram[addr], w1 = P.vram[(addr + 8) & 0x7FFF];
      for (int px = 0; px < 8; px++) {
        int xx = sx + px;
        if (xx < 0 || xx >= 256) continue;
        int bit = hf ? px : 7 - px;
        int pix = ((w0 >> bit) & 1) | (((w0 >> (8 + bit)) & 1) << 1) |
                  (((w1 >> bit) & 1) << 2) | (((w1 >> (8 + bit)) & 1) << 3);
        if (!pix) continue;
        ocol[xx] = P.cgram[128 + pal * 16 + pix];
        oprio[xx] = prio; opal[xx] = pal; oset[xx] = 1;
      }
    }
  }
done:
  for (int x = 0; x < 256; x++) {
    if (!oset[x]) continue;
    lcol[x] = ocol[x];
    lz[x] = zobj[oprio[x]];
    lmath[x] = opal[x] >= 4;
  }
}

static inline uint8_t layer_window_sel(int layer) {
  switch (layer) {
    case 0: return P.w12sel & 0xF;
    case 1: return P.w12sel >> 4;
    case 2: return P.w34sel & 0xF;
    case 3: return P.w34sel >> 4;
    default: return P.wobjsel & 0xF;
  }
}
static inline uint8_t layer_window_log(int layer) {
  return layer < 4 ? (P.wbglog >> (layer * 2)) & 3 : P.wobjlog & 3;
}

static void merge_layer(int layer) {
  uint8_t bit = 1 << layer;
  bool on_main = P.tm & bit, on_sub = P.ts & bit;
  bool win = (P.tmw & bit) || (P.tsw & bit);
  if (win) calc_window(layer_window_sel(layer), layer_window_log(layer), wmask);
  for (int x = 0; x < 256; x++) {
    uint8_t z = lz[x];
    if (!z) continue;
    uint8_t lay = layer;
    if (layer == 4 && !lmath[x]) lay = 6;  // sprite palettes 0-3: never participate in math
    if (on_main && z > mainl[x].z && !((P.tmw & bit) && wmask[x])) { mainl[x].col = lcol[x]; mainl[x].z = z; mainl[x].layer = lay; }
    if (on_sub && z > subl[x].z && !((P.tsw & bit) && wmask[x])) { subl[x].col = lcol[x]; subl[x].z = z; subl[x].layer = lay; }
  }
}

static uint32_t bright_lut[16][32];
static bool lut_ready;

void ppu_render_line(int line) {
  uint32_t *out = &g->fb[(line - 1) * SNES_W];
  if (!lut_ready) {
    for (int b = 0; b < 16; b++)
      for (int c = 0; c < 32; c++) {
        int v = c * b / 15;
        bright_lut[b][c] = (v << 3) | (v >> 2);
      }
    lut_ready = true;
  }
  if (P.inidisp & 0x80) {
    for (int x = 0; x < 256; x++) out[x] = 0xFF000000;
    return;
  }
  int mode = P.bgmode & 7;
  uint16_t backdrop = P.cgram[0];
  for (int x = 0; x < 256; x++) {
    mainl[x].col = backdrop; mainl[x].z = 0; mainl[x].layer = 5;
    subl[x].col = P.fixed_color; subl[x].z = 0; subl[x].layer = 5;
  }
  static const uint8_t bpp_tab[8][4] = {{2,2,2,2},{4,4,2,0},{4,4,0,0},{8,4,0,0},{8,2,0,0},{4,2,0,0},{4,0,0,0},{0,0,0,0}};
  uint8_t zobj[4];
  uint8_t zbg[4][2];
  memset(zbg, 0, sizeof(zbg));
  switch (mode) {
    case 0:
      zbg[0][0]=8; zbg[0][1]=11; zbg[1][0]=7; zbg[1][1]=10; zbg[2][0]=2; zbg[2][1]=5; zbg[3][0]=1; zbg[3][1]=4;
      zobj[0]=3; zobj[1]=6; zobj[2]=9; zobj[3]=12; break;
    case 1:
      zbg[0][0]=8; zbg[0][1]=11; zbg[1][0]=7; zbg[1][1]=10; zbg[2][0]=3; zbg[2][1]=(P.bgmode & 8) ? 13 : 5;
      zobj[0]=4; zobj[1]=6; zobj[2]=9; zobj[3]=12; break;
    case 6:
      zbg[0][0]=2; zbg[0][1]=5; zobj[0]=1; zobj[1]=3; zobj[2]=4; zobj[3]=6; break;
    case 7:
      zbg[0][0]=3; zbg[0][1]=3; zbg[1][0]=1; zbg[1][1]=5; zobj[0]=2; zobj[1]=4; zobj[2]=6; zobj[3]=7; break;
    default:
      zbg[0][0]=3; zbg[0][1]=7; zbg[1][0]=1; zbg[1][1]=5; zobj[0]=2; zobj[1]=4; zobj[2]=6; zobj[3]=8; break;
  }
  uint8_t used = P.tm | P.ts;
  bool hires = mode == 5 || mode == 6;
  memset(lmath, 1, sizeof(lmath));
  for (int i = 0; i < 4; i++) {
    if (!(used & (1 << i))) continue;
    if (mode == 7) {
      if (i == 0) render_mode7(line, false, zbg[0][0], zbg[0][1]);
      else if (i == 1 && (P.setini & 0x40)) render_mode7(line, true, zbg[1][0], zbg[1][1]);
      else continue;
    } else {
      int bpp = bpp_tab[mode][i];
      if (!bpp) continue;
      render_bg(i, bpp, line, zbg[i][0], zbg[i][1], hires);
    }
    merge_layer(i);
  }
  if (used & 0x10) {
    render_obj(line, zobj);
    merge_layer(4);
    memset(lmath, 1, sizeof(lmath));
  }

  // color window
  static bool cwin[256];
  calc_window(P.wobjsel >> 4, (P.wobjlog >> 2) & 3, cwin);
  int clip_mode = P.cgwsel >> 6, math_mode = (P.cgwsel >> 4) & 3;
  bool use_sub = P.cgwsel & 2;
  bool subtract = P.cgadsub & 0x80, half = P.cgadsub & 0x40;
  int bright = P.inidisp & 0xF;
  for (int x = 0; x < 256; x++) {
    uint16_t c = mainl[x].col;
    bool w = cwin[x];
    bool black = clip_mode == 3 || (clip_mode == 1 && !w) || (clip_mode == 2 && w);
    bool math_on = math_mode == 0 || (math_mode == 1 && w) || (math_mode == 2 && !w);
    if (black) c = 0;
    uint8_t lay = mainl[x].layer;
    if (math_on && lay != 6 && (P.cgadsub & (1 << lay))) {
      uint16_t s; bool h = half && !black;
      if (use_sub) {
        if (subl[x].layer == 5) { s = P.fixed_color; h = false; }
        else s = subl[x].col;
      } else s = P.fixed_color;
      int r = c & 0x1F, gg = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
      int sr = s & 0x1F, sg = (s >> 5) & 0x1F, sb = (s >> 10) & 0x1F;
      if (!subtract) { r += sr; gg += sg; b += sb; }
      else { r -= sr; gg -= sg; b -= sb; if (r < 0) r = 0; if (gg < 0) gg = 0; if (b < 0) b = 0; }
      if (h) { r >>= 1; gg >>= 1; b >>= 1; }
      if (r > 31) r = 31; if (gg > 31) gg = 31; if (b > 31) b = 31;
      c = r | (gg << 5) | (b << 10);
    }
    out[x] = 0xFF000000u | (bright_lut[bright][(c >> 10) & 0x1F]) | (bright_lut[bright][(c >> 5) & 0x1F] << 8) |
             (bright_lut[bright][c & 0x1F] << 16);
  }
}

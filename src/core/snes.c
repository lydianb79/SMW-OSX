// System bus, timing, DMA/HDMA, CPU I/O registers, cartridge mapping, save states.
#include "snes.h"
#include <stdlib.h>
#include <string.h>

Snes *g;

#define STATE_MAGIC 0x534D5752u  // 'SMWR'
#define STATE_VERSION 3u

static const uint8_t dma_off[8][4] = {
  {0,0,0,0},{0,1,0,1},{0,0,0,0},{0,0,1,1},{0,1,2,3},{0,1,0,1},{0,0,0,0},{0,0,1,1}};
static const uint8_t dma_len[8] = {1,2,2,4,4,4,2,4};

// ---------------------------------------------------------------- cartridge
static inline uint32_t rom_index(uint32_t off) {
  if (off < g->rom_size) return off;
  if (g->rom_mask) return off & g->rom_mask;
  return off % g->rom_size;
}

static uint8_t cart_read(uint8_t bank, uint16_t off, bool *ok) {
  *ok = true;
  if (!g->hirom) {
    if (off >= 0x8000) return g->rom[rom_index(((uint32_t)(bank & 0x7F) << 15) | (off & 0x7FFF))];
    uint8_t b = bank & 0x7F;
    if (b >= 0x70 && b < 0x7E && g->sram_size)
      return g->sram[(((uint32_t)(b & 0xF) << 15) | off) & (g->sram_size - 1)];
    if (b >= 0x40) return g->rom[rom_index(((uint32_t)(bank & 0x7F) << 15) | (off & 0x7FFF))];
  } else {
    uint8_t b = bank & 0x7F;
    if (b >= 0x40 || off >= 0x8000) return g->rom[rom_index(((uint32_t)(bank & 0x3F) << 16) | off)];
    if (b >= 0x20 && off >= 0x6000 && g->sram_size)
      return g->sram[((((uint32_t)b & 0x1F) << 13) | (off - 0x6000)) & (g->sram_size - 1)];
  }
  *ok = false;
  return g->mdr;
}

static void cart_write(uint8_t bank, uint16_t off, uint8_t v) {
  if (!g->sram_size) return;
  uint8_t b = bank & 0x7F;
  if (!g->hirom) {
    if (b >= 0x70 && b < 0x7E && off < 0x8000)
      g->sram[(((uint32_t)(b & 0xF) << 15) | off) & (g->sram_size - 1)] = v;
  } else {
    if (b >= 0x20 && b < 0x40 && off >= 0x6000 && off < 0x8000)
      g->sram[((((uint32_t)b & 0x1F) << 13) | (off - 0x6000)) & (g->sram_size - 1)] = v;
  }
}

// ---------------------------------------------------------------- timing
static uint8_t bbus_read(uint8_t a);
static void bbus_write(uint8_t a, uint8_t v);
static uint8_t abus_read(uint32_t a);
static void abus_write(uint32_t a, uint8_t v);

static void hdma_init(void) {
  for (int c = 0; c < 8; c++) {
    DmaChannel *d = &g->dma[c];
    d->hdma_completed = false;
    d->hdma_do_transfer = true;
    if (!(g->hdmaen & (1 << c))) continue;
    d->a2a = d->a1t;
    d->nltr = 0;
    // reload
    uint8_t v = abus_read(((uint32_t)d->a1b << 16) | d->a2a);
    d->nltr = v; d->a2a++;
    d->hdma_completed = (v == 0);
    d->hdma_do_transfer = !d->hdma_completed;
    if (d->dmap & 0x40) {
      uint8_t lo = abus_read(((uint32_t)d->a1b << 16) | d->a2a++);
      uint8_t hi = abus_read(((uint32_t)d->a1b << 16) | d->a2a++);
      d->das = lo | (hi << 8);
    }
  }
}

static int hdma_run(void) {
  int cyc = 0;
  bool any = false;
  for (int c = 0; c < 8; c++) {
    DmaChannel *d = &g->dma[c];
    if (!(g->hdmaen & (1 << c)) || d->hdma_completed) continue;
    any = true;
    if (d->hdma_do_transfer) {
      int mode = d->dmap & 7;
      for (int i = 0; i < dma_len[mode]; i++) {
        uint32_t addr;
        if (d->dmap & 0x40) addr = ((uint32_t)d->dasb << 16) | d->das++;
        else addr = ((uint32_t)d->a1b << 16) | d->a2a++;
        uint8_t b = d->bbad + dma_off[mode][i];
        if (d->dmap & 0x80) abus_write(addr, bbus_read(b));
        else bbus_write(b, abus_read(addr));
        cyc += 8;
      }
    }
  }
  for (int c = 0; c < 8; c++) {
    DmaChannel *d = &g->dma[c];
    if (!(g->hdmaen & (1 << c)) || d->hdma_completed) continue;
    d->nltr--;
    d->hdma_do_transfer = d->nltr & 0x80;
    if ((d->nltr & 0x7F) == 0) {
      uint8_t v = abus_read(((uint32_t)d->a1b << 16) | d->a2a++);
      d->nltr = v;
      d->hdma_completed = (v == 0);
      d->hdma_do_transfer = true;
      if (d->dmap & 0x40) {
        uint8_t lo = abus_read(((uint32_t)d->a1b << 16) | d->a2a++);
        uint8_t hi = abus_read(((uint32_t)d->a1b << 16) | d->a2a++);
        d->das = lo | (hi << 8);
        cyc += 16;
      }
      cyc += 8;
    }
  }
  return any ? cyc + 18 : 0;
}

static void new_line(void) {
  g->vpos++;
  if (g->vpos >= 262) {
    g->vpos = 0;
    g->frame_done = true;
    g->frames++;
    g->nmi_flag = false;
    g->ppu.stat77 &= 0x3F;
    g->vdisp = (g->ppu.setini & 0x04) ? 239 : 224;
    g->fb_height = g->vdisp;
  }
  int line = g->vpos;
  if (line == 0) hdma_init();
  if (line >= 1 && line <= g->vdisp) ppu_render_line(line);
  if (line == g->vdisp + 1) {
    g->nmi_flag = true;
    if (g->nmitimen & 0x80) g->cpu.nmi_pending = true;
    ppu_vblank_start();
    if (g->nmitimen & 1) {
      g->joy_auto[0] = g->pad[0];
      g->joy_auto[1] = g->pad[1];
      g->joy_auto[2] = g->joy_auto[3] = 0;
      g->joy_busy_until = g->master + 4224;
    }
  }
  int irqmode = (g->nmitimen >> 4) & 3;
  if (irqmode == 2 && line == g->vtime) g->irq_flag = true;
  if (irqmode == 3 && line == g->vtime && g->htime == 0) g->irq_flag = true;
  if (irqmode == 1 && g->htime == 0) g->irq_flag = true;
  apu_catch_up();
}

void snes_tick(int cycles) {
  g->master += cycles;
  int old = g->hpos;
  int now = old + cycles;
  int irqmode = (g->nmitimen >> 4) & 3;
  if ((irqmode == 1 || (irqmode == 3 && g->vpos == g->vtime)) && g->htime) {
    int target = g->htime * 4 + 14;
    if (old < target && now >= target) g->irq_flag = true;
  }
  if (old < 1104 && now >= 1104 && g->vpos <= g->vdisp) {
    int extra = hdma_run();
    now += extra;
    g->master += extra;
  }
  while (now >= 1364) {
    now -= 1364;
    g->hpos = (uint16_t)now;
    new_line();
  }
  g->hpos = (uint16_t)now;
}

static inline int access_cycles(uint32_t a) {
  uint8_t bank = a >> 16;
  uint16_t off = a & 0xFFFF;
  if (!(bank & 0x40)) {
    if (off < 0x2000) return 8;
    if (off < 0x4000) return 6;
    if (off < 0x4200) return 12;
    if (off < 0x6000) return 6;
    if (off < 0x8000) return 8;
    return ((bank & 0x80) && (g->memsel & 1)) ? 6 : 8;
  }
  if (bank >= 0xC0) return (g->memsel & 1) ? 6 : 8;
  return 8;
}

// ---------------------------------------------------------------- registers
static uint8_t bbus_read(uint8_t a) {
  if (a >= 0x34 && a <= 0x3F) return ppu_read(0x2100 | a);
  if (a >= 0x40 && a <= 0x7F) { apu_catch_up(); return apu_cpu_read(a & 3); }
  if (a == 0x80) { uint8_t v = g->wram[g->wram_addr]; g->wram_addr = (g->wram_addr + 1) & 0x1FFFF; return v; }
  return g->mdr;
}

static void bbus_write(uint8_t a, uint8_t v) {
  if (a <= 0x33) { ppu_write(0x2100 | a, v); return; }
  if (a >= 0x40 && a <= 0x7F) { apu_catch_up(); apu_cpu_write(a & 3, v); return; }
  switch (a) {
    case 0x80: g->wram[g->wram_addr] = v; g->wram_addr = (g->wram_addr + 1) & 0x1FFFF; break;
    case 0x81: g->wram_addr = (g->wram_addr & 0x1FF00) | v; break;
    case 0x82: g->wram_addr = (g->wram_addr & 0x100FF) | (v << 8); break;
    case 0x83: g->wram_addr = (g->wram_addr & 0x0FFFF) | ((v & 1) << 16); break;
  }
}

static uint8_t io_read(uint16_t off) {
  if (off >= 0x2100 && off < 0x2200) return bbus_read(off & 0xFF);
  if (off == 0x4016) {
    uint8_t b = (g->joy_shift[0] >> 31) & 1;
    if (!g->joy_strobe) g->joy_shift[0] = (g->joy_shift[0] << 1) | 1;
    return (g->mdr & 0xFC) | b;
  }
  if (off == 0x4017) {
    uint8_t b = (g->joy_shift[1] >> 31) & 1;
    if (!g->joy_strobe) g->joy_shift[1] = (g->joy_shift[1] << 1) | 1;
    return (g->mdr & 0xE0) | 0x1C | b;
  }
  if (off >= 0x4300 && off < 0x4380) {
    DmaChannel *d = &g->dma[(off >> 4) & 7];
    switch (off & 0xF) {
      case 0: return d->dmap; case 1: return d->bbad;
      case 2: return d->a1t & 0xFF; case 3: return d->a1t >> 8; case 4: return d->a1b;
      case 5: return d->das & 0xFF; case 6: return d->das >> 8; case 7: return d->dasb;
      case 8: return d->a2a & 0xFF; case 9: return d->a2a >> 8; case 0xA: return d->nltr;
      case 0xB: case 0xF: return d->unused;
    }
    return g->mdr;
  }
  switch (off) {
    case 0x4210: { uint8_t v = (g->mdr & 0x70) | (g->nmi_flag ? 0x80 : 0) | 0x02; g->nmi_flag = false; return v; }
    case 0x4211: { uint8_t v = (g->mdr & 0x7F) | (g->irq_flag ? 0x80 : 0); g->irq_flag = false; return v; }
    case 0x4212: {
      uint8_t v = g->mdr & 0x3E;
      if (g->vpos > g->vdisp) v |= 0x80;
      if (g->hpos >= 1096 || g->hpos < 4) v |= 0x40;
      if (g->master < g->joy_busy_until) v |= 0x01;
      return v;
    }
    case 0x4213: return g->wrio;
    case 0x4214: return g->rddiv & 0xFF;
    case 0x4215: return g->rddiv >> 8;
    case 0x4216: return g->rdmpy & 0xFF;
    case 0x4217: return g->rdmpy >> 8;
    case 0x4218: case 0x421A: case 0x421C: case 0x421E: return g->joy_auto[(off - 0x4218) >> 1] & 0xFF;
    case 0x4219: case 0x421B: case 0x421D: case 0x421F: return g->joy_auto[(off - 0x4219) >> 1] >> 8;
  }
  return g->mdr;
}

static void run_dma(uint8_t mask) {
  snes_tick(8);
  for (int c = 0; c < 8; c++) {
    if (!(mask & (1 << c))) continue;
    DmaChannel *d = &g->dma[c];
    int mode = d->dmap & 7;
    int step = (d->dmap & 0x08) ? 0 : ((d->dmap & 0x10) ? -1 : 1);
    int i = 0;
    snes_tick(8);
    do {
      uint8_t b = d->bbad + dma_off[mode][i & 3];
      uint32_t addr = ((uint32_t)d->a1b << 16) | d->a1t;
      if (d->dmap & 0x80) abus_write(addr, bbus_read(b));
      else bbus_write(b, abus_read(addr));
      snes_tick(8);
      d->a1t += step;
      d->das--;
      i++;
    } while (d->das != 0);
  }
}

static void io_write(uint16_t off, uint8_t v) {
  if (off >= 0x2100 && off < 0x2200) { bbus_write(off & 0xFF, v); return; }
  if (off == 0x4016) {
    bool ns = v & 1;
    if (g->joy_strobe && !ns) {
      g->joy_shift[0] = ((uint32_t)g->pad[0] << 16) | 0xFFFF;
      g->joy_shift[1] = ((uint32_t)g->pad[1] << 16) | 0xFFFF;
    }
    g->joy_strobe = ns;
    if (ns) {
      g->joy_shift[0] = ((uint32_t)g->pad[0] << 16) | 0xFFFF;
      g->joy_shift[1] = ((uint32_t)g->pad[1] << 16) | 0xFFFF;
    }
    return;
  }
  if (off >= 0x4300 && off < 0x4380) {
    DmaChannel *d = &g->dma[(off >> 4) & 7];
    switch (off & 0xF) {
      case 0: d->dmap = v; break; case 1: d->bbad = v; break;
      case 2: d->a1t = (d->a1t & 0xFF00) | v; break; case 3: d->a1t = (d->a1t & 0xFF) | (v << 8); break;
      case 4: d->a1b = v; break;
      case 5: d->das = (d->das & 0xFF00) | v; break; case 6: d->das = (d->das & 0xFF) | (v << 8); break;
      case 7: d->dasb = v; break;
      case 8: d->a2a = (d->a2a & 0xFF00) | v; break; case 9: d->a2a = (d->a2a & 0xFF) | (v << 8); break;
      case 0xA: d->nltr = v; break;
      case 0xB: case 0xF: d->unused = v; break;
    }
    return;
  }
  switch (off) {
    case 0x4200: {
      bool old_nmi = g->nmitimen & 0x80;
      g->nmitimen = v;
      if (!(v & 0x30)) g->irq_flag = false;
      if (!old_nmi && (v & 0x80) && g->nmi_flag) g->cpu.nmi_pending = true;
      break;
    }
    case 0x4201:
      if ((g->wrio & 0x80) && !(v & 0x80)) ppu_latch_counters();
      g->wrio = v; break;
    case 0x4202: g->wrmpya = v; break;
    case 0x4203: g->wrmpyb = v; g->rdmpy = g->wrmpya * v; g->rddiv = v; break;
    case 0x4204: g->wrdiv = (g->wrdiv & 0xFF00) | v; break;
    case 0x4205: g->wrdiv = (g->wrdiv & 0xFF) | (v << 8); break;
    case 0x4206:
      g->wrdivb = v;
      if (v) { g->rddiv = g->wrdiv / v; g->rdmpy = g->wrdiv % v; }
      else { g->rddiv = 0xFFFF; g->rdmpy = g->wrdiv; }
      break;
    case 0x4207: g->htime = (g->htime & 0x100) | v; break;
    case 0x4208: g->htime = (g->htime & 0xFF) | ((v & 1) << 8); break;
    case 0x4209: g->vtime = (g->vtime & 0x100) | v; break;
    case 0x420A: g->vtime = (g->vtime & 0xFF) | ((v & 1) << 8); break;
    case 0x420B: run_dma(v); break;
    case 0x420C: g->hdmaen = v; break;
    case 0x420D: g->memsel = v; break;
  }
}

// ---------------------------------------------------------------- bus
static uint8_t raw_read(uint32_t a) {
  uint8_t bank = a >> 16;
  uint16_t off = a & 0xFFFF;
  if (bank == 0x7E || bank == 0x7F) return g->wram[a & 0x1FFFF];
  if (!(bank & 0x40)) {
    if (off < 0x2000) return g->wram[off];
    if (off < 0x6000) return io_read(off);
    if (off < 0x8000 && !g->hirom) return g->mdr;
  }
  bool ok;
  return cart_read(bank, off, &ok);
}

static void raw_write(uint32_t a, uint8_t v) {
  uint8_t bank = a >> 16;
  uint16_t off = a & 0xFFFF;
  if (bank == 0x7E || bank == 0x7F) { g->wram[a & 0x1FFFF] = v; return; }
  if (!(bank & 0x40)) {
    if (off < 0x2000) { g->wram[off] = v; return; }
    if (off < 0x6000) { io_write(off, v); return; }
  }
  cart_write(bank, off, v);
}

static inline bool is_bbus_or_dma(uint32_t a) {
  uint8_t bank = a >> 16;
  uint16_t off = a & 0xFFFF;
  if (bank & 0x40) return false;
  return (off >= 0x2100 && off < 0x2200) || (off >= 0x4300 && off < 0x4380) || off == 0x420B || off == 0x420C;
}

static uint8_t abus_read(uint32_t a) { return is_bbus_or_dma(a) ? g->mdr : raw_read(a); }
static void abus_write(uint32_t a, uint8_t v) { if (!is_bbus_or_dma(a)) raw_write(a, v); }

uint8_t bus_read(uint32_t addr) {
  snes_tick(access_cycles(addr));
  uint8_t v = raw_read(addr & 0xFFFFFF);
  g->mdr = v;
  return v;
}

void bus_write(uint32_t addr, uint8_t v) {
  snes_tick(access_cycles(addr));
  g->mdr = v;
  raw_write(addr & 0xFFFFFF, v);
}

void bus_idle(void) { snes_tick(6); }

// ---------------------------------------------------------------- lifecycle
static int score_header(const uint8_t *r, size_t size, uint32_t off, bool hi) {
  if (off + 0x40 > size) return -100;
  const uint8_t *h = r + off;
  int s = 0;
  uint16_t reset = h[0x3C] | (h[0x3D] << 8);
  if (reset >= 0x8000) s += 3;
  uint8_t map = h[0x15];
  if (hi && (map & 0x0F) == 1) s += 2;
  if (!hi && (map & 0x0F) == 0) s += 2;
  uint16_t chk = h[0x1E] | (h[0x1F] << 8), cmp = h[0x1C] | (h[0x1D] << 8);
  if ((uint16_t)(chk ^ cmp) == 0xFFFF) s += 4;
  for (int i = 0; i < 21; i++) if (h[i] < 0x20 || h[i] > 0x7E) { s -= 1; break; }
  return s;
}

Snes *snes_create(const uint8_t *data, size_t size, char *title, size_t tlen) {
  if (size & 0x3FF) { data += 512; size -= 512; }   // strip copier header
  if (size < 0x8000) return NULL;
  Snes *s = calloc(1, sizeof(Snes));
  uint8_t *rom = malloc(size);
  memcpy(rom, data, size);
  s->rom = rom;
  s->rom_size = (uint32_t)size;
  s->rom_mask = ((size & (size - 1)) == 0) ? (uint32_t)size - 1 : 0;
  int lo = score_header(rom, size, 0x7FC0, false), hi = score_header(rom, size, 0xFFC0, true);
  s->hirom = hi > lo;
  const uint8_t *h = rom + (s->hirom ? 0xFFC0 : 0x7FC0);
  uint8_t sram_code = h[0x18];
  s->sram_size = sram_code ? (1024u << (sram_code > 5 ? 5 : sram_code)) : 0;
  if (title && tlen) {
    size_t n = tlen - 1 < 21 ? tlen - 1 : 21;
    memcpy(title, h, n); title[n] = 0;
    for (int i = (int)n - 1; i >= 0 && title[i] == ' '; i--) title[i] = 0;
  }
  s->audio_cap = 2048;
  s->audio_out = calloc(s->audio_cap * 2, sizeof(int16_t));
  memset(s->sram, 0xFF, sizeof(s->sram));
  snes_reset(s, true);
  return s;
}

void snes_destroy(Snes *s) {
  if (!s) return;
  free((void *)s->rom);
  free(s->audio_out);
  if (g == s) g = NULL;
  free(s);
}

void snes_reset(Snes *s, bool hard) {
  g = s;
  s->magic = STATE_MAGIC;
  s->version = STATE_VERSION;
  if (hard) {
    memset(s->wram, 0x55, sizeof(s->wram));
    memset(s->dma, 0xFF, sizeof(s->dma));
    for (int i = 0; i < 8; i++) { s->dma[i].hdma_completed = false; s->dma[i].hdma_do_transfer = false; }
  }
  s->nmitimen = 0; s->wrio = 0xFF; s->hdmaen = 0; s->memsel = 0;
  s->nmi_flag = s->irq_flag = false;
  s->htime = s->vtime = 0x1FF;
  s->hpos = 0; s->vpos = 0; s->vdisp = 224; s->fb_height = 224;
  s->wram_addr = 0;
  ppu_reset();
  apu_reset();
  s->apu_master_sync = 0;
  s->master = 0;
  cpu_reset();
}

void snes_set_pad(Snes *s, int port, uint16_t b) {
  // never report opposite directions at once
  if ((b & (BTN_LEFT | BTN_RIGHT)) == (BTN_LEFT | BTN_RIGHT)) b &= ~(BTN_LEFT | BTN_RIGHT);
  if ((b & (BTN_UP | BTN_DOWN)) == (BTN_UP | BTN_DOWN)) b &= ~(BTN_UP | BTN_DOWN);
  s->pad[port & 1] = b;
}

const uint32_t *snes_framebuffer(const Snes *s) { return s->fb; }
uint8_t *snes_sram(Snes *s) { return s->sram; }

void snes_run_frame(Snes *s) {
  g = s;
  s->audio_count = 0;
  s->frame_done = false;
  int guard = 0;
  while (!s->frame_done && guard++ < 2000000) cpu_step();
  apu_catch_up();
}

// ---------------------------------------------------------------- save states
size_t snes_state_size(void) { return offsetof(Snes, pad); }

bool snes_save_state(Snes *s, void *buf, size_t len) {
  if (len < snes_state_size()) return false;
  memcpy(buf, s, snes_state_size());
  return true;
}

bool snes_load_state(Snes *s, const void *buf, size_t len) {
  if (len < snes_state_size()) return false;
  const Snes *in = buf;
  if (in->magic != STATE_MAGIC || in->version != STATE_VERSION) return false;
  memcpy(s, buf, snes_state_size());
  g = s;
  return true;
}

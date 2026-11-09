// WDC 65C816 interpreter.
#include "snes.h"

#define C (g->cpu)
enum { FC = 0x01, FZ = 0x02, FI = 0x04, FD = 0x08, FX = 0x10, FM = 0x20, FV = 0x40, FN = 0x80 };
#define MF (C.p & FM)
#define XF (C.p & FX)

static inline uint8_t rd8(uint32_t a) { return bus_read(a & 0xFFFFFF); }
static inline uint16_t rd16(uint32_t a) { uint8_t l = rd8(a); return l | (rd8(a + 1) << 8); }
static inline uint16_t rd16b0(uint16_t a) { uint8_t l = rd8(a); return l | (rd8((uint16_t)(a + 1)) << 8); }
static inline void wr8(uint32_t a, uint8_t v) { bus_write(a & 0xFFFFFF, v); }
static inline void wr16(uint32_t a, uint16_t v) { wr8(a, v & 0xFF); wr8(a + 1, v >> 8); }
static inline void idle(void) { bus_idle(); }

static inline uint8_t fetch8(void) { uint8_t v = rd8(((uint32_t)C.pb << 16) | C.pc); C.pc++; return v; }
static inline uint16_t fetch16(void) { uint16_t l = fetch8(); return l | (fetch8() << 8); }
static inline uint32_t fetch24(void) { uint32_t l = fetch16(); return l | ((uint32_t)fetch8() << 16); }

static inline void push8(uint8_t v) {
  wr8(C.sp, v);
  if (C.e) C.sp = 0x100 | ((C.sp - 1) & 0xFF); else C.sp--;
}
static inline uint8_t pull8(void) {
  if (C.e) C.sp = 0x100 | ((C.sp + 1) & 0xFF); else C.sp++;
  return rd8(C.sp);
}
static inline void push16(uint16_t v) { push8(v >> 8); push8(v & 0xFF); }
static inline uint16_t pull16(void) { uint16_t l = pull8(); return l | (pull8() << 8); }

static inline void nz8(uint8_t v) { C.p = (C.p & ~(FN | FZ)) | (v & 0x80) | (v ? 0 : FZ); }
static inline void nz16(uint16_t v) { C.p = (C.p & ~(FN | FZ)) | ((v >> 8) & 0x80) | (v ? 0 : FZ); }
static inline void setf(uint8_t f, bool on) { if (on) C.p |= f; else C.p &= ~f; }

static void set_p(uint8_t v) {
  if (C.e) v |= FM | FX;
  C.p = v;
  if (XF) { C.x &= 0xFF; C.y &= 0xFF; }
}

// ------------------------------------------------------------ addressing
static uint32_t am_immM(void) { uint32_t a = ((uint32_t)C.pb << 16) | C.pc; C.pc += MF ? 1 : 2; return a; }
static uint32_t am_immX(void) { uint32_t a = ((uint32_t)C.pb << 16) | C.pc; C.pc += XF ? 1 : 2; return a; }
static uint32_t am_dp(void) { uint8_t o = fetch8(); if (C.dp & 0xFF) idle(); return (uint16_t)(C.dp + o); }
static uint32_t am_dpx(void) { uint8_t o = fetch8(); if (C.dp & 0xFF) idle(); idle(); return (uint16_t)(C.dp + o + C.x); }
static uint32_t am_dpy(void) { uint8_t o = fetch8(); if (C.dp & 0xFF) idle(); idle(); return (uint16_t)(C.dp + o + C.y); }
static uint32_t am_dpi(void) { uint16_t p = am_dp(); return ((uint32_t)C.db << 16) | rd16b0(p); }
static uint32_t am_dpxi(void) { uint16_t p = am_dpx(); return ((uint32_t)C.db << 16) | rd16b0(p); }
static uint32_t am_dpiy(void) { uint16_t p = am_dp(); uint32_t b = ((uint32_t)C.db << 16) | rd16b0(p); idle(); return (b + C.y) & 0xFFFFFF; }
static uint32_t am_dpil(void) { uint16_t p = am_dp(); uint32_t l = rd16b0(p); return l | ((uint32_t)rd8((uint16_t)(p + 2)) << 16); }
static uint32_t am_dpily(void) { return (am_dpil() + C.y) & 0xFFFFFF; }
static uint32_t am_abs(void) { return ((uint32_t)C.db << 16) | fetch16(); }
static uint32_t am_absx(void) { uint32_t b = am_abs(); idle(); return (b + C.x) & 0xFFFFFF; }
static uint32_t am_absy(void) { uint32_t b = am_abs(); idle(); return (b + C.y) & 0xFFFFFF; }
static uint32_t am_long(void) { return fetch24(); }
static uint32_t am_longx(void) { return (fetch24() + C.x) & 0xFFFFFF; }
static uint32_t am_sr(void) { uint8_t o = fetch8(); idle(); return (uint16_t)(C.sp + o); }
static uint32_t am_sriy(void) { uint16_t p = am_sr(); uint32_t b = ((uint32_t)C.db << 16) | rd16b0(p); idle(); return (b + C.y) & 0xFFFFFF; }

static uint32_t alu_addr(uint8_t op) {
  switch (op & 0x1F) {
    case 0x01: return am_dpxi();
    case 0x03: return am_sr();
    case 0x05: return am_dp();
    case 0x07: return am_dpil();
    case 0x09: return am_immM();
    case 0x0D: return am_abs();
    case 0x0F: return am_long();
    case 0x11: return am_dpiy();
    case 0x12: return am_dpi();
    case 0x13: return am_sriy();
    case 0x15: return am_dpx();
    case 0x17: return am_dpily();
    case 0x19: return am_absy();
    case 0x1D: return am_absx();
    default:   return am_longx();
  }
}

// ------------------------------------------------------------ ALU
static void adc(uint16_t v) {
  if (MF) {
    int a = C.a & 0xFF, r;
    v &= 0xFF;
    int c = C.p & FC;
    if (C.p & FD) {
      r = (a & 0x0F) + (v & 0x0F) + c;
      if (r > 0x09) r += 0x06;
      c = r > 0x0F;
      r = (a & 0xF0) + (v & 0xF0) + (c << 4) + (r & 0x0F);
    } else r = a + v + c;
    setf(FV, ~(a ^ v) & (a ^ r) & 0x80);
    if ((C.p & FD) && r > 0x9F) r += 0x60;
    setf(FC, r > 0xFF);
    C.a = (C.a & 0xFF00) | (r & 0xFF);
    nz8(r);
  } else {
    int a = C.a, r, c = C.p & FC;
    if (C.p & FD) {
      r = (a & 0x000F) + (v & 0x000F) + c;
      if (r > 0x0009) r += 0x0006;
      c = r > 0x000F;
      r = (a & 0x00F0) + (v & 0x00F0) + (c << 4) + (r & 0x000F);
      if (r > 0x009F) r += 0x0060;
      c = r > 0x00FF;
      r = (a & 0x0F00) + (v & 0x0F00) + (c << 8) + (r & 0x00FF);
      if (r > 0x09FF) r += 0x0600;
      c = r > 0x0FFF;
      r = (a & 0xF000) + (v & 0xF000) + (c << 12) + (r & 0x0FFF);
    } else r = a + v + c;
    setf(FV, ~(a ^ v) & (a ^ r) & 0x8000);
    if ((C.p & FD) && r > 0x9FFF) r += 0x6000;
    setf(FC, r > 0xFFFF);
    C.a = r;
    nz16(C.a);
  }
}

static void sbc(uint16_t v) {
  if (MF) {
    int a = C.a & 0xFF, r, c = C.p & FC;
    v = ~v & 0xFF;
    if (C.p & FD) {
      r = (a & 0x0F) + (v & 0x0F) + c;
      if (r <= 0x0F) r -= 0x06;
      c = r > 0x0F;
      r = (a & 0xF0) + (v & 0xF0) + (c << 4) + (r & 0x0F);
    } else r = a + v + c;
    setf(FV, ~(a ^ v) & (a ^ r) & 0x80);
    if ((C.p & FD) && r <= 0xFF) r -= 0x60;
    setf(FC, r > 0xFF);
    C.a = (C.a & 0xFF00) | (r & 0xFF);
    nz8(r);
  } else {
    int a = C.a, r, c = C.p & FC;
    v = ~v;
    if (C.p & FD) {
      r = (a & 0x000F) + (v & 0x000F) + c;
      if (r <= 0x000F) r -= 0x0006;
      c = r > 0x000F;
      r = (a & 0x00F0) + (v & 0x00F0) + (c << 4) + (r & 0x000F);
      if (r <= 0x00FF) r -= 0x0060;
      c = r > 0x00FF;
      r = (a & 0x0F00) + (v & 0x0F00) + (c << 8) + (r & 0x00FF);
      if (r <= 0x0FFF) r -= 0x0600;
      c = r > 0x0FFF;
      r = (a & 0xF000) + (v & 0xF000) + (c << 12) + (r & 0x0FFF);
    } else r = a + v + c;
    setf(FV, ~(a ^ v) & (a ^ r) & 0x8000);
    if ((C.p & FD) && r <= 0xFFFF) r -= 0x6000;
    setf(FC, r > 0xFFFF);
    C.a = r;
    nz16(C.a);
  }
}

static void cmp_w(uint16_t reg, uint16_t v, bool eight) {
  if (eight) { int r = (reg & 0xFF) - (v & 0xFF); setf(FC, r >= 0); nz8(r); }
  else { int r = reg - v; setf(FC, r >= 0); nz16(r); }
}

static void alu_op(int grp, uint32_t a) {
  if (grp == 4) { if (MF) wr8(a, C.a); else wr16(a, C.a); return; }  // STA
  uint16_t v = MF ? rd8(a) : rd16(a);
  switch (grp) {
    case 0: if (MF) { C.a |= v & 0xFF; nz8(C.a); } else { C.a |= v; nz16(C.a); } break;
    case 1: if (MF) { C.a &= v | 0xFF00; nz8(C.a); } else { C.a &= v; nz16(C.a); } break;
    case 2: if (MF) { C.a ^= v & 0xFF; nz8(C.a); } else { C.a ^= v; nz16(C.a); } break;
    case 3: adc(v); break;
    case 5: if (MF) { C.a = (C.a & 0xFF00) | (v & 0xFF); nz8(v); } else { C.a = v; nz16(v); } break;
    case 6: cmp_w(C.a, v, MF); break;
    case 7: sbc(v); break;
  }
}

// read-modify-write helpers: kind 0 ASL 1 ROL 2 LSR 3 ROR 4 DEC 5 INC 6 TSB 7 TRB
static uint16_t rmw_val(int kind, uint16_t v, bool eight) {
  uint16_t msb = eight ? 0x80 : 0x8000, mask = eight ? 0xFF : 0xFFFF;
  switch (kind) {
    case 0: setf(FC, v & msb); v = (v << 1) & mask; break;
    case 1: { bool c = C.p & FC; setf(FC, v & msb); v = ((v << 1) | c) & mask; break; }
    case 2: setf(FC, v & 1); v >>= 1; break;
    case 3: { bool c = C.p & FC; setf(FC, v & 1); v = (v >> 1) | (c ? msb : 0); break; }
    case 4: v = (v - 1) & mask; break;
    case 5: v = (v + 1) & mask; break;
    case 6: setf(FZ, !(v & C.a & mask)); return (v | C.a) & mask;
    case 7: setf(FZ, !(v & C.a & mask)); return (v & ~C.a) & mask;
  }
  if (eight) nz8(v); else nz16(v);
  return v;
}

static void rmw_mem(int kind, uint32_t a) {
  if (MF) { uint8_t v = rd8(a); idle(); wr8(a, rmw_val(kind, v, true)); }
  else { uint16_t v = rd16(a); idle(); uint16_t r = rmw_val(kind, v, false); wr8(a + 1, r >> 8); wr8(a, r & 0xFF); }
}

static void rmw_acc(int kind) {
  idle();
  if (MF) C.a = (C.a & 0xFF00) | rmw_val(kind, C.a & 0xFF, true);
  else C.a = rmw_val(kind, C.a, false);
}

static void branch(bool cond) {
  int8_t o = (int8_t)fetch8();
  if (cond) { idle(); C.pc += o; }
}

static void interrupt(uint16_t native_vec, uint16_t emu_vec) {
  if (!C.e) push8(C.pb);
  push16(C.pc);
  push8(C.e ? (C.p & ~0x10) : C.p);
  C.p = (C.p | FI) & ~FD;
  C.pb = 0;
  C.pc = rd16(C.e ? emu_vec : native_vec);
}

static void ld_reg(uint16_t *r, uint32_t a, bool eight) {
  if (eight) { *r = rd8(a); nz8(*r); } else { *r = rd16(a); nz16(*r); }
}
static void st_reg(uint16_t r, uint32_t a, bool eight) { if (eight) wr8(a, r); else wr16(a, r); }

static void block_move(int dir) {
  uint8_t dst = fetch8(), src = fetch8();
  C.db = dst;
  uint8_t v = rd8(((uint32_t)src << 16) | C.x);
  wr8(((uint32_t)dst << 16) | C.y, v);
  idle(); idle();
  if (XF) { C.x = (C.x + dir) & 0xFF; C.y = (C.y + dir) & 0xFF; }
  else { C.x += dir; C.y += dir; }
  if (C.a-- != 0) C.pc -= 3;
}

void cpu_reset(void) {
  C.e = true; C.p = FM | FX | FI; C.sp = 0x1FF; C.dp = 0; C.db = 0; C.pb = 0;
  C.x &= 0xFF; C.y &= 0xFF;
  C.waiting = C.stopped = C.nmi_pending = false;
  C.pc = rd16(0xFFFC);
}

void cpu_step(void) {
  if (C.stopped) { snes_tick(24); return; }
  if (C.nmi_pending) {
    C.nmi_pending = false; C.waiting = false;
    idle(); idle();
    interrupt(0xFFEA, 0xFFFA);
    return;
  }
  if (g->irq_flag) {
    if (C.waiting) C.waiting = false;
    if (!(C.p & FI)) { idle(); idle(); interrupt(0xFFEE, 0xFFFE); return; }
  }
  if (C.waiting) { snes_tick(12); return; }

  uint8_t op = fetch8();
  uint8_t lo = op & 0x1F;
  if (op != 0x89 && (((lo & 1) && lo != 0x0B && lo != 0x1B) || lo == 0x12)) {
    // the regular ALU block: ORA AND EOR ADC STA LDA CMP SBC
    alu_op(op >> 5, alu_addr(op));
    return;
  }

  uint32_t a;
  switch (op) {
    // ---- RMW memory
    case 0x06: rmw_mem(0, am_dp()); break;   case 0x0E: rmw_mem(0, am_abs()); break;
    case 0x16: rmw_mem(0, am_dpx()); break;  case 0x1E: rmw_mem(0, am_absx()); break;
    case 0x26: rmw_mem(1, am_dp()); break;   case 0x2E: rmw_mem(1, am_abs()); break;
    case 0x36: rmw_mem(1, am_dpx()); break;  case 0x3E: rmw_mem(1, am_absx()); break;
    case 0x46: rmw_mem(2, am_dp()); break;   case 0x4E: rmw_mem(2, am_abs()); break;
    case 0x56: rmw_mem(2, am_dpx()); break;  case 0x5E: rmw_mem(2, am_absx()); break;
    case 0x66: rmw_mem(3, am_dp()); break;   case 0x6E: rmw_mem(3, am_abs()); break;
    case 0x76: rmw_mem(3, am_dpx()); break;  case 0x7E: rmw_mem(3, am_absx()); break;
    case 0xC6: rmw_mem(4, am_dp()); break;   case 0xCE: rmw_mem(4, am_abs()); break;
    case 0xD6: rmw_mem(4, am_dpx()); break;  case 0xDE: rmw_mem(4, am_absx()); break;
    case 0xE6: rmw_mem(5, am_dp()); break;   case 0xEE: rmw_mem(5, am_abs()); break;
    case 0xF6: rmw_mem(5, am_dpx()); break;  case 0xFE: rmw_mem(5, am_absx()); break;
    case 0x04: rmw_mem(6, am_dp()); break;   case 0x0C: rmw_mem(6, am_abs()); break;
    case 0x14: rmw_mem(7, am_dp()); break;   case 0x1C: rmw_mem(7, am_abs()); break;
    // ---- RMW accumulator
    case 0x0A: rmw_acc(0); break; case 0x2A: rmw_acc(1); break;
    case 0x4A: rmw_acc(2); break; case 0x6A: rmw_acc(3); break;
    case 0x3A: rmw_acc(4); break; case 0x1A: rmw_acc(5); break;
    // ---- BIT
    case 0x89: { uint16_t v = MF ? rd8(am_immM()) : rd16(am_immM());
      setf(FZ, !(v & C.a & (MF ? 0xFF : 0xFFFF))); break; }
    case 0x24: case 0x2C: case 0x34: case 0x3C: {
      a = op == 0x24 ? am_dp() : op == 0x2C ? am_abs() : op == 0x34 ? am_dpx() : am_absx();
      if (MF) { uint8_t v = rd8(a); setf(FZ, !(v & C.a & 0xFF)); C.p = (C.p & 0x3F) | (v & 0xC0); }
      else { uint16_t v = rd16(a); setf(FZ, !(v & C.a)); C.p = (C.p & 0x3F) | ((v >> 8) & 0xC0); }
      break;
    }
    // ---- loads / stores of X, Y, Z
    case 0xA2: ld_reg(&C.x, am_immX(), XF); break;
    case 0xA6: ld_reg(&C.x, am_dp(), XF); break;
    case 0xB6: ld_reg(&C.x, am_dpy(), XF); break;
    case 0xAE: ld_reg(&C.x, am_abs(), XF); break;
    case 0xBE: ld_reg(&C.x, am_absy(), XF); break;
    case 0xA0: ld_reg(&C.y, am_immX(), XF); break;
    case 0xA4: ld_reg(&C.y, am_dp(), XF); break;
    case 0xB4: ld_reg(&C.y, am_dpx(), XF); break;
    case 0xAC: ld_reg(&C.y, am_abs(), XF); break;
    case 0xBC: ld_reg(&C.y, am_absx(), XF); break;
    case 0x86: st_reg(C.x, am_dp(), XF); break;
    case 0x96: st_reg(C.x, am_dpy(), XF); break;
    case 0x8E: st_reg(C.x, am_abs(), XF); break;
    case 0x84: st_reg(C.y, am_dp(), XF); break;
    case 0x94: st_reg(C.y, am_dpx(), XF); break;
    case 0x8C: st_reg(C.y, am_abs(), XF); break;
    case 0x64: st_reg(0, am_dp(), MF); break;
    case 0x74: st_reg(0, am_dpx(), MF); break;
    case 0x9C: st_reg(0, am_abs(), MF); break;
    case 0x9E: st_reg(0, am_absx(), MF); break;
    // ---- compares X/Y
    case 0xE0: cmp_w(C.x, XF ? rd8(am_immX()) : rd16(am_immX()), XF); break;
    case 0xE4: a = am_dp(); cmp_w(C.x, XF ? rd8(a) : rd16(a), XF); break;
    case 0xEC: a = am_abs(); cmp_w(C.x, XF ? rd8(a) : rd16(a), XF); break;
    case 0xC0: cmp_w(C.y, XF ? rd8(am_immX()) : rd16(am_immX()), XF); break;
    case 0xC4: a = am_dp(); cmp_w(C.y, XF ? rd8(a) : rd16(a), XF); break;
    case 0xCC: a = am_abs(); cmp_w(C.y, XF ? rd8(a) : rd16(a), XF); break;
    // ---- inc/dec index
    case 0xE8: idle(); C.x = XF ? (C.x + 1) & 0xFF : C.x + 1; if (XF) nz8(C.x); else nz16(C.x); break;
    case 0xCA: idle(); C.x = XF ? (C.x - 1) & 0xFF : C.x - 1; if (XF) nz8(C.x); else nz16(C.x); break;
    case 0xC8: idle(); C.y = XF ? (C.y + 1) & 0xFF : C.y + 1; if (XF) nz8(C.y); else nz16(C.y); break;
    case 0x88: idle(); C.y = XF ? (C.y - 1) & 0xFF : C.y - 1; if (XF) nz8(C.y); else nz16(C.y); break;
    // ---- branches
    case 0x10: branch(!(C.p & FN)); break;
    case 0x30: branch(C.p & FN); break;
    case 0x50: branch(!(C.p & FV)); break;
    case 0x70: branch(C.p & FV); break;
    case 0x90: branch(!(C.p & FC)); break;
    case 0xB0: branch(C.p & FC); break;
    case 0xD0: branch(!(C.p & FZ)); break;
    case 0xF0: branch(C.p & FZ); break;
    case 0x80: branch(true); break;
    case 0x82: { int16_t o = (int16_t)fetch16(); idle(); C.pc += o; break; }
    // ---- jumps / calls
    case 0x4C: C.pc = fetch16(); break;
    case 0x5C: { uint32_t t = fetch24(); C.pc = t & 0xFFFF; C.pb = t >> 16; break; }
    case 0x6C: { uint16_t p = fetch16(); C.pc = rd16b0(p); break; }
    case 0x7C: { uint16_t p = fetch16(); idle(); C.pc = rd16(((uint32_t)C.pb << 16) | (uint16_t)(p + C.x)); break; }
    case 0xDC: { uint16_t p = fetch16(); uint16_t l = rd16b0(p); C.pb = rd8((uint16_t)(p + 2)); C.pc = l; break; }
    case 0x20: { uint16_t t = fetch16(); idle(); push16(C.pc - 1); C.pc = t; break; }
    case 0xFC: { uint16_t p = fetch16(); push16(C.pc - 1); idle();
      C.pc = rd16(((uint32_t)C.pb << 16) | (uint16_t)(p + C.x)); break; }
    case 0x22: { uint16_t t = fetch16(); push8(C.pb); idle(); uint8_t b = fetch8();
      push16(C.pc - 1); C.pc = t; C.pb = b; break; }
    case 0x60: idle(); idle(); C.pc = pull16() + 1; idle(); break;
    case 0x6B: idle(); idle(); C.pc = pull16() + 1; C.pb = pull8(); break;
    case 0x40: idle(); idle(); set_p(pull8()); C.pc = pull16(); if (!C.e) C.pb = pull8(); break;
    case 0x00: fetch8(); interrupt(0xFFE6, 0xFFFE); break;
    case 0x02: fetch8(); interrupt(0xFFE4, 0xFFF4); break;
    // ---- stack
    case 0x48: idle(); if (MF) push8(C.a); else push16(C.a); break;
    case 0xDA: idle(); if (XF) push8(C.x); else push16(C.x); break;
    case 0x5A: idle(); if (XF) push8(C.y); else push16(C.y); break;
    case 0x68: idle(); idle(); if (MF) { C.a = (C.a & 0xFF00) | pull8(); nz8(C.a); } else { C.a = pull16(); nz16(C.a); } break;
    case 0xFA: idle(); idle(); if (XF) { C.x = pull8(); nz8(C.x); } else { C.x = pull16(); nz16(C.x); } break;
    case 0x7A: idle(); idle(); if (XF) { C.y = pull8(); nz8(C.y); } else { C.y = pull16(); nz16(C.y); } break;
    case 0x08: idle(); push8(C.p); break;
    case 0x28: idle(); idle(); set_p(pull8()); break;
    case 0x8B: idle(); push8(C.db); break;
    case 0xAB: idle(); idle(); C.db = pull8(); nz8(C.db); break;
    case 0x4B: idle(); push8(C.pb); break;
    case 0x0B: idle(); push16(C.dp); break;
    case 0x2B: idle(); idle(); C.dp = pull16(); nz16(C.dp); break;
    case 0xF4: push16(fetch16()); break;
    case 0xD4: { uint16_t p = am_dp(); push16(rd16b0(p)); break; }
    case 0x62: { int16_t o = (int16_t)fetch16(); idle(); push16(C.pc + o); break; }
    // ---- transfers
    case 0xAA: idle(); if (XF) { C.x = C.a & 0xFF; nz8(C.x); } else { C.x = C.a; nz16(C.x); } break;
    case 0xA8: idle(); if (XF) { C.y = C.a & 0xFF; nz8(C.y); } else { C.y = C.a; nz16(C.y); } break;
    case 0x8A: idle(); if (MF) { C.a = (C.a & 0xFF00) | (C.x & 0xFF); nz8(C.a); } else { C.a = C.x; nz16(C.a); } break;
    case 0x98: idle(); if (MF) { C.a = (C.a & 0xFF00) | (C.y & 0xFF); nz8(C.a); } else { C.a = C.y; nz16(C.a); } break;
    case 0x9B: idle(); C.y = C.x; if (XF) nz8(C.y); else nz16(C.y); break;
    case 0xBB: idle(); C.x = C.y; if (XF) nz8(C.x); else nz16(C.x); break;
    case 0xBA: idle(); if (XF) { C.x = C.sp & 0xFF; nz8(C.x); } else { C.x = C.sp; nz16(C.x); } break;
    case 0x9A: idle(); C.sp = C.e ? (0x100 | (C.x & 0xFF)) : C.x; break;
    case 0x5B: idle(); C.dp = C.a; nz16(C.dp); break;
    case 0x7B: idle(); C.a = C.dp; nz16(C.a); break;
    case 0x1B: idle(); C.sp = C.e ? (0x100 | (C.a & 0xFF)) : C.a; break;
    case 0x3B: idle(); C.a = C.sp; nz16(C.a); break;
    case 0xEB: idle(); idle(); C.a = (C.a >> 8) | (C.a << 8); nz8(C.a); break;
    // ---- flags
    case 0x18: idle(); C.p &= ~FC; break;
    case 0x38: idle(); C.p |= FC; break;
    case 0x58: idle(); C.p &= ~FI; break;
    case 0x78: idle(); C.p |= FI; break;
    case 0xB8: idle(); C.p &= ~FV; break;
    case 0xD8: idle(); C.p &= ~FD; break;
    case 0xF8: idle(); C.p |= FD; break;
    case 0xC2: { uint8_t v = fetch8(); idle(); set_p(C.p & ~v); break; }
    case 0xE2: { uint8_t v = fetch8(); idle(); set_p(C.p | v); break; }
    case 0xFB: {
      idle();
      bool c = C.p & FC;
      setf(FC, C.e);
      C.e = c;
      if (C.e) { C.p |= FM | FX; C.x &= 0xFF; C.y &= 0xFF; C.sp = 0x100 | (C.sp & 0xFF); }
      break;
    }
    // ---- misc
    case 0xEA: idle(); break;
    case 0x42: fetch8(); break;
    case 0x44: block_move(-1); break;
    case 0x54: block_move(1); break;
    case 0xCB: idle(); idle(); C.waiting = true; break;
    case 0xDB: idle(); idle(); C.stopped = true; break;
    default: idle(); break;
  }
}

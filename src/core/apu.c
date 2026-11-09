// Sound subsystem: SPC700 interpreter, timers, I/O ports and the S-DSP
// (BRR decoding, ADSR/GAIN envelopes, noise, pitch modulation, echo + FIR).
#include "snes.h"
#include <string.h>
#include <math.h>

#define A (g->apu)
enum { SN = 0x80, SV = 0x40, SP = 0x20, SB = 0x10, SH = 0x08, SI = 0x04, SZ = 0x02, SC = 0x01 };

static const uint8_t ipl_rom[64] = {
  0xCD,0xEF,0xBD,0xE8,0x00,0xC6,0x1D,0xD0,0xFC,0x8F,0xAA,0xF4,0x8F,0xBB,0xF5,0x78,
  0xCC,0xF4,0xD0,0xFB,0x2F,0x19,0xEB,0xF4,0xD0,0xFC,0x7E,0xF4,0xD0,0x0B,0xE4,0xF5,
  0xCB,0xF4,0xD7,0x00,0xFC,0xD0,0xF3,0xAB,0x01,0x10,0xEF,0x7E,0xF4,0x10,0xEB,0xBA,
  0xF6,0xDA,0x00,0xBA,0xF4,0xC4,0xF4,0xDD,0x5D,0xD0,0xDB,0x1F,0x00,0x00,0xC0,0xFF};

static const uint8_t cyc_tab[256] = {
/*        0 1 2 3 4 5 6 7 8 9 A B C D E F */
/* 0 */   2,8,4,5,3,4,3,6,2,6,5,4,5,4,6,8,
/* 1 */   2,8,4,5,4,5,5,6,5,5,6,5,2,2,4,6,
/* 2 */   2,8,4,5,3,4,3,6,2,6,5,4,5,4,5,4,
/* 3 */   2,8,4,5,4,5,5,6,5,5,6,5,2,2,3,8,
/* 4 */   2,8,4,5,3,4,3,6,2,6,4,4,5,4,6,6,
/* 5 */   2,8,4,5,4,5,5,6,5,5,4,5,2,2,4,3,
/* 6 */   2,8,4,5,3,4,3,6,2,6,4,4,5,4,5,5,
/* 7 */   2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,6,
/* 8 */   2,8,4,5,3,4,3,6,2,6,5,4,5,2,4,5,
/* 9 */   2,8,4,5,4,5,5,6,5,5,5,5,2,2,12,5,
/* A */   3,8,4,5,3,4,3,6,2,6,4,4,5,2,4,4,
/* B */   2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,4,
/* C */   3,8,4,5,4,5,4,7,2,5,6,4,5,2,4,9,
/* D */   2,8,4,5,5,6,6,7,4,5,5,5,2,2,6,3,
/* E */   2,8,4,5,3,4,3,6,2,4,5,3,4,3,4,3,
/* F */   2,8,4,5,4,5,5,6,3,4,5,4,2,2,4,3};

// ------------------------------------------------------------- memory
static void timers_step(int cycles);

static uint8_t spc_read(uint16_t a) {
  if (a >= 0xF0 && a <= 0xFF) {
    switch (a) {
      case 0xF2: return A.dsp_addr;
      case 0xF3: return dsp_read(A.dsp_addr & 0x7F);
      case 0xF4: case 0xF5: case 0xF6: case 0xF7: return A.inports[a - 0xF4];
      case 0xFD: case 0xFE: case 0xFF: { uint8_t v = A.timer_out[a - 0xFD]; A.timer_out[a - 0xFD] = 0; return v; }
      case 0xF8: case 0xF9: return A.ram[a];
      default: return 0;
    }
  }
  if (a >= 0xFFC0 && (A.control & 0x80)) return ipl_rom[a - 0xFFC0];
  return A.ram[a];
}

static void spc_write(uint16_t a, uint8_t v) {
  if (a >= 0xF0 && a <= 0xFF) {
    switch (a) {
      case 0xF1:
        for (int i = 0; i < 3; i++)
          if ((v & (1 << i)) && !(A.control & (1 << i))) { A.timer_stage[i] = 0; A.timer_out[i] = 0; }
        if (v & 0x10) { A.inports[0] = A.inports[1] = 0; }
        if (v & 0x20) { A.inports[2] = A.inports[3] = 0; }
        A.control = v;
        break;
      case 0xF2: A.dsp_addr = v; break;
      case 0xF3: if (A.dsp_addr < 0x80) dsp_write(A.dsp_addr, v); break;
      case 0xF4: case 0xF5: case 0xF6: case 0xF7: A.outports[a - 0xF4] = v; break;
      case 0xFA: case 0xFB: case 0xFC: A.timer_target[a - 0xFA] = v; break;
    }
  }
  A.ram[a] = v;
}

static inline uint8_t fetch(void) { return spc_read(A.pc++); }
static inline uint16_t fetch16(void) { uint16_t l = fetch(); return l | (fetch() << 8); }
static inline uint16_t dpa(uint8_t o) { return ((A.psw & SP) ? 0x100 : 0) | o; }
static inline uint8_t rdp(uint8_t o) { return spc_read(dpa(o)); }
static inline void wdp(uint8_t o, uint8_t v) { spc_write(dpa(o), v); }
static inline uint16_t rdp16(uint8_t o) { return rdp(o) | (rdp((uint8_t)(o + 1)) << 8); }
static inline void push(uint8_t v) { spc_write(0x100 | A.sp--, v); }
static inline uint8_t pop(void) { return spc_read(0x100 | ++A.sp); }
static inline void nz(uint8_t v) { A.psw = (A.psw & ~(SN | SZ)) | (v & 0x80) | (v ? 0 : SZ); }
static inline void nz16(uint16_t v) { A.psw = (A.psw & ~(SN | SZ)) | ((v >> 8) & 0x80) | (v ? 0 : SZ); }
static inline void sf(uint8_t f, bool on) { if (on) A.psw |= f; else A.psw &= ~f; }

static uint8_t op_adc(uint8_t a, uint8_t b) {
  int c = A.psw & SC, r = a + b + c;
  sf(SV, ~(a ^ b) & (a ^ r) & 0x80);
  sf(SH, ((a & 0xF) + (b & 0xF) + c) > 0xF);
  sf(SC, r > 0xFF);
  nz(r);
  return r;
}
static uint8_t aluop(int k, uint8_t a, uint8_t b) {
  switch (k) {
    case 0: a |= b; nz(a); return a;
    case 1: a &= b; nz(a); return a;
    case 2: a ^= b; nz(a); return a;
    case 3: { int r = a - b; sf(SC, r >= 0); nz(r); return a; }
    case 4: return op_adc(a, b);
    default: return op_adc(a, ~b);
  }
}
static uint8_t shift(int k, uint8_t v) {
  bool c = A.psw & SC;
  switch (k) {
    case 0: sf(SC, v & 0x80); v <<= 1; break;               // ASL
    case 1: sf(SC, v & 0x80); v = (v << 1) | c; break;      // ROL
    case 2: sf(SC, v & 1); v >>= 1; break;                  // LSR
    case 3: sf(SC, v & 1); v = (v >> 1) | (c << 7); break;  // ROR
  }
  nz(v);
  return v;
}

static int branch(bool cond) {
  int8_t o = (int8_t)fetch();
  if (cond) { A.pc += o; return 2; }
  return 0;
}

static int spc_step(void) {
  if (A.halted) return 2;
  uint8_t op = fetch();
  int cyc = cyc_tab[op];
  uint8_t lo = op & 0x0F;
  int k = op >> 5;
  // ------------------------------------------------ ALU block (rows 0x0-0xB, cols 4-9)
  if (lo >= 4 && lo <= 9 && k <= 5) {
    bool odd = op & 0x10;
    uint16_t addr;
    switch (lo) {
      case 4: addr = odd ? dpa((uint8_t)(fetch() + A.x)) : dpa(fetch()); A.a = aluop(k, A.a, spc_read(addr)); break;
      case 5: addr = fetch16(); if (odd) addr += A.x; A.a = aluop(k, A.a, spc_read(addr)); break;
      case 6: if (odd) addr = fetch16() + A.y; else addr = dpa(A.x); A.a = aluop(k, A.a, spc_read(addr)); break;
      case 7: {
        uint8_t o = fetch();
        if (odd) addr = rdp16(o) + A.y; else addr = rdp16((uint8_t)(o + A.x));
        A.a = aluop(k, A.a, spc_read(addr)); break;
      }
      case 8:
        if (!odd) A.a = aluop(k, A.a, fetch());
        else { uint8_t imm = fetch(); uint8_t d = fetch(); uint8_t r = aluop(k, rdp(d), imm); if (k != 3) wdp(d, r); }
        break;
      case 9:
        if (!odd) { uint8_t s = fetch(); uint8_t d = fetch(); uint8_t sv = rdp(s); uint8_t r = aluop(k, rdp(d), sv); if (k != 3) wdp(d, r); }
        else { uint8_t sv = rdp(A.y); uint8_t r = aluop(k, rdp(A.x), sv); if (k != 3) wdp(A.x, r); }
        break;
    }
    return cyc;
  }
  // ------------------------------------------------ bit ops columns 1-3
  if (lo == 1) {  // TCALL
    int n = op >> 4;
    push(A.pc >> 8); push(A.pc & 0xFF);
    uint16_t va = 0xFFDE - n * 2;
    A.pc = spc_read(va) | (spc_read(va + 1) << 8);
    return cyc;
  }
  if (lo == 2) {  // SET1 / CLR1
    uint8_t d = fetch(); uint8_t bit = 1 << (op >> 5);
    uint8_t v = rdp(d);
    wdp(d, (op & 0x10) ? (v & ~bit) : (v | bit));
    return cyc;
  }
  if (lo == 3) {  // BBS / BBC
    uint8_t d = fetch(); uint8_t bit = 1 << (op >> 5);
    bool set = rdp(d) & bit;
    return cyc + branch((op & 0x10) ? !set : set);
  }

  switch (op) {
    // branches & flags (column 0)
    case 0x00: break;
    case 0x10: cyc += branch(!(A.psw & SN)); break;
    case 0x20: A.psw &= ~SP; break;
    case 0x30: cyc += branch(A.psw & SN); break;
    case 0x40: A.psw |= SP; break;
    case 0x50: cyc += branch(!(A.psw & SV)); break;
    case 0x60: A.psw &= ~SC; break;
    case 0x70: cyc += branch(A.psw & SV); break;
    case 0x80: A.psw |= SC; break;
    case 0x90: cyc += branch(!(A.psw & SC)); break;
    case 0xA0: A.psw |= SI; break;
    case 0xB0: cyc += branch(A.psw & SC); break;
    case 0xC0: A.psw &= ~SI; break;
    case 0xD0: cyc += branch(!(A.psw & SZ)); break;
    case 0xE0: A.psw &= ~(SV | SH); break;
    case 0xF0: cyc += branch(A.psw & SZ); break;

    // MOV store / load (cols 4-7, rows C-F)
    case 0xC4: { uint8_t d = fetch(); wdp(d, A.a); break; }
    case 0xD4: { uint8_t d = fetch(); wdp((uint8_t)(d + A.x), A.a); break; }
    case 0xE4: A.a = rdp(fetch()); nz(A.a); break;
    case 0xF4: A.a = rdp((uint8_t)(fetch() + A.x)); nz(A.a); break;
    case 0xC5: spc_write(fetch16(), A.a); break;
    case 0xD5: spc_write(fetch16() + A.x, A.a); break;
    case 0xE5: A.a = spc_read(fetch16()); nz(A.a); break;
    case 0xF5: A.a = spc_read(fetch16() + A.x); nz(A.a); break;
    case 0xC6: wdp(A.x, A.a); break;
    case 0xD6: spc_write(fetch16() + A.y, A.a); break;
    case 0xE6: A.a = rdp(A.x); nz(A.a); break;
    case 0xF6: A.a = spc_read(fetch16() + A.y); nz(A.a); break;
    case 0xC7: { uint8_t o = fetch(); spc_write(rdp16((uint8_t)(o + A.x)), A.a); break; }
    case 0xD7: { uint8_t o = fetch(); spc_write(rdp16(o) + A.y, A.a); break; }
    case 0xE7: { uint8_t o = fetch(); A.a = spc_read(rdp16((uint8_t)(o + A.x))); nz(A.a); break; }
    case 0xF7: { uint8_t o = fetch(); A.a = spc_read(rdp16(o) + A.y); nz(A.a); break; }
    // column 8 rows C-F
    case 0xC8: aluop(3, A.x, fetch()); break;
    case 0xD8: { uint8_t d = fetch(); wdp(d, A.x); break; }
    case 0xE8: A.a = fetch(); nz(A.a); break;
    case 0xF8: A.x = rdp(fetch()); nz(A.x); break;
    // column 9 rows C-F
    case 0xC9: spc_write(fetch16(), A.x); break;
    case 0xD9: { uint8_t d = fetch(); wdp((uint8_t)(d + A.y), A.x); break; }
    case 0xE9: A.x = spc_read(fetch16()); nz(A.x); break;
    case 0xF9: A.x = rdp((uint8_t)(fetch() + A.y)); nz(A.x); break;

    // column A
    case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x8A: case 0xAA: case 0xCA: case 0xEA: {
      uint16_t w = fetch16(); uint16_t addr = w & 0x1FFF; int bit = w >> 13;
      bool m = (spc_read(addr) >> bit) & 1;
      bool c = A.psw & SC;
      switch (op) {
        case 0x0A: sf(SC, c || m); break;
        case 0x2A: sf(SC, c || !m); break;
        case 0x4A: sf(SC, c && m); break;
        case 0x6A: sf(SC, c && !m); break;
        case 0x8A: sf(SC, c != m); break;
        case 0xAA: sf(SC, m); break;
        case 0xCA: { uint8_t v = spc_read(addr); v = c ? (v | (1 << bit)) : (v & ~(1 << bit)); spc_write(addr, v); break; }
        case 0xEA: { uint8_t v = spc_read(addr) ^ (1 << bit); spc_write(addr, v); break; }
      }
      break;
    }
    case 0x1A: case 0x3A: {
      uint8_t d = fetch(); uint16_t w = rdp16(d); w += (op == 0x3A) ? 1 : -1;
      wdp(d, w & 0xFF); wdp((uint8_t)(d + 1), w >> 8); nz16(w); break;
    }
    case 0x5A: { uint16_t w = rdp16(fetch()); uint16_t ya = (A.y << 8) | A.a; int r = ya - w; sf(SC, r >= 0); nz16(r); break; }
    case 0x7A: case 0x9A: {
      uint16_t w = rdp16(fetch()); uint16_t ya = (A.y << 8) | A.a; int r;
      if (op == 0x7A) {
        r = ya + w;
        sf(SC, r > 0xFFFF); sf(SV, ~(ya ^ w) & (ya ^ r) & 0x8000);
        sf(SH, ((ya & 0xFFF) + (w & 0xFFF)) > 0xFFF);
      } else {
        r = ya - w;
        sf(SC, r >= 0); sf(SV, (ya ^ w) & (ya ^ r) & 0x8000);
        sf(SH, ((ya & 0xFFF) - (w & 0xFFF)) >= 0);
      }
      A.a = r & 0xFF; A.y = (r >> 8) & 0xFF; nz16(r); break;
    }
    case 0xBA: { uint16_t w = rdp16(fetch()); A.a = w & 0xFF; A.y = w >> 8; nz16(w); break; }
    case 0xDA: { uint8_t d = fetch(); wdp(d, A.a); wdp((uint8_t)(d + 1), A.y); break; }
    case 0xFA: { uint8_t s = fetch(); uint8_t d = fetch(); wdp(d, rdp(s)); break; }

    // column B: shifts/inc/dec on dp, MOV Y
    case 0x0B: case 0x2B: case 0x4B: case 0x6B: { uint8_t d = fetch(); wdp(d, shift(op >> 5, rdp(d))); break; }
    case 0x1B: case 0x3B: case 0x5B: case 0x7B: { uint8_t d = (uint8_t)(fetch() + A.x); wdp(d, shift(op >> 5, rdp(d))); break; }
    case 0x8B: { uint8_t d = fetch(); uint8_t v = rdp(d) - 1; nz(v); wdp(d, v); break; }
    case 0x9B: { uint8_t d = (uint8_t)(fetch() + A.x); uint8_t v = rdp(d) - 1; nz(v); wdp(d, v); break; }
    case 0xAB: { uint8_t d = fetch(); uint8_t v = rdp(d) + 1; nz(v); wdp(d, v); break; }
    case 0xBB: { uint8_t d = (uint8_t)(fetch() + A.x); uint8_t v = rdp(d) + 1; nz(v); wdp(d, v); break; }
    case 0xCB: { uint8_t d = fetch(); wdp(d, A.y); break; }
    case 0xDB: { uint8_t d = fetch(); wdp((uint8_t)(d + A.x), A.y); break; }
    case 0xEB: A.y = rdp(fetch()); nz(A.y); break;
    case 0xFB: A.y = rdp((uint8_t)(fetch() + A.x)); nz(A.y); break;

    // column C
    case 0x0C: case 0x2C: case 0x4C: case 0x6C: { uint16_t a = fetch16(); spc_write(a, shift(op >> 5, spc_read(a))); break; }
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: A.a = shift(op >> 5, A.a); break;
    case 0x8C: { uint16_t a = fetch16(); uint8_t v = spc_read(a) - 1; nz(v); spc_write(a, v); break; }
    case 0x9C: A.a--; nz(A.a); break;
    case 0xAC: { uint16_t a = fetch16(); uint8_t v = spc_read(a) + 1; nz(v); spc_write(a, v); break; }
    case 0xBC: A.a++; nz(A.a); break;
    case 0xCC: spc_write(fetch16(), A.y); break;
    case 0xDC: A.y--; nz(A.y); break;
    case 0xEC: A.y = spc_read(fetch16()); nz(A.y); break;
    case 0xFC: A.y++; nz(A.y); break;

    // column D
    case 0x0D: push(A.psw); break;
    case 0x1D: A.x--; nz(A.x); break;
    case 0x2D: push(A.a); break;
    case 0x3D: A.x++; nz(A.x); break;
    case 0x4D: push(A.x); break;
    case 0x5D: A.x = A.a; nz(A.x); break;
    case 0x6D: push(A.y); break;
    case 0x7D: A.a = A.x; nz(A.a); break;
    case 0x8D: A.y = fetch(); nz(A.y); break;
    case 0x9D: A.x = A.sp; nz(A.x); break;
    case 0xAD: aluop(3, A.y, fetch()); break;
    case 0xBD: A.sp = A.x; break;
    case 0xCD: A.x = fetch(); nz(A.x); break;
    case 0xDD: A.a = A.y; nz(A.a); break;
    case 0xED: A.psw ^= SC; break;
    case 0xFD: A.y = A.a; nz(A.y); break;

    // column E
    case 0x0E: case 0x4E: {
      uint16_t a = fetch16(); uint8_t v = spc_read(a);
      nz((uint8_t)(A.a - v));
      spc_write(a, op == 0x0E ? (v | A.a) : (v & ~A.a)); break;
    }
    case 0x1E: aluop(3, A.x, spc_read(fetch16())); break;
    case 0x3E: aluop(3, A.x, rdp(fetch())); break;
    case 0x5E: aluop(3, A.y, spc_read(fetch16())); break;
    case 0x7E: aluop(3, A.y, rdp(fetch())); break;
    case 0x2E: { uint8_t d = fetch(); uint8_t v = rdp(d); cyc += branch(A.a != v); break; }
    case 0xDE: { uint8_t d = fetch(); uint8_t v = rdp((uint8_t)(d + A.x)); cyc += branch(A.a != v); break; }
    case 0x6E: { uint8_t d = fetch(); uint8_t v = rdp(d) - 1; wdp(d, v); cyc += branch(v != 0); break; }
    case 0xFE: A.y--; cyc += branch(A.y != 0); break;
    case 0x8E: A.psw = pop(); break;
    case 0xAE: A.a = pop(); break;
    case 0xCE: A.x = pop(); break;
    case 0xEE: A.y = pop(); break;
    case 0x9E: {
      uint16_t ya = (A.y << 8) | A.a; uint8_t x = A.x;
      sf(SV, A.y >= x);
      sf(SH, (A.y & 15) >= (x & 15));
      if (A.y < (x << 1)) { A.a = ya / x; A.y = ya % x; }
      else { A.a = 255 - (ya - (x << 9)) / (256 - x); A.y = x + (ya - (x << 9)) % (256 - x); }
      nz(A.a); break;
    }
    case 0xBE:
      if (!(A.psw & SC) || A.a > 0x99) { A.a -= 0x60; A.psw &= ~SC; }
      if (!(A.psw & SH) || (A.a & 15) > 9) A.a -= 6;
      nz(A.a); break;

    // column F
    case 0x0F: push(A.pc >> 8); push(A.pc & 0xFF); push(A.psw); A.psw = (A.psw | SB) & ~SI;
      A.pc = spc_read(0xFFDE) | (spc_read(0xFFDF) << 8); break;
    case 0x1F: { uint16_t a = fetch16() + A.x; A.pc = spc_read(a) | (spc_read(a + 1) << 8); break; }
    case 0x2F: cyc += branch(true) - 2; break;
    case 0x3F: { uint16_t t = fetch16(); push(A.pc >> 8); push(A.pc & 0xFF); A.pc = t; break; }
    case 0x4F: { uint8_t t = fetch(); push(A.pc >> 8); push(A.pc & 0xFF); A.pc = 0xFF00 | t; break; }
    case 0x5F: A.pc = fetch16(); break;
    case 0x6F: { uint16_t l = pop(); A.pc = l | (pop() << 8); break; }
    case 0x7F: { A.psw = pop(); uint16_t l = pop(); A.pc = l | (pop() << 8); break; }
    case 0x8F: { uint8_t imm = fetch(); uint8_t d = fetch(); wdp(d, imm); break; }
    case 0x9F: A.a = (A.a >> 4) | (A.a << 4); nz(A.a); break;
    case 0xAF: wdp(A.x++, A.a); break;
    case 0xBF: A.a = rdp(A.x++); nz(A.a); break;
    case 0xCF: { uint16_t r = A.y * A.a; A.a = r & 0xFF; A.y = r >> 8; nz(A.y); break; }
    case 0xDF:
      if ((A.psw & SC) || A.a > 0x99) { A.a += 0x60; A.psw |= SC; }
      if ((A.psw & SH) || (A.a & 15) > 9) A.a += 6;
      nz(A.a); break;
    case 0xEF: case 0xFF: A.halted = true; break;
    default: break;
  }
  return cyc;
}

// ------------------------------------------------------------- timers / scheduling
static void timers_step(int cycles) {
  for (int i = 0; i < 3; i++) {
    int period = i == 2 ? 16 : 128;
    A.timer_div[i] += cycles;
    while (A.timer_div[i] >= period) {
      A.timer_div[i] -= period;
      if (!(A.control & (1 << i))) continue;
      A.timer_stage[i]++;
      if (A.timer_stage[i] == A.timer_target[i]) {
        A.timer_stage[i] = 0;
        A.timer_out[i] = (A.timer_out[i] + 1) & 0xF;
      }
    }
  }
}

void apu_reset(void) {
  memset(&A, 0, sizeof(A));
  for (int i = 0; i < 0x10000; i++) A.ram[i] = (i & 0x20) ? 0xFF : 0x00;
  A.control = 0x80;
  A.pc = ipl_rom[62] | (ipl_rom[63] << 8);
  A.sp = 0xEF;
  dsp_reset();
}

void apu_catch_up(void) {
  uint64_t target = g->master * 128000ull / 2684659ull;
  while (A.cycles < target) {
    int c = spc_step();
    A.cycles += c;
    timers_step(c);
    A.sample_div += c;
    while (A.sample_div >= 32) { A.sample_div -= 32; dsp_sample(); }
  }
}

uint8_t apu_cpu_read(int port) { return A.outports[port]; }
void apu_cpu_write(int port, uint8_t v) { A.inports[port] = v; }

// ======================================================================= DSP
#define D (A.dsp)
#define R(v, r) (D.regs[((v) << 4) | (r)])

static const uint16_t rate_period[32] = {
  0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
  64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};
static const uint16_t rate_offset[32] = {
  0, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
  0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0};

static int16_t interp_gauss[256][4], interp_cubic[256][4];
static bool tables_ready;

static void build_tables(void) {
  for (int f = 0; f < 256; f++) {
    double t = f / 256.0;
    double d[4] = {1 + t, t, 1 - t, 2 - t};
    double w[4], sum = 0;
    for (int i = 0; i < 4; i++) { w[i] = exp(-1.255 * d[i] * d[i]); if (d[i] >= 2) w[i] = 0; sum += w[i]; }
    for (int i = 0; i < 4; i++) interp_gauss[f][i] = (int16_t)lround(w[i] / sum * 2040.0);
    // Catmull-Rom cubic
    double t2 = t * t, t3 = t2 * t;
    double c0 = -0.5 * t3 + t2 - 0.5 * t;
    double c1 = 1.5 * t3 - 2.5 * t2 + 1;
    double c2 = -1.5 * t3 + 2 * t2 + 0.5 * t;
    double c3 = 0.5 * t3 - 0.5 * t2;
    interp_cubic[f][0] = (int16_t)lround(c0 * 2048); interp_cubic[f][1] = (int16_t)lround(c1 * 2048);
    interp_cubic[f][2] = (int16_t)lround(c2 * 2048); interp_cubic[f][3] = (int16_t)lround(c3 * 2048);
  }
  tables_ready = true;
}

static inline int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

void dsp_reset(void) {
  if (!tables_ready) build_tables();
  memset(&D, 0, sizeof(D));
  D.regs[0x6C] = 0xE0;
  D.noise = 0x4000;
  D.counter = 0;
  for (int i = 0; i < 8; i++) D.v[i].env_mode = 0;
}

void dsp_write(uint8_t addr, uint8_t v) {
  D.regs[addr] = v;
  if (addr == 0x4C) D.kon_pending |= v;
  else if (addr == 0x7C) D.regs[0x7C] = 0;
}

uint8_t dsp_read(uint8_t addr) { return D.regs[addr]; }

static void decode_block(Voice *vc) {
  vc->buf[0] = vc->buf[16]; vc->buf[1] = vc->buf[17]; vc->buf[2] = vc->buf[18];
  uint16_t ad = vc->brr_addr;
  uint8_t h = A.ram[ad];
  vc->brr_header = h;
  int shift = h >> 4, filter = h & 0x0C;
  for (int i = 0; i < 16; i++) {
    uint8_t byte = A.ram[(uint16_t)(ad + 1 + (i >> 1))];
    int s = (i & 1) ? (byte & 0xF) : (byte >> 4);
    s = (s ^ 8) - 8;
    s = (s << shift) >> 1;
    if (shift >= 0xD) s = (s >> 25) << 11;
    int p1 = vc->buf[3 + i - 1];
    int p2 = vc->buf[3 + i - 2] >> 1;
    if (filter >= 8) {
      s += p1; s -= p2;
      if (filter == 8) { s += p2 >> 4; s += (p1 * -3) >> 6; }
      else { s += (p1 * -13) >> 7; s += (p2 * 3) >> 4; }
    } else if (filter) {
      s += p1 >> 1; s += (-p1) >> 5;
    }
    s = clamp16(s);
    vc->buf[3 + i] = (int16_t)(s * 2);
  }
}

static inline bool rate_tick(int rate) {
  if (!rate) return false;
  return ((D.counter + rate_offset[rate]) % rate_period[rate]) == 0;
}

static void run_envelope(Voice *vc, int v) {
  int env = vc->env;
  if (vc->env_mode == 0) {           // release
    env -= 8;
    if (env < 0) env = 0;
    vc->env = env;
    return;
  }
  uint8_t adsr1 = R(v, 5), adsr2 = R(v, 6), gain = R(v, 7);
  int rate, mode = vc->env_mode;
  if (adsr1 & 0x80) {
    if (mode >= 2) {
      env -= ((env - 1) >> 8) + 1;
      rate = mode == 2 ? ((adsr1 >> 3) & 0x0E) + 0x10 : adsr2 & 0x1F;
    } else {
      rate = ((adsr1 & 0x0F) << 1) + 1;
      env += rate < 31 ? 0x20 : 0x400;
    }
  } else {
    if (!(gain & 0x80)) { env = (gain & 0x7F) << 4; rate = 31; }
    else {
      rate = gain & 0x1F;
      switch ((gain >> 5) & 3) {
        case 0: env -= 0x20; break;
        case 1: env -= ((env - 1) >> 8) + 1; break;
        case 2: env += 0x20; break;
        case 3: env += (vc->env < 0x600) ? 0x20 : 0x08; break;
      }
    }
  }
  if (mode == 2 && (adsr1 & 0x80) && (env >> 8) == (adsr2 >> 5)) vc->env_mode = 3;
  if (env < 0 || env > 0x7FF) {
    env = env < 0 ? 0 : 0x7FF;
    if (vc->env_mode == 1) vc->env_mode = 2;
  }
  if (rate_tick(rate)) vc->env = env;
}

void dsp_sample(void) {
  if (--D.counter < 0) D.counter = 0x77FF;
  uint8_t flg = D.regs[0x6C];
  uint16_t dir = D.regs[0x5D] << 8;
  D.every_other = !D.every_other;
  uint8_t kon = 0, koff = 0;
  if (D.every_other) { kon = D.kon_pending; D.kon_pending = 0; koff = D.regs[0x5C]; }

  if (rate_tick(flg & 0x1F)) {
    int fb = (D.noise << 13) ^ (D.noise << 14);
    D.noise = (fb & 0x4000) ^ (D.noise >> 1);
  }

  int mainL = 0, mainR = 0, echoL = 0, echoR = 0;
  const int16_t (*itab)[4] = g->interp_mode ? interp_cubic : interp_gauss;
  int prev_out = 0;
  for (int v = 0; v < 8; v++) {
    Voice *vc = &D.v[v];
    uint8_t bit = 1 << v;
    if (kon & bit) {
      uint16_t e = dir + R(v, 4) * 4;
      vc->brr_addr = A.ram[e] | (A.ram[(uint16_t)(e + 1)] << 8);
      vc->kon_delay = 5;
      vc->env = 0; vc->env_mode = 1;
      vc->interp_pos = 0;
      memset(vc->buf, 0, sizeof(vc->buf));
      decode_block(vc);
      D.regs[0x7C] &= ~bit;
    }
    if (koff & bit) vc->env_mode = 0;
    if (flg & 0x80) { vc->env_mode = 0; vc->env = 0; }

    int out = 0;
    if (vc->kon_delay > 0) {
      vc->kon_delay--;
    } else {
      int pitch = (R(v, 2) | (R(v, 3) << 8)) & 0x3FFF;
      if (v > 0 && (D.regs[0x2D] & bit)) pitch += ((prev_out >> 5) * pitch) >> 10;
      if (pitch < 0) pitch = 0; if (pitch > 0x3FFF) pitch = 0x3FFF;
      int s;
      if (D.regs[0x3D] & bit) s = (int16_t)(D.noise * 2);
      else {
        int idx = vc->interp_pos >> 12;
        int f = (vc->interp_pos >> 4) & 0xFF;
        const int16_t *w = itab[f];
        const int16_t *in = &vc->buf[idx];
        s = (w[0] * in[0]) >> 11;
        s += (w[1] * in[1]) >> 11;
        s += (w[2] * in[2]) >> 11;
        s = (int16_t)s;
        s += (w[3] * in[3]) >> 11;
        s = clamp16(s) & ~1;
      }
      run_envelope(vc, v);
      out = ((s * vc->env) >> 11) & ~1;
      vc->interp_pos += pitch;
      while (vc->interp_pos >= (16u << 12)) {
        vc->interp_pos -= 16u << 12;
        uint8_t h = vc->brr_header;
        if (h & 1) {
          D.regs[0x7C] |= bit;
          uint16_t e = dir + R(v, 4) * 4;
          vc->brr_addr = A.ram[(uint16_t)(e + 2)] | (A.ram[(uint16_t)(e + 3)] << 8);
          if (!(h & 2)) { vc->env_mode = 0; vc->env = 0; }
        } else {
          vc->brr_addr += 9;
        }
        decode_block(vc);
      }
    }
    vc->out = out;
    prev_out = out;
    R(v, 8) = vc->env >> 4;
    R(v, 9) = out >> 8;
    int l = (out * (int8_t)R(v, 0)) >> 7;
    int r = (out * (int8_t)R(v, 1)) >> 7;
    mainL = clamp16(mainL + l); mainR = clamp16(mainR + r);
    if (D.regs[0x4D] & bit) { echoL = clamp16(echoL + l); echoR = clamp16(echoR + r); }
  }

  // ---- echo
  uint16_t esa = D.regs[0x6D] << 8;
  if (D.echo_pos == 0) D.echo_len = (D.regs[0x7D] & 0x0F) ? (D.regs[0x7D] & 0x0F) * 2048 : 4;
  uint16_t ea = esa + D.echo_pos;
  int el = (int16_t)(A.ram[ea] | (A.ram[(uint16_t)(ea + 1)] << 8));
  int er = (int16_t)(A.ram[(uint16_t)(ea + 2)] | (A.ram[(uint16_t)(ea + 3)] << 8));
  D.hist_pos = (D.hist_pos + 1) & 7;
  D.echo_hist[D.hist_pos][0] = el >> 1;
  D.echo_hist[D.hist_pos][1] = er >> 1;
  int fl = 0, fr = 0;
  for (int i = 0; i < 8; i++) {
    int h = (D.hist_pos + 1 + i) & 7;
    int c = (int8_t)D.regs[(i << 4) | 0x0F];
    if (i == 7) { fl = (int16_t)fl; fr = (int16_t)fr; }
    fl += (D.echo_hist[h][0] * c) >> 6;
    fr += (D.echo_hist[h][1] * c) >> 6;
  }
  fl = clamp16(fl) & ~1; fr = clamp16(fr) & ~1;
  if (!(flg & 0x20)) {
    int wl = clamp16(echoL + ((fl * (int8_t)D.regs[0x0D]) >> 7)) & ~1;
    int wr = clamp16(echoR + ((fr * (int8_t)D.regs[0x0D]) >> 7)) & ~1;
    A.ram[ea] = wl & 0xFF; A.ram[(uint16_t)(ea + 1)] = (wl >> 8) & 0xFF;
    A.ram[(uint16_t)(ea + 2)] = wr & 0xFF; A.ram[(uint16_t)(ea + 3)] = (wr >> 8) & 0xFF;
  }
  D.echo_pos += 4;
  if (D.echo_pos >= D.echo_len) D.echo_pos = 0;

  int outL = clamp16(((mainL * (int8_t)D.regs[0x0C]) >> 7) + ((fl * (int8_t)D.regs[0x2C]) >> 7));
  int outR = clamp16(((mainR * (int8_t)D.regs[0x1C]) >> 7) + ((fr * (int8_t)D.regs[0x3C]) >> 7));
  if (flg & 0x40) outL = outR = 0;

  if (g->audio_out && g->audio_count < g->audio_cap) {
    g->audio_out[g->audio_count * 2] = (int16_t)outL;
    g->audio_out[g->audio_count * 2 + 1] = (int16_t)outR;
    g->audio_count++;
  }
}

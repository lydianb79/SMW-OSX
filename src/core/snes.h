// SNES runtime core — executes the 65816 / SPC700 code of a user-supplied ROM.
// Everything that makes up machine state lives in the Snes struct so save
// states are a straight struct snapshot.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNES_W 256
#define SNES_H 240

enum {
  BTN_B = 1 << 15, BTN_Y = 1 << 14, BTN_SELECT = 1 << 13, BTN_START = 1 << 12,
  BTN_UP = 1 << 11, BTN_DOWN = 1 << 10, BTN_LEFT = 1 << 9, BTN_RIGHT = 1 << 8,
  BTN_A = 1 << 7, BTN_X = 1 << 6, BTN_L = 1 << 5, BTN_R = 1 << 4,
};

typedef struct {
  uint16_t a, x, y, sp, pc, dp;
  uint8_t pb, db, p;
  bool e, waiting, stopped, nmi_pending;
} Cpu;

typedef struct {
  uint8_t dmap, bbad, a1b, dasb, nltr, unused;
  uint16_t a1t, das, a2a;
  bool hdma_do_transfer, hdma_completed;
} DmaChannel;

typedef struct {
  uint16_t hofs, vofs, tilemap_base, chr_base;
  uint8_t tilemap_size;
  bool big_tiles;
} BgLayer;

typedef struct {
  uint16_t vram[0x8000];
  uint16_t cgram[256];
  uint8_t oam[544];

  uint8_t inidisp, obsel, bgmode, mosaic, setini;
  uint16_t oam_addr_reload, oam_addr; // oam_addr is a byte address (0..0x3ff)
  bool oam_prio_rot;
  uint8_t oam_latch;
  BgLayer bg[4];
  uint8_t bgofs_latch, bghofs_latch, m7_latch;
  uint8_t vmain;
  uint16_t vram_addr, vram_latch;
  uint8_t m7sel;
  int16_t m7a, m7b, m7c, m7d, m7x, m7y, m7hofs, m7vofs;
  uint8_t cg_addr, cg_latch;
  bool cg_flip;
  uint8_t w12sel, w34sel, wobjsel, wh0, wh1, wh2, wh3, wbglog, wobjlog;
  uint8_t tm, ts, tmw, tsw, cgwsel, cgadsub;
  uint16_t fixed_color;
  uint16_t hlatch, vlatch;
  bool hlatch_flip, vlatch_flip, counter_latched;
  uint8_t ppu1_mdr, ppu2_mdr, stat77;
  uint16_t mosaic_start_line;
} Ppu;

typedef struct {
  int16_t buf[19];          // [0..2] tail of previous block, [3..18] current block
  uint32_t interp_pos;      // 12.12 fixed: integer part = sample index within block
  uint16_t brr_addr;
  uint8_t brr_header;
  bool brr_valid;
  int env, env_mode;        // 0 release, 1 attack, 2 decay, 3 sustain
  int kon_delay;
  int out;                  // last output (for pitch modulation)
} Voice;

typedef struct {
  uint8_t regs[128];
  Voice v[8];
  int counter;
  int noise;
  int echo_pos, echo_len;
  int echo_hist[8][2];
  int hist_pos;
  uint8_t kon_pending, koff_latched;
  bool every_other;
} Dsp;

typedef struct {
  uint8_t ram[0x10000];
  uint8_t a, x, y, sp, psw;
  uint16_t pc;
  bool halted;
  Dsp dsp;
  uint8_t inports[4], outports[4];
  uint8_t control, dsp_addr;
  uint8_t timer_target[3], timer_stage[3], timer_out[3];
  uint16_t timer_div[3];
  uint64_t cycles;          // SPC cycles executed
  int sample_div;
  int64_t debt;             // cycles owed (negative = ran ahead)
} Apu;

typedef struct Snes {
  uint32_t magic, version;
  Cpu cpu;
  Ppu ppu;
  Apu apu;
  DmaChannel dma[8];
  uint8_t wram[0x20000];
  uint8_t sram[0x8000];
  uint32_t wram_addr;

  uint8_t nmitimen, wrio, wrmpya, wrmpyb, wrdivb, hdmaen, memsel, mdr;
  uint16_t wrdiv, rddiv, rdmpy, htime, vtime;
  bool nmi_flag, irq_flag;
  uint16_t joy_auto[4];
  uint32_t joy_shift[2];
  bool joy_strobe;
  uint64_t joy_busy_until;

  uint16_t hpos, vpos;
  uint64_t master;
  uint64_t frames;
  uint64_t apu_master_sync;
  int vdisp;
  bool frame_done;

  uint32_t fb[SNES_W * SNES_H];   // BGRA8888, rows 0..223 (or 238) valid
  int fb_height;

  // ---- not part of the saved machine state (restored after load) ----
  uint16_t pad[2];
  const uint8_t *rom;
  uint32_t rom_size, rom_mask, sram_size;
  bool hirom;
  int16_t *audio_out;     // stereo int16 at 32 kHz, filled per frame
  int audio_count, audio_cap;
  int interp_mode;        // 0 = hardware gaussian, 1 = cubic hermite (modern)
} Snes;

extern Snes *g;

// lifecycle
Snes *snes_create(const uint8_t *rom, size_t size, char *title_out, size_t title_len);
void snes_destroy(Snes *s);
void snes_reset(Snes *s, bool hard);
void snes_run_frame(Snes *s);
void snes_set_pad(Snes *s, int port, uint16_t buttons);
const uint32_t *snes_framebuffer(const Snes *s);
uint8_t *snes_sram(Snes *s);

// save states
size_t snes_state_size(void);
bool snes_save_state(Snes *s, void *buf, size_t len);
bool snes_load_state(Snes *s, const void *buf, size_t len);

// internal API between modules
void snes_tick(int cycles);
uint8_t bus_read(uint32_t addr);
void bus_write(uint32_t addr, uint8_t v);
void bus_idle(void);

void cpu_reset(void);
void cpu_step(void);

void ppu_reset(void);
uint8_t ppu_read(uint16_t addr);
void ppu_write(uint16_t addr, uint8_t v);
void ppu_render_line(int line);
void ppu_vblank_start(void);
void ppu_latch_counters(void);

void apu_reset(void);
void apu_catch_up(void);
uint8_t apu_cpu_read(int port);
void apu_cpu_write(int port, uint8_t v);
void dsp_reset(void);
void dsp_write(uint8_t addr, uint8_t v);
uint8_t dsp_read(uint8_t addr);
void dsp_sample(void);

#ifdef __cplusplus
}
#endif

// Headless test runner: runs N frames with an optional input script and dumps PPM screenshots.
#include "../src/core/snes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: smwtest rom frames [outprefix] [script: frame:buttons,...]\n"); return 1; }
  FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *d = malloc(n); fread(d, 1, n, f); fclose(f);
  char title[32]; Snes *s = snes_create(d, n, title, sizeof title);
  printf("title '%s' hirom=%d sram=%u\n", title, s->hirom, s->sram_size);
  int frames = atoi(argv[2]); const char *pre = argc > 3 ? argv[3] : "out";
  const char *script = argc > 4 ? argv[4] : "";
  long audio = 0; double peak = 0;
  for (int i = 1; i <= frames; i++) {
    // script format: start-end:hexbuttons;...
    uint16_t b = 0; const char *p = script;
    while (*p) { int a, e; unsigned v; if (sscanf(p, "%d-%d:%x", &a, &e, &v) == 3 && i >= a && i <= e) b = v;
      p = strchr(p, ';'); if (!p) break; p++; }
    snes_set_pad(s, 0, b);
    snes_run_frame(s);
    audio += s->audio_count;
    for (int k = 0; k < s->audio_count * 2; k++) { double v = abs(s->audio_out[k]); if (v > peak) peak = v; }
    if (i % 60 == 0 || i == frames) {
      char fn[256]; snprintf(fn, sizeof fn, "%s_%05d.ppm", pre, i);
      FILE *o = fopen(fn, "wb"); fprintf(o, "P6 256 %d 255\n", s->fb_height);
      for (int y = 0; y < s->fb_height; y++) for (int x = 0; x < 256; x++) { uint32_t c = s->fb[y * 256 + x];
        fputc((c >> 16) & 255, o); fputc((c >> 8) & 255, o); fputc(c & 255, o); }
      fclose(o);
      printf("frame %d pc=%02X:%04X spc_pc=%04X audio=%ld peak=%.0f\n", i, s->cpu.pb, s->cpu.pc, s->apu.pc, audio, peak);
    }
  }
  return 0;
}

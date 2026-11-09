#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ao_init(double out_rate);
void ao_clear(void);
void ao_push(const int16_t *stereo, int frames);              // emulator thread
void ao_render(float *left, float *right, int frames);         // audio thread
void ao_set_params(int modern, float volume, float reverb, float width, int muted);
int ao_fill(void);

#ifdef __cplusplus
}
#endif

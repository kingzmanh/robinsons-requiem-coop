// rrq-coop test harness: hidden window, scripted input, screenshots.
// Env vars:
//   RRQ_HIDDEN=1          create the SDL window hidden
//   RRQ_INPUT=<file>      timed input script (see rrq_harness.c)
//   RRQ_SHOTDIR=<dir>     where "shot" commands and RRQ_SHOT_EVERY write BMPs
//   RRQ_SHOT_EVERY=<ms>   periodic screenshots
//   RRQ_QUIT_MS=<ms>      stop the VM after this long
#pragma once
#include "config.h"

int  rrq_harness_active(void);
int  rrq_harness_hidden(void);
void rrq_harness_init(void);
void rrq_harness_tick(void);                       // from the SDL event loop
void rrq_harness_after_render(const u32 *argb, int w, int h);
const u8 *rrq_get_keyboard_state(int *numkeys);    // SDL state + harness-held keys
int  rrq_harness_mouse(int *x, int *y, int *lb, int *rb, int *lbc, int *rbc);

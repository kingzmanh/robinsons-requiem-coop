// rrq-coop per-player input (see rrq_input.c).
#pragma once
#include "config.h"
#include <SDL2/SDL.h>
#include "sys.h"

int  rrq_input_player(void);
void rrq_input_select(int p);                 // VM thread, at entity slice start
mouse_t *rrq_input_mouse(void);               // mouse state of the player the VM reads now
const u8 *rrq_input_keys(const u8 *real);     // held-key array of that player
void rrq_input_p2_tick(u32 now);
void rrq_input_p2_key(SDL_Keycode kc, u32 now, u32 hold);
void rrq_input_p2_move(int x, int y);
void rrq_input_p2_click(int x, int y, int right, u32 now, u32 hold);

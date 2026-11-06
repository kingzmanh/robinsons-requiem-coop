// rrq-coop per-player input. Player 0 reads the real devices (SDL, plus harness overrides).
// Player 1 reads a virtual device (mouse, held keys, key latch) fed by the harness today, and
// later by a second gamepad or the network. The VM sees the device of the player that owns the
// entity currently running (rrq_input_select is called from rrq_coop_slice_begin).
#include "rrq_input.h"
#include "rrq_harness.h"
#include "alis.h"
#include "sys.h"
#include <SDL2/SDL.h>
#include <string.h>

extern mouse_t mouse;
extern SDL_Keysym button;

static int cur;                       // player whose input the VM reads now
static SDL_Keysym vbutton[2];         // key latch per player (io_inkey's `button`)
static mouse_t vmouse;                // player 1 mouse
static u8 vkeys[SDL_NUM_SCANCODES];   // player 1 held keys
static u32 vkey_release[SDL_NUM_SCANCODES];
static u32 vlb_release, vrb_release;

int rrq_input_player(void) { return cur; }

void rrq_input_select(int p)
{
    if (p == cur) return;
    vbutton[cur] = button;            // park this player's key latch
    button = vbutton[p];
    cur = p;
}

mouse_t *rrq_input_mouse(void) { return cur == 1 ? &vmouse : &mouse; }

const u8 *rrq_input_keys(const u8 *real)
{
    return cur == 1 ? vkeys : real;
}

// ---- feeding player 1 (any thread; plain stores, consumed by the VM thread) ----
void rrq_input_p2_tick(u32 now)
{
    for (int k = 0; k < SDL_NUM_SCANCODES; k++)
        if (vkeys[k] && now >= vkey_release[k]) vkeys[k] = 0;
    if (vmouse.lb && now >= vlb_release) vmouse.lb = 0;
    if (vmouse.rb && now >= vrb_release) vmouse.rb = 0;
}

void rrq_input_p2_key(SDL_Keycode kc, u32 now, u32 hold)
{
    SDL_Scancode sc = SDL_GetScancodeFromKey(kc);
    vkeys[sc] = 1; vkey_release[sc] = now + hold;
    SDL_Keysym ks = {0}; ks.sym = kc; ks.scancode = sc;
    if (cur == 1) button = ks; else vbutton[1] = ks;
}

void rrq_input_p2_move(int x, int y) { vmouse.x = x; vmouse.y = y; }

void rrq_input_p2_click(int x, int y, int right, u32 now, u32 hold)
{
    vmouse.x = x; vmouse.y = y;
    if (right) { vmouse.rb = 1; vmouse.rb_clicked = 1; vrb_release = now + hold; }
    else { vmouse.lb = 1; vmouse.lb_clicked = 1; vlb_release = now + hold; }
}

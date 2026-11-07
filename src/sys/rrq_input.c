// rrq-coop per-player input devices.
// Local play: player 0 reads the real devices (SDL + harness overrides); player 1 reads a
// virtual device (fed by the harness, later a second gamepad).
// Online (rrq_net): BOTH players read virtual devices filled from the lockstep input stream,
// so both PCs see exactly the same input on the same frame.
// The VM sees the device of the player that owns the running entity (rrq_input_select).
#include "rrq_input.h"
#include "rrq_harness.h"
#include "../rrq_net.h"
#include "alis.h"
#include "sys.h"
#include <SDL2/SDL.h>
#include <string.h>

extern mouse_t mouse;
extern SDL_Keysym button;

static int cur;
static rrq_dev_t dev[2];
static u32 vkey_release[SDL_NUM_SCANCODES];
static u32 vlb_release, vrb_release;

int rrq_input_player(void) { return cur; }

// lockstep (re)start: both PCs must begin with identical input state
void rrq_input_reset(void)
{
    memset(dev, 0, sizeof dev);
    memset(&button, 0, sizeof button);
    cur = 0;
}
rrq_dev_t *rrq_input_dev(int p) { return &dev[p & 1]; }

static int virt(int p) { return p == 1 || rrq_net_active(); }

void rrq_input_select(int p)
{
    if (p == cur) return;
    // the VM's key latch is the global `button`; park it in the device of the player we leave
    dev[cur].latch = button;
    button = dev[p].latch;
    cur = p;
}

mouse_t *rrq_input_mouse(void) { return virt(cur) ? &dev[cur].m : &mouse; }

const u8 *rrq_input_keys(const u8 *real) { return virt(cur) ? dev[cur].keys : real; }

// ---- feeding the local player-1 device from the harness (local tests only) ----
void rrq_input_p2_tick(u32 now)
{
    if (rrq_net_active()) return;
    for (int k = 0; k < SDL_NUM_SCANCODES; k++)
        if (dev[1].keys[k] && now >= vkey_release[k]) dev[1].keys[k] = 0;
    if (dev[1].m.lb && now >= vlb_release) dev[1].m.lb = 0;
    if (dev[1].m.rb && now >= vrb_release) dev[1].m.rb = 0;
}

void rrq_input_p2_key(SDL_Keycode kc, u32 now, u32 hold)
{
    SDL_Scancode sc = SDL_GetScancodeFromKey(kc);
    dev[1].keys[sc] = 1; vkey_release[sc] = now + hold;
    SDL_Keysym ks = {0}; ks.sym = kc; ks.scancode = sc;
    if (cur == 1) button = ks; else dev[1].latch = ks;
}

void rrq_input_p2_move(int x, int y) { dev[1].m.x = x; dev[1].m.y = y; }

void rrq_input_p2_click(int x, int y, int right, u32 now, u32 hold)
{
    dev[1].m.x = x; dev[1].m.y = y;
    if (right) { dev[1].m.rb = 1; dev[1].m.rb_clicked = 1; vrb_release = now + hold; }
    else { dev[1].m.lb = 1; dev[1].m.lb_clicked = 1; vlb_release = now + hold; }
}

// rrq-coop test harness. Input script format, one command per line ('#' = comment):
//   <ms> key <SDL key name> [hold_ms]     e.g. "1500 key Return", "2000 key Space 50"
//   <ms> click <x> <y> [l|r] [hold_ms]     game pixel coords (320x200)
//   <ms> move <x> <y>
//   <ms> shot <name>                       writes <RRQ_SHOTDIR>/<name>.bmp
//   <ms> savestate | loadstate             engine savestate (alis.state in the data dir)
//   <ms> mark <text>                       prints a marker line to stdout
//   <ms> quit
// Times are ms since the harness started; "+<ms>" means relative to the previous line.
#include "rrq_harness.h"
#include "alis.h"
#include "rrq_trace.h"
#include "rrq_coop.h"
#include "rrq_input.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { u32 t; char cmd[16]; char a[64]; int x, y, hold; char btn; } hev_t;

static hev_t *evs; static int nevs, iev;
static int active, hidden;
static u32 t0;
static char shotdir[512];
static u32 shot_every, next_shot, quit_ms;
static char pending_shot[128];
static u8 keys[SDL_NUM_SCANCODES];
static u32 key_release[SDL_NUM_SCANCODES];
static int m_override, mx = 160, my = 100, mlb, mrb, mlbc, mrbc;
static u32 lb_release, rb_release;
extern SDL_Keysym button;

int rrq_harness_active(void) { return active; }
int rrq_harness_hidden(void) { return hidden; }

void rrq_harness_init(void)
{
    const char *h = getenv("RRQ_HIDDEN");
    hidden = h && *h && *h != '0';
    const char *sd = getenv("RRQ_SHOTDIR");
    snprintf(shotdir, sizeof shotdir, "%s", sd ? sd : ".");
    const char *se = getenv("RRQ_SHOT_EVERY");
    shot_every = se ? (u32)atoi(se) : 0;
    const char *q = getenv("RRQ_QUIT_MS");
    quit_ms = q ? (u32)atoi(q) : 0;
    const char *in = getenv("RRQ_INPUT");
    if (in && *in)
    {
        FILE *f = fopen(in, "r");
        if (!f) { printf("[rrq] cannot open RRQ_INPUT %s\n", in); }
        else
        {
            char line[256]; u32 last = 0; int cap = 0;
            while (fgets(line, sizeof line, f))
            {
                char *p = line; while (*p == ' ' || *p == '\t') p++;
                if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
                hev_t e; memset(&e, 0, sizeof e); e.btn = 'l';
                char ts[32] = {0}; char b[16] = {0};
                int n = sscanf(p, "%31s %15s", ts, e.cmd);
                if (n < 2) continue;
                e.t = ts[0] == '+' ? last + (u32)atoi(ts + 1) : (u32)atoi(ts);
                last = e.t;
                char *rest = strstr(p, e.cmd) + strlen(e.cmd);
                if (!strcmp(e.cmd, "key")) { e.hold = 120; sscanf(rest, "%63s %d", e.a, &e.hold); }
                else if (!strcmp(e.cmd, "click")) { e.hold = 120; sscanf(rest, "%d %d %15s %d", &e.x, &e.y, b, &e.hold); if (b[0]) e.btn = b[0]; }
                else if (!strcmp(e.cmd, "move")) sscanf(rest, "%d %d", &e.x, &e.y);
                else { while (*rest == ' ') rest++; snprintf(e.a, sizeof e.a, "%s", rest); e.a[strcspn(e.a, "\r\n")] = 0; }
                if (nevs == cap) { cap = cap ? cap * 2 : 64; evs = realloc(evs, cap * sizeof *evs); }
                evs[nevs++] = e;
            }
            fclose(f);
            printf("[rrq] %d input events from %s\n", nevs, in);
        }
    }
    active = hidden || nevs || shot_every || quit_ms;
    t0 = SDL_GetTicks();
    next_shot = shot_every;
}

static void save_bmp(const u32 *argb, int w, int h, const char *name)
{
    char path[700];
    snprintf(path, sizeof path, "%s/%s.bmp", shotdir, name);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)argb, w, h, 32, w * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s) { SDL_SaveBMP(s, path); SDL_FreeSurface(s); }
    printf("[rrq] shot %s (t=%u)\n", path, SDL_GetTicks() - t0);
    fflush(stdout);
}

void rrq_harness_after_render(const u32 *argb, int w, int h)
{
    if (!active) return;
    u32 t = SDL_GetTicks() - t0;
    if (pending_shot[0]) { save_bmp(argb, w, h, pending_shot); pending_shot[0] = 0; }
    if (shot_every && t >= next_shot)
    {
        char n[64]; snprintf(n, sizeof n, "auto_%07u", t);
        save_bmp(argb, w, h, n);
        next_shot = t + shot_every;
    }
}

void rrq_harness_tick(void)
{
    if (!active) return;
    u32 t = SDL_GetTicks() - t0;
    for (int k = 0; k < SDL_NUM_SCANCODES; k++)
        if (keys[k] && t >= key_release[k]) keys[k] = 0;
    if (mlb && t >= lb_release) mlb = 0;
    rrq_input_p2_tick(t);
    if (mrb && t >= rb_release) mrb = 0;
    while (iev < nevs && evs[iev].t <= t)
    {
        hev_t *e = &evs[iev++];
        if (!strcmp(e->cmd, "key"))
        {
            SDL_Keycode kc = SDL_GetKeyFromName(e->a);
            SDL_Scancode sc = SDL_GetScancodeFromKey(kc);
            if (kc == SDLK_UNKNOWN) { printf("[rrq] unknown key '%s'\n", e->a); continue; }
            keys[sc] = 1; key_release[sc] = t + e->hold;
            button.sym = kc; button.scancode = sc; button.mod = 0;
        }
        else if (!strcmp(e->cmd, "click"))
        {
            m_override = 1; mx = e->x; my = e->y;
            if (e->btn == 'r') { mrb = 1; mrbc = 1; rb_release = t + e->hold; }
            else { mlb = 1; mlbc = 1; lb_release = t + e->hold; }
        }
        else if (!strcmp(e->cmd, "move")) { m_override = 1; mx = e->x; my = e->y; }
        else if (!strcmp(e->cmd, "shot")) snprintf(pending_shot, sizeof pending_shot, "%s", e->a);
        else if (!strcmp(e->cmd, "savestate")) alis.state = eAlisStateSave;
        else if (!strcmp(e->cmd, "loadstate")) alis.state = eAlisStateLoad;
        else if (!strcmp(e->cmd, "mark")) { printf("[rrq] mark t=%u %s\n", t, e->a); fflush(stdout); rrq_trace_mark(e->a); }
        else if (!strcmp(e->cmd, "dumpctx")) rrq_trace_dump_context(e->a);
        else if (!strcmp(e->cmd, "dument")) rrq_trace_dump_entities(e->a[0] ? e->a : "-");
        else if (!strcmp(e->cmd, "spawn")) { char nm[32] = {0}, fl[8] = {0}; int ow = 0; sscanf(e->a, "%31s %d %7s", nm, &ow, fl); rrq_coop_request_spawn(nm, ow | (!strcmp(fl, "rb") ? 0x100 : 0)); }
        else if (!strcmp(e->cmd, "avatar")) { char nm[32] = {0}; int r = 0, dx = 0, dy = 0; sscanf(e->a, "%31s %d %d %d", nm, &r, &dx, &dy); rrq_coop_request_avatar(nm, r, dx, dy); }
        else if (!strcmp(e->cmd, "npcavatar")) rrq_coop_request_npcavatar(e->a);
        else if (!strcmp(e->cmd, "dumprec")) rrq_coop_request_dump(e->a);
        else if (!strcmp(e->cmd, "p2key"))
        {
            char nm[48] = {0}; int hold = 120; sscanf(e->a, "%47s %d", nm, &hold);
            SDL_Keycode kc = SDL_GetKeyFromName(nm);
            if (kc != SDLK_UNKNOWN) rrq_input_p2_key(kc, t, hold);
        }
        else if (!strcmp(e->cmd, "p2click"))
        {
            int x = 0, y = 0, hold = 120; char b[8] = "l"; sscanf(e->a, "%d %d %7s %d", &x, &y, b, &hold);
            rrq_input_p2_click(x, y, b[0] == 'r', t, hold);
        }
        else if (!strcmp(e->cmd, "p2move")) { int x = 0, y = 0; sscanf(e->a, "%d %d", &x, &y); rrq_input_p2_move(x, y); }
        else if (!strcmp(e->cmd, "p2view")) rrq_coop_request_p2view(e->a);
        else if (!strcmp(e->cmd, "drive")) rrq_coop_request_drive(atoi(e->a));
        else if (!strcmp(e->cmd, "player")) rrq_coop_request_player(atoi(e->a));
        else if (!strcmp(e->cmd, "pokew") || !strcmp(e->cmd, "pokeb"))
        {
            unsigned off = 0; int val = 0;
            sscanf(e->a, "%x %d", &off, &val);
            rrq_coop_request_poke((int)off, val, e->cmd[4] == 'w');
        }
        else if (!strcmp(e->cmd, "quit")) { printf("[rrq] quit t=%u\n", t); alis.state = eAlisStateStopped; }
    }
    if (quit_ms && t >= quit_ms && alis.state) { printf("[rrq] RRQ_QUIT_MS reached\n"); alis.state = eAlisStateStopped; }
}

const u8 *rrq_get_keyboard_state(int *numkeys)
{
    static u8 merged[SDL_NUM_SCANCODES];
    int n = 0;
    const u8 *s = SDL_GetKeyboardState(&n);
    if (numkeys) *numkeys = n;
    if (!active) return rrq_input_keys(s);
    for (int k = 0; k < SDL_NUM_SCANCODES; k++) merged[k] = (k < n ? s[k] : 0) | keys[k];
    return rrq_input_keys(merged);
}

int rrq_harness_mouse(int *x, int *y, int *lb, int *rb, int *lbc, int *rbc)
{
    if (!m_override) return 0;
    *x = mx; *y = my;
    *lb = mlb; *rb = mrb;
    if (mlbc) { *lbc = 1; mlbc = 0; }
    if (mrbc) { *rbc = 1; mrbc = 0; }
    return 1;
}

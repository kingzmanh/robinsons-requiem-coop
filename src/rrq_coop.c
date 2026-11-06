// rrq-coop core (Phase 2 proof of concept): per-player main-variable records.
// The player-owned main-variable ranges (rrq_player_ranges.h, from analysis/player_vars.tsv)
// are kept per player; switching the active player swaps those bytes in and out of the
// main script's memory. Requests come from other threads and are applied on the VM thread
// at a safe point (top of the scheduler loop) via rrq_coop_service().
#include "rrq_coop.h"
#include "rrq_player_ranges.h"
#include "alis.h"
#include "mem.h"
#include "script.h"
#include "sys/rrq_input.h"
#include <strings.h>
#include <stdio.h>
#include <string.h>

#define RRQ_MAX_PLAYERS 2

static u8 *store[RRQ_MAX_PLAYERS];
static u32 store_size;
static int active;
static int have[RRQ_MAX_PLAYERS];
static volatile int req_player = -1;
static volatile int req_poke, poke_off, poke_val, poke_w;
static char req_spawn[32];
static volatile int req_spawn_flag;

static void ensure(void)
{
    if (store_size) return;
    for (u32 i = 0; i < RRQ_PLAYER_RANGE_COUNT; i++) store_size += rrq_player_ranges[i][1] - rrq_player_ranges[i][0];
    for (int p = 0; p < RRQ_MAX_PLAYERS; p++) store[p] = calloc(1, store_size);
}

static void copy_out(u8 *dst)
{
    u8 *m = alis.mem + alis.basemain;
    for (u32 i = 0, k = 0; i < RRQ_PLAYER_RANGE_COUNT; i++)
    {
        u32 n = rrq_player_ranges[i][1] - rrq_player_ranges[i][0];
        memcpy(dst + k, m + rrq_player_ranges[i][0], n);
        k += n;
    }
}

static void copy_in(const u8 *src)
{
    u8 *m = alis.mem + alis.basemain;
    for (u32 i = 0, k = 0; i < RRQ_PLAYER_RANGE_COUNT; i++)
    {
        u32 n = rrq_player_ranges[i][1] - rrq_player_ranges[i][0];
        memcpy(m + rrq_player_ranges[i][0], src + k, n);
        k += n;
    }
}

int rrq_coop_active_player(void) { return active; }

static int quiet;
void rrq_coop_switch_quiet(int p) { quiet = 1; rrq_coop_switch(p); quiet = 0; }

void rrq_coop_switch(int p)
{
    ensure();
    if (p < 0 || p >= RRQ_MAX_PLAYERS || p == active) return;
    copy_out(store[active]);
    have[active] = 1;
    if (!have[p])
    {
        // First use: the new player starts as a copy of the current one.
        memcpy(store[p], store[active], store_size);
        have[p] = 1;
        printf("[coop] player %d record created (%u bytes, cloned from player %d)\n", p, store_size, active);
    }
    copy_in(store[p]);
    if (!quiet) printf("[coop] active player %d -> %d\n", active, p);
    active = p;
    fflush(stdout);
}

void rrq_coop_request_player(int p) { req_player = p; }
void rrq_coop_request_poke(int off, int val, int is_word) { poke_off = off; poke_val = val; poke_w = is_word; req_poke = 1; }

static volatile int req_spawn_owner;
void rrq_coop_request_spawn(const char *name, int owner) { snprintf(req_spawn, sizeof req_spawn, "%s", name); req_spawn_owner = owner; req_spawn_flag = 1; }

static sAlisScriptLive *find_live(const char *name)
{
    for (int i = 0; i < MAX_SCRIPTS; i++)
        if (alis.live_scripts[i] && alis.live_scripts[i]->name && !strcasecmp(alis.live_scripts[i]->name, name))
            return alis.live_scripts[i];
    return NULL;
}

// Create a second instance of a loaded script as a new entity, inheriting position etc. from the
// first live instance of the same script (what clivin does when a script spawns a child).
sAlisScriptLive *rrq_coop_spawn(const char *name)
{
    sAlisScriptData *prog = NULL;
    for (int i = 0; i < alis.nbprog; i++)
        if (alis.loaded_scripts[i] && !strcasecmp(alis.loaded_scripts[i]->name, name)) { prog = alis.loaded_scripts[i]; break; }
    sAlisScriptLive *parent = find_live(name);
    if (!prog || !parent) { printf("[coop] spawn %s: not loaded / no parent\n", name); return NULL; }
    sAlisScriptLive *saved = alis.script;
    ALIS_SET_SCRIPT(parent);
    sAlisScriptLive *e = script_live(prog);
    if (e)
    {
        u8 *prev = alis.mem + parent->vram_org, *next = alis.mem + e->vram_org;
        // same inheritance as clivin (v3.x): position, angles, a few context words
        memcpy(next + 0x00, prev + 0x00, 4); memcpy(next + 0x08, prev + 0x08, 4); memcpy(next + 0x10, prev + 0x10, 4);
        memcpy(next + 0x18, prev + 0x18, 0x34 - 0x18);
        memcpy(next + 0x40, prev + 0x40, 2); memcpy(next + 0x44, prev + 0x44, 2); memcpy(next + 0x48, prev + 0x48, 2);
        printf("[coop] spawned %s vram=%06x (parent %06x)\n", name, e->vram_org, parent->vram_org);
    }
    ALIS_SET_SCRIPT(saved);
    fflush(stdout);
    return e;
}

// ---- entity ownership: which player's record must be active while an entity runs ----
#define RRQ_MAX_OWNED 32
static u32 owned_vram[RRQ_MAX_OWNED];
static u8 owned_player[RRQ_MAX_OWNED];
static int nowned;
static volatile int drive_val = -1;
void rrq_coop_switch_quiet(int p);     // test input for P2: movement command (DRDEP) applied each slice

void rrq_coop_set_owner(u32 vram, int p)
{
    for (int i = 0; i < nowned; i++)
        if (owned_vram[i] == vram) { owned_player[i] = (u8)p; return; }
    if (nowned < RRQ_MAX_OWNED) { owned_vram[nowned] = vram; owned_player[nowned++] = (u8)p; }
}

int rrq_coop_owner(u32 vram)
{
    for (int i = 0; i < nowned; i++)
        if (owned_vram[i] == vram) return owned_player[i];
    return 0;   // world entities and P1's own entities run in player 0's context
}

void rrq_coop_request_drive(int v) { drive_val = v; }

// Called right before the frame is composed: the displayed view is the local player's.
void rrq_coop_before_draw(void)
{
    if (nowned && active != 0) rrq_coop_switch_quiet(0);
    rrq_input_select(0);
}

// Called by alis_loop before an entity slice runs (VM thread).
void rrq_coop_slice_begin(void)
{
    if (!nowned || !alis.script) return;
    int p = rrq_coop_owner(alis.script->vram_org);
    if (p != active) rrq_coop_switch_quiet(p);
    rrq_input_select(p);
    if (p == 1 && drive_val >= 0)
        xwrite8(alis.basemain + 0x1a8f, (u8)drive_val);   // DRDEP: what P2's yoda would write
}

// ---- P2 view capture (proof of concept) ----
// Save the full engine state, point the 3D screen's camera at P2's hero with P2's record active,
// draw one frame, write it as a BMP, then restore the saved state.
#include "image.h"
#include <SDL2/SDL.h>
void alis_save_state(void);
void alis_load_state(void);
static char req_view[64];
static volatile int req_view_flag;
void rrq_coop_request_p2view(const char *name) { snprintf(req_view, sizeof req_view, "%s", name); req_view_flag = 1; }

static void save_pixelbuf(const char *path)
{
    pixelbuf_t *b = &host.pixelbuf;
    u32 *argb = malloc((size_t)b->w * b->h * 4);
    for (int i = 0; i < b->w * b->h; i++)
        argb[i] = b->rgb ? b->rgb[i] : (b->palette[b->data[i]] | 0xff000000u);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(argb, b->w, b->h, 32, b->w * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s) { SDL_SaveBMP(s, path); SDL_FreeSurface(s); }
    free(argb);
}

static void p2view(const char *name)
{
    sAlisScriptLive *p2hero = NULL;
    for (int i = 0; i < MAX_SCRIPTS; i++)
    {
        sAlisScriptLive *s = alis.live_scripts[i];
        if (s && s->name && !strcasecmp(s->name, "heros.IO") && rrq_coop_owner(s->vram_org) == 1) { p2hero = s; break; }
    }
    if (!p2hero) { printf("[coop] p2view: no P2 hero\n"); return; }
    int was = active;
    char path[700];
    const char *dir = getenv("RRQ_SHOTDIR");
    alis_save_state();
    rrq_coop_switch_quiet(1);
    u32 scr = alis.basemain + 0x0152;                        // RRQ 3D view screen (MAIN cdefsc 0x0152)
    u16 old = xread16(scr + 0x60);
    u16 p2id = xread16(p2hero->vram_org - 0x0e);             // the id heros passes to cscfollow (olocw -0x0e)
    xwrite16(scr + 0x60, p2id);
    printf("[coop] p2view: camera follow %04x -> %04x\n", old, p2id);
    draw();
    snprintf(path, sizeof path, "%s/%s.bmp", dir ? dir : ".", name);
    save_pixelbuf(path);
    printf("[coop] p2view saved %s\n", path);
    alis_load_state();
    active = was;
    fflush(stdout);
}

// ---- per-frame P2 render (split screen) ----
// After the local frame is drawn, snapshot the render-relevant state, draw again with P2's
// record (P2 camera), keep P2's pixels, and restore. The game state stays exactly as before.
#include "screen.h"
extern const u32 kSpriteMemSize;
static int split_mode = -1;
static u32 *p2frame;
static u16 p2w, p2h;
static SDL_mutex *p2lock;
static u8 *snap_mem, *snap_sprite, *snap_phys, *snap_log;
static sAlisVM snap_alis;
static sImage snap_image;
static sScreen snap_screen;
int rrq_in_p2_draw;

int rrq_coop_split(void)
{
    if (split_mode < 0) { const char *e = getenv("RRQ_SPLIT"); split_mode = e && *e && *e != '0'; }
    return split_mode;
}

// Copies P2's last frame (ARGB) into dst (w*h); returns 0 if none yet.
int rrq_coop_p2_frame(u32 *dst, int w, int h)
{
    if (!p2frame || !p2lock) return 0;
    SDL_LockMutex(p2lock);
    int ok = p2w == w && p2h == h;
    if (ok) memcpy(dst, p2frame, (size_t)w * h * 4);
    SDL_UnlockMutex(p2lock);
    return ok;
}

static int have_p2_hero(void)
{
    for (int i = 0; i < nowned; i++)
        if (owned_player[i] == 1) return 1;
    return 0;
}

void rrq_coop_after_draw(void)
{
    if (!rrq_coop_split() || !have_p2_hero() || rrq_in_p2_draw) return;
    u32 ram = alis.platform.ram_sz, fb = (u32)alis.platform.width * alis.platform.height;
    if (!snap_mem)
    {
        snap_mem = malloc(ram); snap_sprite = malloc(kSpriteMemSize);
        snap_phys = malloc(fb); snap_log = malloc(fb);
        p2lock = SDL_CreateMutex();
    }
    // snapshot
    snap_alis = alis; snap_image = image; snap_screen = screen;
    memcpy(snap_mem, alis.mem, ram);
    memcpy(snap_sprite, image.spritemem, kSpriteMemSize);
    memcpy(snap_phys, image.physic, fb);
    if (image.logic != image.physic) memcpy(snap_log, image.logic, fb);
    pixelbuf_t savedpb = host.pixelbuf;
    int was = active;

    rrq_coop_switch_quiet(1);
    rrq_in_p2_draw = 1;
    draw();
    rrq_in_p2_draw = 0;

    pixelbuf_t *b = &host.pixelbuf;
    SDL_LockMutex(p2lock);
    if (p2w != b->w || p2h != b->h) { free(p2frame); p2frame = malloc((size_t)b->w * b->h * 4); p2w = b->w; p2h = b->h; }
    for (int i = 0; i < b->w * b->h; i++)
        p2frame[i] = b->rgb ? b->rgb[i] : (b->palette[b->data[i]] | 0xff000000u);
    SDL_UnlockMutex(p2lock);

    // restore (the main script's memory comes back with P1's record in it)
    u8 *mem = alis.mem;
    memcpy(mem, snap_mem, ram);
    alis = snap_alis; image = snap_image; screen = snap_screen;
    memcpy(image.spritemem, snap_sprite, kSpriteMemSize);
    memcpy(image.physic, snap_phys, fb);
    if (image.logic != image.physic) memcpy(image.logic, snap_log, fb);
    host.pixelbuf = savedpb;
    active = was;
}

void rrq_coop_service(void)
{
    if (req_view_flag) { req_view_flag = 0; p2view(req_view); }
    if (req_spawn_flag)
    {
        req_spawn_flag = 0;
        sAlisScriptLive *e = rrq_coop_spawn(req_spawn);
        if (e && req_spawn_owner) { rrq_coop_set_owner(e->vram_org, req_spawn_owner); printf("[coop] %s vram=%06x owned by player %d\n", req_spawn, e->vram_org, req_spawn_owner); }
    }
    if (req_player >= 0) { int p = req_player; req_player = -1; rrq_coop_switch(p); }
    if (req_poke)
    {
        req_poke = 0;
        if (poke_w) xwrite16(alis.basemain + poke_off, (s16)poke_val);
        else xwrite8(alis.basemain + poke_off, (u8)poke_val);
        printf("[coop] poke main[%04x] = %d (player %d)\n", poke_off, poke_val, active);
        fflush(stdout);
    }
}

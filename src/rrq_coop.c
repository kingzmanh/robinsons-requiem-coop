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

// Called by alis_loop before an entity slice runs (VM thread).
void rrq_coop_slice_begin(void)
{
    if (!nowned || !alis.script) return;
    int p = rrq_coop_owner(alis.script->vram_org);
    if (p != active) rrq_coop_switch_quiet(p);
    if (p == 1 && drive_val >= 0)
        xwrite8(alis.basemain + 0x1a8f, (u8)drive_val);   // DRDEP: what P2's yoda would write
}

void rrq_coop_service(void)
{
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

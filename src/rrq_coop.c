// rrq-coop core (Phase 2 proof of concept): per-player main-variable records.
// The player-owned main-variable ranges (rrq_player_ranges.h, from analysis/player_vars.tsv)
// are kept per player; switching the active player swaps those bytes in and out of the
// main script's memory. Requests come from other threads and are applied on the VM thread
// at a safe point (top of the scheduler loop) via rrq_coop_service().
#define SDL_MAIN_HANDLED
#include "rrq_coop.h"
#include "rrq_player_ranges.h"
#include "alis.h"
#include "mem.h"
#include "script.h"
#include "image.h"
#include "sys/rrq_input.h"
#include "rrq_net.h"
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

static volatile int req_spawn_owner, req_spawn_rb;
void rrq_coop_request_spawn(const char *name, int owner) { snprintf(req_spawn, sizeof req_spawn, "%s", name); req_spawn_owner = owner & 0xff; req_spawn_rb = owner >> 8; req_spawn_flag = 1; }

static sAlisScriptLive *find_live(const char *name)
{
    for (int i = 0; i < MAX_SCRIPTS; i++)
        if (alis.live_scripts[i] && alis.live_scripts[i]->name && !strcasecmp(alis.live_scripts[i]->name, name))
            return alis.live_scripts[i];
    return NULL;
}

// ---- P2 avatar: a puppet entity (never runs its own code) that shows a figure from its script's
// graphics at P2's hero position; drawn in P1's view so P1 can see P2.
void put(u16 idx);
void savecoord(u32 addr);
void updtcoord(u32 addr);
static sAlisScriptLive *avatar;
static u16 avatar_res;
static s16 avatar_dx, avatar_dy;   // test offset: with no P2 hero, place it relative to P1's hero
static char req_avatar[32]; static int req_avatar_res = -1; static volatile int req_avatar_flag;
static char req_npcav[32]; static volatile int req_npcav_flag;
void rrq_coop_request_npcavatar(const char *s) { snprintf(req_npcav, sizeof req_npcav, "%s", s); req_npcav_flag = 1; }
void rrq_coop_request_avatar(const char *script, int res, int dx, int dy)
{ snprintf(req_avatar, sizeof req_avatar, "%s", script); req_avatar_res = res; avatar_dx = dx; avatar_dy = dy; req_avatar_flag = 1; }

static sAlisScriptLive *hero_of(int player)
{
    for (int i = 0; i < MAX_SCRIPTS; i++)
    {
        sAlisScriptLive *s = alis.live_scripts[i];
        if (s && s->name && !strcasecmp(s->name, "heros.IO") && rrq_coop_owner(s->vram_org) == player) return s;
    }
    return NULL;
}

static void avatar_create(const char *script, int res)
{
    if (avatar) { avatar_res = (u16)res; printf("[coop] avatar resource %d\n", res); fflush(stdout); return; }
    sAlisScriptData *prog = NULL;
    for (int i = 0; i < alis.nbprog; i++)
        if (alis.loaded_scripts[i] && !strcasecmp(alis.loaded_scripts[i]->name, script)) { prog = alis.loaded_scripts[i]; break; }
    sAlisScriptLive *h = hero_of(0);
    if (!prog || !h) { printf("[coop] avatar: %s not loaded or no hero\n", script); return; }
    sAlisScriptLive *saved = alis.script;
    ALIS_SET_SCRIPT(h);
    avatar = script_live(prog);
    ALIS_SET_SCRIPT(saved);
    if (!avatar) return;
    u8 *prev = alis.mem + h->vram_org, *next = alis.mem + avatar->vram_org;
    memcpy(next, prev, 0x34);
    // same setup as the game's 3D creatures: cscreen 0x0152, clinking 0, creducing 0x1f 0x08
    set_0x16_screen_id(avatar->vram_org, 0x0152);
    set_0x2a_clinking(avatar->vram_org, 0);
    set_0x26_creducing(avatar->vram_org, 0x1f);
    set_0x27_creducing(avatar->vram_org, 0x08);
    xwrite8(avatar->vram_org - 4, 0);          // asleep: the scheduler never runs its code
    avatar_res = (u16)res;
    printf("[coop] avatar %s res %d vram=%06x\n", script, res, avatar->vram_org);
    fflush(stdout);
}

// NPC avatar: a clone of a live NPC (its own code animates and draws it); we pin its position.
static sAlisScriptLive *npcav;
static void npcav_create(const char *script)
{
    sAlisScriptLive *src = find_live(script);
    if (!src) { printf("[coop] npcavatar: no live %s\n", script); return; }
    sAlisScriptData *prog = src->data;
    sAlisScriptLive *saved = alis.script;
    ALIS_SET_SCRIPT(src);
    npcav = script_live(prog);
    ALIS_SET_SCRIPT(saved);
    if (!npcav) return;
    u32 sz = prog->header.vram_alloc_sz;
    memcpy(alis.mem + npcav->vram_org, alis.mem + src->vram_org, sz);   // same character type/state vars
    printf("[coop] npcavatar %s vram=%06x (clone of %06x, %u bytes of vars)\n", script, npcav->vram_org, src->vram_org, sz);
    fflush(stdout);
}

static void npcav_pin(void)
{
    if (!npcav) return;
    sAlisScriptLive *t = hero_of(1);
    if (!t) t = hero_of(0);
    if (!t) return;
    u32 av = npcav->vram_org, h = t->vram_org;
    memcpy(alis.mem + av, alis.mem + h, 0x18);
    if (!hero_of(1)) { xwrite16(av, (s16)xread16(av) + avatar_dx); xwrite16(av + 8, (s16)xread16(av + 8) + avatar_dy); }
    xwrite16(av + 0x10, (s16)xread16(h + 0x10) + 6);    // NPC humans stand ~6 units above the hero's z
    memcpy(alis.mem + av + 0x18, alis.mem + h + 0x18, 0x18);
}

static u32 lastpos[2]; static int aframe[2], atick[2];   // avatar walk animation per view
static int avatar_target = 1;
static int avatar_freeze;      // P2 view pass: draw the avatar without advancing its animation     // which player's hero the avatar stands in for (the other player)
static void avatar_update(void)
{
    npcav_pin();
    if (!avatar) return;
    sAlisScriptLive *t = hero_of(avatar_target);
    u32 av = avatar->vram_org;
    // snapshot the avatar's previous position/angle first: updtcoord() then moves AND turns its
    // sprites by the difference (it only re-angles sprites when the angle changed since savecoord)
    sAlisScriptLive *saved = alis.script;
    ALIS_SET_SCRIPT(avatar);
    savecoord(av);
    if (t)
    {
        memcpy(alis.mem + av, alis.mem + t->vram_org, 0x18);   // x, y, z (+fractions)
        memcpy(alis.mem + av + 0x18, alis.mem + t->vram_org + 0x18, 0x18);   // angles
        xwrite16(av + 0x10, (s16)xread16(av + 0x10) + 6);                      // stand on the ground
    }
    else if ((t = hero_of(0)))
    {
        memcpy(alis.mem + av, alis.mem + t->vram_org, 0x30);
        xwrite16(av, (s16)xread16(av) + avatar_dx);
        xwrite16(av + 8, (s16)xread16(av + 8) + avatar_dy);
    }
    { const char *dz = getenv("RRQ_AVATAR_DZ"); if (dz) xwrite16(av + 0x10, (s16)xread16(av + 0x10) + atoi(dz)); }
    xwrite8(av - 4, 0);
    alis.flagmain = 0;
    image.invert_x = 0;
    image.depx = image.depy = image.depz = 0;
    image.numelem = 0;
    // body: walking frames 20/28/36/44 (as human.IO), upper part attached as element -2
    // walk animation: one state per view (the target player), advanced only by the main frame,
    // so the extra P2 view pass on one PC cannot change what both PCs draw/compute next
    int vi = avatar_target & 1;
    u32 posnow = (u32)xread16(av) ^ ((u32)xread16(av + 8) << 16);
    if (!avatar_freeze && posnow != lastpos[vi] && (++atick[vi] & 3) == 0) aframe[vi] = (aframe[vi] + 1) & 3;
    if (!avatar_freeze) lastpos[vi] = posnow;
    int frame = aframe[vi];
    if (avatar_res >= 1000)
    {
        put((u16)(20 + 8 * frame));
        image.depx = image.depy = image.depz = 0;
        image.numelem = (s8)0xfe;
        put((u16)(avatar_res - 1000));
    }
    else
        put(avatar_res);
    updtcoord(av);
    // putin() rewrites the sprite's angle field and updtcoord() only restores it on frames where the
    // angle changed, so set the facing of every avatar sprite from the player's heading each frame
    {
        u16 wcaz = xread16(av + ALIS_SCR_WCAZ);
        for (sSprite *sp = SPRITE_VAR(get_0x18_unknown(av)); sp != NULL; sp = SPRITE_VAR(sp->to_next))
        {
            u32 want = (sp->sprite_0x28 & 0xFFFF) | ((u32)wcaz << 16);
            if (sp->sprite_0x28 != want) { sp->sprite_0x28 = want; if (sp->state == 0) sp->state = 2; }
        }
    }
    ALIS_SET_SCRIPT(saved);
}

// Create a second instance of a loaded script as a new entity, inheriting position etc. from the
// first live instance of the same script (what clivin does when a script spawns a child).
static int spawning;     // rrq_coop_spawn clones are not UI children
sAlisScriptLive *rrq_coop_spawn(const char *name)
{
    sAlisScriptData *prog = NULL;
    for (int i = 0; i < alis.nbprog; i++)
        if (alis.loaded_scripts[i] && !strcasecmp(alis.loaded_scripts[i]->name, name)) { prog = alis.loaded_scripts[i]; break; }
    sAlisScriptLive *parent = find_live(name);
    if (!prog || !parent) { printf("[coop] spawn %s: not loaded / no parent\n", name); return NULL; }
    sAlisScriptLive *saved = alis.script;
    ALIS_SET_SCRIPT(parent);
    spawning = 1;
    sAlisScriptLive *e = script_live(prog);
    spawning = 0;
    if (e)
    {
        u8 *prev = alis.mem + parent->vram_org, *next = alis.mem + e->vram_org;
        // same inheritance as clivin (v3.x): position, angles, a few context words
        memcpy(next + 0x00, prev + 0x00, 4); memcpy(next + 0x08, prev + 0x08, 4); memcpy(next + 0x10, prev + 0x10, 4);
        memcpy(next + 0x18, prev + 0x18, 0x34 - 0x18);
        memcpy(next + 0x40, prev + 0x40, 2); memcpy(next + 0x44, prev + 0x44, 2); memcpy(next + 0x48, prev + 0x48, 2);
        // inherited context, as clivin does (screen id etc.): yoda draws its HUD into the screen
        // it inherits, without it every put() is dropped
        *(s16 *)(next - 0x28) = *(s16 *)(prev - 0x0e);
        *(s16 *)(next - 0x16) = *(s16 *)(prev - 0x16);
        *(s16 *)(next - 0x22) = *(s16 *)(prev - 0x22);
        *(s16 *)(next - 0x2a) = *(s16 *)(next - 0x0e);
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
static void rb_begin(void);
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

// ---- world-write rollback: for P2 instances of scripts that mix player and world updates
// (time.IO: body needs + clock/weather). P1's instance owns the world part; P2's instance only
// keeps its writes to the player record.
static u32 rb_vram[RRQ_MAX_OWNED];
static int nrb;
static u8 *rb_snap;
static u32 rb_size;
static int rb_armed;

void rrq_coop_set_rollback(u32 vram) { if (nrb < RRQ_MAX_OWNED) rb_vram[nrb++] = vram; }

static int is_rb(u32 vram)
{
    for (int i = 0; i < nrb; i++) if (rb_vram[i] == vram) return 1;
    return 0;
}

static void rb_begin(void)
{
    rb_armed = 0;
    if (!nrb || !alis.script || !is_rb(alis.script->vram_org)) return;
    u32 sz = alis.main->data->header.vram_alloc_sz;
    if (!sz) sz = 0x10000;
    if (sz != rb_size) { rb_snap = realloc(rb_snap, sz); rb_size = sz; }
    memcpy(rb_snap, alis.mem + alis.basemain, sz);
    rb_armed = 1;
}

static void npcav_pin(void);
void rrq_coop_slice_end(void)
{
    rrq_pal_world_end();
    rrq_pal_select(0);
    if (npcav && alis.script == npcav) npcav_pin();
    if (!rb_armed) return;
    rb_armed = 0;
    u8 *m = alis.mem + alis.basemain;
    u32 pos = 0;
    for (u32 i = 0; i < RRQ_PLAYER_RANGE_COUNT; i++)
    {
        u32 lo = rrq_player_ranges[i][0], hi = rrq_player_ranges[i][1];
        if (lo > pos && pos < rb_size) memcpy(m + pos, rb_snap + pos, (lo < rb_size ? lo : rb_size) - pos);
        if (hi > pos) pos = hi;
    }
    if (pos < rb_size) memcpy(m + pos, rb_snap + pos, rb_size - pos);
}

// Print a few body stats of both players (record of the inactive one comes from its store).
void rrq_coop_dump(const char *tag)
{
    static const struct { const char *n; u16 off; } V[] = {
        {"SANG", 0x1932}, {"EAU", 0x1936}, {"SAC", 0x1938}, {"FAT", 0x193e}, {"ENER", 0x1942},
        {"PSY", 0x1944}, {"TEMPEX", 0x1946}, {"SOMMEIL", 0x19dc}, {"VFAT", 0x1940}, {"clock1a08", 0x1a08} };
    ensure();
    copy_out(store[active]);
    for (int p = 0; p < RRQ_MAX_PLAYERS; p++)
    {
        if (!have[p] && p != active) continue;
        printf("[coop] rec %s P%d:", tag, p + 1);
        for (u32 v = 0; v < sizeof V / sizeof V[0]; v++)
        {
            // find the value inside the store
            u32 k = 0; int found = 0;
            for (u32 i = 0; i < RRQ_PLAYER_RANGE_COUNT && !found; i++)
            {
                u32 lo = rrq_player_ranges[i][0], hi = rrq_player_ranges[i][1];
                if (V[v].off >= lo && V[v].off + 1 < hi) { s16 val = (s16)(store[p][k + V[v].off - lo] | (store[p][k + V[v].off - lo + 1] << 8)); printf(" %s=%d", V[v].n, val); found = 1; }
                k += hi - lo;
            }
            if (!found) printf(" %s=(world)%d", V[v].n, (s16)xread16(alis.basemain + V[v].off));
        }
        printf("\n");
    }
    printf("[coop] world: day clock 0x3bec=%d main 0x1a08=%d timeclock=%u\n", (s16)xread16(alis.basemain + 0x3bec), (s16)xread16(alis.basemain + 0x1a08), alis.timeclock);
    fflush(stdout);
}

// ---- per-player HUD: both players' controller/mouse entities draw into the same HUD screen.
// Each view hides the other player's sprites by moving them far off-screen for that draw.
static const char *hud_scripts[] = { "yoda.IO", "souris.IO", "heros.IO", "time.IO", "caporal.IO" };
// UI entities a player's entities started (computer, map, inventory panels...): they draw into the
// shared HUD screen, so they belong to that player: they run in that player's context and are only
// shown in that player's view.
#define RRQ_MAX_UI 64
static u32 ui_vram[RRQ_MAX_UI];
static u8 ui_player[RRQ_MAX_UI];
static int nui;
static int ui_owner(u32 vram)
{
    for (int i = 0; i < nui; i++) if (ui_vram[i] == vram) return ui_player[i];
    return -1;
}
static int is_player_entity(sAlisScriptLive *e, int p);
void rrq_coop_entity_started(void *child_, void *parent_)
{
    sAlisScriptLive *child = child_, *parent = parent_;
    if (!child) return;
    // a new entity can reuse a dead one's memory: drop whatever the tables said about that address
    u32 v = child->vram_org;
    for (int i = 0; i < nui; i++) if (ui_vram[i] == v) { ui_vram[i] = ui_vram[--nui]; ui_player[i] = ui_player[nui]; i--; }
    for (int i = 0; i < nowned; i++) if (owned_vram[i] == v) { owned_vram[i] = owned_vram[--nowned]; owned_player[i] = owned_player[nowned]; i--; }
    for (int i = 0; i < nrb; i++) if (rb_vram[i] == v) { rb_vram[i] = rb_vram[--nrb]; i--; }
    if (!nowned || spawning || !parent) return;
    if (get_0x16_screen_id(child->vram_org) == 0x0152) return;      // world things (3D view) stay shared
    int p = is_player_entity(parent, 1) ? 1 : is_player_entity(parent, 0) ? 0 : -1;
    if (p < 0) return;
    int k = nui;
    for (int i = 0; i < nui; i++) if (ui_vram[i] == child->vram_org) { k = i; break; }
    if (k == nui) { if (nui >= RRQ_MAX_UI) return; nui++; }
    ui_vram[k] = child->vram_org; ui_player[k] = (u8)p;
    if (p == 1)
    {
        rrq_coop_set_owner(child->vram_org, 1);
        if (is_rb(parent->vram_org)) rrq_coop_set_rollback(child->vram_org);
    }
}

static int is_player_entity(sAlisScriptLive *e, int p)
{
    if (!e || !e->name) return 0;
    int u = ui_owner(e->vram_org);
    if (u >= 0) return u == p;
    int named = 0;
    for (u32 k = 0; k < sizeof hud_scripts / sizeof hud_scripts[0]; k++) if (!strcasecmp(e->name, hud_scripts[k])) named = 1;
    if (!named) return 0;
    int owner = -1;
    for (int i = 0; i < nowned; i++) if (owned_vram[i] == e->vram_org) { owner = owned_player[i]; break; }
    return p == 1 ? owner == 1 : owner < 0;     // player 1's are the game's original instances
}

static void hide_hud(int p, int hide)
{
    if (!nowned) return;
    s16 d = hide ? 8000 : -8000;
    for (int i = 0; i < MAX_SCRIPTS; i++)
    {
        sAlisScriptLive *e = alis.live_scripts[i];
        if (!is_player_entity(e, p)) continue;
        for (sSprite *sp = SPRITE_VAR(get_0x18_unknown(e->vram_org)); sp != NULL; sp = SPRITE_VAR(sp->to_next))
        {
            sp->depx += d;
            if (sp->state == 0) sp->state = 2;
        }
    }
}

// Called right before the frame is composed: the displayed view is the local player's.
void rrq_coop_start(void)
{
    // P2's set of player entities: hero, controller (actions/HUD logic), mouse/UI, body simulation
    if (hero_of(1)) return;
    static const char *own[] = { "heros.IO", "yoda.IO", "souris.IO" };
    for (int i = 0; i < 3; i++)
    {
        sAlisScriptLive *e = rrq_coop_spawn(own[i]);
        if (e) rrq_coop_set_owner(e->vram_org, 1);
    }
    sAlisScriptLive *t = rrq_coop_spawn("time.IO");
    if (t) { rrq_coop_set_owner(t->vram_org, 1); rrq_coop_set_rollback(t->vram_org); }
    if (!avatar && !getenv("RRQ_NO_AVATAR")) avatar_create("human.IO", 1116);
    printf("[coop] co-op started: P2 has hero/yoda/souris/time + avatar\n");
    fflush(stdout);
}

int rrq_coop_has_hero(void) { return hero_of(0) != NULL; }

// ---- co-op tables for an online resync (not part of the engine savestate) ----
typedef struct { u32 magic, store_size, nowned, nrb, avatar_vram; int active; int have[RRQ_MAX_PLAYERS];
                 u32 owned_vram[RRQ_MAX_OWNED]; u8 owned_player[RRQ_MAX_OWNED]; u32 rb_vram[RRQ_MAX_OWNED]; u16 avatar_res;
                 u32 an_lastpos[2]; int an_frame[2], an_tick[2];
                 u32 nui, ui_vram[RRQ_MAX_UI]; u8 ui_player[RRQ_MAX_UI]; } coop_blob_hdr;

u32 rrq_coop_export(u8 **out)
{
    ensure();
    copy_out(store[active]);
    u32 n = sizeof(coop_blob_hdr) + RRQ_MAX_PLAYERS * store_size;
    u8 *b = calloc(1, n);
    coop_blob_hdr *h = (coop_blob_hdr *)b;
    h->magic = 0x43505252u; h->store_size = store_size; h->nowned = nowned; h->nrb = nrb;
    h->avatar_vram = avatar ? avatar->vram_org : 0; h->avatar_res = avatar_res; h->active = active;
    for (int p = 0; p < RRQ_MAX_PLAYERS; p++) h->have[p] = have[p];
    memcpy(h->owned_vram, owned_vram, sizeof owned_vram); memcpy(h->owned_player, owned_player, sizeof owned_player);
    memcpy(h->rb_vram, rb_vram, sizeof rb_vram);
    h->nui = nui; memcpy(h->ui_vram, ui_vram, sizeof ui_vram); memcpy(h->ui_player, ui_player, sizeof ui_player);
    memcpy(h->an_lastpos, lastpos, sizeof lastpos); memcpy(h->an_frame, aframe, sizeof aframe); memcpy(h->an_tick, atick, sizeof atick);
    for (int p = 0; p < RRQ_MAX_PLAYERS; p++) memcpy(b + sizeof *h + p * store_size, store[p], store_size);
    *out = b;
    return n;
}

void rrq_coop_import(const u8 *b, u32 n)
{
    ensure();
    const coop_blob_hdr *h = (const coop_blob_hdr *)b;
    if (n < sizeof *h || h->magic != 0x43505252u || h->store_size != store_size) return;
    nowned = h->nowned; nrb = h->nrb; active = h->active;
    for (int p = 0; p < RRQ_MAX_PLAYERS; p++) have[p] = h->have[p];
    memcpy(owned_vram, h->owned_vram, sizeof owned_vram); memcpy(owned_player, h->owned_player, sizeof owned_player);
    memcpy(rb_vram, h->rb_vram, sizeof rb_vram);
    nui = h->nui; memcpy(ui_vram, h->ui_vram, sizeof ui_vram); memcpy(ui_player, h->ui_player, sizeof ui_player);
    memcpy(lastpos, h->an_lastpos, sizeof lastpos); memcpy(aframe, h->an_frame, sizeof aframe); memcpy(atick, h->an_tick, sizeof atick);
    for (int p = 0; p < RRQ_MAX_PLAYERS; p++) memcpy(store[p], b + sizeof *h + p * store_size, store_size);
    avatar = NULL; npcav = NULL;
    for (int i = 0; i < MAX_SCRIPTS; i++)
        if (h->avatar_vram && alis.live_scripts[i] && alis.live_scripts[i]->vram_org == h->avatar_vram) avatar = alis.live_scripts[i];
    avatar_res = h->avatar_res;
    rrq_pal_reset();
    printf("[coop] co-op tables imported (%u owned entities, avatar %s)\n", nowned, avatar ? "yes" : "no");
    fflush(stdout);
}

// ---- co-op saves: <name>.SAV.p2 holds player 2's record + hero position next to the game's save ----
#define P2SAVE_MAGIC 0x32505252u   // "RRP2"
static char sav_path[700];
static unsigned sav_mode;
static int sav_open;
static u8 *pending_rec;            // record loaded from a sidecar, applied once P2 exists again
static u8 pending_pos[0x30];

void rrq_coop_file_opened(const char *path, unsigned mode)
{
    size_t n = strlen(path);
    sav_open = n > 4 && !strcasecmp(path + n - 4, ".SAV");
    if (sav_open) { snprintf(sav_path, sizeof sav_path, "%s.p2", path); sav_mode = mode; }
}

void rrq_coop_file_closed(void)
{
    if (!sav_open) return;
    sav_open = 0;
    ensure();
    int writing = (sav_mode & 0x0100) != 0;   // 0x02xx/0x03xx = create/write, 0x00xx = read
    if (writing)
    {
        sAlisScriptLive *h = hero_of(1);
        if (!h || !have[1]) { remove(sav_path); return; }
        if (active == 1) copy_out(store[1]);
        FILE *f = fopen(sav_path, "wb");
        if (!f) return;
        u32 hdr[2] = { P2SAVE_MAGIC, store_size };
        fwrite(hdr, sizeof hdr, 1, f);
        fwrite(store[1], 1, store_size, f);
        fwrite(alis.mem + h->vram_org, 1, 0x30, f);
        fclose(f);
        printf("[coop] saved player 2 -> %s\n", sav_path); fflush(stdout);
    }
    else
    {
        FILE *f = fopen(sav_path, "rb");
        if (!f) { printf("[coop] %s: no player 2 data, player 2 starts fresh\n", sav_path); fflush(stdout); return; }
        u32 hdr[2] = {0};
        if (fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == P2SAVE_MAGIC && hdr[1] == store_size)
        {
            free(pending_rec); pending_rec = malloc(store_size);
            if (fread(pending_rec, 1, store_size, f) != store_size || fread(pending_pos, 1, 0x30, f) != 0x30) { free(pending_rec); pending_rec = NULL; }
            else { printf("[coop] loaded player 2 from %s\n", sav_path); fflush(stdout); }
        }
        fclose(f);
    }
}

static void apply_pending_p2(void)
{
    sAlisScriptLive *h = hero_of(1);
    if (!pending_rec || !h) return;
    memcpy(store[1], pending_rec, store_size);
    have[1] = 1;
    if (active == 1) copy_in(store[1]);
    memcpy(alis.mem + h->vram_org, pending_pos, 0x30);
    free(pending_rec); pending_rec = NULL;
    printf("[coop] player 2 restored from save\n"); fflush(stdout);
}

static int is_live(sAlisScriptLive *e)
{
    if (!e) return 0;
    for (int i = 0; i < MAX_SCRIPTS; i++) if (alis.live_scripts[i] == e) return 1;
    return 0;
}

// Drop ownership of entities that no longer exist (vram addresses get reused), forget a dead
// avatar, and (online) create player 2's entities whenever gameplay has a hero but P2 has none:
// at the first gameplay frame, and again after a level change or a loaded save killed them.
static int auto_start;
void rrq_coop_set_auto(int on) { auto_start = on; }
// The compass (caporal.IO) is one world entity that reads the hero it finds; P2 gets its own instance,
// which finds P2's hero through rrq_coop_fix_herolist. Respawned whenever the game recreates P1's.
static void ensure_compass(void)
{
    if (!hero_of(1)) return;
    int p1 = 0, p2 = 0;
    for (int k = 0; k < MAX_SCRIPTS; k++)
    {
        sAlisScriptLive *e = alis.live_scripts[k];
        if (!e || !e->name || strcasecmp(e->name, "caporal.IO")) continue;
        int o = -1;
        for (int i = 0; i < nowned; i++) if (owned_vram[i] == e->vram_org) { o = owned_player[i]; break; }
        if (o == 1) p2 = 1; else p1 = 1;
    }
    if (!p1 || p2) return;
    sAlisScriptLive *c = rrq_coop_spawn("caporal.IO");
    if (c) { rrq_coop_set_owner(c->vram_org, 1); rrq_coop_set_rollback(c->vram_org); }
}

static void maintain(void)
{
    for (int i = 0; i < nowned; i++)
    {
        int live = 0;
        for (int k = 0; k < MAX_SCRIPTS; k++) if (alis.live_scripts[k] && alis.live_scripts[k]->vram_org == owned_vram[i]) { live = 1; break; }
        if (!live) { owned_vram[i] = owned_vram[nowned - 1]; owned_player[i] = owned_player[nowned - 1]; nowned--; i--; }
    }
    for (int i = 0; i < nrb; i++)
    {
        int live = 0;
        for (int k = 0; k < MAX_SCRIPTS; k++) if (alis.live_scripts[k] && alis.live_scripts[k]->vram_org == rb_vram[i]) { live = 1; break; }
        if (!live) { rb_vram[i] = rb_vram[nrb - 1]; nrb--; i--; }
    }
    for (int i = 0; i < nui; i++)
    {
        int live = 0;
        for (int k = 0; k < MAX_SCRIPTS; k++) if (alis.live_scripts[k] && alis.live_scripts[k]->vram_org == ui_vram[i]) { live = 1; break; }
        if (!live) { ui_vram[i] = ui_vram[--nui]; ui_player[i] = ui_player[nui]; i--; }
    }
    if (avatar && !is_live(avatar)) avatar = NULL;
    if (npcav && !is_live(npcav)) npcav = NULL;
    if ((auto_start || rrq_net_active()) && hero_of(0) && !hero_of(1)) rrq_coop_start();
    ensure_compass();
    apply_pending_p2();
}

void rrq_coop_before_draw(void)
{
    rrq_net_frame();
    maintain();
    if (nowned && active != 0) rrq_coop_switch_quiet(0);
    rrq_input_select(0);
    rrq_pal_select(0);
    avatar_update();
    hide_hud(1, 1);
}

// World entities (NPCs, animals, objects) act on the nearest player: they run with that player's
// record active, so e.g. a tiger's bite wounds the player it attacked (cneartyp already picks
// the nearest hero). Entities without a position (MAIN, zone, UI scripts) stay with player 1.
static sAlisScriptLive *hero_of(int player);

// interaction: a world entity that just received a message from a player's entity (a click, a
// "use", a trade...) runs in that player's context for a while, whoever stands closer
#define RRQ_MAX_INTERACT 16
static u32 inter_vram[RRQ_MAX_INTERACT], inter_until[RRQ_MAX_INTERACT];
static u8 inter_player[RRQ_MAX_INTERACT];
void rrq_coop_note_message(u32 vram)
{
    if (!nowned || !alis.script) return;
    int from = -1;
    for (int i = 0; i < nowned; i++) if (owned_vram[i] == alis.script->vram_org) { from = owned_player[i]; break; }
    if (from < 0) return;                       // world-to-world messages keep the nearest rule
    int slot = 0;
    for (int i = 0; i < RRQ_MAX_INTERACT; i++)
    {
        if (inter_vram[i] == vram) { slot = i; break; }
        if (inter_until[i] < inter_until[slot]) slot = i;
    }
    inter_vram[slot] = vram; inter_player[slot] = (u8)from;
    inter_until[slot] = alis.timeclock + 60 * 3;   // ~3 s of game time
}

static int context_for(u32 vram)
{
    int o = -1;
    for (int i = 0; i < nowned; i++) if (owned_vram[i] == vram) { o = owned_player[i]; break; }
    if (o >= 0) return o;
    for (int i = 0; i < RRQ_MAX_INTERACT; i++)
        if (inter_vram[i] == vram && alis.timeclock < inter_until[i]) return inter_player[i];
    s16 x = (s16)xread16(vram), y = (s16)xread16(vram + 8);
    if (!x && !y) return 0;
    sAlisScriptLive *h0 = hero_of(0), *h1 = hero_of(1);
    if (!h0 || !h1) return 0;
    s32 dx0 = x - (s16)xread16(h0->vram_org), dy0 = y - (s16)xread16(h0->vram_org + 8);
    s32 dx1 = x - (s16)xread16(h1->vram_org), dy1 = y - (s16)xread16(h1->vram_org + 8);
    return (dx1 * dx1 + dy1 * dy1) < (dx0 * dx0 + dy0 * dy0) ? 1 : 0;
}

// cfindtyp results: when an entity running in player 2's context looks up one of player 1's own
// entities (hero, yoda, souris, time), it gets player 2's instance of that script instead.
static int owner_of_vram(u32 vram)
{
    for (int i = 0; i < nowned; i++) if (owned_vram[i] == vram) return owned_player[i];
    return -1;
}
void rrq_coop_fix_herolist(s16 *tab, int n)
{
    if (!nowned || active == 0) return;
    for (int t = 0; t < n; t++)
    {
        int slot = tab[t] / ENT_STRIDE;
        if (slot < 0 || slot >= MAX_SCRIPTS) continue;
        sAlisScriptLive *e = alis.live_scripts[slot];
        if (!e || !e->name || owner_of_vram(e->vram_org) >= 0) continue;   // already P2's, or world
        for (int i = 0; i < MAX_SCRIPTS; i++)
        {
            sAlisScriptLive *c = alis.live_scripts[i];
            if (c && c->name && c != e && !strcasecmp(c->name, e->name) && owner_of_vram(c->vram_org) == active)
            { tab[t] = (s16)(i * ENT_STRIDE); break; }
        }
    }
}

// Host-only session actions: in player 2's computer screen (ordi.IO) the LOAD and NEW game buttons
// would load / restart the game for both players, so player 2's clicks on them are dropped.
// Depends only on game state and P2's input, so both PCs drop the same clicks.
static const struct { s16 x0, y0, x1, y1; } host_only_btn[] = {
    { 104,  8, 147, 29 },   // LOAD (floppy, arrow right)
    { 109, 49, 147, 73 },   // NEW game
};
static int p2_computer_open(void)
{
    for (int i = 0; i < nui; i++)
    {
        if (ui_player[i] != 1) continue;
        for (int k = 0; k < MAX_SCRIPTS; k++)
        {
            sAlisScriptLive *e = alis.live_scripts[k];
            if (e && e->vram_org == ui_vram[i] && e->name && !strcasecmp(e->name, "ordi.IO")) return 1;
        }
    }
    return 0;
}
static void block_p2_host_buttons(void)
{
    mouse_t *m = rrq_input_mouse();
    if (!m || !(m->lb || m->lb_clicked) || !p2_computer_open()) return;
    for (u32 i = 0; i < sizeof host_only_btn / sizeof host_only_btn[0]; i++)
        if (m->x >= host_only_btn[i].x0 && m->x <= host_only_btn[i].x1 && m->y >= host_only_btn[i].y0 && m->y <= host_only_btn[i].y1)
        {
            m->lb = 0; m->lb_clicked = 0;
            return;
        }
}

// Called by alis_loop before an entity slice runs (VM thread).
void rrq_coop_slice_begin(void)
{
    if (!nowned || !alis.script) return;
    int p = context_for(alis.script->vram_org);
    if (p != active) rrq_coop_switch_quiet(p);
    rrq_input_select(p);
    if (p == 1) block_p2_host_buttons();
    rb_begin();
    // palette: a player's own entities work on that player's palette, world entities on both
    if (is_player_entity(alis.script, 1)) rrq_pal_select(1);
    else { rrq_pal_select(0); if (!is_player_entity(alis.script, 0)) rrq_pal_world_begin(); }
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
static u8 *snap_mem, *snap_sprite, *snap_phys, *snap_log, *snap_bg, *snap_depth;
static u32 *snap_terr;
static u32 snap_bg_n, snap_terr_n;
extern u32 image_buf_cap;
static sAlisVM snap_alis;
static sImage snap_image;
static sScreen snap_screen;
int rrq_in_p2_draw;
static int p2pass_skip;
void rrq_coop_skip_p2pass(int n) { p2pass_skip = n; }

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

// The window shows the local frame with the palette it was drawn with: P2's entities swap their own
// palette in while they run, which must not leak into what is on screen.
static u32 disp_pal[256];
static int disp_pal_ok;
void sys_lock_renderer(void);
void sys_unlock_renderer(void);
const u32 *rrq_coop_display_palette(void) { return disp_pal_ok && nowned ? disp_pal : NULL; }

void rrq_coop_after_draw(void)
{
    hide_hud(1, 0);
    if (nowned)
    {
        sys_lock_renderer();
        memcpy(disp_pal, image.mpalet, sizeof disp_pal);
        disp_pal_ok = 1;
        sys_unlock_renderer();
    }
    if (getenv("RRQ_NO_P2PASS")) return;   // test switch
    if (p2pass_skip > 0) { p2pass_skip--; return; }
    if (!(rrq_coop_split() || rrq_net_local_player() == 1) || !have_p2_hero() || rrq_in_p2_draw) return;
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
    // host-side render caches the next frame (and pixel picking) build on
    if (image.bgcache_alloc)
    {
        if (snap_bg_n != image_buf_cap) { free(snap_bg); snap_bg = malloc(image_buf_cap); snap_bg_n = image_buf_cap; }
        memcpy(snap_bg, image.bgcache_alloc, image_buf_cap);
    }
    u32 tn = image.terrgba ? (u32)image.terrgbaw * (image.terrgbah + 9) : 0;
    if (tn)
    {
        if (snap_terr_n != tn) { free(snap_terr); free(snap_depth); snap_terr = malloc(tn * 4); snap_depth = malloc(tn); snap_terr_n = tn; }
        memcpy(snap_terr, image.terrgba, tn * 4);
        if (image.depthbuf) memcpy(snap_depth, image.depthbuf, tn);
    }
    pixelbuf_t savedpb = host.pixelbuf;
    int was = active;

    rrq_coop_switch_quiet(1);
    rrq_pal_select(1);
    hide_hud(0, 1);
    avatar_target = 0;            // in P2's view the avatar is P1
    avatar_freeze = 1;
    avatar_update();
    avatar_freeze = 0;
    avatar_target = 1;
    // full redraw: the screens normally repaint only what changed since the last (P1) frame
    image.fremap = 1;
    for (u16 sc = screen.ptscreen; sc; sc = xread16(alis.basemain + sc + 0x5c))
        set_scr_state(sc, (s8)(get_scr_state(sc) | 0x80));
    rrq_in_p2_draw = 1;
    draw();
    rrq_in_p2_draw = 0;

    pixelbuf_t *b = &host.pixelbuf;
    SDL_LockMutex(p2lock);
    if (p2w != b->w || p2h != b->h) { free(p2frame); p2frame = malloc((size_t)b->w * b->h * 4); p2w = b->w; p2h = b->h; }
    // read the freshly drawn page directly (host.pixelbuf.data is only re-pointed when insid == 0)
    const u8 *src = image.physic ? image.physic : b->data;
    for (int i = 0; i < b->w * b->h; i++)
        p2frame[i] = b->rgb ? b->rgb[i] : (b->palette[src[i]] | 0xff000000u);
    SDL_UnlockMutex(p2lock);
    rrq_pal_select(0);

    // restore (the main script's memory comes back with P1's record in it)
    u8 *mem = alis.mem;
    memcpy(mem, snap_mem, ram);
    int live_state = alis.state;          // may have been changed by another thread (quit/save)
    alis = snap_alis; image = snap_image; screen = snap_screen;
    alis.state = live_state;
    memcpy(image.spritemem, snap_sprite, kSpriteMemSize);
    memcpy(image.physic, snap_phys, fb);
    if (image.logic != image.physic) memcpy(image.logic, snap_log, fb);
    if (image.bgcache_alloc && snap_bg_n == image_buf_cap) memcpy(image.bgcache_alloc, snap_bg, image_buf_cap);
    if (tn && image.terrgba) { memcpy(image.terrgba, snap_terr, tn * 4); if (image.depthbuf) memcpy(image.depthbuf, snap_depth, tn); }
    host.pixelbuf = savedpb;
    active = was;
}

static char req_dump[32]; static volatile int req_dump_flag;
void rrq_coop_request_dump(const char *t) { snprintf(req_dump, sizeof req_dump, "%s", t); req_dump_flag = 1; }

static volatile int req_start;
void rrq_coop_request_start(void) { req_start = 1; }

static volatile int req_pos_flag, req_pos_p, req_pos_x, req_pos_y, req_pos_a;
void rrq_coop_request_setpos(int p, int x, int y, int a) { req_pos_p = p; req_pos_x = x; req_pos_y = y; req_pos_a = a; req_pos_flag = 1; }

void rrq_coop_service(void)
{
    if (req_pos_flag)
    {
        req_pos_flag = 0;
        sAlisScriptLive *h = hero_of(req_pos_p);
        if (h) { xwrite16(h->vram_org, (s16)req_pos_x); xwrite16(h->vram_org + 8, (s16)req_pos_y); xwrite16(h->vram_org + 0x28, (s16)req_pos_a); printf("[coop] P%d hero -> (%d,%d) ang %d\n", req_pos_p + 1, req_pos_x, req_pos_y, req_pos_a); fflush(stdout); }
    }
    if (req_start) { req_start = 0; rrq_coop_start(); }
    if (req_dump_flag) { req_dump_flag = 0; rrq_coop_dump(req_dump); }
    if (req_avatar_flag) { req_avatar_flag = 0; avatar_create(req_avatar, req_avatar_res); }
    if (req_npcav_flag) { req_npcav_flag = 0; npcav_create(req_npcav); }
    if (req_view_flag) { req_view_flag = 0; p2view(req_view); }
    if (req_spawn_flag)
    {
        req_spawn_flag = 0;
        sAlisScriptLive *e = rrq_coop_spawn(req_spawn);
        if (e && req_spawn_rb) rrq_coop_set_rollback(e->vram_org);
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

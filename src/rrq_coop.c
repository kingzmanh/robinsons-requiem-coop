// rrq-coop core (Phase 2 proof of concept): per-player main-variable records.
// The player-owned main-variable ranges (rrq_player_ranges.h, from analysis/player_vars.tsv)
// are kept per player; switching the active player swaps those bytes in and out of the
// main script's memory. Requests come from other threads and are applied on the VM thread
// at a safe point (top of the scheduler loop) via rrq_coop_service().
#include "rrq_coop.h"
#include "rrq_player_ranges.h"
#include "alis.h"
#include "mem.h"
#include <stdio.h>
#include <string.h>

#define RRQ_MAX_PLAYERS 2

static u8 *store[RRQ_MAX_PLAYERS];
static u32 store_size;
static int active;
static int have[RRQ_MAX_PLAYERS];
static volatile int req_player = -1;
static volatile int req_poke, poke_off, poke_val, poke_w;

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
    printf("[coop] active player %d -> %d\n", active, p);
    active = p;
    fflush(stdout);
}

void rrq_coop_request_player(int p) { req_player = p; }
void rrq_coop_request_poke(int off, int val, int is_word) { poke_off = off; poke_val = val; poke_w = is_word; req_poke = 1; }

void rrq_coop_service(void)
{
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

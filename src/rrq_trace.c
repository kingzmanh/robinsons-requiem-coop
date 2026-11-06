// rrq-coop analysis tracing.
//   RRQ_TRACE_MAIN=<file>  log every change to the main script's variables, per entity slice:
//                          tick \t script \t entity_vram \t offset \t len \t old_hex \t new_hex
//   RRQ_TRACE_ENT=<file>   log entity table changes (spawns/kills) per slice
// Harness "mark" lines are copied into the trace so actions can be lined up with writes.
#include "rrq_trace.h"
#include "alis.h"
#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *tf;
static u8 *snap;
static u32 snap_size;
static int inited;
static char cur_name[64];
static u32 cur_vram;

static void init(void)
{
    inited = 1;
    const char *p = getenv("RRQ_TRACE_MAIN");
    if (p && *p)
    {
        tf = fopen(p, "w");
        if (tf) fprintf(tf, "tick\tscript\tvram\toffset\tlen\told\tnew\n");
    }
}

void rrq_trace_mark(const char *text)
{
    if (!inited) init();
    if (tf) { fprintf(tf, "#MARK\t%u\t%s\n", alis.timeclock, text); fflush(tf); }
}

void rrq_trace_slice_begin(void)
{
    if (!inited) init();
    if (!tf || !alis.main || !alis.script) return;
    u32 sz = alis.main->data->header.vram_alloc_sz;
    if (!sz) sz = 0x10000;
    if (sz != snap_size) { snap = realloc(snap, sz); snap_size = sz; }
    memcpy(snap, alis.mem + alis.basemain, sz);
    snprintf(cur_name, sizeof cur_name, "%s", alis.script->name ? alis.script->name : "?");
    cur_vram = alis.script->vram_org;
}

void rrq_trace_slice_end(void)
{
    if (!tf || !snap) return;
    const u8 *now = alis.mem + alis.basemain;
    for (u32 i = 0; i < snap_size; i++)
    {
        if (snap[i] == now[i]) continue;
        u32 j = i;
        while (j < snap_size && (snap[j] != now[j] || (j + 1 < snap_size && snap[j + 1] != now[j + 1] && j - i < 64))) j++;
        u32 len = j - i;
        if (len > 64) len = 64;
        fprintf(tf, "%u\t%s\t%06x\t%04x\t%u\t", alis.timeclock, cur_name, cur_vram, i, len);
        for (u32 k = 0; k < len; k++) fprintf(tf, "%02x", snap[i + k]);
        fputc('\t', tf);
        for (u32 k = 0; k < len; k++) fprintf(tf, "%02x", now[i + k]);
        fputc('\n', tf);
        i = j;
    }
}

// List every live entity (slot, script, vram) to stdout and the trace.
void rrq_trace_dump_entities(const char *tag)
{
    if (!inited) init();
    int n = 0;
    for (int i = 0; i < MAX_SCRIPTS; i++)
    {
        sAlisScriptLive *s = alis.live_scripts[i];
        if (!s) continue;
        u32 v = s->vram_org;
        printf("[rrq] ent %s slot=%04x id=%d script=%s vram=%06x pos=(%d,%d,%d) ang=(%d,%d,%d) wait=%d\n", tag, i * ENT_STRIDE, i,
               s->name ? s->name : "?", v, (s16)xread16(v), (s16)xread16(v + 8), (s16)xread16(v + 0x10),
               (s16)xread16(v + 0x18), (s16)xread16(v + 0x20), (s16)xread16(v + 0x28), xread8(v - 4));
        if (tf) fprintf(tf, "#ENT\t%u\t%s\t%04x\t%s\t%06x\n", alis.timeclock, tag, i * ENT_STRIDE, s->name ? s->name : "?", s->vram_org);
        n++;
    }
    printf("[rrq] ent %s total=%d\n", tag, n);
    fflush(stdout);
}

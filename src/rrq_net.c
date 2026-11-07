// rrq-coop online co-op: deterministic lockstep over TCP.
//
// Both PCs run the same VM from the same start state. Per VM frame (one scheduler pass) each side
// sends its local player's input for frame f + delay; frame f runs only when both inputs for f
// are known. Player 0 = host, player 1 = client. The host also sends the number of 17 ms timer
// ticks to apply before each frame (the original timer drives alis.timeclock, waits and palette
// fades), so time advances identically. Every 30 frames both sides exchange a checksum of the
// main script's memory to detect desyncs.
//
// Env (until there is a menu):  RRQ_NET=host[:port]  or  RRQ_NET=join:<ip>[:port]
//                               RRQ_NET_DELAY=<frames> (default 3)
#define SDL_MAIN_HANDLED
#include <winsock2.h>
#include <ws2tcpip.h>
#include "rrq_net.h"
#include "rrq_coop.h"
#include "sys/rrq_input.h"
#include "alis.h"
#include "mem.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_MAGIC 0x31515252u   // "RRQ1"
#define RING 64
#define MAXHELD 8

typedef struct {
    u32 frame;
    s16 mx, my;
    u8 lb, rb, lbc, rbc;
    s32 key_sym;      // key pressed since the last frame (0 = none)
    u16 key_sc;
    u8 nheld, pad;
    u16 held[MAXHELD];
    u16 ticks;        // host: timer ticks to run before this frame
    u16 pad2;
    u32 cs_frame, cs; // checksum report (cs_frame = 0: none)
} pkt_t;

static int mode;                  // 0 off, 1 host, 2 client
static SOCKET sock = INVALID_SOCKET;
static int delay = 3;
static u32 frame;                 // next frame to run
static pkt_t local_q[RING], remote_q[RING];
static u8 have_local[RING], have_remote[RING];
static SDL_atomic_t pending_ticks;
static SDL_mutex *klock;
static s32 lkey_sym; static u16 lkey_sc;
static u32 my_cs[RING], my_cs_frame[RING];
static int started;

int rrq_net_active(void) { return mode != 0; }
int rrq_net_local_player(void) { return mode == 2 ? 1 : 0; }

static int send_all(const void *b, int n)
{
    const char *p = b;
    while (n > 0) { int r = send(sock, p, n, 0); if (r <= 0) return -1; p += r; n -= r; }
    return 0;
}

static int recv_all(void *b, int n)
{
    char *p = b;
    while (n > 0) { int r = recv(sock, p, n, 0); if (r <= 0) return -1; p += r; n -= r; }
    return 0;
}

void rrq_net_init(void)
{
    const char *e = getenv("RRQ_NET");
    if (!e || !*e) return;
    const char *d = getenv("RRQ_NET_DELAY");
    if (d) delay = atoi(d);
    if (delay < 1) delay = 1;
    if (delay > RING / 2) delay = RING / 2;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    klock = SDL_CreateMutex();
    int port = 27531;
    char host[128] = "127.0.0.1";
    u32 seed = 0;
    if (!strncmp(e, "host", 4))
    {
        if (e[4] == ':') port = atoi(e + 5);
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof one);
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { printf("[net] cannot listen on %d\n", port); return; }
        printf("[net] hosting on port %d, waiting for player 2...\n", port); fflush(stdout);
        sock = accept(ls, NULL, NULL);
        closesocket(ls);
        seed = (u32)SDL_GetTicks() * 2654435761u;
        u32 hello[3] = { NET_MAGIC, (u32)delay, seed };
        send_all(hello, sizeof hello);
        mode = 1;
    }
    else if (!strncmp(e, "join:", 5))
    {
        snprintf(host, sizeof host, "%s", e + 5);
        char *c = strchr(host, ':'); if (c) { *c = 0; port = atoi(c + 1); }
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port);
        inet_pton(AF_INET, host, &a.sin_addr);
        printf("[net] joining %s:%d ...\n", host, port); fflush(stdout);
        int tries = 0;
        while (connect(sock, (struct sockaddr *)&a, sizeof a) != 0)
        {
            if (++tries > 300) { printf("[net] cannot connect\n"); return; }
            closesocket(sock); sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); Sleep(100);
        }
        u32 hello[3];
        if (recv_all(hello, sizeof hello) || hello[0] != NET_MAGIC) { printf("[net] bad hello\n"); return; }
        delay = (int)hello[1]; seed = hello[2];
        mode = 2;
    }
    int one = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    srand(seed);
    printf("[net] connected as player %d, input delay %d frames, seed %08x\n", mode == 1 ? 1 : 2, delay, seed);
    fflush(stdout);
}

// SDL timer thread: in net mode the timer only counts; ticks are applied at frame boundaries.
void rrq_net_timer_tick(void) { SDL_AtomicAdd(&pending_ticks, 1); }

// SDL / harness thread: a key went down on this PC.
void rrq_net_local_key(s32 sym, u16 sc)
{
    if (!klock) return;
    SDL_LockMutex(klock); lkey_sym = sym; lkey_sc = sc; SDL_UnlockMutex(klock);
}

static void capture(pkt_t *p, u32 f)
{
    extern mouse_t mouse;
    memset(p, 0, sizeof *p);
    p->frame = f;
    p->mx = (s16)mouse.x; p->my = (s16)mouse.y; p->lb = mouse.lb; p->rb = mouse.rb;
    p->lbc = mouse.lb_clicked; p->rbc = mouse.rb_clicked;
    mouse.lb_clicked = 0; mouse.rb_clicked = 0;
    SDL_LockMutex(klock);
    p->key_sym = lkey_sym; p->key_sc = lkey_sc; lkey_sym = 0; lkey_sc = 0;
    SDL_UnlockMutex(klock);
    const u8 *ks = rrq_harness_raw_keys();
    for (int k = 0; k < SDL_NUM_SCANCODES && p->nheld < MAXHELD; k++)
        if (ks[k]) p->held[p->nheld++] = (u16)k;
    if (mode == 1)
    {
        int t = SDL_AtomicSet(&pending_ticks, 0);
        p->ticks = (u16)(t > 60 ? 60 : t);
    }
}

static u32 checksum(void)
{
    u32 h = 2166136261u;
    u32 sz = alis.main->data->header.vram_alloc_sz;
    const u8 *m = alis.mem + alis.basemain;
    for (u32 i = 0; i < sz; i++) h = (h ^ m[i]) * 16777619u;
    for (int i = 0; i < MAX_SCRIPTS; i++)
        if (alis.live_scripts[i]) { u32 v = alis.live_scripts[i]->vram_org; for (int k = 0; k < 0x30; k++) h = (h ^ alis.mem[v + k]) * 16777619u; }
    return h;
}

void itroutine_body(void);
void alis_save_state(void);
void alis_load_state(void);

static void apply(int player, const pkt_t *p)
{
    rrq_dev_t *d = rrq_input_dev(player);
    d->m.x = p->mx; d->m.y = p->my; d->m.lb = p->lb; d->m.rb = p->rb;
    if (p->lbc) d->m.lb_clicked = 1;
    if (p->rbc) d->m.rb_clicked = 1;
    memset(d->keys, 0, sizeof d->keys);
    for (int i = 0; i < p->nheld && i < MAXHELD; i++) if (p->held[i] < SDL_NUM_SCANCODES) d->keys[p->held[i]] = 1;
    if (p->key_sym) { d->latch.sym = p->key_sym; d->latch.scancode = (SDL_Scancode)p->key_sc; d->latch.mod = 0; }
}

static void store_remote(const pkt_t *p)
{
    int s = p->frame % RING;
    remote_q[s] = *p; have_remote[s] = 1;
    if (p->cs_frame)
    {
        int c = p->cs_frame % RING;
        if (my_cs_frame[c] == p->cs_frame && my_cs[c] != p->cs)
            printf("[net] DESYNC at frame %u: local %08x remote %08x\n", p->cs_frame, my_cs[c], p->cs);
        else if (my_cs_frame[c] == p->cs_frame && (p->cs_frame % 600) == 0)
            printf("[net] in sync at frame %u (%08x)\n", p->cs_frame, p->cs);
        fflush(stdout);
    }
}

static u32 pending_cs_frame, pending_cs;

// Called on the VM thread at the end of every scheduler pass (before draw).
void rrq_net_frame(void)
{
    static int tried;
    if (!tried) { tried = 1; rrq_net_init(); }
    if (!mode || sock == INVALID_SOCKET) return;
    if (!started)
    {
        started = 1;
        // join: the host's complete engine state goes to the client, so both start identical
        char path[600];
        snprintf(path, sizeof path, "%salis.state", alis.platform.path);
        if (mode == 1)
        {
            alis_save_state();
            FILE *f = fopen(path, "rb");
            u32 n = 0; u8 *buf = NULL;
            if (f) { fseek(f, 0, SEEK_END); n = (u32)ftell(f); rewind(f); buf = malloc(n); fread(buf, 1, n, f); fclose(f); }
            send_all(&n, 4); if (n) send_all(buf, (int)n);
            free(buf);
            printf("[net] sent start state (%u bytes)\n", n); fflush(stdout);
        }
        else
        {
            u32 n = 0;
            recv_all(&n, 4);
            u8 *buf = malloc(n ? n : 1);
            if (n && recv_all(buf, (int)n) == 0)
            {
                char tmp[620]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
                FILE *f = fopen(tmp, "wb"); if (f) { fwrite(buf, 1, n, f); fclose(f); }
                remove(path); rename(tmp, path);
                alis_load_state();
                printf("[net] loaded host start state (%u bytes)\n", n); fflush(stdout);
            }
            free(buf);
        }
        SDL_AtomicSet(&pending_ticks, 0);
        rrq_coop_start();
        // pre-fill the first `delay` frames with empty input on both sides
        for (u32 f = 0; f < (u32)delay; f++)
        {
            pkt_t z; memset(&z, 0, sizeof z); z.frame = f;
            local_q[f % RING] = z; have_local[f % RING] = 1;
            remote_q[f % RING] = z; have_remote[f % RING] = 1;
        }
    }
    {   // diagnostics: RRQ_NET_DUMP=<frame> writes main memory + all entity variables
        static int dumpf = -2;
        if (dumpf == -2) { const char *e = getenv("RRQ_NET_DUMP"); dumpf = e ? atoi(e) : -1; }
        if ((int)frame == dumpf)
        {
            char path[600]; const char *dir = getenv("RRQ_SHOTDIR");
            snprintf(path, sizeof path, "%s/netdump_%u.bin", dir ? dir : ".", frame);
            FILE *f = fopen(path, "wb");
            if (f)
            {
                u32 sz = alis.main->data->header.vram_alloc_sz;
                fwrite(alis.mem + alis.basemain, 1, sz, f);
                for (int i = 0; i < MAX_SCRIPTS; i++)
                    if (alis.live_scripts[i])
                    {
                        sAlisScriptLive *e = alis.live_scripts[i];
                        u32 vs = e->data ? e->data->header.vram_alloc_sz : 0;
                        fwrite(&i, 4, 1, f); fwrite(&vs, 4, 1, f);
                        fwrite(alis.mem + e->vram_org - 0x34, 1, 0x34 + vs, f);
                    }
                fclose(f);
                printf("[net] dumped %s (timeclock %u)\n", path, alis.timeclock); fflush(stdout);
            }
        }
    }
    // checksum of the state after frame-1 (reported with the next packet)
    if (frame && (frame % 30) == 0)
    {
        int c = frame % RING; my_cs[c] = checksum(); my_cs_frame[c] = frame;
        pending_cs_frame = frame; pending_cs = my_cs[c];
    }
    // send local input for frame + delay
    u32 f = frame + delay;
    pkt_t lp; capture(&lp, f);
    lp.cs_frame = pending_cs_frame; lp.cs = pending_cs; pending_cs_frame = 0;
    local_q[f % RING] = lp; have_local[f % RING] = 1;
    if (send_all(&lp, sizeof lp)) { printf("[net] connection lost\n"); mode = 0; return; }
    // wait for the remote input of this frame
    int s = frame % RING;
    while (!(have_remote[s] && remote_q[s].frame == frame))
    {
        pkt_t rp;
        if (recv_all(&rp, sizeof rp)) { printf("[net] connection lost\n"); fflush(stdout); mode = 0; return; }
        store_remote(&rp);
    }
    const pkt_t *mine = &local_q[s], *theirs = &remote_q[s];
    const pkt_t *p1 = mode == 1 ? mine : theirs, *p2 = mode == 1 ? theirs : mine;
    // time: the host's tick count for this frame
    for (int t = 0; t < p1->ticks; t++) itroutine_body();
    apply(0, p1);
    apply(1, p2);
    have_remote[s] = 0; have_local[s] = 0;
    frame++;
}

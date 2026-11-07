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
int rrq_coop_has_hero(void);
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
    u16 flags;        // bit 0: resync requested (a checksum mismatch was seen)
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
static int want_resync;            // set when a desync is detected; sent with the next packet
static u32 last_remote_frame;


static int fake_timer = -1;   // RRQ_FAKE_NET_TIMER=1: local test of the frame-driven timer
static int fake(void) { if (fake_timer < 0) { const char *e = getenv("RRQ_FAKE_NET_TIMER"); fake_timer = e && *e == '1'; } return fake_timer; }
static u8 *pending_blob; static u32 pending_blob_n;
u32 rrq_coop_export(u8 **out);
void rrq_coop_import(const u8 *b, u32 n);
static volatile int conn_ready;     // connection + hello done (background thread)
static int lockstep;                 // lockstep running: inputs/time come from the session
static u32 session_seed;
// From the moment the start state is (re)loaded until the lockstep runs, the timer only counts (and
// those counts are dropped): a direct tick in that window would start this PC one tick ahead.
// The same window also counts as online for input: the pass that runs right after the load must
// not read this PC's real mouse/keys (they differ between the PCs).
static volatile int freeze_timer;
int rrq_net_active(void) { return lockstep || freeze_timer; }
static u8 *host_blob; static u32 host_blob_n;
void rrq_coop_switch_quiet(int p);
void rrq_coop_skip_p2pass(int n);
// right before the engine saves (host, during a sync): main memory gets player 1's record and the
// co-op tables are captured at exactly this point, so state + tables always match
void rrq_net_before_save(void)
{
    if (mode != 1 || started != 1) return;
    rrq_coop_switch_quiet(0);
    free(host_blob); host_blob = NULL;
    host_blob_n = rrq_coop_export(&host_blob);
}
// right after the engine loaded the host's state: take over the host's co-op tables too
void rrq_net_after_load(void)
{
    if (pending_blob) { rrq_coop_import(pending_blob, pending_blob_n); free(pending_blob); pending_blob = NULL; rrq_coop_skip_p2pass(2); }
    if (freeze_timer) rrq_input_reset();   // both PCs run the pass after the load with empty input
}
int rrq_net_frame_timer(void) { return lockstep || freeze_timer || fake(); }
static int local_player;            // stays set after a disconnect, so each PC keeps its own view
int rrq_net_local_player(void) { return local_player; }

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

static char net_arg[200];
static int net_thread(void *unused)
{
    (void)unused;
    const char *e = net_arg;
    int port = 27531;
    char host[128] = "127.0.0.1";
    u32 seed = 0;
    if (!strncmp(e, "host", 4))
    {
        if (e[4] == ':') port = atoi(e + 5);
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof one);
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { printf("[net] cannot listen on %d\n", port); return 0; }
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
            if (++tries > 600) { printf("[net] cannot connect\n"); return 0; }
            closesocket(sock); sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); Sleep(100);
        }
        u32 hello[3];
        if (recv_all(hello, sizeof hello) || hello[0] != NET_MAGIC) { printf("[net] bad hello\n"); return 0; }
        delay = (int)hello[1]; seed = hello[2];
        mode = 2;
    }
    int one = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    session_seed = seed;
    local_player = mode == 2 ? 1 : 0;
    printf("[net] connected as player %d, input delay %d frames%s\n", mode == 1 ? 1 : 2, delay,
           mode == 1 ? " - the session starts once you are in the game" : " - waiting for the host to be in the game");
    fflush(stdout);
    conn_ready = 1;
    return 0;
}

void rrq_net_init(void)
{
    const char *e = getenv("RRQ_NET");
    if (!e || !*e) return;
    snprintf(net_arg, sizeof net_arg, "%s", e);
    const char *d = getenv("RRQ_NET_DELAY");
    if (d) delay = atoi(d);
    if (delay < 1) delay = 1;
    if (delay > RING / 2) delay = RING / 2;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    klock = SDL_CreateMutex();
    SDL_CreateThread(net_thread, "rrq_net", NULL);
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
    if (p->key_sym)
    {
        SDL_Keysym ks = {0}; ks.sym = p->key_sym; ks.scancode = (SDL_Scancode)p->key_sc;
        rrq_input_set_latch(player, ks);
    }
}

static void store_remote(const pkt_t *p)
{
    int s = p->frame % RING;
    remote_q[s] = *p; have_remote[s] = 1; last_remote_frame = p->frame;
    if (p->cs_frame)
    {
        int c = p->cs_frame % RING;
        if (my_cs_frame[c] == p->cs_frame && my_cs[c] != p->cs)
            { printf("[net] DESYNC at frame %u: local %08x remote %08x - resyncing\n", p->cs_frame, my_cs[c], p->cs); want_resync = 1; }
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
    if (!mode && fake()) { int t = SDL_AtomicSet(&pending_ticks, 0); for (int i = 0; i < t && i < 60; i++) itroutine_body(); return; }
    if (!conn_ready || sock == INVALID_SOCKET) return;
    if (!lockstep && mode == 1 && started == 0 && !rrq_coop_has_hero()) return;   // host: wait until in the game
    if (started < 3)
    {
        // join, using the engine's own save/load (done at the top of the VM loop):
        //  host:   frame A request save -> frame B send the file
        //  client: frame A receive the file, request load -> frame B
        //  both:   frame B (state identical) start co-op and the lockstep
        char path[600];
        snprintf(path, sizeof path, "%salis.state", alis.platform.path);
        if (started == 0)
        {
            started = 1;
            if (mode == 1) { alis.state = eAlisStateSave; return; }
            u32 n = 0;
            recv_all(&n, 4);
            u8 *buf = malloc(n ? n : 1);
            if (n && recv_all(buf, (int)n) == 0)
            {
                char tmp[620]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
                FILE *f = fopen(tmp, "wb"); if (f) { fwrite(buf, 1, n, f); fclose(f); }
                remove(path); rename(tmp, path);
                freeze_timer = 1;
                alis.state = eAlisStateLoad;
                printf("[net] received host start state (%u bytes)\n", n); fflush(stdout);
            }
            free(buf);
            u32 bn = 0; recv_all(&bn, 4);
            free(pending_blob); pending_blob = NULL; pending_blob_n = 0;
            if (bn && bn < (64u << 20)) { pending_blob = malloc(bn); if (recv_all(pending_blob, (int)bn) == 0) pending_blob_n = bn; }
            return;
        }
        if (started == 1)
        {
            started = 2;
            if (mode == 1)
            {
                FILE *f = fopen(path, "rb");
                u32 n = 0; u8 *buf = NULL;
                if (f) { fseek(f, 0, SEEK_END); n = (u32)ftell(f); rewind(f); buf = malloc(n); fread(buf, 1, n, f); fclose(f); }
                send_all(&n, 4); if (n) send_all(buf, (int)n);
                send_all(&host_blob_n, 4); if (host_blob_n) send_all(host_blob, (int)host_blob_n);
                // the host reloads the same state and the same tables
                free(pending_blob); pending_blob = host_blob; pending_blob_n = host_blob_n; host_blob = NULL; host_blob_n = 0;
                free(buf);
                printf("[net] sent start state (%u bytes)\n", n); fflush(stdout);
                // the host already ran one pass past the saved state; it reloads so both start equal
                freeze_timer = 1;
                alis.state = eAlisStateLoad;
                return;
            }
        }
        started = 3;
        lockstep = 1;
        freeze_timer = 0;
        frame = 0; last_remote_frame = 0; want_resync = 0;
        rrq_input_reset();
        memset(have_local, 0, sizeof have_local); memset(have_remote, 0, sizeof have_remote);
        memset(my_cs_frame, 0, sizeof my_cs_frame);
        rrq_net_after_load();
        srand(session_seed);
        SDL_AtomicSet(&pending_ticks, 0);
        // player 2's entities are created by rrq_coop's maintain() once gameplay has a hero
        // pre-fill the first `delay` frames with empty input on both sides
        for (u32 f = 0; f < (u32)delay; f++)
        {
            pkt_t z; memset(&z, 0, sizeof z); z.frame = f;
            local_q[f % RING] = z; have_local[f % RING] = 1;
            remote_q[f % RING] = z; have_remote[f % RING] = 1;
        }
        printf("[net] lockstep running\n"); fflush(stdout);
    }
    {   // diagnostics: RRQ_NET_DUMP=<frame> writes main memory + all entity variables
        static int dumpf = -2;
        if (dumpf == -2) { const char *e = getenv("RRQ_NET_DUMP"); dumpf = e ? atoi(e) : -1; }
        if ((int)frame == dumpf)
        {
            char path[600]; const char *dir = getenv("RRQ_SHOTDIR");
            static int dumpn; snprintf(path, sizeof path, "%s/netdump_%u_%d.bin", dir ? dir : ".", frame, dumpn++);
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
    {
        static int fd = -2;
        if (fd == -2) { const char *e = getenv("RRQ_NET_FORCE_DESYNC"); fd = e ? atoi(e) : -1; }
        if (mode == 2 && (int)frame == fd && lockstep) { alis.mem[alis.basemain + 0x2000] ^= 0x5a; fd = -1; printf("[net] test: forced a desync at frame %u\n", frame); fflush(stdout); }
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
    if (want_resync) lp.flags |= 1;
    local_q[f % RING] = lp; have_local[f % RING] = 1;
    if (send_all(&lp, sizeof lp)) { printf("[net] connection lost\n"); mode = 0; lockstep = 0; freeze_timer = 0; closesocket(sock); sock = INVALID_SOCKET; return; }
    // wait for the remote input of this frame
    int s = frame % RING;
    while (!(have_remote[s] && remote_q[s].frame == frame))
    {
        pkt_t rp;
        if (recv_all(&rp, sizeof rp)) { printf("[net] connection lost\n"); fflush(stdout); mode = 0; lockstep = 0; freeze_timer = 0; closesocket(sock); sock = INVALID_SOCKET; return; }
        store_remote(&rp);
    }
    const pkt_t *mine = &local_q[s], *theirs = &remote_q[s];
    const pkt_t *p1 = mode == 1 ? mine : theirs, *p2 = mode == 1 ? theirs : mine;
    // time: the host's tick count for this frame
    for (int t = 0; t < p1->ticks; t++) itroutine_body();
    apply(0, p1);
    apply(1, p2);
    int resync = (p1->flags | p2->flags) & 1;
    have_remote[s] = 0; have_local[s] = 0;
    frame++;
    if (resync)
    {
        // both sides decide this on the same frame. Drain the remote packets already in flight
        // (up to frame + delay) so the stream is clean, then redo the join sync from the host.
        while (last_remote_frame < frame - 1 + (u32)delay)
        {
            pkt_t rp;
            if (recv_all(&rp, sizeof rp)) { mode = 0; lockstep = 0; freeze_timer = 0; closesocket(sock); sock = INVALID_SOCKET; return; }
            last_remote_frame = rp.frame;
        }
        printf("[net] resync at frame %u\n", frame); fflush(stdout);
        lockstep = 0; freeze_timer = 0;
        started = 0;
    }
}

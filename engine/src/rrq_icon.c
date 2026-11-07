// rrq-coop: build the program icon from the player's own copy of the game (run by the installer).
//
//   RRQ.exe --make-icon <out.ico> [--icon-exe <exe to patch>] <data_path>
//
// Runs hidden and muted, skips the intro (Escape / the language screen), waits until the options
// menu is on screen, cuts the hero out of it and writes a multi-size .ico. With --icon-exe the icon
// also replaces that program's icon resource (the copy that runs can't patch itself).
// No game art ships with the mod: the icon only exists on PCs that have the game.
#include "rrq_icon.h"
#include "alis.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef _WIN32
#include <windows.h>
#endif

static char out_path[1024], exe_path[1024];
static int mode;
static volatile int menu_seen;
static int frames_in_menu, finished;

int rrq_icon_mode(void) { return mode; }
int rrq_icon_menu_seen(void) { return menu_seen; }

void rrq_icon_setup(const char *out, const char *exe)
{
    mode = 1;
    snprintf(out_path, sizeof out_path, "%s", out ? out : "rrq.ico");
    snprintf(exe_path, sizeof exe_path, "%s", exe ? exe : "");
    putenv("RRQ_HIDDEN=1");
    putenv("SDL_AUDIODRIVER=dummy");
    putenv("RRQ_QUIT_MS=120000");      // give up after 2 minutes
    putenv("RRQ_WASD=0");
}

// ---- the picture ----
#define ICON_N 256
static const int HERO_X = 21, HERO_Y = 61, HERO_W = 44, HERO_H = 44;   // hero in the 320x200 options menu

static int in_rrect(float x, float y, float x0, float y0, float x1, float y1, float r)
{
    if (x < x0 || x > x1 || y < y0 || y > y1) return 0;
    float cx = x < x0 + r ? x0 + r : x > x1 - r ? x1 - r : x;
    float cy = y < y0 + r ? y0 + r : y > y1 - r ? y1 - r : y;
    float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

// coverage (0..16) of a rounded rectangle over one pixel, 4x4 supersampled
static int cover(int px, int py, float x0, float y0, float x1, float y1, float r)
{
    int n = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++)
            n += in_rrect(px + (sx + 0.5f) / 4, py + (sy + 0.5f) / 4, x0, y0, x1, y1, r);
    return n;
}

typedef struct { float r, g, b, a; } px_t;

static void blend(px_t *d, float r, float g, float b, float a)
{
    float oa = a + d->a * (1 - a);
    if (oa <= 0) { d->r = d->g = d->b = d->a = 0; return; }
    d->r = (r * a + d->r * d->a * (1 - a)) / oa;
    d->g = (g * a + d->g * d->a * (1 - a)) / oa;
    d->b = (b * a + d->b * d->a * (1 - a)) / oa;
    d->a = oa;
}

static void compose(const u32 *hero, px_t *img)
{
    for (int y = 0; y < ICON_N; y++)
        for (int x = 0; x < ICON_N; x++)
        {
            px_t *p = &img[y * ICON_N + x];
            memset(p, 0, sizeof *p);
            float bg = cover(x, y, 0, 0, 256, 256, 40) / 16.0f;
            if (bg > 0) blend(p, 40, 28, 12, bg);
            float art = cover(x, y, 18, 18, 238, 238, 26) / 16.0f;
            if (art > 0)
            {
                u32 c = hero[((y - 18) / 5 < 0 ? 0 : (y - 18) / 5 >= HERO_H ? HERO_H - 1 : (y - 18) / 5) * HERO_W +
                             ((x - 18) / 5 < 0 ? 0 : (x - 18) / 5 >= HERO_W ? HERO_W - 1 : (x - 18) / 5)];
                blend(p, (c >> 16) & 255, (c >> 8) & 255, c & 255, art);
            }
            float ring = (cover(x, y, 4, 4, 252, 252, 36) - cover(x, y, 12, 12, 244, 244, 28)) / 16.0f;
            if (ring > 0) blend(p, 196, 160, 60, ring);
        }
}

// box-filtered resize of the 256x256 master to s x s, BGRA bottom-up rows for the .ico
static void shrink(const px_t *img, int s, u8 *bgra)
{
    float f = (float)ICON_N / s;
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++)
        {
            float r = 0, g = 0, b = 0, a = 0; int n = 0;
            for (int sy = 0; sy < 8; sy++)
                for (int sx = 0; sx < 8; sx++)
                {
                    int ix = (int)((x + (sx + 0.5f) / 8) * f), iy = (int)((y + (sy + 0.5f) / 8) * f);
                    const px_t *p = &img[(iy < ICON_N ? iy : ICON_N - 1) * ICON_N + (ix < ICON_N ? ix : ICON_N - 1)];
                    r += p->r * p->a; g += p->g * p->a; b += p->b * p->a; a += p->a; n++;
                }
            u8 *o = &bgra[((s - 1 - y) * s + x) * 4];
            if (a > 0) { o[0] = (u8)(b / a + 0.5f); o[1] = (u8)(g / a + 0.5f); o[2] = (u8)(r / a + 0.5f); }
            else o[0] = o[1] = o[2] = 0;
            o[3] = (u8)(a / n * 255 + 0.5f);
        }
}

static const int sizes[] = { 16, 24, 32, 48, 64, 128, 256 };
#define NSIZES ((int)(sizeof sizes / sizeof sizes[0]))

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }

// one icon image: BITMAPINFOHEADER + BGRA pixels + an empty AND mask
static u8 *dib(const px_t *img, int s, u32 *len)
{
    u32 mask = (u32)((s + 31) / 32) * 4 * s;
    *len = 40 + s * s * 4 + mask;
    u8 *d = calloc(1, *len);
    put32(d, 40); put32(d + 4, s); put32(d + 8, s * 2); put16(d + 12, 1); put16(d + 14, 32);
    put32(d + 20, s * s * 4 + mask);
    shrink(img, s, d + 40);
    return d;
}

static int write_ico(const px_t *img, u8 *imgs[], u32 lens[])
{
    FILE *f = fopen(out_path, "wb");
    if (!f) return 0;
    u8 h[6]; put16(h, 0); put16(h + 2, 1); put16(h + 4, NSIZES);
    fwrite(h, 1, 6, f);
    u32 off = 6 + 16 * NSIZES;
    for (int i = 0; i < NSIZES; i++)
    {
        u8 e[16] = {0};
        e[0] = sizes[i] >= 256 ? 0 : (u8)sizes[i]; e[1] = e[0];
        put16(e + 4, 1); put16(e + 6, 32); put32(e + 8, lens[i]); put32(e + 12, off);
        fwrite(e, 1, 16, f);
        off += lens[i];
    }
    for (int i = 0; i < NSIZES; i++) fwrite(imgs[i], 1, lens[i], f);
    (void)img;
    return fclose(f) == 0;
}

#ifdef _WIN32
// replace the program's icon group (IDI_ICON1, as in icons/alis.rc) with ours
static int patch_exe(u8 *imgs[], u32 lens[])
{
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, exe_path, -1, w, 1024);
    HANDLE h = BeginUpdateResourceW(w, FALSE);
    if (!h) return 0;
    WORD lang = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    u8 grp[6 + 14 * NSIZES];
    put16(grp, 0); put16(grp + 2, 1); put16(grp + 4, NSIZES);
    int ok = 1;
    for (int i = 0; i < NSIZES; i++)
    {
        ok &= UpdateResourceW(h, (LPCWSTR)RT_ICON, MAKEINTRESOURCEW(i + 1), lang, imgs[i], lens[i]) != 0;
        u8 *e = grp + 6 + 14 * i;
        memset(e, 0, 14);
        e[0] = sizes[i] >= 256 ? 0 : (u8)sizes[i]; e[1] = e[0];
        put16(e + 4, 1); put16(e + 6, 32); put32(e + 8, lens[i]); put16(e + 12, (u16)(i + 1));
    }
    ok &= UpdateResourceW(h, (LPCWSTR)RT_GROUP_ICON, L"IDI_ICON1", lang, grp, sizeof grp) != 0;
    return EndUpdateResourceW(h, !ok) && ok;
}
#endif

static void finish(const u32 *frame, int w, int h)
{
    u32 hero[HERO_W * HERO_H];
    long sum = 0;
    for (int y = 0; y < HERO_H; y++)
        for (int x = 0; x < HERO_W; x++)
        {
            int sx = (HERO_X + x) * w / 320, sy = (HERO_Y + y) * h / 200;
            u32 c = frame[sy * w + sx];
            hero[y * HERO_W + x] = c;
            sum += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
        }
    if (sum / (HERO_W * HERO_H * 3) < 20) return;          // menu not drawn yet (fading in): wait

    finished = 1;
    px_t *img = malloc(sizeof(px_t) * ICON_N * ICON_N);
    compose(hero, img);
    u8 *imgs[NSIZES]; u32 lens[NSIZES];
    for (int i = 0; i < NSIZES; i++) imgs[i] = dib(img, sizes[i], &lens[i]);
    int ok = write_ico(img, imgs, lens);
    printf("[icon] %s %s\n", ok ? "wrote" : "could not write", out_path);
#ifdef _WIN32
    if (ok && exe_path[0])
    {
        int p = patch_exe(imgs, lens);
        printf("[icon] %s %s\n", p ? "patched" : "could not patch", exe_path);
    }
#endif
    for (int i = 0; i < NSIZES; i++) free(imgs[i]);
    free(img);
    fflush(stdout);
    alis.state = eAlisStateStopped;
}

// VM thread, after each frame is drawn
void rrq_icon_after_draw(void)
{
    if (!mode || finished) return;
    if (!menu_seen)
    {
        for (int i = 0; i < MAX_SCRIPTS; i++)
        {
            sAlisScriptLive *s = alis.live_scripts[i];
            if (s && s->name && !strncasecmp(s->name, "newpref", 7)) { menu_seen = 1; break; }
        }
        if (!menu_seen) return;
    }
    if (++frames_in_menu < 30) return;                    // let the menu finish drawing / fading in
    pixelbuf_t *b = &host.pixelbuf;
    const u8 *src = image.physic ? image.physic : b->data;
    if (!src || b->w < 320 || b->h < 200) return;
    u32 *argb = malloc((size_t)b->w * b->h * 4);
    for (int i = 0; i < b->w * b->h; i++)
        argb[i] = b->rgb ? b->rgb[i] : (image.mpalet[src[i]] | 0xff000000u);
    finish(argb, b->w, b->h);
    free(argb);
}

// Unpack every ALIS script (*.IO) of a game folder to <out>/<NAME>.bin using the
// maestun/alis unpacker. Build: tools/rrq_unpack/build.sh
// Usage: rrq_unpack <game_dir> <out_dir>
#include "alis.h"
#include "platform.h"
#include "script.h"
#include "unpack.h"
#include "mem.h"
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

int disalis = 0;
int authormode = 0;
void dbglog(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: %s <game_dir> <out_dir>\n", argv[0]); return 1; }
    sPlatform *pl = pl_guess(argv[1], 0);
    if (!pl_supported(pl)) { printf("platform not supported\n"); return 1; }
    alis.platform = *pl;
    vram_init();
    script_guess_game(pl->main);
    printf("game uid %d version %d kind %d main %s\n", alis.platform.uid, alis.platform.version, alis.platform.kind, pl->main);

    DIR *d = opendir(argv[1]);
    struct dirent *e;
    int n = 0;
    FILE *idx = NULL;
    char p[1024];
    snprintf(p, sizeof p, "%s/index.tsv", argv[2]);
    idx = fopen(p, "w");
    fprintf(idx, "file\tmagic\tpacked\tunpacked_size\tid\n");
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l < 4 || strcasecmp(e->d_name + l - 3, ".io")) continue;
        char in[1024];
        snprintf(in, sizeof in, "%s/%s", argv[1], e->d_name);
        FILE *fp = fopen(in, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END); long isz = ftell(fp); rewind(fp);
        u8 hdr[6]; fread(hdr, 1, 6, fp);
        u32 magic = ((u32)hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
        if (alis.platform.is_little_endian)
            magic = ((u32)hdr[3] << 24) | (hdr[2] << 16) | (hdr[1] << 8) | hdr[0];
        u16 check = alis.platform.is_little_endian ? (hdr[4] | (hdr[5] << 8)) : ((hdr[4] << 8) | hdr[5]);
        u32 usz = magic & 0xffffff;
        alis.typepack = magic >> 24;
        if (alis.platform.kind == EPlatformPC && alis.platform.version != 11) alis.typepack &= 0xfe;
        u8 *buf = calloc(1, usz + 0x10000);
        int got;
        int packed = alis.typepack < 0;
        if (packed) {
            fclose(fp);
            got = unpack_script(in, buf);
        } else {
            long seekto = kPackedHeaderSize + (check == 0 ? kVMSpecsSize : 0);
            fseek(fp, seekto, SEEK_SET);
            got = (int)fread(buf, 1, isz - seekto, fp);
            fclose(fp);
        }
        char out[1024];
        snprintf(out, sizeof out, "%s/%s.bin", argv[2], e->d_name);
        if (got > 0) {
            FILE *o = fopen(out, "wb"); fwrite(buf, 1, got, o); fclose(o);
        }
        fprintf(idx, "%s\t%08x\t%d\t%d\t%d\n", e->d_name, magic, packed, got, check);
        printf("%-14s magic %08x packed %d -> %d\n", e->d_name, magic, packed, got);
        free(buf);
        n++;
    }
    fclose(idx);
    closedir(d);
    printf("%d scripts\n", n);
    return 0;
}

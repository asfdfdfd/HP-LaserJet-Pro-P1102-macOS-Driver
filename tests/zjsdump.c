/*
 * zjsdump - test helper: walk a ZjStream document and decode every page
 * into P4 (PBM), printing a per-page summary of the interesting items.
 *
 * Usage: zjsdump doc.zjs
 * Writes out-000.p4, out-001.p4, ... into the current directory and
 * prints a line per page: index, DMDUPLEX, DMCOPIES, RASTER_X/Y, plus
 * PAUSE markers between pages.  Useful to verify duplex page order,
 * rotation and copies against the raw engine output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jbig.h"

#define ZJT_START_DOC    0
#define ZJT_END_DOC      1
#define ZJT_START_PAGE   2
#define ZJT_END_PAGE     3
#define ZJT_JBIG_BIH     4
#define ZJT_JBIG_BID     5
#define ZJT_END_JBIG     6
#define ZJT_2600N_PAUSE 11

#define ZJI_DMDUPLEX  2
#define ZJI_DMCOPIES  4
#define ZJI_RASTER_X 12
#define ZJI_RASTER_Y 13

static unsigned char *read_file(const char *path, size_t *np)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    d = malloc(sz ? (size_t)sz : 1);
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz)
    {
        free(d);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *np = (size_t)sz;
    return d;
}

static unsigned long rd32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static int write_page_p4(const unsigned char *jbig, size_t len, int idx)
{
    struct jbg_dec_state s;
    unsigned long cnt = 0;
    int rc;
    char name[64];
    FILE *out;

    jbg_dec_init(&s);
    while ((rc = jbg_dec_in(&s, jbig + cnt, len - cnt, &cnt)) == JBG_EAGAIN &&
           cnt < len) { }
    if (rc)
    {
        fprintf(stderr, "zjsdump: page %d: JBIG decode failed rc=%d\n",
                idx, rc);
        jbg_dec_free(&s);
        return 1;
    }

    unsigned long w = jbg_dec_getwidth(&s);
    unsigned long h = jbg_dec_getheight(&s);
    unsigned long bpl = w / 8;
    const unsigned char *im = jbg_dec_getimage(&s, 0);

    snprintf(name, sizeof(name), "out-%03d.p4", idx);
    out = fopen(name, "wb");
    if (!out)
    {
        fprintf(stderr, "zjsdump: cannot write %s\n", name);
        jbg_dec_free(&s);
        return 1;
    }
    fprintf(out, "P4\n%lu %lu\n", w, h);
    fwrite(im, 1, bpl * h, out);
    fclose(out);

    /* Black-pixel bounding box: lets tests identify the page and its
     * orientation from marker fixtures. */
    {
        unsigned long x0 = w, y0 = h, x1 = 0, y1 = 0;
        unsigned long y, x;
        for (y = 0; y < h; ++y)
        {
            const unsigned char *r = im + y * bpl;
            for (x = 0; x < w; ++x)
                if (r[x / 8] & (0x80 >> (x & 7)))
                {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        }
        if (x1 >= x0 && y1 >= y0)
            printf("  bbox x=%lu..%lu y=%lu..%lu\n", x0, x1, y0, y1);
        else
            printf("  bbox empty\n");
    }
    jbg_dec_free(&s);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char *d;
    size_t n;
    size_t i;
    int pageno = 0, have_pause = 0;
    unsigned char *jb = NULL;
    size_t jblen = 0, jbcap = 0;

    if (argc < 2)
    {
        fprintf(stderr, "Usage: zjsdump doc.zjs\n");
        return 2;
    }
    d = read_file(argv[1], &n);
    if (!d)
    {
        fprintf(stderr, "zjsdump: cannot read %s\n", argv[1]);
        return 1;
    }

    /* The document starts with PJL; the ZjStream begins at the JZJZ
     * signature (see start_doc in foo2zjs.c). */
    for (i = 0; i + 4 < n; ++i)
        if (d[i] == 'J' && d[i + 1] == 'Z' && d[i + 2] == 'J' &&
            d[i + 3] == 'Z')
        {
            i += 4;
            break;
        }
    if (i >= n)
    {
        fprintf(stderr, "zjsdump: no JZJZ signature\n");
        free(d);
        return 1;
    }

    for (; i + 16 <= n; )
    {
        unsigned long sz = rd32(d + i);
        unsigned long ty = rd32(d + i + 4);
        unsigned long items = rd32(d + i + 8);

        if (sz < 16 || i + sz > n)
        {
            fprintf(stderr, "zjsdump: bad chunk size %lu at %zu\n", sz, i);
            break;
        }

        switch (ty)
        {
        case ZJT_START_PAGE:
        {
            size_t k = i + 16;
            size_t end = i + sz;
            unsigned long dup = 0, copies = 0, rx = 0, ry = 0;
            while (k + 8 <= end)
            {
                unsigned long isz = rd32(d + k);
                unsigned long item = ((unsigned long)d[k + 4] << 8) | d[k + 5];
                unsigned long val;
                if (isz < 8 || k + isz > end)
                    break;
                val = rd32(d + k + 8);
                if (item == ZJI_DMDUPLEX) dup = val;
                else if (item == ZJI_DMCOPIES) copies = val;
                else if (item == ZJI_RASTER_X) rx = val;
                else if (item == ZJI_RASTER_Y) ry = val;
                k += isz;
            }
            printf("PAGE %d: duplex=%lu copies=%lu raster=%lux%lu%s\n",
                   pageno, dup, copies, rx, ry,
                   have_pause ? " (after PAUSE)" : "");
            have_pause = 0;
            break;
        }
        case ZJT_JBIG_BIH:
        case ZJT_JBIG_BID:
            if (jblen + sz - 16 > jbcap)
            {
                size_t ncap = jbcap ? jbcap * 2 : 65536;
                unsigned char *nb = realloc(jb, ncap);
                if (!nb)
                {
                    fprintf(stderr, "zjsdump: out of memory\n");
                    return 1;
                }
                jb = nb;
                jbcap = ncap;
            }
            memcpy(jb + jblen, d + i + 16, sz - 16);
            jblen += sz - 16;
            break;
        case ZJT_END_PAGE:
            if (write_page_p4(jb, jblen, pageno))
                return 1;
            ++pageno;
            jblen = 0;
            break;
        case ZJT_2600N_PAUSE:
            printf("PAUSE\n");
            have_pause = 1;
            break;
        case ZJT_END_DOC:
            goto done;
        default:
            break;
        }
        i += sz;
    }
done:
    printf("TOTAL %d pages\n", pageno);
    free(jb);
    free(d);
    return 0;
}

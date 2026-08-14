/*
 * jbverify - test helper: decode the JBIG planes of a ZjStream and check
 * that a page was error-diffused (dithered) rather than hard-thresholded.
 *
 * Usage: jbverify zjs-file
 * Exits 0 if a mid-gray row of the first page shows dithering
 * (many black/white transitions and sane black coverage).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jbig.h"

static unsigned char *extract_jbig(const char *path, size_t *lenp)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    unsigned char *d = malloc(n ? (size_t)n : 1);
    if (!d) { fclose(f); return NULL; }
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return NULL; }
    fclose(f);

    long i = -1;
    for (long k = 0; k + 4 < n; ++k)
        if (d[k] == 'J' && d[k+1] == 'Z' && d[k+2] == 'J' && d[k+3] == 'Z')
        { i = k + 4; break; }
    if (i < 0) { free(d); return NULL; }

    size_t cap = n, len = 0;
    unsigned char *out = malloc(cap);
    if (!out) { free(d); return NULL; }
    while (i + 16 <= n)
    {
        unsigned long sz = ((unsigned long)d[i] << 24) |
                           ((unsigned long)d[i+1] << 16) |
                           ((unsigned long)d[i+2] << 8) | d[i+3];
        unsigned long ty = ((unsigned long)d[i+4] << 24) |
                           ((unsigned long)d[i+5] << 16) |
                           ((unsigned long)d[i+6] << 8) | d[i+7];
        if (ty == 4 || ty == 5)      /* JBIG BIH / BID */
        {
            if (len + sz - 16 > cap)
            {
                cap *= 2;
                unsigned char *nb = realloc(out, cap);
                if (!nb) { free(out); free(d); return NULL; }
                out = nb;
            }
            memcpy(out + len, d + i + 16, sz - 16);
            len += sz - 16;
        }
        if (ty == 6)                 /* END_JBIG */
            break;
        if (sz < 16 || i + (long)sz > n)
            break;
        i += (long)sz;
    }
    free(d);
    *lenp = len;
    return out;
}

int main(int argc, char **argv)
{
    int any = 0, selftest = 0, notdithered = 0;
    if (argc >= 3 && strcmp(argv[1], "--any") == 0)
    {
        any = 1;
        argv++;
        argc--;
    }
    else if (argc >= 3 && strcmp(argv[1], "--selftest") == 0)
    {
        selftest = 1;
        argv++;
        argc--;
    }
    else if (argc >= 3 && strcmp(argv[1], "--not-dithered") == 0)
    {
        notdithered = 1;
        argv++;
        argc--;
    }
    if (argc < 2)
    {
        fprintf(stderr, "Usage: jbverify [--any] zjs-file\n");
        return 2;
    }
    size_t len = 0;
    unsigned char *data = extract_jbig(argv[1], &len);
    if (!data)
    {
        fprintf(stderr, "jbverify: no JBIG data\n");
        return 1;
    }

    struct jbg_dec_state s;
    jbg_dec_init(&s);
    unsigned long cnt = 0;
    int rc;
    while ((rc = jbg_dec_in(&s, data + cnt, len - cnt, &cnt)) == JBG_EAGAIN &&
           cnt < len) { }
    free(data);
    if (rc)
    {
        fprintf(stderr, "jbverify: JBIG decode failed rc=%d\n", rc);
        jbg_dec_free(&s);
        return 1;
    }

    unsigned long w = jbg_dec_getwidth(&s);
    unsigned long h = jbg_dec_getheight(&s);
    unsigned char *im = jbg_dec_getimage(&s, 0);
    unsigned long bpl = w / 8;

    if (selftest)
    {
        /* Self-test page: the three gray blocks (Bayer 25/50/75%) must
         * have clearly different black coverage.  Decoded page is the
         * 128-rounded bitmap (5120 wide); blocks live at x = 700+i*1380,
         * y = 2900..3700, 1000 px wide, no raster padding. */
        static const struct { int x0, y0; } blocks[3] = {
            { 700, 2900 }, { 2080, 2900 }, { 3460, 2900 } };
        double cov[3];
        for (int i = 0; i < 3; ++i)
        {
            unsigned long black = 0, tot = 0;
            for (unsigned long y = (unsigned long)blocks[i].y0;
                 y < blocks[i].y0 + 800; y += 3)
                for (unsigned long x = (unsigned long)blocks[i].x0;
                     x < blocks[i].x0 + 1000; x += 3)
                {
                    if (im[y * bpl + x / 8] & (0x80 >> (x % 8)))
                        ++black;
                    ++tot;
                }
            cov[i] = (double)black / tot;
        }
        jbg_dec_free(&s);
        fprintf(stderr, "jbverify: gray blocks %.0f%%/%.0f%%/%.0f%%\n",
                cov[0] * 100, cov[1] * 100, cov[2] * 100);
        if (cov[0] < 0.15 || cov[0] > 0.35 ||
            cov[1] < 0.40 || cov[1] > 0.60 ||
            cov[2] < 0.65 || cov[2] > 0.85 ||
            !(cov[0] < cov[1] && cov[1] < cov[2]))
        {
            fprintf(stderr, "jbverify: gray blocks not distinct\n");
            return 1;
        }
        return 0;
    }

    if (notdithered)
    {
        /* A hard-thresholded gradient must NOT show dithering: find a row
         * with substantial black coverage and require few transitions. */
        int found = 0;
        for (unsigned long y = 0; y < h; y += 7)
        {
            unsigned long black = 0;
            unsigned char *r = im + y * bpl;
            for (unsigned long x = 0; x < w; ++x)
                if (r[x / 8] & (0x80 >> (x % 8)))
                    ++black;
            double frac = (double)black / w;
            if (frac > 0.20 && frac < 0.80)
            {
                long trans = 0;
                int prev = -1;
                for (unsigned long x = 0; x < w; ++x)
                {
                    int cur = (r[x / 8] & (0x80 >> (x % 8))) ? 1 : 0;
                    if (prev >= 0 && cur != prev)
                        ++trans;
                    prev = cur;
                }
                if (trans < (long)(w / 20))
                {
                    found = 1;
                    break;
                }
            }
        }
        jbg_dec_free(&s);
        if (!found)
        {
            fprintf(stderr, "jbverify: page looks dithered\n");
            return 1;
        }
        return 0;
    }

    if (any)
    {
        /* Just verify the page decodes and has visible content. */
        unsigned long black = 0;
        unsigned long tot = 0;
        for (unsigned long y = 0; y < h; y += 17)
            for (unsigned long x = 0; x < w; x += 17)
            {
                if (im[y * bpl + x / 8] & (0x80 >> (x % 8)))
                    ++black;
                ++tot;
            }
        jbg_dec_free(&s);
        if (black < tot / 1000)
        {
            fprintf(stderr, "jbverify: page is blank\n");
            return 1;
        }
        return 0;
    }

    /* Scan rows for one with roughly half the pixels black (mid-gray) and
     * count transitions; dithering yields many transitions. */
    int found = 0;
    for (unsigned long y = 0; y < h; y += 7)
    {
        unsigned long black = 0;
        unsigned char *r = im + y * bpl;
        for (unsigned long x = 0; x < w; ++x)
            if (r[x / 8] & (0x80 >> (x % 8)))
                ++black;
        double frac = (double)black / w;
        if (frac > 0.20 && frac < 0.80)
        {
            long trans = 0;
            int prev = -1;
            for (unsigned long x = 0; x < w; ++x)
            {
                int cur = (r[x / 8] & (0x80 >> (x % 8))) ? 1 : 0;
                if (prev >= 0 && cur != prev)
                    ++trans;
                prev = cur;
            }
            if (trans > (long)(w / 4))
            {
                found = 1;
                break;
            }
        }
    }

    jbg_dec_free(&s);
    if (!found)
    {
        fprintf(stderr, "jbverify: page does not look dithered\n");
        return 1;
    }
    return 0;
}

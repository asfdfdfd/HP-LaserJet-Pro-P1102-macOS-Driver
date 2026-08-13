/*
 * rastertozjs - CUPS raster -> ZjStream filter for HP LaserJet Pro P1102.
 *
 * Copyright (C) 2026  the hp-p1102-macos-driver authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The ZjStream encoder itself is vendored from the foo2zjs project
 * (GPL-2.0-or-later), see src/zjs/vendor/foo2zjs.c.
 *
 * Chain:  PDF -> cgpdftoraster (macOS built-in) -> CUPS raster (stdin)
 *         -> this filter -> ZjStream + PJL (stdout)
 *         -> usb backend (macOS built-in) -> printer
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <cups/raster.h>
#include "zjs_engine.h"

#define DEBUG(...) do { \
    if (getenv("RAS2ZJS_DEBUG")) fprintf(stderr, "rastertozjs: " __VA_ARGS__); \
} while (0)

/* Job options parsed from the CUPS options string (argv[5]). */
typedef struct {
    const char *page_size;    /* "Letter", "A4", ... */
    const char *media_type;   /* "Standard", ... */
    const char *media_source; /* "Auto", ... */
    int  draft;               /* Quality=draft */
    int  density;             /* 0 = not set */
    int  copies;
} job_opts_t;

/* Shared state between the PBM writer thread and the ZJS main thread. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int             ready;    /* first page header published */
    int             err;      /* writer failed */
    int             pipe_fd;  /* write end of the PBM pipe */
    FILE            *dump;    /* debug: also write PBM here (RAS2ZJS_DEBUG) */
    unsigned        width;    /* page width in pixels (first page) */
    unsigned        height;
    unsigned        res_x;    /* HWResolution from raster header */
    unsigned        res_y;
} ctx_t;

static void publish_header(ctx_t *ctx, const cups_page_header2_t *hdr)
{
    pthread_mutex_lock(&ctx->mutex);
    ctx->width  = hdr->cupsWidth;
    ctx->height = hdr->cupsHeight;
    ctx->res_x  = hdr->HWResolution[0];
    ctx->res_y  = hdr->HWResolution[1];
    ctx->ready  = 1;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);
}

static void wait_header(ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->mutex);
    while (!ctx->ready && !ctx->err)
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    pthread_mutex_unlock(&ctx->mutex);
}

static void fail(ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->mutex);
    ctx->err = 1;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);
}

/*
 * Convert one CUPS raster row into P4 (PBM, 1 bit, MSB first, 1 = black)
 * row data of "bpl" bytes.  "line" holds hdr->cupsBytesPerLine bytes.
 */
static void convert_row(const unsigned char *line, unsigned char *pbmrow,
                        unsigned w, unsigned bpl,
                        const cups_page_header2_t *hdr)
{
    unsigned x;
    cups_cspace_t cs = hdr->cupsColorSpace;
    unsigned bpc = hdr->cupsBitsPerColor;
    unsigned ncol = hdr->cupsNumColors;

    memset(pbmrow, 0, bpl);

    switch (cs)
    {
        case CUPS_CSPACE_K:      /* 1 = black */
            if (bpc == 1)
            {
                memcpy(pbmrow, line, bpl);
                return;
            }
            /* 8-bit K: threshold */
            for (x = 0; x < w; ++x)
                if (line[x] < 128)
                    pbmrow[x / 8] |= 0x80 >> (x & 7);
            return;

        case CUPS_CSPACE_W:      /* 1 = white */
        case CUPS_CSPACE_SW:
            if (bpc == 1)
            {
                for (x = 0; x < bpl; ++x)
                    pbmrow[x] = ~line[x];
                return;
            }
            /* 8-bit luminance: threshold */
            for (x = 0; x < w; ++x)
                if (line[x] < 128)
                    pbmrow[x / 8] |= 0x80 >> (x & 7);
            return;

        case CUPS_CSPACE_RGB:
        case CUPS_CSPACE_SRGB:
            for (x = 0; x < w; ++x)
            {
                const unsigned char *p = line + 3 * x;
                unsigned lum = (299u * p[0] + 587u * p[1] + 114u * p[2]) / 1000u;
                if (lum < 128)
                    pbmrow[x / 8] |= 0x80 >> (x & 7);
            }
            return;

        case CUPS_CSPACE_RGBA:
            for (x = 0; x < w; ++x)
            {
                const unsigned char *p = line + 4 * x;
                unsigned lum = (299u * p[0] + 587u * p[1] + 114u * p[2]) / 1000u;
                if (lum < 128)
                    pbmrow[x / 8] |= 0x80 >> (x & 7);
            }
            return;

        default:
            if (ncol == 1 && bpc == 16)     /* GRAYE and friends */
            {
                for (x = 0; x < w; ++x)
                {
                    unsigned s = (line[2 * x] << 8) | line[2 * x + 1];
                    if (s < 32768)
                        pbmrow[x / 8] |= 0x80 >> (x & 7);
                }
                return;
            }
            fprintf(stderr, "rastertozjs: unsupported colorspace %d "
                    "(bpc=%u ncol=%u)\n", (int)cs, bpc, ncol);
            exit(1);
    }
}

/*
 * Page geometry: macOS cgpdftoraster renders only the imageable area
 * (the PPD margins are NOT part of the bitmap), whereas the ZjStream
 * expects a full-page bitmap with white margins.  Rebuild that here.
 */
typedef struct {
    unsigned full_w, full_h;   /* full page in pixels */
    unsigned img_w, img_h;     /* raster size */
    unsigned off_x;            /* left margin in pixels */
    unsigned off_y;            /* top margin in pixels (row 0 = top) */
    int      pad;              /* 1 = pad, 0 = pass through as-is */
} page_geom_t;

static void page_geometry(const cups_page_header2_t *hdr, page_geom_t *g)
{
    double resx = hdr->HWResolution[0] ? hdr->HWResolution[0] : 600;
    double resy = hdr->HWResolution[1] ? hdr->HWResolution[1] : 600;
    double pw = hdr->cupsPageSize[0] ? hdr->cupsPageSize[0] : hdr->PageSize[0];
    double ph = hdr->cupsPageSize[1] ? hdr->cupsPageSize[1] : hdr->PageSize[1];
    double L = hdr->cupsImagingBBox[0], B = hdr->cupsImagingBBox[1];
    double R = hdr->cupsImagingBBox[2], T = hdr->cupsImagingBBox[3];

    g->img_w = hdr->cupsWidth;
    g->img_h = hdr->cupsHeight;

    if (pw <= 0 || ph <= 0 || (L == 0 && B == 0 && R == 0 && T == 0) ||
            R <= L || T <= B)
    {
        /* No usable geometry: pass the raster through unchanged. */
        g->full_w = g->img_w;
        g->full_h = g->img_h;
        g->off_x = 0;
        g->off_y = 0;
        g->pad = 0;
        return;
    }

    g->full_w = (unsigned)(pw * resx / 72.0 + 0.5);
    g->full_h = (unsigned)(ph * resy / 72.0 + 0.5);
    g->off_x = (unsigned)(L * resx / 72.0 + 0.5);
    g->off_y = g->full_h - (unsigned)(T * resy / 72.0 + 0.5);

    if (g->full_w < g->img_w || g->full_h < g->img_h ||
            g->off_x + g->img_w > g->full_w || g->off_y + g->img_h > g->full_h)
    {
        g->full_w = g->img_w;
        g->full_h = g->img_h;
        g->off_x = 0;
        g->off_y = 0;
        g->pad = 0;
        return;
    }

    g->pad = 1;
}

/*
 * Writer thread: read CUPS raster from stdin, publish the first page
 * geometry, then stream every page as P4 into the pipe.
 */
static void *pbm_writer(void *arg)
{
    ctx_t *ctx = arg;
    FILE *out = fdopen(ctx->pipe_fd, "w");
    cups_raster_t *ras = cupsRasterOpen(0, CUPS_RASTER_READ);
    cups_page_header2_t hdr;
    unsigned char *line = NULL, *pbmrow = NULL, *fullrow = NULL;
    unsigned rowbpl = 0, fullbpl = 0;
    int first = 1;

    if (!out)
    {
        fprintf(stderr, "rastertozjs: fdopen(pipe) failed\n");
        fail(ctx);
        return NULL;
    }
    if (getenv("RAS2ZJS_DEBUG"))
        ctx->dump = fopen("/var/folders/l4/tb3nxtbn5nsfryljr2647qb80000gn/T/opencode/dbg.pbm", "w");
    if (!ras)
    {
        fprintf(stderr, "rastertozjs: cupsRasterOpen failed\n");
        fail(ctx);
        return NULL;
    }

    while (cupsRasterReadHeader2(ras, &hdr) > 0)
    {
        page_geom_t g;
        unsigned y;

        page_geometry(&hdr, &g);

        if (first)
        {
            publish_header(ctx, &hdr);
            first = 0;
        }

        if (g.img_w == 0 || g.img_h == 0)
            continue;

        if (!line || (hdr.cupsBytesPerLine > 0 &&
                (rowbpl != (g.img_w + 7) / 8 || fullbpl != (g.full_w + 7) / 8)))
        {
            free(line);
            free(pbmrow);
            free(fullrow);
            rowbpl = (g.img_w + 7) / 8;
            fullbpl = (g.full_w + 7) / 8;
            line = malloc(hdr.cupsBytesPerLine ? hdr.cupsBytesPerLine : 1);
            pbmrow = malloc(rowbpl ? rowbpl : 1);
            fullrow = malloc(fullbpl ? fullbpl : 1);
            if (!line || !pbmrow || !fullrow)
            {
                fprintf(stderr, "rastertozjs: out of memory\n");
                fail(ctx);
                return NULL;
            }
        }

        fprintf(out, "P4\n%u %u\n", g.full_w, g.full_h);
        if (ctx->dump)
            fprintf(ctx->dump, "P4\n%u %u\n", g.full_w, g.full_h);
        for (y = 0; y < g.full_h; ++y)
        {
            if (g.pad && (y < g.off_y || y >= g.off_y + g.img_h))
            {
                memset(fullrow, 0, fullbpl);
            }
            else
            {
                size_t got = cupsRasterReadPixels(ras, line,
                                                 hdr.cupsBytesPerLine);
                if (got != hdr.cupsBytesPerLine)
                {
                    fprintf(stderr, "rastertozjs: short row read "
                            "(page row %u, got %zu of %u)\n",
                            y, got, hdr.cupsBytesPerLine);
                    fail(ctx);
                    fclose(out);
                    return NULL;
                }
                convert_row(line, pbmrow, g.img_w, rowbpl, &hdr);
                if (!g.pad)
                {
                    if (fwrite(pbmrow, 1, rowbpl, out) != rowbpl)
                    {
                        fail(ctx);
                        fclose(out);
                        return NULL;
                    }
                    if (ctx->dump)
                        fwrite(pbmrow, 1, rowbpl, ctx->dump);
                    continue;
                }
                memset(fullrow, 0, fullbpl);
                {
                    unsigned i;
                    for (i = 0; i < rowbpl; ++i)
                    {
                        unsigned char b = pbmrow[i];
                        unsigned bit;
                        if (!b)
                            continue;
                        for (bit = 0; bit < 8; ++bit)
                        {
                            unsigned p;
                            if (!(b & (0x80 >> bit)))
                                continue;
                            p = g.off_x + i * 8 + bit;
                            if (p >= fullbpl * 8)
                                continue;
                            fullrow[p >> 3] |= 0x80 >> (p & 7);
                        }
                    }
                }
            }
            if (fwrite(fullrow, 1, fullbpl, out) != fullbpl)
            {
                fail(ctx);
                fclose(out);
                return NULL;
            }
            if (ctx->dump)
                fwrite(fullrow, 1, fullbpl, ctx->dump);
        }
    }

    fclose(out);
    free(line);
    free(pbmrow);
    free(fullrow);
    cupsRasterClose(ras);
    return NULL;
}

/* ---------------- option parsing ---------------- */

typedef struct { const char *name; int code; } map_t;

static const map_t paper_codes[] = {
    { "Letter", 1 }, { "Legal", 5 }, { "A4", 9 }, { "A5", 11 },
    { "A6", 70 }, { "B5", 13 }, { "Executive", 7 },
    { "Env10", 20 }, { "EnvDL", 27 }, { "EnvC5", 28 }, { "EnvB5", 34 },
    { "EnvMonarch", 37 }, { "Postcard", 43 }, { "DoublePostcard", 82 },
    { "16k195x270", 264 }, { "16k184x260", 263 }, { "16k197x273", 257 },
    { NULL, 1 }
};

static const map_t media_codes[] = {
    { "Standard", 1 }, { "Transparency", 2 }, { "Envelope", 267 },
    { "Letterhead", 513 }, { "Bond", 260 }, { "Heavy", 262 },
    { "Light", 258 }, { "Labels", 265 }, { "Rough", 263 },
    { "ExtraHeavy", 283 }, { "Medium", 282 }, { "Color", 512 },
    { "Preprinted", 514 }, { "Prepunched", 515 }, { "Recycled", 516 },
    { "Vellum", 273 }, { NULL, 1 }
};

static const map_t source_codes[] = {
    { "Auto", 7 }, { "Upper", 1 }, { "Manual", 4 }, { "Envelope", 5 },
    { NULL, 7 }
};

static int lookup(const map_t *tab, const char *val, int deflt)
{
    for (; tab->name; ++tab)
        if (strcasecmp(tab->name, val) == 0)
            return tab->code;
    return deflt;
}

static void parse_options(const char *options, job_opts_t *opts)
{
    char buf[4096];
    char *save, *tok;

    opts->page_size = NULL;
    opts->media_type = NULL;
    opts->media_source = NULL;
    opts->draft = 0;
    opts->density = 0;
    opts->copies = 1;

    if (!options || !*options)
        return;

    strncpy(buf, options, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save))
    {
        char *eq = strchr(tok, '=');
        if (!eq)
            continue;
        *eq = 0;
        if (strcasecmp(tok, "PageSize") == 0)
            opts->page_size = eq + 1;
        else if (strcasecmp(tok, "MediaType") == 0)
            opts->media_type = eq + 1;
        else if (strcasecmp(tok, "MediaSource") == 0)
            opts->media_source = eq + 1;
        else if (strcasecmp(tok, "Quality") == 0)
            opts->draft = (strcasecmp(eq + 1, "draft") == 0);
        else if (strcasecmp(tok, "Density") == 0)
        {
            const char *v = eq + 1;
            while (*v && (*v < '0' || *v > '9'))
                ++v;
            opts->density = atoi(v);
        }
    }
}

int main(int argc, char **argv)
{
    ctx_t ctx;
    pthread_t thread;
    int pipefd[2];
    FILE *in;
    const char *options = "";
    int copies = 1;
    job_opts_t opts;
    int zargc;
    char *zargv[16];
    char rbuf[32], gbuf[32], pb[16], mb[16], sb[16], nb[16], tb[16];
    unsigned resx, resy;
    int paper, media, source;
    int rc;

    if (argc < 6)
    {
        fprintf(stderr, "Usage: rastertozjs job-id user title copies options [file]\n");
        return 1;
    }

    copies = atoi(argv[4]);
    options = argv[5];
    parse_options(options, &opts);
    if (copies > 1)
        opts.copies = copies;

    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.mutex, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    if (pipe(pipefd) < 0)
    {
        fprintf(stderr, "rastertozjs: pipe() failed\n");
        return 1;
    }
    ctx.pipe_fd = pipefd[1];

    if (pthread_create(&thread, NULL, pbm_writer, &ctx) != 0)
    {
        fprintf(stderr, "rastertozjs: pthread_create failed\n");
        return 1;
    }

    wait_header(&ctx);

    if (ctx.err)
    {
        fprintf(stderr, "rastertozjs: failed to read CUPS raster input\n");
        return 1;
    }

    resx = ctx.res_x ? ctx.res_x : 600;
    resy = ctx.res_y ? ctx.res_y : 600;
    if (resx != 600 && resx != 1200)
        resx = 600;
    if (resy != 600 && resy != 1200)
        resy = 600;

    DEBUG("page %ux%u at %ux%u dpi\n", ctx.width, ctx.height, resx, resy);

    paper  = opts.page_size  ? lookup(paper_codes,  opts.page_size,  1) : 1;
    media  = opts.media_type ? lookup(media_codes,  opts.media_type, 1) : 1;
    source = opts.media_source ? lookup(source_codes, opts.media_source, 7) : 7;

    snprintf(rbuf, sizeof(rbuf), "-r%ux%u", resx, resy);
    snprintf(gbuf, sizeof(gbuf), "-g%ux%u", ctx.width, ctx.height);
    snprintf(pb, sizeof(pb), "-p%d", paper);
    snprintf(mb, sizeof(mb), "-m%d", media);
    snprintf(sb, sizeof(sb), "-s%d", source);
    snprintf(nb, sizeof(nb), "-n%d", opts.copies);
    tb[0] = 0;
    if (opts.draft)
        strcpy(tb, "-t");

    zargc = 0;
    zargv[zargc++] = "rastertozjs";
    zargv[zargc++] = "-z2";        /* HP LaserJet Pro P1102 */
    zargv[zargc++] = "-P";         /* no START_PLANE */
    zargv[zargc++] = "-L0";        /* no logical clipping */
    zargv[zargc++] = rbuf;
    zargv[zargc++] = gbuf;
    zargv[zargc++] = pb;
    zargv[zargc++] = mb;
    zargv[zargc++] = sb;
    zargv[zargc++] = nb;
    if (tb[0])
        zargv[zargc++] = tb;
    if (opts.density >= 1 && opts.density <= 5)
    {
        snprintf(tb, sizeof(tb), "-T%d", opts.density);
        zargv[zargc++] = tb;
    }
    zargv[zargc] = NULL;

    in = fdopen(pipefd[0], "r");
    if (!in)
        return 1;

    /* The vendored engine writes the full ZjStream document to stdout. */
    rc = zjs_main(zargc, zargv, in);

    fclose(in);
    pthread_join(thread, NULL);
    return rc;
}

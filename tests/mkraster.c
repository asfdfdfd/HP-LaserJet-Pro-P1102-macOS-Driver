/*
 * mkraster - test helper: P4 (pbmraw) or P5 (pgmraw, -g) -> CUPS raster
 * (v3), for testing the rastertozjs filter without a full CUPS stack.
 *
 * Usage: mkraster [-r XRESxYRES] [-s WxHpoints] [-m MARGINPT] [-g] [-K]
 *         [in.pbm] > out.raster
 *   -m MARGINPT  set symmetric page margins (points): the raster covers
 *                only the imageable area, like cgpdftoraster does
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cups/raster.h>

static void die(const char *msg) { fprintf(stderr, "mkraster: %s\n", msg); exit(1); }

int main(int argc, char **argv)
{
    int resx = 600, resy = 600;
    float pw = 612, ph = 792;
    int gray = 0;
    int kmode = 0;   /* -K: read P5 where 0=white,255=black, write K 8bpp */
    float margin = 0;
    int i;
    FILE *in = stdin;
    cups_page_header2_t hdr;
    cups_raster_t *ras;
    unsigned w, h, bpl;
    unsigned char *row;
    char line[256];

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
        {
            if (sscanf(argv[++i], "%dx%d", &resx, &resy) != 2)
                die("bad -r");
        }
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
        {
            if (sscanf(argv[++i], "%fx%f", &pw, &ph) != 2)
                die("bad -s");
        }
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
        {
            margin = (float)atof(argv[++i]);
            if (margin < 0 || margin >= pw / 2 || margin >= ph / 2)
                die("bad -m");
        }
        else if (strcmp(argv[i], "-g") == 0)
            gray = 1;
        else if (strcmp(argv[i], "-K") == 0)
            kmode = 1;
        else
        {
            in = fopen(argv[i], "rb");
            if (!in) die("cannot open input");
        }
    }

    if (fgets(line, sizeof(line), in) == NULL ||
            strncmp(line, (gray || kmode) ? "P5" : "P4", 2) != 0)
        die("not a P4/P5 pbm stream");
    do { if (fgets(line, sizeof(line), in) == NULL) die("eof in header"); }
    while (line[0] == '#');
    if (sscanf(line, "%u %u", &w, &h) != 2)
    {
        char *sp = strchr(line, ' ');
        if (!sp) die("bad dimensions");
        w = (unsigned)atoi(line);
        h = (unsigned)atoi(sp);
        if (fgets(line, sizeof(line), in) == NULL) die("eof after dims");
    }
    if (gray || kmode)
    {
        if (fgets(line, sizeof(line), in) == NULL) die("eof in maxval");
    }

    bpl = gray || kmode ? w : (w + 7) / 8;

    memset(&hdr, 0, sizeof(hdr));
    strncpy(hdr.MediaClass, "PwgRaster", sizeof(hdr.MediaClass) - 1);
    hdr.HWResolution[0] = resx;
    hdr.HWResolution[1] = resy;
    hdr.PageSize[0] = (unsigned)pw;
    hdr.PageSize[1] = (unsigned)ph;
    hdr.cupsWidth = w;
    hdr.cupsHeight = h;
    hdr.cupsBitsPerColor = (gray || kmode) ? 8 : 1;
    hdr.cupsBitsPerPixel = (gray || kmode) ? 8 : 1;
    hdr.cupsBytesPerLine = bpl;
    hdr.cupsColorOrder = CUPS_ORDER_CHUNKED;
    hdr.cupsColorSpace = kmode ? CUPS_CSPACE_K : (gray ? CUPS_CSPACE_W : CUPS_CSPACE_K);
    hdr.cupsNumColors = 1;
    hdr.cupsPageSize[0] = pw;
    hdr.cupsPageSize[1] = ph;
    hdr.cupsImagingBBox[0] = margin;
    hdr.cupsImagingBBox[1] = margin;
    hdr.cupsImagingBBox[2] = pw - margin;
    hdr.cupsImagingBBox[3] = ph - margin;

    ras = cupsRasterOpen(1, CUPS_RASTER_WRITE);
    if (!ras) die("cupsRasterOpen failed");

    row = malloc(bpl);
    if (!row) die("malloc");

    /* Write one or more pages: the input may contain several P4 headers. */
    for (;;)
    {
        unsigned y;

        if (!cupsRasterWriteHeader2(ras, &hdr))
            die("cupsRasterWriteHeader2 failed");

        for (y = 0; y < h; ++y)
        {
            if (fread(row, 1, bpl, in) != bpl)
                die("short pbm row");
            if (cupsRasterWritePixels(ras, row, bpl) != bpl)
                die("cupsRasterWritePixels failed");
        }

        /* Try to read the next P4/P5 header; stop on EOF. */
        {
            long pos = ftell(in);
            char *got = fgets(line, sizeof(line), in);
            int again = 0;

            if (got && strncmp(line, (gray || kmode) ? "P5" : "P4", 2) == 0)
            {
                do { if (fgets(line, sizeof(line), in) == NULL) break; }
                while (line[0] == '#');
                if (sscanf(line, "%u %u", &w, &h) == 2)
                {
                    if (gray)
                    {
                        if (fgets(line, sizeof(line), in) == NULL) break;
                    }
                    bpl = gray || kmode ? w : (w + 7) / 8;
                    hdr.cupsWidth = w;
                    hdr.cupsHeight = h;
                    hdr.cupsBytesPerLine = bpl;
                    row = realloc(row, bpl);
                    if (!row) die("realloc");
                    again = 1;
                }
            }
            else if (got)
                die("garbage between pages");

            if (!again)
            {
                if (got)
                    fseek(in, pos, SEEK_SET);
                break;
            }
        }
    }

    cupsRasterClose(ras);
    free(row);
    fclose(in);
    return 0;
}

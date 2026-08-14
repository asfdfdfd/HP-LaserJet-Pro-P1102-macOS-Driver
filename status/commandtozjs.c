/*
 * commandtozjs - CUPS command filter for the HP LaserJet Professional P1102.
 *
 * Implements the CUPS command file format (application/vnd.cups-command):
 * the PPD advertises `*cupsCommands: "ReportLevels ReportStatus"` and macOS
 * System Settings uses it to display the supply level / printer state.
 *
 * The command text (#CUPS-COMMAND\nReportLevels) arrives on stdin; we query
 * the printer's EWS-over-USB interface and answer with ATTR:/STATE: lines
 * on STDERR - the CUPS scheduler reads the job's status pipe (the stderr
 * of filters/backend) and parses ATTR:/STATE: from there.  stdout must stay
 * empty: it is forwarded to the backend and written to the printer.
 *
 * GPL-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "ews.h"
#include "zjs_engine.h"

#define SUPPLY_LOW 20     /* below this % report marker-supply-low-report */

static char *read_stream(FILE *in)
{
    size_t cap = 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    int c;
    while ((c = fgetc(in)) != EOF)
    {
        if (len + 1 >= cap)
        {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    buf[len] = 0;
    return buf;
}

static int has_command(const char *cmd, const char *name)
{
    return cmd && strstr(cmd, name) != NULL;
}

/* ---------------- self-test page ---------------- */

#define ST_W 5100          /* letter @ 600 dpi */
#define ST_H 6600
#define ST_BPL ((ST_W + 7) / 8)

/* 5x7 bitmap font, 5 bits per row, MSB first.  Indexed by char code. */
static const unsigned char font5x7[64][7] = {
    /* space */  {0,0,0,0,0,0,0},
    /* ! */      {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    /* " */      {0x0A,0x0A,0x0A,0,0,0,0},
    /* # */      {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},
    /* $ */      {0x04,0x0E,0x14,0x0E,0x05,0x0E,0x04},
    /* % */      {0x19,0x19,0x02,0x04,0x08,0x13,0x13},
    /* & */      {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D},
    /* ' */      {0x04,0x04,0x04,0,0,0,0},
    /* ( */      {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    /* ) */      {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    /* * */      {0,0,0x0A,0x04,0x0A,0,0},
    /* + */      {0,0x04,0x04,0x1F,0x04,0x04,0},
    /* , */      {0,0,0,0,0,0x0C,0x08},
    /* - */      {0,0,0,0x1F,0,0,0},
    /* . */      {0,0,0,0,0,0x0C,0x0C},
    /* / */      {0x01,0x02,0x02,0x04,0x08,0x08,0x10},
    /* 0 */      {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    /* 1 */      {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* 2 */      {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    /* 3 */      {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    /* 4 */      {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    /* 5 */      {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    /* 6 */      {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    /* 7 */      {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    /* 8 */      {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* 9 */      {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* : */      {0,0x0C,0x0C,0,0x0C,0x0C,0},
    /* ; */      {0,0x0C,0x0C,0,0x0C,0x08},
    /* < */      {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    /* = */      {0,0,0x1F,0,0x1F,0,0},
    /* > */      {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    /* ? */      {0x0E,0x11,0x01,0x02,0x04,0,0x04},
    /* @ */      {0x0E,0x11,0x17,0x15,0x17,0x10,0x0F},
    /* A */      {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* B */      {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* C */      {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    /* D */      {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    /* E */      {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    /* F */      {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* G */      {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    /* H */      {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* I */      {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* J */      {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    /* K */      {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* L */      {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* M */      {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    /* N */      {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    /* O */      {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* P */      {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* Q */      {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    /* R */      {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* S */      {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    /* T */      {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* U */      {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* V */      {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    /* W */      {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    /* X */      {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* Y */      {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    /* Z */      {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
};

static void st_pixel(unsigned char *bm, int x, int y)
{
    if (x < 0 || x >= ST_W || y < 0 || y >= ST_H)
        return;
    bm[y * ST_BPL + x / 8] |= (unsigned char)(0x80 >> (x & 7));
}

static void st_rect(unsigned char *bm, int x0, int y0, int x1, int y1)
{
    int x, y;
    for (y = y0; y <= y1; ++y)
        for (x = x0; x <= x1; ++x)
            st_pixel(bm, x, y);
}

static void st_text(unsigned char *bm, int x, int y, const char *s, int scale)
{
    for (; *s; ++s)
    {
        unsigned char c = (unsigned char)*s;
        const unsigned char *g = font5x7[(c >= ' ' && c <= '_') ? c - ' ' : 0];
        int r, b;
        for (r = 0; r < 7; ++r)
            for (b = 0; b < 5; ++b)
                if (g[r] & (0x10 >> b))
                    st_rect(bm, x + b * scale, y + r * scale,
                            x + b * scale + scale - 1,
                            y + r * scale + scale - 1);
        x += 6 * scale;
    }
}

static int print_self_test(void)
{
    unsigned char *bm = calloc((size_t)ST_H * ST_BPL, 1);
    if (!bm)
        return 1;

    int m = 94;   /* 4 mm margin at 600 dpi */
    /* frame */
    st_rect(bm, m, m, ST_W - 1 - m, m);
    st_rect(bm, m, ST_H - 1 - m, ST_W - 1 - m, ST_H - 1 - m);
    st_rect(bm, m, m, m, ST_H - 1 - m);
    st_rect(bm, ST_W - 1 - m, m, ST_W - 1 - m, ST_H - 1 - m);
    /* corner crosshairs */
    st_rect(bm, m - 40, m - 2, m + 40, m + 2);
    st_rect(bm, m - 2, m - 40, m + 2, m + 40);
    st_rect(bm, ST_W - 1 - m - 40, m - 2, ST_W - 1 - m + 40, m + 2);
    st_rect(bm, ST_W - 1 - m - 2, m - 40, ST_W - 1 - m + 2, m + 40);

    /* title */
    st_text(bm, 700, 400, "HP P1102 SELF-TEST", 8);
    st_text(bm, 900, 700, "OPEN SOURCE DRIVER", 4);
    st_text(bm, 1150, 900, "600 DPI  BLACK/WHITE", 3);

    /* resolution bars: 1,2,4,8,16 px vertical stripes */
    {
        int widths[5] = { 1, 2, 4, 8, 16 };
        const char *labels[5] = { "1 PX", "2 PX", "4 PX", "8 PX", "16 PX" };
        int y = 1300, y2 = 2400, i;
        st_text(bm, 900, 1150, "RESOLUTION STRIPES", 2);
        for (i = 0; i < 5; ++i)
        {
            int x0 = 700 + i * 780, x;
            st_text(bm, x0, y2 + 20, labels[i], 2);
            for (x = 0; x < 640; x += 2 * widths[i])
                st_rect(bm, x0 + x, y, x0 + x + widths[i] - 1, y2);
        }
    }

    /* gray blocks: 25% / 50% / 75% dither patterns */
    {
        int y = 2900, y2 = 3700, i;
        const char *labels[3] = { "25%", "50%", "75%" };
        st_text(bm, 900, 2700, "GRAY BLOCKS (DITHER)", 2);
        for (i = 0; i < 3; ++i)
        {
            int x0 = 700 + i * 1380, x, y2c;
            for (y2c = y; y2c <= y2; ++y2c)
                for (x = 0; x < 1000; ++x)
                {
                    int on = 0;
                    if (i == 0)      on = ((x / 2) + (y2c / 2)) & 1;
                    else if (i == 1) on = (x + y2c) & 1;
                    else             on = !(((x / 2) + (y2c / 2)) & 1);
                    if (on)
                        st_pixel(bm, x0 + x, y2c);
                }
            st_text(bm, x0, y2 + 20, labels[i], 2);
        }
    }

    /* diagonal lines */
    {
        int y, y0 = 4100, x;
        st_text(bm, 900, 3900, "DIAGONAL LINES", 2);
        for (y = 0; y < 500; ++y)
            for (x = 0; x < 9; ++x)
                st_pixel(bm, 700 + y * 6, y0 + y + x);
        for (y = 0; y < 500; ++y)
            for (x = 0; x < 9; ++x)
                st_pixel(bm, 700 + 500 * 6 - y * 6, y0 + y + x);
    }

    /* footer */
    {
        char buf[128];
        time_t now = time(NULL);
        struct tm *tmp = localtime(&now);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tmp);
        st_text(bm, 700, 5600, "HP LASERJET PROFESSIONAL P1102", 2);
        st_text(bm, 700, 5850, buf, 2);
        st_text(bm, 700, 6050, "OPEN SOURCE DRIVER  -  RASTERTOZJS", 2);
    }

    /* build the P4 stream and hand it to the vendored zjs engine */
    size_t plen = (size_t)ST_H * ST_BPL + 32;
    size_t datalen;
    unsigned char *pbm = malloc(plen);
    if (!pbm)
    {
        free(bm);
        return 1;
    }
    int hlen = snprintf((char *)pbm, 32, "P4\n%d %d\n", ST_W, ST_H);
    memcpy(pbm + hlen, bm, (size_t)ST_H * ST_BPL);
    free(bm);
    datalen = (size_t)hlen + (size_t)ST_H * ST_BPL;

    FILE *in = fmemopen(pbm, datalen, "r");
    if (!in)
    {
        free(pbm);
        return 1;
    }
    char *zargv[] = {
        "commandtozjs", "-z2", "-P", "-L0",
        "-r600x600", "-g5100x6600", "-p1", "-m1", "-s7", "-n1", NULL
    };
    /* the engine writes the ZjStream to stdout; cupsd forwards it to
     * the backend which sends it to the printer */
    zjs_main(10, zargv, in);
    free(pbm);
    return 0;
}

int main(int argc, char **argv)
{

    /* CUPS passes the command either on stdin or as the optional
     * argv[6] file argument (job document).  Read from whichever exists. */
    FILE *cmd_in = stdin;
    if (argc > 6 && argv[6] && strcmp(argv[6], "-") != 0)
    {
        cmd_in = fopen(argv[6], "r");
        if (!cmd_in)
            fprintf(stderr, "ERROR: commandtozjs: cannot open '%s'\n",
                    argv[6]);
    }
    char *cmd = read_stream(cmd_in);
    if (cmd_in != stdin)
        fclose(cmd_in);
    if (!cmd)
    {
        fprintf(stderr, "ERROR: commandtozjs: no command on stdin\n");
        return 1;
    }

    /* Commands are case-insensitive; normalize. */
    for (char *p = cmd; *p; ++p)
        *p = (char)tolower((unsigned char)*p);

    if (has_command(cmd, "reportlevels"))
    {
        ews_conn_t conn = {0};
        if (ews_open(&conn) != 0)
        {
            fprintf(stderr, "ERROR: commandtozjs: EWS interface unavailable\n");
            return 0;   /* no data; CUPS shows unknown levels */
        }

        unsigned char *raw = NULL;
        size_t len = 0;
        ews_get(&conn, "/DevMgmt/ConsumableConfigDyn.xml", &raw, &len);
        ews_close(&conn);

        char *level = NULL, *cstate = NULL;
        if (raw)
        {
            size_t bl;
            unsigned char *body = ews_dechunk(raw, len, &bl);
            if (body)
            {
                level = ews_xml_field((char *)body,
                                      "dd:ConsumablePercentageLevelRemaining");
                cstate = ews_xml_field((char *)body, "dd:ConsumableState");
            }
            free(body);
        }
        free(raw);

        int lvl = -1;   /* unknown */
        if (level && *level)
            lvl = atoi(level);
        free(level);

        fprintf(stderr, "ATTR: marker-colors=#000000 marker-levels=%d "
                "marker-names=Black marker-types=toner\n", lvl);
        if (lvl >= 0 && lvl < SUPPLY_LOW)
            fprintf(stderr, "STATE: +marker-supply-low-report\n");
        else
            fprintf(stderr, "STATE: -marker-supply-low-report\n");
        if (cstate)
            fprintf(stderr, "STATE: %s\n", cstate);
        free(cstate);
    }
    else if (has_command(cmd, "printselftestpage"))
    {
        return print_self_test();
    }
    else if (has_command(cmd, "reportstatus"))
    {
        ews_conn_t conn = {0};
        if (ews_open(&conn) != 0)
        {
            fprintf(stderr, "STATE: offline\n");
            return 0;
        }
        unsigned char *raw = NULL;
        size_t len = 0;
        ews_get(&conn, "/DevMgmt/ProductStatusDyn.xml", &raw, &len);
        ews_close(&conn);
        char *status = NULL;
        if (raw)
        {
            size_t bl;
            unsigned char *body = ews_dechunk(raw, len, &bl);
            if (body)
                status = ews_xml_field((char *)body, "pscat:StatusCategory");
            free(body);
        }
        free(raw);
        fprintf(stderr, "STATE: %s\n", status ? status : "idle");
        free(status);
    }

    free(cmd);
    return 0;
}

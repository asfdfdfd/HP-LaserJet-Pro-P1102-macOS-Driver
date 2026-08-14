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
#include "ews.h"

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

int main(int argc, char **argv)
{
    for (int i = 0; i < argc; ++i)
        fprintf(stderr, "ERROR: commandtozjs: argv[%d]='%s'\n", i, argv[i]);

    {
        /* Diagnostics (CUPS sandbox test). */
        fprintf(stderr, "ERROR: commandtozjs started pid=%d cwd=%s\n",
                (int)getpid(), getenv("TMPDIR") ? getenv("TMPDIR") : "?");
        const char *td = getenv("TMPDIR");
        char path[512];
        snprintf(path, sizeof(path), "%s/commandtozjs-ran", td ? td : "/tmp");
        FILE *m = fopen(path, "w");
        if (m) { fprintf(m, "pid=%d\n", (int)getpid()); fclose(m); }
        else
            fprintf(stderr, "ERROR: commandtozjs marker write failed\n");
    }

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
    {
        char dbg[80];
        strncpy(dbg, cmd, sizeof(dbg) - 1);
        dbg[sizeof(dbg) - 1] = 0;
        for (char *p = dbg; *p; ++p)
            if (*p == '\n' || *p == '\r')
                *p = ' ';
        fprintf(stderr, "ERROR: commandtozjs: cmd='%s'\n", dbg);
    }

    /* Commands are case-insensitive; normalize. */
    for (char *p = cmd; *p; ++p)
        *p = (char)tolower((unsigned char)*p);

    if (has_command(cmd, "reportlevels"))
    {
        setenv("EWS_DEBUG", "1", 1);
        ews_conn_t conn = {0};
        if (ews_open(&conn) != 0)
        {
            fprintf(stderr, "ERROR: commandtozjs: EWS interface unavailable\n");
            return 0;   /* no data; CUPS shows unknown levels */
        }
        fprintf(stderr, "ERROR: commandtozjs: EWS opened ok\n");

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

        fprintf(stderr, "ERROR: commandtozjs: reportlevels parsed\n");

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

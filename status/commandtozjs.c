/*
 * commandtozjs - CUPS command filter for the HP LaserJet Professional P1102.
 *
 * Implements the CUPS command file format (application/vnd.cups-command):
 * the PPD advertises `*cupsCommands: "ReportLevels ReportStatus"` and macOS
 * System Settings uses it to display the supply level / printer state.
 *
 * The command text (#CUPS-COMMAND\nReportLevels) arrives on stdin; we query
 * the printer's EWS-over-USB interface and answer with ATTR:/STATE: lines
 * on stdout, which the CUPS scheduler parses.
 *
 * GPL-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ews.h"

#define SUPPLY_LOW 10     /* below this % report marker-supply-low-report */

static char *read_command(void)
{
    size_t cap = 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    int c;
    while ((c = getchar()) != EOF)
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
    (void)argc;
    (void)argv;

    char *cmd = read_command();
    if (!cmd)
        return 1;

    /* Commands are case-insensitive; normalize. */
    for (char *p = cmd; *p; ++p)
        *p = (char)tolower((unsigned char)*p);

    if (has_command(cmd, "reportlevels"))
    {
        ews_conn_t conn = {0};
        if (ews_open(&conn) != 0)
        {
            fprintf(stderr, "commandtozjs: EWS interface unavailable\n");
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

        printf("ATTR: marker-colors=#000000 marker-levels=%d "
               "marker-names=Black marker-types=toner\n", lvl);
        if (lvl >= 0 && lvl < SUPPLY_LOW)
            printf("STATE: +marker-supply-low-report\n");
        else
            printf("STATE: -marker-supply-low-report\n");
        if (cstate)
            printf("STATE: %s\n", cstate);
        free(cstate);
    }
    else if (has_command(cmd, "reportstatus"))
    {
        ews_conn_t conn = {0};
        if (ews_open(&conn) != 0)
        {
            printf("STATE: offline\n");
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
        printf("STATE: %s\n", status ? status : "idle");
        free(status);
    }

    free(cmd);
    return 0;
}

/*
 * p1102status - query HP LaserJet Professional P1102 status over USB.
 *
 * The printer does not support PJL INFO queries; it exposes a small
 * embedded web server (EWS) on a vendor-specific USB interface
 * (class FF/02/10) speaking plain HTTP over the bulk pipes.  We fetch
 *   /DevMgmt/ProductStatusDyn.xml   - printer state
 *   /DevMgmt/ConsumableConfigDyn.xml - consumable info + toner %
 * This is the same mechanism HP's own usbink utility uses.
 *
 * GPL-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ews.h"

static void query(int want_json)
{
    ews_conn_t conn = {0};
    if (ews_open(&conn) != 0)
    {
        fprintf(stderr, "p1102status: printer not found or EWS interface "
                        "busy\n");
        exit(1);
    }

    unsigned char *raw1 = NULL, *raw2 = NULL, *raw3 = NULL;
    size_t len1 = 0, len2 = 0, len3 = 0;
    ews_get(&conn, "/DevMgmt/ProductStatusDyn.xml", &raw1, &len1);
    ews_get(&conn, "/DevMgmt/ConsumableConfigDyn.xml", &raw2, &len2);
    ews_get(&conn, "/DevMgmt/ProductUsageDyn.xml", &raw3, &len3);
    ews_close(&conn);

    char *status = NULL, *level = NULL, *cstate = NULL;
    char *cname = NULL, *brand = NULL, *serial = NULL;
    char *total_imp = NULL, *cart_imp = NULL, *pages_left = NULL;
    if (raw1)
    {
        size_t bl;
        unsigned char *body = ews_dechunk(raw1, len1, &bl);
        if (body)
            status = ews_xml_field((char *)body, "pscat:StatusCategory");
        free(body);
    }
    if (raw2)
    {
        size_t bl;
        unsigned char *body = ews_dechunk(raw2, len2, &bl);
        if (body)
        {
            level = ews_xml_field((char *)body, "dd:ConsumablePercentageLevelRemaining");
            cstate = ews_xml_field((char *)body, "dd:ConsumableState");
            cname = ews_xml_field((char *)body, "dd:ConsumableFamilyName");
            brand = ews_xml_field((char *)body, "dd:Brand");
            serial = ews_xml_field((char *)body, "dd:SerialNumber");
        }
        free(body);
    }
    if (raw3)
    {
        size_t bl;
        unsigned char *body = ews_dechunk(raw3, len3, &bl);
        if (body)
        {
            total_imp = ews_xml_field_n((char *)body, "dd:TotalImpressions", 0);
            cart_imp = ews_xml_field_n((char *)body, "dd:TotalImpressions", 1);
            pages_left = ews_xml_field((char *)body,
                                       "dd:EstimatedPagesRemaining");
        }
        free(body);
    }

    free(raw1);
    free(raw2);
    free(raw3);

    if (!status && !cname)
    {
        fprintf(stderr, "p1102status: no response from the printer\n");
        exit(1);
    }

    if (want_json)
    {
        const char *st = status
            ? (strcmp(status, "inPowerSave") == 0 ? "sleeping" : status)
            : NULL;
        printf("{\n");
        printf("  \"printer\": \"HP LaserJet Professional P1102\",\n");
        printf("  \"status\": %s\"%s\",\n", st ? "" : "null", st ? st : "");
        printf("  \"consumable\": \"%s\",\n", cname ? cname : "null");
        printf("  \"consumable_state\": \"%s\",\n", cstate ? cstate : "null");
        printf("  \"brand\": \"%s\",\n", brand ? brand : "null");
        printf("  \"cartridge_serial\": \"%s\",\n", serial ? serial : "null");
        printf("  \"toner_percent\": %s,\n",
               level && *level ? level : "null");
        printf("  \"total_pages\": %s,\n",
               total_imp && *total_imp ? total_imp : "null");
        printf("  \"cartridge_pages\": %s,\n",
               cart_imp && *cart_imp ? cart_imp : "null");
        printf("  \"pages_remaining\": %s\n",
               pages_left && *pages_left ? pages_left : "null");
        printf("}\n");
    }
    else
    {
        printf("Printer:  HP LaserJet Professional P1102\n");
        printf("Status:   %s\n", status ? status : "unknown");
        if (cname)
            printf("Supply:   %s (state %s, %s)\n", cname,
                   cstate ? cstate : "unknown", brand ? brand : "?");
        if (serial)
            printf("Cartridge serial: %s\n", serial);
        if (level && *level)
            printf("Toner level: %s%%\n", level);
        else
            printf("Toner level: unknown\n");
        if (total_imp && *total_imp)
            printf("Total pages: %s\n", total_imp);
        if (cart_imp && *cart_imp)
            printf("Cartridge pages: %s\n", cart_imp);
        if (pages_left && *pages_left)
            printf("Estimated pages remaining: %s\n", pages_left);
    }

    free(status);
    free(level);
    free(cstate);
    free(cname);
    free(brand);
    free(serial);
    free(total_imp);
    free(cart_imp);
    free(pages_left);
}

int main(int argc, char **argv)
{
    int want_json = 0;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--json") == 0)
            want_json = 1;
    query(want_json);
    return 0;
}

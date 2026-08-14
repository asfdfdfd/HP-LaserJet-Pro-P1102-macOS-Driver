/*
 * p1102status - query HP LaserJet Professional P1102 status over USB.
 *
 * The printer does not support PJL INFO queries and its cartridge has no
 * memory chip, so a toner *percentage* is not available.  What the printer
 * does expose is a small embedded web server (EWS) on a vendor-specific USB
 * interface (class FF/02/10) that speaks plain HTTP over the bulk pipes.
 *
 * We fetch two XML documents:
 *   /DevMgmt/ProductStatusDyn.xml       - printer state
 *   /DevMgmt/ConsumableConfigDyn.xml    - consumable info
 *
 * This is the same mechanism HP's own usbink utility uses.  The HTTP-over-
 * USB transport (raw HTTP on the bulk endpoints of interface FF/02/10) is
 * the one used by hplip's "Marvell EWS" channel.
 *
 * GPL-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#define VID 0x03f0
#define PID 0x002a
#define IFACE_CLASS_PRINTER 7
#define IFACE_CLASS_EWS     255

#define HTTP_TIMEOUT_MS 2000

typedef struct {
    IOUSBInterfaceInterface500 **iface;
    UInt8 out_pipe;
    UInt8 in_pipe;
} ews_conn_t;

static io_service_t find_interface(int want_class)
{
    io_service_t intf = 0;
    CFMutableDictionaryRef matching = IOServiceMatching("IOUSBHostInterface");
    io_iterator_t iter = 0;
    IOReturn kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter);
    if (kr != kIOReturnSuccess || !iter)
        return 0;

    io_service_t s;
    while ((s = IOIteratorNext(iter)))
    {
        CFMutableDictionaryRef props = NULL;
        int v = 0, p = 0, icls = 0;
        IORegistryEntryCreateCFProperties(s, &props, kCFAllocatorDefault, 0);
        if (props)
        {
            CFNumberRef nv = CFDictionaryGetValue(props, CFSTR(kUSBVendorID));
            CFNumberRef np = CFDictionaryGetValue(props, CFSTR(kUSBProductID));
            CFNumberRef nc = CFDictionaryGetValue(props, CFSTR("bInterfaceClass"));
            if (nv) CFNumberGetValue(nv, kCFNumberIntType, &v);
            if (np) CFNumberGetValue(np, kCFNumberIntType, &p);
            if (nc) CFNumberGetValue(nc, kCFNumberIntType, &icls);
            CFRelease(props);
        }
        if (v == VID && p == PID && icls == want_class)
        {
            intf = s;
            break;
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(iter);
    return intf;
}

static int ews_open(ews_conn_t *c)
{
    io_service_t intf = find_interface(IFACE_CLASS_EWS);
    if (!intf)
        return -1;

    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    IOReturn kr = IOCreatePlugInInterfaceForService(intf,
        kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID,
        (IOCFPlugInInterface ***)&plug, &score);
    IOObjectRelease(intf);
    if (kr != kIOReturnSuccess || !plug)
        return -1;

    HRESULT res = (*plug)->QueryInterface(plug,
        CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID *)&c->iface);
    IODestroyPlugInInterface(plug);
    if (res != 0 || !c->iface)
        return -1;

    kr = (*c->iface)->USBInterfaceOpen(c->iface);
    if (kr != kIOReturnSuccess && kr != kIOReturnExclusiveAccess)
        return -1;

    UInt8 nPipes = 0;
    c->out_pipe = c->in_pipe = 0;
    (*c->iface)->GetNumEndpoints(c->iface, &nPipes);
    for (UInt8 p = 1; p <= nPipes; ++p)
    {
        UInt8 dir, num, trType, interval;
        UInt16 maxPacket;
        kr = (*c->iface)->GetPipeProperties(c->iface, p, &dir, &num, &trType,
            &maxPacket, &interval);
        if (kr) continue;
        if (trType == kUSBBulk && dir == kUSBOut)
            c->out_pipe = p;
        if (trType == kUSBBulk && dir == kUSBIn && !c->in_pipe)
            c->in_pipe = p;
    }
    if (!c->out_pipe || !c->in_pipe)
        return -1;
    return 0;
}

static void ews_close(ews_conn_t *c)
{
    if (!c->iface) return;
    (*c->iface)->USBInterfaceClose(c->iface);
    (*c->iface)->Release(c->iface);
    c->iface = NULL;
}

/* HTTP GET and return the raw response (headers + body) in *out. */
static int ews_get(ews_conn_t *c, const char *path, unsigned char **out,
                   size_t *outlen)
{
    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: localhost\r\nUser-Agent: p1102-status\r\n"
        "Connection: close\r\n\r\n", path);

    IOReturn rc = (*c->iface)->WritePipeTO(c->iface, c->out_pipe, req, rlen,
        1000, 1000);
    if (rc != kIOReturnSuccess)
        return -1;

    size_t cap = 16384, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) return -1;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    long deadline = tv.tv_sec * 1000 + tv.tv_usec / 1000 + HTTP_TIMEOUT_MS;

    int got_any = 0, quiet = 0;
    for (;;)
    {
        gettimeofday(&tv, NULL);
        if (tv.tv_sec * 1000 + tv.tv_usec / 1000 > deadline)
            break;

        UInt32 got = (UInt32)(cap - len);
        if (got == 0)
        {
            cap *= 2;
            unsigned char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
            got = (UInt32)(cap - len);
        }
        rc = (*c->iface)->ReadPipeTO(c->iface, c->in_pipe, buf + len, &got,
            800, 800);
        if (rc != kIOReturnSuccess)
        {
            if (got_any)
                break;      /* got the response, pipe errored afterwards */
            usleep(200000);
            continue;
        }
        if (got > 0)
        {
            len += got;
            got_any = 1;
            quiet = 0;
            continue;
        }
        /* empty read */
        if (got_any && ++quiet >= 2)
            break;
        usleep(150000);
    }
    *out = buf;
    *outlen = len;
    return got_any ? 0 : -1;
}

/* Strip HTTP chunked-encoding framing: returns malloc'd body or NULL. */
static unsigned char *dechunk(const unsigned char *raw, size_t len,
                              size_t *bodylen)
{
    unsigned char *body = malloc(len + 1);
    if (!body) return NULL;

    /* Find \r\n\r\n after the status line */
    const unsigned char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < len; ++i)
        if (raw[i] == '\r' && raw[i+1] == '\n' && raw[i+2] == '\r' &&
            raw[i+3] == '\n')
        {
            hdr_end = raw + i + 4;
            break;
        }
    if (!hdr_end)
    {
        memcpy(body, raw, len);
        *bodylen = len;
        body[len] = 0;
        return body;
    }

    size_t w = 0;
    const unsigned char *p = hdr_end;
    const unsigned char *end = raw + len;
    for (;;)
    {
        while (p < end && (*p == '\r' || *p == '\n'))
            ++p;
        if (p >= end) break;
        size_t sz = 0;
        while (p < end && *p != '\r' && *p != '\n')
        {
            int c = *p++;
            if (c >= '0' && c <= '9') sz = sz * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') sz = sz * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') sz = sz * 16 + (c - 'A' + 10);
            else break;
        }
        while (p < end && (*p == '\r' || *p == '\n'))
            ++p;
        if (sz == 0) break;
        if (p + sz > end) sz = (size_t)(end - p);
        memcpy(body + w, p, sz);
        w += sz;
        p += sz;
    }
    body[w] = 0;
    *bodylen = w;
    return body;
}

static char *xml_field(const char *xml, const char *tag)
{
    char open[128], close[128];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *s = strstr(xml, open);
    if (!s) return NULL;
    s += strlen(open);
    const char *e = strstr(s, close);
    if (!e) return NULL;
    char *val = malloc((size_t)(e - s) + 1);
    if (!val) return NULL;
    memcpy(val, s, (size_t)(e - s));
    val[e - s] = 0;
    /* trim */
    char *b = val, *t = val + strlen(val);
    while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') ++b;
    while (t > b && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r' ||
                     t[-1] == '\n')) --t;
    *t = 0;
    if (!*b) { free(val); return NULL; }
    memmove(val, b, strlen(b) + 1);
    return val;
}

int main(int argc, char **argv)
{
    int want_json = 0;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--json") == 0)
            want_json = 1;

    ews_conn_t conn = {0};
    if (ews_open(&conn) != 0)
    {
        fprintf(stderr, "p1102status: printer not found or EWS interface "
                        "busy\n");
        return 1;
    }

    unsigned char *raw1 = NULL, *raw2 = NULL;
    size_t len1 = 0, len2 = 0;
    ews_get(&conn, "/DevMgmt/ProductStatusDyn.xml", &raw1, &len1);
    ews_get(&conn, "/DevMgmt/ConsumableConfigDyn.xml", &raw2, &len2);
    ews_close(&conn);

    char *status = NULL, *level = NULL, *cstate = NULL;
    char *cname = NULL, *brand = NULL, *serial = NULL;
    if (raw1)
    {
        size_t bl;
        unsigned char *body = dechunk(raw1, len1, &bl);
        if (getenv("P1102STATUS_DEBUG"))
        {
            FILE *df = fopen("/tmp/p1102_status1.xml", "w");
            if (df) { fwrite(body, 1, bl, df); fclose(df); }
        }
        if (body)
            status = xml_field((char *)body, "pscat:StatusCategory");
        free(body);
    }
    if (raw2)
    {
        size_t bl;
        unsigned char *body = dechunk(raw2, len2, &bl);
        if (getenv("P1102STATUS_DEBUG"))
        {
            FILE *df = fopen("/tmp/p1102_status2.xml", "w");
            if (df) { fwrite(body, 1, bl, df); fclose(df); }
        }
        if (body)
        {
            level = xml_field((char *)body, "dd:ConsumablePercentageLevelRemaining");
            cstate = xml_field((char *)body, "dd:ConsumableState");
            cname = xml_field((char *)body, "dd:ConsumableFamilyName");
            brand = xml_field((char *)body, "dd:Brand");
            serial = xml_field((char *)body, "dd:SerialNumber");
        }
        free(body);
    }

    free(raw1);
    free(raw2);

    if (!status && !cname)
    {
        fprintf(stderr, "p1102status: no response from the printer\n");
        return 1;
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
        printf("  \"toner_percent\": %s\n",
               level && *level ? level : "null");
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
            printf("Toner level: unknown (cartridge has no memory chip)\n");
    }

    free(status);
    free(level);
    free(cstate);
    free(cname);
    free(brand);
    free(serial);
    return 0;
}

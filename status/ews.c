/*
 * ews.c - HP LaserJet Professional P1102 EWS-over-USB transport.
 *
 * The printer exposes a tiny embedded web server on a vendor-specific USB
 * interface (class 255, subclass 2, protocol 16 = FF/02/10) that speaks
 * plain HTTP over its bulk endpoints.  This is the transport hplip calls
 * the "Marvell EWS" channel and is the same mechanism HP's usbink utility
 * uses for toner/status queries.
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
#include "ews.h"

#define VID 0x03f0
#define PID 0x002a
#define IFACE_CLASS_EWS 255
#define HTTP_TIMEOUT_MS 2000

static io_service_t ews_find_interface(void)
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
        if (v == VID && p == PID && icls == IFACE_CLASS_EWS)
        {
            intf = s;
            break;
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(iter);
    return intf;
}

int ews_open(ews_conn_t *c)
{
    io_service_t intf = ews_find_interface();
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

void ews_close(ews_conn_t *c)
{
    if (!c->iface) return;
    (*c->iface)->USBInterfaceClose(c->iface);
    (*c->iface)->Release(c->iface);
    c->iface = NULL;
}

int ews_send(ews_conn_t *c, const void *req, size_t reqlen,
            unsigned char **out, size_t *outlen)
{
    IOReturn rc = (*c->iface)->WritePipeTO(c->iface, c->out_pipe,
        (void *)req, (UInt32)reqlen, 1000, 1000);
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
                break;
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
        if (got_any && ++quiet >= 2)
            break;
        usleep(150000);
    }
    *out = buf;
    *outlen = len;
    return got_any ? 0 : -1;
}

int ews_get(ews_conn_t *c, const char *path, unsigned char **out,
            size_t *outlen)
{
    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: localhost\r\nUser-Agent: p1102-status\r\n"
        "Connection: close\r\n\r\n", path);
    return ews_send(c, req, (size_t)rlen, out, outlen);
}

unsigned char *ews_dechunk(const unsigned char *raw, size_t len,
                           size_t *bodylen)
{
    unsigned char *body = malloc(len + 1);
    if (!body) return NULL;

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

char *ews_xml_field_n(const char *xml, const char *tag, int n)
{
    char open[128], close[128];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *s = xml;
    while (n-- > 0)
    {
        s = strstr(s, open);
        if (!s) return NULL;
        s += strlen(open);
    }
    s = strstr(s, open);
    if (!s) return NULL;
    s += strlen(open);
    const char *e = strstr(s, close);
    if (!e) return NULL;
    char *val = malloc((size_t)(e - s) + 1);
    if (!val) return NULL;
    memcpy(val, s, (size_t)(e - s));
    val[e - s] = 0;
    char *b = val, *t = val + strlen(val);
    while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') ++b;
    while (t > b && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r' ||
                     t[-1] == '\n')) --t;
    *t = 0;
    if (!*b) { free(val); return NULL; }
    memmove(val, b, strlen(b) + 1);
    return val;
}

char *ews_xml_field(const char *xml, const char *tag)
{
    return ews_xml_field_n(xml, tag, 0);
}

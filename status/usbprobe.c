/*
 * usbprobe - send a PJL query to the HP LaserJet P1102 over USB and dump
 * the response.  Used to determine the toner status protocol.
 *
 * Matches the printer-class interface service directly (like Apple's own
 * CUPS usb backend) and uses the classic IOUSBInterfaceInterface245 API.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#define VID 0x03f0
#define PID 0x002a

static void dump(const unsigned char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i += 16)
    {
        size_t j;
        printf("%04zx: ", i);
        for (j = 0; j < 16 && i + j < n; ++j)
            printf("%02x ", b[i + j]);
        printf("  ");
        for (j = 0; j < 16 && i + j < n; ++j)
        {
            unsigned char c = b[i + j];
            putchar(c >= 32 && c < 127 ? c : '.');
        }
        putchar('\n');
    }
}

int main(int argc, char **argv)
{
    const char *query = "\x1b%-12345X@PJL INFO CONSUMABLES\r\n\x1b%-12345X";
    int use_ews = 0;
    if (argc > 1 && strcmp(argv[1], "levels") == 0)
        query = "\x1b%-12345X@PJL INFO LEVELS\r\n\x1b%-12345X";
    else if (argc > 1 && strcmp(argv[1], "status") == 0)
        query = "\x1b%-12345X@PJL INFO STATUS\r\n\x1b%-12345X";
    else if (argc > 1 && strcmp(argv[1], "ews") == 0)
    {
        use_ews = 1;
        query =
            "GET /DevMgmt/ProductStatusDyn.xml HTTP/1.1\r\n"
            "Host: localhost\r\nUser-Agent: p1102-status\r\n"
            "Connection: close\r\n\r\n";
    }

    /* Property matching does not work against IOUSBHost nubs on recent
     * macOS, so enumerate the class and filter by properties manually. */
    io_service_t intf = 0;
    int vid = VID, pid = PID;

    {
        CFMutableDictionaryRef matching = IOServiceMatching("IOUSBHostInterface");
        io_iterator_t iter = 0;
        IOReturn kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter);
        if (kr == kIOReturnSuccess && iter)
        {
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
                if (v == vid && p == pid &&
                        (use_ews ? (icls == 255) : (icls == 7)))
                {
                    intf = s;
                    break;
                }
                IOObjectRelease(s);
            }
            IOObjectRelease(iter);
        }
    }
    if (!intf)
    { fprintf(stderr, "P1102 interface not found\n"); return 1; }

    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    IOReturn kr = IOCreatePlugInInterfaceForService(intf, kIOUSBInterfaceUserClientTypeID,
        kIOCFPlugInInterfaceID, (IOCFPlugInInterface ***)&plug, &score);
    IOObjectRelease(intf);
    if (kr != kIOReturnSuccess || !plug)
    { fprintf(stderr, "plugin failed %x\n", kr); return 1; }

    IOUSBInterfaceInterface245 **iface = NULL;
    HRESULT res = (*plug)->QueryInterface(plug,
        CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID245), (LPVOID *)&iface);
    IODestroyPlugInInterface(plug);
    if (res != 0 || !iface)
    { fprintf(stderr, "QueryInterface failed %x\n", res); return 1; }

    IOReturn rc = (*iface)->USBInterfaceOpen(iface);
    fprintf(stderr, "USBInterfaceOpen: %x\n", rc);
    if (rc != kIOReturnSuccess && rc != kIOReturnExclusiveAccess)
        return 1;

    UInt8 nPipes = 0, inPipe = 0, outPipe = 0;
    (*iface)->GetNumEndpoints(iface, &nPipes);
    fprintf(stderr, "endpoints: %d\n", nPipes);
    for (UInt8 p = 1; p <= nPipes; ++p)
    {
        UInt8 dir, num, trType, interval;
        UInt16 maxPacket;
        rc = (*iface)->GetPipeProperties(iface, p, &dir, &num, &trType,
            &maxPacket, &interval);
        if (rc) continue;
        fprintf(stderr, "pipe %d dir=%s num=%d trType=%d maxPkt=%d\n",
            p, dir == kUSBIn ? "IN" : "OUT", num, trType, maxPacket);
        if (trType == kUSBBulk && dir == kUSBOut)
            outPipe = p;
        if (trType == kUSBBulk && dir == kUSBIn && !inPipe)
            inPipe = p;
        if (trType == kUSBInterrupt && dir == kUSBIn && !inPipe)
            inPipe = p;
    }

    if (!outPipe)
    { fprintf(stderr, "no bulk OUT pipe\n"); return 1; }

    rc = (*iface)->WritePipe(iface, outPipe, (void *)query,
        (UInt32)strlen(query));
    fprintf(stderr, "WritePipe(%zu bytes): %x\n", strlen(query), rc);
    usleep(300000);

    if (inPipe)
    {
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            unsigned char buf[8192];
            UInt32 got = sizeof(buf);
            memset(buf, 0, sizeof(buf));
            rc = (*iface)->ReadPipe(iface, inPipe, buf, &got);
            fprintf(stderr, "ReadPipe: %x got %u bytes\n", rc, got);
            if (rc == kIOReturnSuccess && got > 0)
            {
                dump(buf, got);
                if (got < sizeof(buf))
                    break;
                continue;
            }
            if (rc != kIOReturnSuccess && got == 0)
                break;
            usleep(300000);
        }
    }

    (*iface)->USBInterfaceClose(iface);
    (*iface)->Release(iface);
    return 0;
}

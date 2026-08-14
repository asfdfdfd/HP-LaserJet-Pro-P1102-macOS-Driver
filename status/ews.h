/* ews.h - HP P1102 embedded web server (EWS) over USB transport. */
#ifndef EWS_H
#define EWS_H

#include <stddef.h>
#include <IOKit/usb/IOUSBLib.h>

typedef struct {
    IOUSBInterfaceInterface500 **iface;
    UInt8 out_pipe;
    UInt8 in_pipe;
} ews_conn_t;

/* Open the vendor interface (FF/02/10) of the P1102.  0 on success. */
int ews_open(ews_conn_t *c);

void ews_close(ews_conn_t *c);

/* HTTP GET over the bulk pipes.  Returns 0 and allocates *out on success. */
int ews_get(ews_conn_t *c, const char *path, unsigned char **out,
            size_t *outlen);

/* Send a raw HTTP request over the bulk pipes.  Returns 0 and allocates
 * *out with the full response (headers + body) on success. */
int ews_send(ews_conn_t *c, const void *req, size_t reqlen,
             unsigned char **out, size_t *outlen);

/* Strip HTTP chunked framing; returns malloc'd body (NUL-terminated). */
unsigned char *ews_dechunk(const unsigned char *raw, size_t len,
                           size_t *bodylen);

/* Extract the text of <tag>...</tag> (trimmed); malloc'd or NULL. */
char *ews_xml_field(const char *xml, const char *tag);

/* Same, but the n-th occurrence (0-based). */
char *ews_xml_field_n(const char *xml, const char *tag, int n);

#endif /* EWS_H */

/*
 * p1102cmd - submit a CUPS command (application/vnd.cups-command) to the
 * HP LaserJet Professional P1102 queue, e.g.:
 *
 *   p1102cmd [queue] [command]
 *
 * Uses the official CUPS client API (Print-Job with the command document),
 * so authentication and job handling are done by libcups.
 *
 * GPL-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <cups/cups.h>
#include <cups/ipp.h>

int main(int argc, char **argv)
{
    const char *queue = argc > 1 ? argv[1] : "HP_P1102";
    const char *command = argc > 2 ? argv[2] : "PrintSelfTestPage";
    char doc[512];
    int dlen;
    char fname[] = "/tmp/p1102cmd-XXXXXX";
    int fd;
    http_t *http;
    ipp_t *request, *response;
    ipp_status_t status;
    char uri[512], resource[512];

    dlen = snprintf(doc, sizeof(doc), "#CUPS-COMMAND\n%s\n", command);
    if (dlen <= 0 || dlen >= (int)sizeof(doc))
    {
        fprintf(stderr, "p1102cmd: command too long\n");
        return 1;
    }

    fd = mkstemp(fname);
    if (fd < 0)
    {
        perror("p1102cmd: mkstemp");
        return 1;
    }
    if (write(fd, doc, (size_t)dlen) != dlen)
    {
        perror("p1102cmd: write");
        close(fd);
        unlink(fname);
        return 1;
    }
    close(fd);

    int cancel = 0;
    http = httpConnect2("localhost", 631, NULL, AF_UNSPEC,
                        HTTP_ENCRYPTION_IF_REQUESTED, 1, 10000, &cancel);
    if (!http)
    {
        fprintf(stderr, "p1102cmd: cannot connect to CUPS: %s\n",
                cupsLastErrorString());
        unlink(fname);
        return 1;
    }

    snprintf(uri, sizeof(uri), "ipp://localhost/printers/%s", queue);
    snprintf(resource, sizeof(resource), "/printers/%s", queue);

    request = ippNewRequest(IPP_OP_PRINT_JOB);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI,
                 "printer-uri", NULL, uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE,
                 "document-format", NULL, "application/vnd.cups-command");
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                 "requesting-user-name", NULL, cupsUser());
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                 "job-name", NULL, command);

    response = cupsDoFileRequest(http, request, resource, fname);
    status = response ? ippGetStatusCode(response) : IPP_STATUS_ERROR_INTERNAL;

    if (status >= IPP_STATUS_REDIRECTION_OTHER_SITE)
    {
        fprintf(stderr, "p1102cmd: %s failed: %s\n", command,
                ippErrorString(status));
    }
    else
    {
        printf("Sent %s to %s\n", command, queue);
    }

    if (response)
        ippDelete(response);
    /* cupsDoFileRequest takes ownership of and frees "request". */
    httpClose(http);
    unlink(fname);

    return status >= IPP_STATUS_REDIRECTION_OTHER_SITE ? 1 : 0;
}

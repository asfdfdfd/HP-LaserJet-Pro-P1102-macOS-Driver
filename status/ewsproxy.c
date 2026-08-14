/*
 * ewsproxy - local HTTP proxy that tunnels requests to the HP LaserJet
 * Professional P1102's embedded web server (EWS) over USB.
 *
 * The equivalent of HP's "HtmlConfig" utility: it listens on localhost and
 * forwards every HTTP request through the printer's vendor-specific USB
 * interface (FF/02/10), so the printer's own web pages (settings, page
 * counters, self-test, etc.) can be browsed in Safari/Chrome.
 *
 * Usage:  ewsproxy [path]      (default path: /SSI/index.htm)
 *
 * GPL-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ews.h"

#define MAX_REQ 65536

static pthread_mutex_t ews_mutex = PTHREAD_MUTEX_INITIALIZER;

static void handle_client(int fd)
{
    unsigned char *req = malloc(MAX_REQ);
    if (!req) { close(fd); return; }

    size_t len = 0;
    for (;;)
    {
        ssize_t n = read(fd, req + len, MAX_REQ - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
        if (len >= MAX_REQ - 1) break;
        /* Headers end; body follows only if Content-Length says so. */
        if (len >= 4 && memmem(req, len, "\r\n\r\n", 4))
        {
            /* check Content-Length */
            char buf[64] = {0};
            size_t have = len;
            for (size_t i = 0; i + 16 < have; ++i)
                if (strncasecmp((char *)req + i, "content-length:", 15) == 0)
                {
                    size_t j = i + 15;
                    size_t k = 0;
                    while (j < have && req[j] != '\r' && req[j] != '\n' &&
                           k < sizeof(buf) - 1)
                        buf[k++] = (char)req[j++];
                    break;
                }
            long cl = atol(buf);
            if (cl <= 0)
                break;
            if (have >= (size_t)cl + 4)
                break;
        }
    }

    if (len > 0)
    {
        pthread_mutex_lock(&ews_mutex);
        unsigned char *resp = NULL;
        size_t rlen = 0;
        ews_conn_t conn = {0};
        if (ews_open(&conn) == 0)
        {
            ews_send(&conn, req, len, &resp, &rlen);
            ews_close(&conn);
        }
        pthread_mutex_unlock(&ews_mutex);

        if (resp)
        {
            write(fd, resp, rlen);
            free(resp);
        }
        else
        {
            const char *err = "HTTP/1.1 502 Bad Gateway\r\n"
                "Content-Type: text/plain\r\nContent-Length: 25\r\n\r\n"
                "printer not available\r\n";
            write(fd, err, strlen(err));
        }
    }
    close(fd);
    free(req);
}

static void *client_thread(void *arg)
{
    handle_client((int)(intptr_t)arg);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/SSI/index.htm";
    if (path[0] != '/')
        path = "/SSI/index.htm";

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    { perror("bind"); return 1; }
    socklen_t alen = sizeof(addr);
    if (getsockname(srv, (struct sockaddr *)&addr, &alen) < 0)
    { perror("getsockname"); return 1; }
    if (listen(srv, 8) < 0)
    { perror("listen"); return 1; }

    int port = ntohs(addr.sin_port);
    printf("P1102 EWS proxy on http://localhost:%d%s\n", port, path);
    fflush(stdout);

    char url[256];
    snprintf(url, sizeof(url), "http://localhost:%d%s", port, path);
    if (fork() == 0)
    {
        execlp("open", "open", url, NULL);
        exit(0);
    }

    for (;;)
    {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        pthread_t th;
        pthread_create(&th, NULL, client_thread,
                       (void *)(intptr_t)fd);
        pthread_detach(th);
    }
    return 0;
}

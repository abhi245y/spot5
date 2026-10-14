// spot5 - Pairing helper payload: advertises the app and answers the phone.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Apps may not bind UDP 5353, and a media app may not listen on any port
// (both EACCES); payloads may. The app sends this file to elfldr
// (127.0.0.1:9021) when pairing starts, then connects to it on the lock port
// (127.0.0.1:7865), the control link:
//   helper -> app   "READY\n"
//   app -> helper   "INFO <length>\n" + the GET /spotify_info JSON
//   helper -> app   "LOGIN <length>\n" + each POST body from a phone
// The helper serves /spotify_info on 7864 with that JSON, advertises
// _spotify-connect._tcp under its "remoteName" with bell's responder
// (mdnssvc), and exits when the control link closes (pairing over, or the
// app closed).
//
// One copy at a time: a second copy finds the lock port taken and exits.

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ps5/klog.h>

#include "mdnssvc.h"

#define HTTP_PORT 7864 // the zeroconf endpoint the phone talks to
#define LOCK_PORT 7865 // held while a helper runs; also the control link
#define CONTROL_WAIT_SECONDS 10
#define MAX_REQUEST 32768 // a real login POST is about 1 KB

// Spotify's "OK, logging in" reply to the phone's POST.
static const char POST_REPLY[] = "{\"status\":101,\"spotifyError\":0,\"statusString\":\"ERROR-OK\"}";

static void set_timeouts(int fd, int seconds)
{
    struct timeval timeout = {seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static int send_all(int fd, const char *data, size_t size)
{
    while (size > 0)
    {
        const ssize_t sent = send(fd, data, size, 0);
        if (sent <= 0)
            return -1;
        data += sent;
        size -= (size_t)sent;
    }
    return 0;
}

static int recv_all(int fd, char *data, size_t size)
{
    while (size > 0)
    {
        const ssize_t got = recv(fd, data, size, 0);
        if (got <= 0)
            return -1;
        data += got;
        size -= (size_t)got;
    }
    return 0;
}

// Reads "INFO <length>\n<json>" from the app into a new string (free it).
static char *read_info(int control)
{
    char line[32];
    size_t used = 0;
    while (used < sizeof(line) - 1 && recv(control, line + used, 1, 0) == 1 && line[used] != '\n')
        ++used;
    line[used] = 0;
    long length = strncmp(line, "INFO ", 5) == 0 ? strtol(line + 5, NULL, 10) : -1;
    if (length <= 0 || length > MAX_REQUEST)
        return NULL;
    char *info = malloc((size_t)length + 1);
    if (!info || recv_all(control, info, (size_t)length) != 0)
    {
        free(info);
        return NULL;
    }
    info[length] = 0;
    return info;
}

// Copies the JSON's "remoteName" into name. Returns 0 when there is one.
static int device_name(const char *info, char *name, size_t size)
{
    // ponytail: the JSON is the app's own; only \" and \\ escapes are undone
    // (names come from the kit keyboard, no control characters).
    const char *key = strstr(info, "\"remoteName\":\"");
    if (!key)
        return -1;
    const char *from = key + strlen("\"remoteName\":\"");
    size_t out = 0;
    for (; *from && *from != '"' && out < size - 1; ++from)
    {
        if (*from == '\\' && from[1])
            ++from;
        name[out++] = *from;
    }
    name[out] = 0;
    return out ? 0 : -1;
}

static void reply(int fd, int status, const char *body)
{
    char head[160];
    const int length = snprintf(head, sizeof(head),
                                "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                                "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                status, status == 200 ? "OK" : status == 400 ? "Bad Request" : "Not Found",
                                strlen(body));
    if (send_all(fd, head, (size_t)length) == 0)
        send_all(fd, body, strlen(body));
}

// One request from a phone: GET returns the app's info, POST hands the body
// (the encrypted login, form-encoded) to the app. Returns -1 if the control
// link failed.
static int serve(int client, const char *info, int control)
{
    static char request[MAX_REQUEST + 1];
    size_t used = 0;
    char *body = NULL;
    while (used < MAX_REQUEST && !body)
    {
        const ssize_t got = recv(client, request + used, MAX_REQUEST - used, 0);
        if (got <= 0)
            return 0;
        used += (size_t)got;
        request[used] = 0;
        char *end = strstr(request, "\r\n\r\n");
        if (end)
            body = end + 4;
    }
    if (!body)
        return 0;
    long length = 0;
    for (char *line = strstr(request, "\r\n"); line && line < body; line = strstr(line + 2, "\r\n"))
        if (strncasecmp(line + 2, "Content-Length:", 15) == 0)
            length = strtol(line + 17, NULL, 10);
    const size_t head = (size_t)(body - request);
    if (length < 0 || head + (size_t)length > MAX_REQUEST)
    {
        reply(client, 400, "{\"status\":102}");
        return 0;
    }
    while (used < head + (size_t)length)
    {
        const ssize_t got = recv(client, request + used, head + (size_t)length - used, 0);
        if (got <= 0)
            return 0;
        used += (size_t)got;
    }

    const int is_get = strncmp(request, "GET /spotify_info", 17) == 0;
    const int is_post = strncmp(request, "POST /spotify_info", 18) == 0;
    if (is_get)
        reply(client, 200, info);
    else if (is_post && length > 0)
    {
        char line[32];
        const int size = snprintf(line, sizeof(line), "LOGIN %ld\n", length);
        if (send_all(control, line, (size_t)size) != 0 || send_all(control, body, (size_t)length) != 0)
            return -1;
        klog_printf("[MDNS] login received from a phone; passed to the app\n");
        reply(client, 200, POST_REPLY);
    }
    else
        reply(client, is_post ? 400 : 404, "{\"status\":102}");
    return 0;
}

static int listen_on(in_addr_t address, int port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in where = {0};
    where.sin_len = sizeof(where);
    where.sin_family = AF_INET;
    where.sin_port = htons(port);
    where.sin_addr.s_addr = htonl(address);
    const int on = 1;
    if (fd >= 0)
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (fd < 0 || bind(fd, (struct sockaddr *)&where, sizeof(where)) != 0 || listen(fd, backlog) != 0)
    {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
}

// Waits up to seconds for a connection on fd; returns it or -1.
static int accept_within(int fd, int seconds)
{
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(fd, &ready);
    struct timeval timeout = {seconds, 0};
    if (select(fd + 1, &ready, NULL, NULL, &timeout) <= 0)
        return -1;
    return accept(fd, NULL, NULL);
}

// The first interface that is up, multicast and not loopback.
static struct in_addr local_address(void)
{
    struct in_addr found = {0};
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0)
        return found;
    for (struct ifaddrs *i = list; i; i = i->ifa_next)
    {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) &&
            (i->ifa_flags & IFF_MULTICAST) && !(i->ifa_flags & IFF_LOOPBACK))
        {
            found = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

int main(void)
{
    // elfldr points stdout at the app's socket, which the app closes.
    signal(SIGPIPE, SIG_IGN);

    const int lock = listen_on(INADDR_LOOPBACK, LOCK_PORT, 1);
    if (lock < 0)
    {
        klog_printf("[MDNS] helper already running\n");
        return 0;
    }
    const int control = accept_within(lock, CONTROL_WAIT_SECONDS);
    char *info = NULL;
    char name[128];
    if (control >= 0)
    {
        set_timeouts(control, 3);
        if (send_all(control, "READY\n", 6) == 0)
            info = read_info(control);
    }
    const int http = info && device_name(info, name, sizeof(name)) == 0
                         ? listen_on(INADDR_ANY, HTTP_PORT, 8)
                         : -1;
    const struct in_addr host = local_address();
    struct mdnsd *responder = http >= 0 && host.s_addr ? mdnsd_start(host, false) : NULL;
    if (!responder)
    {
        klog_printf("[MDNS] could not start: control %d, info %s, http %d, address %s\n", control,
                    info ? "yes" : "no", http, inet_ntoa(host));
        if (http >= 0)
            close(http);
        if (control >= 0)
            close(control);
        close(lock);
        free(info);
        return 1;
    }
    // Own host name, so it never clashes with the console's own responder.
    mdnsd_set_hostname(responder, "spot5.local", host);
    const char *txt[] = {"VERSION=1.0", "CPath=/spotify_info", "Stack=SP", NULL};
    struct mdns_service *service =
        mdnsd_register_svc(responder, name, "_spotify-connect._tcp.local", HTTP_PORT, NULL, txt);
    klog_printf("[MDNS] advertising \"%s\" on %s:%d\n", name, inet_ntoa(host), HTTP_PORT);

    for (;;)
    {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(control, &ready);
        FD_SET(http, &ready);
        if (select((control > http ? control : http) + 1, &ready, NULL, NULL, NULL) < 0)
            break;
        if (FD_ISSET(control, &ready))
        {
            char ignored[64];
            if (recv(control, ignored, sizeof(ignored), MSG_DONTWAIT) <= 0)
                break; // the app ended pairing or closed
        }
        if (FD_ISSET(http, &ready))
        {
            const int client = accept(http, NULL, NULL);
            if (client < 0)
                continue;
            set_timeouts(client, 3);
            const int failed = serve(client, info, control);
            close(client);
            if (failed)
                break;
        }
    }
    // Sockets first, so a new helper can start at once.
    close(http);
    close(control);
    close(lock);
    mdns_service_remove(responder, service);
    mdnsd_stop(responder);
    free(info);
    klog_printf("[MDNS] pairing over; helper exiting\n");
    return 0;
}

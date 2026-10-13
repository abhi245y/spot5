// ps5-spotify - mDNS helper payload: advertises the app while it pairs.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Apps may not bind UDP 5353 (EACCES), payloads may. The app sends this file
// to elfldr (127.0.0.1:9021) when pairing starts. It reads the device name
// from the app's own zeroconf endpoint, advertises _spotify-connect._tcp on
// that port with bell's built-in responder (mdnssvc), and exits once the
// endpoint stops answering (pairing over, or the app closed).
//
// One copy at a time: a second copy finds the lock port taken and exits.

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ps5/klog.h>

#include "mdnssvc.h"

#define APP_PORT 7864  // the app's zeroconf web server (spotify/pairing.cpp)
#define LOCK_PORT 7865 // held while a helper runs
#define POLL_SECONDS 2
#define MISSES_TO_EXIT 2 // one restart of the app's server is not the end

// Reads GET /spotify_info from the app and copies "remoteName" into name.
// Returns 0 when the app answered with a name.
static int app_device_name(char *name, size_t size)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in app = {0};
    app.sin_len = sizeof(app);
    app.sin_family = AF_INET;
    app.sin_port = htons(APP_PORT);
    app.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timeval timeout = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&app, sizeof(app)) != 0)
    {
        close(fd);
        return -1;
    }
    static const char request[] = "GET /spotify_info HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    char reply[4096];
    size_t used = 0;
    ssize_t got = send(fd, request, sizeof(request) - 1, 0) < 0 ? -1 : 1;
    while (got > 0 && used < sizeof(reply) - 1)
    {
        got = recv(fd, reply + used, sizeof(reply) - 1 - used, 0);
        if (got > 0)
            used += (size_t)got;
    }
    close(fd);
    reply[used] = 0;

    // ponytail: the reply is the app's own JSON; only \" and \\ escapes are
    // undone (names come from the kit keyboard, no control characters).
    const char *key = strstr(reply, "\"remoteName\":\"");
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

static int take_lock(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in lock = {0};
    lock.sin_len = sizeof(lock);
    lock.sin_family = AF_INET;
    lock.sin_port = htons(LOCK_PORT);
    lock.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || bind(fd, (struct sockaddr *)&lock, sizeof(lock)) != 0 || listen(fd, 1) != 0)
    {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
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

    const int lock = take_lock();
    if (lock < 0)
    {
        klog_printf("[MDNS] helper already running\n");
        return 0;
    }
    char name[128];
    if (app_device_name(name, sizeof(name)) != 0)
    {
        klog_printf("[MDNS] app not answering on port %d; nothing to advertise\n", APP_PORT);
        close(lock);
        return 1;
    }
    const struct in_addr host = local_address();
    struct mdnsd *responder = host.s_addr ? mdnsd_start(host, false) : NULL;
    if (!responder)
    {
        klog_printf("[MDNS] could not start the responder on %s\n", inet_ntoa(host));
        close(lock);
        return 1;
    }
    // Own host name, so it never clashes with the console's own responder.
    mdnsd_set_hostname(responder, "ps5-spotify.local", host);
    const char *txt[] = {"VERSION=1.0", "CPath=/spotify_info", "Stack=SP", NULL};
    struct mdns_service *service =
        mdnsd_register_svc(responder, name, "_spotify-connect._tcp.local", APP_PORT, NULL, txt);
    klog_printf("[MDNS] advertising \"%s\" on %s:%d\n", name, inet_ntoa(host), APP_PORT);

    char now[128];
    for (int misses = 0; misses < MISSES_TO_EXIT;)
    {
        sleep(POLL_SECONDS);
        if (app_device_name(now, sizeof(now)) != 0)
        {
            ++misses;
            continue;
        }
        misses = 0;
        if (strcmp(now, name) != 0) // renamed while pairing
        {
            mdns_service_remove(responder, service);
            strcpy(name, now);
            service = mdnsd_register_svc(responder, name, "_spotify-connect._tcp.local",
                                         APP_PORT, NULL, txt);
            klog_printf("[MDNS] now advertising \"%s\"\n", name);
        }
    }
    mdns_service_remove(responder, service);
    mdnsd_stop(responder);
    close(lock);
    klog_printf("[MDNS] pairing over; helper exiting\n");
    return 0;
}

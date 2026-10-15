/*
 * xemu XB Online connection check
 *
 * Looks at the things that stop Rainbow Six 3: Black Arrow from reaching
 * Xbox Live on another PC ("Xbox Live not found"): the console network, the
 * XB Online DNS server, the PC clock, the game disc, the title update and the
 * machine account. Each result is shown on screen and written to xemu.log.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/sockets.h"
#include "qemu/thread.h"
#include "xemu-settings.h"
#include "xemu-net.h"
#include "xemu-diag.h"
#include "xemu-xbonline.h"
#include "xemu-xbcheck.h"
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <iphlpapi.h>
#endif

/* A clock further off than this breaks the Kerberos sign-in */
#define CLOCK_LIMIT_SECONDS 120

/* Seconds between 1900 (NTP) and 1970 (Unix) */
#define NTP_UNIX_OFFSET 2208988800ULL

static QemuMutex lock;
static bool lock_ready;
static bool running;
static int port_busy;
static XbCheckItem items[XBCHECK_MAX_ITEMS];
static int item_count;

static void check_init(void)
{
    if (!lock_ready) {
        qemu_mutex_init(&lock);
        lock_ready = true;
    }
}

static int item_add(const char *name)
{
    int i;

    qemu_mutex_lock(&lock);
    i = item_count++;
    items[i].name = name;
    items[i].state = XBCHECK_RUNNING;
    items[i].action = XBCHECK_ACTION_NONE;
    snprintf(items[i].detail, sizeof(items[i].detail), "Checking...");
    qemu_mutex_unlock(&lock);
    return i;
}

static void G_GNUC_PRINTF(3, 4)
item_set(int i, XbCheckState state, const char *fmt, ...)
{
    static const char *const names[] = {
        [XBCHECK_RUNNING] = "...", [XBCHECK_OK] = "ok",
        [XBCHECK_INFO] = "info",   [XBCHECK_WARN] = "WARNING",
        [XBCHECK_FAIL] = "PROBLEM",
    };
    va_list ap;

    qemu_mutex_lock(&lock);
    va_start(ap, fmt);
    vsnprintf(items[i].detail, sizeof(items[i].detail), fmt, ap);
    va_end(ap);
    items[i].state = state;
    fprintf(stderr, "xbcheck: %s: %s: %s\n", items[i].name, names[state],
            items[i].detail);
    qemu_mutex_unlock(&lock);
}

/* UDP socket connected to host:port with a receive timeout, or -1 */
static int udp_open(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    char service[8];
    int fd = -1;

    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) {
        return -1;
    }
    fd = qemu_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
#ifdef _WIN32
        DWORD tv = timeout_ms;
#else
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
#endif
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const void *)&tv, sizeof(tv));
        if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    return fd;
}

/*
 * Ask a DNS server for the address of a name. Returns 1 with the first
 * address, 0 if the server answered without one, -1 if it did not answer.
 */
static int dns_lookup(const char *server, const char *name, uint32_t *addr)
{
    uint8_t q[512], r[512];
    int len = 12, result = -1;
    uint16_t id = g_random_int() & 0xffff;
    int fd = udp_open(server, 53, 3000);

    if (fd < 0) {
        return -1;
    }

    memset(q, 0, 12);
    stw_be_p(q, id);
    q[2] = 0x01;        /* recursion desired */
    stw_be_p(q + 4, 1); /* one question */
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        q[len++] = n;
        memcpy(q + len, p, n);
        len += n;
        p += n + (dot ? 1 : 0);
    }
    q[len++] = 0;
    stw_be_p(q + len, 1); /* A */
    stw_be_p(q + len + 2, 1); /* IN */
    len += 4;

    for (int attempt = 0; attempt < 2 && result < 0; attempt++) {
        ssize_t n;
        if (send(fd, q, len, 0) != len) {
            continue;
        }
        while ((n = recv(fd, r, sizeof(r), 0)) > 0) {
            int o = len, answers;
            if (n < 12 || lduw_be_p(r) != id) {
                continue; /* a late answer to an earlier attempt */
            }
            result = 0;
            answers = lduw_be_p(r + 6);
            /* Answers follow the question, which the server repeats as is */
            while (answers-- > 0 && o + 12 <= n) {
                uint16_t type, rdlen;
                if ((r[o] & 0xc0) == 0xc0) {
                    o += 2;
                } else {
                    while (o < n && r[o]) {
                        o += r[o] + 1;
                    }
                    o++;
                }
                if (o + 10 > n) {
                    break;
                }
                type = lduw_be_p(r + o);
                rdlen = lduw_be_p(r + o + 8);
                o += 10;
                if (type == 1 && rdlen == 4 && o + 4 <= n) {
                    *addr = (uint32_t)ldl_be_p(r + o);
                    result = 1;
                    break;
                }
                o += rdlen;
            }
            break;
        }
    }
    close(fd);
    return result;
}

static void check_network(void)
{
    int i = item_add("Console network");
    const char *dns = xemu_net_dns_redirect_server();

    if (!xemu_net_is_enabled()) {
        item_set(i, XBCHECK_FAIL,
                 "The network is off. Machine > Settings > Network: turn it "
                 "on, set it to NAT and the Xbox Live service to XB Online.");
    } else if (g_config.net.backend != CONFIG_NET_BACKEND_NAT) {
        item_set(i, XBCHECK_WARN,
                 "The network is not set to NAT, so lookups do not go to XB "
                 "Online. Machine > Settings > Network: choose NAT.");
    } else if (!dns) {
        item_set(i, XBCHECK_FAIL,
                 "Xbox Live service is not set. Machine > Settings > "
                 "Network: set it to XB Online.");
    } else if (g_config.net.dns_redirect != CONFIG_NET_DNS_REDIRECT_XB_ONLINE) {
        item_set(i, XBCHECK_WARN,
                 "Xbox Live service is set to %s, not XB Online. Set it back "
                 "to XB Online in Machine > Settings > Network to play.",
                 g_config.net.dns_redirect == CONFIG_NET_DNS_REDIRECT_INSIGNIA
                     ? "Insignia" : "a custom server");
    } else if (port_busy) {
        item_set(i, XBCHECK_WARN,
                 "On, but port %d is held by another program: Xbox Live works, "
                 "hosting does not. Close other emulators and restart.",
                 port_busy);
    } else {
        item_set(i, XBCHECK_OK, "On, NAT, XB Online.");
    }
}

#ifdef _WIN32
/* Names of the network adapter that holds an address, false if none does */
static bool adapter_of(uint32_t addr, char *name, size_t name_len,
                       char *desc, size_t desc_len, bool *virtual_if)
{
    ULONG size = 32 * 1024;
    IP_ADAPTER_ADDRESSES *list = NULL;
    bool found = false;
    ULONG r = ERROR_BUFFER_OVERFLOW;

    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        g_free(list);
        list = g_malloc(size);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                 GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER, NULL, list, &size);
    }
    for (IP_ADAPTER_ADDRESSES *a = r == NO_ERROR ? list : NULL;
         a && !found; a = a->Next) {
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u;
             u = u->Next) {
            struct sockaddr_in *sin =
                (struct sockaddr_in *)u->Address.lpSockaddr;
            if (sin->sin_family == AF_INET &&
                ntohl(sin->sin_addr.s_addr) == addr) {
                gchar *n = g_utf16_to_utf8((gunichar2 *)a->FriendlyName, -1,
                                           NULL, NULL, NULL);
                gchar *d = g_utf16_to_utf8((gunichar2 *)a->Description, -1,
                                           NULL, NULL, NULL);
                snprintf(name, name_len, "%s", n ? n : "?");
                snprintf(desc, desc_len, "%s", d ? d : "?");
                g_free(n);
                g_free(d);
                /* PPP, tunnel and virtual adapters are how VPNs connect */
                *virtual_if = a->IfType == IF_TYPE_PPP ||
                              a->IfType == IF_TYPE_TUNNEL ||
                              a->IfType == IF_TYPE_PROP_VIRTUAL;
                found = true;
                break;
            }
        }
    }
    g_free(list);
    return found;
}
#endif

static bool looks_like_vpn(const char *name, const char *desc)
{
    static const char *const words[] = {
        "vpn", "wireguard", "wintun", "tap-windows", "tap adapter", "openvpn",
        "nordlynx", "surfshark", "proton", "mullvad", "expressvpn", "windscribe",
        "hamachi", "zerotier", "radmin", "tailscale", "cloudflare", "anyconnect",
        "fortinet", "forticlient", "globalprotect", "pangp", "cyberghost",
        "ipvanish", "private internet access", "hotspot shield", "tunnel",
    };
    gchar *text = g_ascii_strdown(name, -1);
    gchar *d = g_ascii_strdown(desc, -1);
    bool found = false;

    for (int i = 0; i < ARRAY_SIZE(words) && !found; i++) {
        found = strstr(text, words[i]) || strstr(d, words[i]);
    }
    g_free(text);
    g_free(d);
    return found;
}

/*
 * The console's traffic leaves through whichever adapter Windows uses for
 * the internet. Through a VPN, DNS lookups are usually answered by the VPN
 * itself, so the console never reaches XB Online.
 */
static void check_route(void)
{
    int i = item_add("Internet connection");
    uint32_t addr;
    char ip[16], name[128] = "", desc[128] = "";
    bool virtual_if = false, known = false;

    if (!xemu_net_lan_address(&addr)) {
        item_set(i, XBCHECK_FAIL,
                 "This PC has no internet connection the console can use.");
        return;
    }
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", addr >> 24, (addr >> 16) & 255,
             (addr >> 8) & 255, addr & 255);
#ifdef _WIN32
    known = adapter_of(addr, name, sizeof(name), desc, sizeof(desc),
                       &virtual_if);
#endif
    if (known && (virtual_if || looks_like_vpn(name, desc))) {
        item_set(i, XBCHECK_WARN,
                 "Goes through \"%s\" (%s, %s), which looks like a VPN. VPNs "
                 "answer Xbox Live lookups themselves, so the game says Xbox "
                 "Live not found. Turn the VPN off (also its auto-connect and "
                 "kill switch) and restart Black Arrow Online.",
                 name, desc, ip);
    } else if (known) {
        item_set(i, XBCHECK_OK, "Through \"%s\" (%s), address %s.", name, desc,
                 ip);
    } else {
        item_set(i, XBCHECK_OK, "Address %s.", ip);
    }
}

static void check_dns(void)
{
    int i = item_add("XB Online server");
    const char *server = xemu_net_dns_redirect_server();
    uint32_t addr = 0;
    int r;

    if (!server) {
        item_set(i, XBCHECK_FAIL, "Not checked: no Xbox Live service is set.");
        return;
    }
    r = dns_lookup(server, "macs.xboxlive.com", &addr);
    if (r < 0) {
        item_set(i, XBCHECK_FAIL,
                 "No answer from %s. Something on this PC or network blocks "
                 "it: firewall or antivirus (allow xemu), VPN, or the router. "
                 "Try another network (phone hotspot) to tell which.", server);
    } else if (r == 0) {
        item_set(i, XBCHECK_FAIL,
                 "%s answered but does not know Xbox Live. The network may "
                 "be replacing DNS answers (router, ISP or DNS filter).",
                 server);
    } else if (g_config.net.dns_redirect == CONFIG_NET_DNS_REDIRECT_XB_ONLINE &&
               addr != (uint32_t)ldl_be_p((uint8_t[]){ 159, 223, 51, 240 })) {
        item_set(i, XBCHECK_FAIL,
                 "Xbox Live was sent to %u.%u.%u.%u instead of XB Online. "
                 "Something between this PC and the internet answers DNS "
                 "itself: router DNS settings, ISP, Pi-hole, NextDNS, "
                 "antivirus web shield or VPN.",
                 addr >> 24, (addr >> 16) & 255, (addr >> 8) & 255,
                 addr & 255);
    } else {
        item_set(i, XBCHECK_OK, "Reached %s, Xbox Live at %u.%u.%u.%u.",
                 server, addr >> 24, (addr >> 16) & 255, (addr >> 8) & 255,
                 addr & 255);
    }
}

/* Seconds the PC clock is ahead of a time server, or false if none answered */
static bool clock_offset(int64_t *offset, char *server_used, size_t len)
{
    static const char *const servers[] = {
        "time.windows.com", "time.google.com", "pool.ntp.org",
    };

    for (int s = 0; s < ARRAY_SIZE(servers); s++) {
        uint8_t pkt[48] = { 0x1b }; /* client request, version 3 */
        int fd = udp_open(servers[s], 123, 2000);
        int64_t sent, now;
        ssize_t n;

        if (fd < 0) {
            continue;
        }
        sent = g_get_real_time();
        if (send(fd, pkt, sizeof(pkt), 0) == sizeof(pkt)) {
            n = recv(fd, pkt, sizeof(pkt), 0);
            now = g_get_real_time();
            if (n == sizeof(pkt) && ldl_be_p(pkt + 40)) {
                int64_t server_time = (int64_t)(uint32_t)ldl_be_p(pkt + 40) -
                                      NTP_UNIX_OFFSET;
                int64_t local_time = (sent + now) / 2 / G_USEC_PER_SEC;
                *offset = local_time - server_time;
                snprintf(server_used, len, "%s", servers[s]);
                close(fd);
                return true;
            }
        }
        close(fd);
    }
    return false;
}

static void check_clock(void)
{
    int i = item_add("PC clock");
    char server[32];
    int64_t offset;

    if (!clock_offset(&offset, server, sizeof(server))) {
        item_set(i, XBCHECK_INFO,
                 "Could not reach a time server to compare. Make sure the "
                 "Windows date, time and time zone are right (Settings > Time "
                 "> Set time automatically).");
    } else if (offset > CLOCK_LIMIT_SECONDS || offset < -CLOCK_LIMIT_SECONDS) {
        int64_t a = offset < 0 ? -offset : offset;
        item_set(i, XBCHECK_FAIL,
                 "This PC's clock is %" PRId64 " h %" PRId64 " min %s. Xbox "
                 "Live sign-in fails when it is off by more than 2 minutes. "
                 "Windows Settings > Time: turn on Set time automatically and "
                 "Set time zone automatically, press Sync now, restart.",
                 a / 3600, (a / 60) % 60, offset < 0 ? "behind" : "ahead");
    } else {
        item_set(i, XBCHECK_OK, "Right to within %" PRId64 " s (%s).",
                 offset < 0 ? -offset : offset, server);
    }
}

/* Number of disc images in the games folder */
static int games_in_folder(void)
{
    const char *dir_path = g_config.general.games_dir;
    GDir *dir = dir_path && dir_path[0] ? g_dir_open(dir_path, 0, NULL) : NULL;
    const char *name;
    int count = 0;

    while (dir && (name = g_dir_read_name(dir))) {
        if (g_str_has_suffix(name, ".iso") || g_str_has_suffix(name, ".ISO") ||
            g_str_has_suffix(name, ".xiso")) {
            count++;
        }
    }
    if (dir) {
        g_dir_close(dir);
    }
    return count;
}

static void check_disc(void)
{
    int i = item_add("Game disc");
    const char *path = g_config.sys.files.dvd_path;
    XbOnlineXbe xbe;

    if ((!path || !path[0] || xemu_xbonline_is_system_disc(path)) &&
        games_in_folder() == 0) {
        item_set(i, XBCHECK_INFO,
                 "No game yet. Copy your Black Arrow disc image (.iso) into "
                 "the \"games\" folder, then choose XB Online > Play and your game.");
        qemu_mutex_lock(&lock);
        items[i].action = XBCHECK_ACTION_OPEN_GAMES_FOLDER;
        qemu_mutex_unlock(&lock);
    } else if (!path || !path[0]) {
        item_set(i, XBCHECK_INFO, "No disc loaded. To play, choose XB Online "
                 "> Play and your game.");
    } else if (xemu_xbonline_is_system_disc(path)) {
        item_set(i, XBCHECK_INFO, "A dashboard or setup disc is loaded, not "
                 "the game. To play, choose XB Online > Play and your game.");
    } else if (!xemu_xbonline_disc_xbe(path, &xbe)) {
        item_set(i, XBCHECK_WARN,
                 "Could not read the loaded disc image as an Xbox disc. Use "
                 "a plain .iso of the original Black Arrow disc.");
    } else if (xbe.title_id != XEMU_XBONLINE_TITLE_ID) {
        item_set(i, XBCHECK_INFO, "%s (title %08X) is loaded, not Black Arrow.",
                 xbe.name, xbe.title_id);
    } else {
        item_set(i, XBCHECK_OK, "%s, disc version %u.", xbe.name, xbe.version);
    }
}

static void check_title_update(void)
{
    int i = item_add("Game update");
    const char *hdd = g_config.sys.files.hdd_path;
    XbOnlineXbe xbe;
    int maps, r;

    r = hdd && hdd[0] ? xemu_xbonline_hdd_title_update(hdd,
                                                      XEMU_XBONLINE_TITLE_ID,
                                                      &xbe, &maps)
                      : -1;
    if (r < 0) {
        item_set(i, XBCHECK_INFO, "Could not read the hard disk image.");
    } else if (r == 0) {
        item_set(i, XBCHECK_FAIL,
                 "Black Arrow title update is not installed. Without it the "
                 "game wants to download the update, which XB Online does not "
                 "serve. Menu: XB Online > Install Game Update + Maps.");
    } else if (xbe.version < XEMU_XBONLINE_TITLE_UPDATE_VERSION) {
        item_set(i, XBCHECK_FAIL,
                 "An older title update (version %x) is installed. Menu: XB "
                 "Online > Install Game Update + Maps.", xbe.version);
    } else {
        item_set(i, XBCHECK_OK, "Title update %x installed, %d map pack%s.",
                 xbe.version, maps, maps == 1 ? "" : "s");
    }
}

static void check_account(void)
{
    int i = item_add("Xbox Live account");
    const char *hdd = g_config.sys.files.hdd_path;
    int r = hdd && hdd[0] ? xemu_xbonline_hdd_has_account(hdd) : -1;

    if (r < 0) {
        item_set(i, XBCHECK_INFO, "Could not read the hard disk image.");
    } else if (r == 0) {
        item_set(i, XBCHECK_FAIL,
                 "This console has never been set up for Xbox Live. Menu: XB "
                 "Online > XBL Dashboard (Account Setup) and add your account.");
    } else {
        item_set(i, XBCHECK_OK, "Console is set up. In the game, sign in with "
                 "the account the XBL Dashboard added.");
    }
}

static void *check_thread(void *opaque)
{
    fprintf(stderr, "xbcheck: start\n");
    check_network();
    check_route();
    check_dns();
    check_clock();
    check_disc();
    check_title_update();
    check_account();
    fprintf(stderr, "xbcheck: done\n");

    qemu_mutex_lock(&lock);
    running = false;
    qemu_mutex_unlock(&lock);
    xemu_diag_write_check();
    return NULL;
}

void xemu_xbcheck_start(void)
{
    QemuThread thread;

    check_init();
    qemu_mutex_lock(&lock);
    if (running) {
        qemu_mutex_unlock(&lock);
        return;
    }
    running = true;
    item_count = 0;
    qemu_mutex_unlock(&lock);

    qemu_thread_create(&thread, "xbcheck", check_thread, NULL,
                       QEMU_THREAD_DETACHED);
}

int xemu_xbcheck_get(XbCheckItem *out, int max, bool *is_running)
{
    int n;

    check_init();
    qemu_mutex_lock(&lock);
    n = MIN(item_count, max);
    memcpy(out, items, n * sizeof(*out));
    *is_running = running;
    qemu_mutex_unlock(&lock);
    return n;
}

void xemu_xbcheck_note_port_busy(int port)
{
    port_busy = port;
}

void xemu_xbcheck_open_games_folder(void)
{
    const char *dir = g_config.general.games_dir;
    gchar *path, *url;

    if (!dir || !dir[0]) {
        return;
    }
    path = g_canonicalize_filename(dir, NULL);
    g_mkdir_with_parents(path, 0755);
#ifdef _WIN32
    /* Explorer opens a folder given as a plain path */
    url = g_strdup(path);
#else
    url = g_filename_to_uri(path, NULL, NULL);
#endif
    if (url) {
        SDL_OpenURL(url);
    }
    g_free(url);
    g_free(path);
}

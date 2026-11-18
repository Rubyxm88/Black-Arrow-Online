/*
 * XB Online (xb.live) server address: see xemu-xbl-server.h
 *
 * Test switches (environment):
 *   XEMU_XBL_SERVER_NAME     name to look up instead of online.xb.live
 *   XEMU_XBL_SERVER_URL      address file URL instead of the GitHub one
 *   XEMU_XBL_SERVER_BUILTIN  built-in address to use instead of this build's
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/http.h"
#include "qemu/sockets.h"
#include "block/aio.h"
#include "xemu-settings.h"
#include "xemu-net.h"
#include "xemu-xbcheck.h"
#include "xemu-xbl-server.h"

#define XBL_SERVER_NAME "online.xb.live"
#define XBL_SERVER_BUILTIN "152.42.145.249"
#define XBL_SERVER_URL                                                       \
    "https://raw.githubusercontent.com/Rubyxm88/Black-Arrow-Online/main/"    \
    "black-arrow-online/xbl-server.txt"
#define XBL_SERVER_FILE "xbl-server.txt"
/* XB Online's server answers this name with its own address */
#define XBL_PROBE_NAME "macs.xboxlive.com"
#define XBL_PROBE_MS 1500
#define XBL_RECHECK_GAP_MS (120 * 1000)

static QemuMutex lock;
static QemuCond done_cond;
static bool inited, started, first_done, looking;
static char current[INET_ADDRSTRLEN];
static char source[96];
static int64_t last_look_ms;

static void init_once(void);

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = g_getenv(name);
    return v && *v ? v : fallback;
}

static bool valid_ipv4(const char *s, uint32_t *out)
{
    struct in_addr a;
    if (!s || !inet_aton(s, &a)) {
        return false;
    }
    if (out) {
        *out = ntohl(a.s_addr);
    }
    /* never 0.0.0.0, loopback or the like */
    return a.s_addr != 0 && (ntohl(a.s_addr) >> 24) != 127;
}

static char *saved_path(void)
{
    return g_build_filename(xemu_settings_get_base_path(), XBL_SERVER_FILE,
                            NULL);
}

static bool read_saved(char *out, size_t len)
{
    g_autofree char *path = saved_path();
    g_autofree char *text = NULL;

    if (!g_file_get_contents(path, &text, NULL, NULL)) {
        return false;
    }
    g_strstrip(text);
    if (!valid_ipv4(text, NULL)) {
        return false;
    }
    g_strlcpy(out, text, len);
    return true;
}

static void write_saved(const char *addr)
{
    g_autofree char *path = saved_path();
    g_autofree char *line = g_strdup_printf("%s\n", addr);
    char old[INET_ADDRSTRLEN];

    if (read_saved(old, sizeof(old)) && !strcmp(old, addr)) {
        return;
    }
    if (!g_file_set_contents(path, line, -1, NULL)) {
        fprintf(stderr, "xbl-server: could not save %s\n", path);
    }
}

/* IPv4 address of a name through this PC's own DNS */
static bool resolve(const char *name, char *out, size_t len)
{
    struct addrinfo hints = { .ai_family = AF_INET,
                              .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    bool ok = false;

    if (getaddrinfo(name, "53", &hints, &res) == 0 && res) {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        ok = inet_ntop(AF_INET, &sin->sin_addr, out, len) != NULL &&
             valid_ipv4(out, NULL);
    }
    if (res) {
        freeaddrinfo(res);
    }
    return ok;
}

/* True if addr answers DNS like XB Online: the probe name, with itself */
static bool answers(const char *addr)
{
    uint32_t want, got = 0;
    int r;

    if (!valid_ipv4(addr, &want)) {
        return false;
    }
    r = xemu_xbcheck_dns_query(addr, XBL_PROBE_NAME, &got, XBL_PROBE_MS);
    fprintf(stderr, "xbl-server: %s %s\n", addr,
            r < 0     ? "did not answer"
            : r == 0  ? "answered without an address"
            : got == want ? "answers like XB Online"
                          : "answered with another address");
    return r > 0 && got == want;
}

/* The GitHub address file: "name=..." and/or "address=..." lines */
static void read_file(char *name, size_t name_len, char *addr,
                      size_t addr_len)
{
    const char *url = env_or("XEMU_XBL_SERVER_URL", XBL_SERVER_URL);
    g_autoptr(GByteArray) data = g_byte_array_new();
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;
    int code = http_get(url, data, NULL, NULL);

    name[0] = addr[0] = 0;
    if (code != 200) {
        fprintf(stderr, "xbl-server: address file not read (%d)\n", code);
        return;
    }
    text = g_strndup((const char *)data->data, MIN(data->len, 4096));
    lines = g_strsplit(text, "\n", 0);
    for (char **l = lines; *l; l++) {
        char *line = g_strstrip(*l);
        if (g_str_has_prefix(line, "name=")) {
            g_strlcpy(name, g_strstrip(line + 5), name_len);
        } else if (g_str_has_prefix(line, "address=")) {
            char *a = g_strstrip(line + 8);
            if (valid_ipv4(a, NULL)) {
                g_strlcpy(addr, a, addr_len);
            }
        }
    }
}

typedef struct Candidate {
    char addr[INET_ADDRSTRLEN];
    char from[96];
} Candidate;

static void add(Candidate *list, int *n, const char *addr, const char *from)
{
    if (!valid_ipv4(addr, NULL) || *n >= 6) {
        return;
    }
    for (int i = 0; i < *n; i++) {
        if (!strcmp(list[i].addr, addr)) {
            return; /* already tried */
        }
    }
    g_strlcpy(list[*n].addr, addr, sizeof(list[*n].addr));
    g_strlcpy(list[*n].from, from, sizeof(list[*n].from));
    (*n)++;
}

/* Runs on the main loop: hand a new address to the DNS redirect */
static void apply_bh(void *opaque)
{
    xemu_net_dns_server_changed();
}

static void look(void)
{
    Candidate list[6];
    int n = 0, tried = 0, pick = -1;
    char addr[INET_ADDRSTRLEN], fname[256], faddr[INET_ADDRSTRLEN];
    const char *name = env_or("XEMU_XBL_SERVER_NAME", XBL_SERVER_NAME);
    bool changed;

    /* 1. the name */
    if (resolve(name, addr, sizeof(addr))) {
        g_autofree char *from = g_strdup_printf("name %s", name);
        add(list, &n, addr, from);
    } else {
        fprintf(stderr, "xbl-server: %s not found\n", name);
    }
    for (; tried < n && pick < 0; tried++) {
        if (answers(list[tried].addr)) {
            pick = tried;
        }
    }

    /* 2. the address file */
    if (pick < 0) {
        read_file(fname, sizeof(fname), faddr, sizeof(faddr));
        if (fname[0] && strcmp(fname, name) &&
            resolve(fname, addr, sizeof(addr))) {
            g_autofree char *from =
                g_strdup_printf("address file, name %s", fname);
            add(list, &n, addr, from);
        }
        add(list, &n, faddr, "address file");
        for (; tried < n && pick < 0; tried++) {
            if (answers(list[tried].addr)) {
                pick = tried;
            }
        }
    }

    /* 3. last good, 4. built in */
    if (pick < 0) {
        if (read_saved(addr, sizeof(addr))) {
            add(list, &n, addr, "last good address");
        }
        add(list, &n, env_or("XEMU_XBL_SERVER_BUILTIN", XBL_SERVER_BUILTIN),
            "built in");
        for (; tried < n && pick < 0; tried++) {
            if (answers(list[tried].addr)) {
                pick = tried;
            }
        }
    }

    if (pick >= 0) {
        write_saved(list[pick].addr);
    }

    qemu_mutex_lock(&lock);
    if (pick < 0 && n > 0) {
        /* nothing answered: offline, or DNS is answered by the network */
        changed = strcmp(current, list[0].addr) != 0;
        g_strlcpy(current, list[0].addr, sizeof(current));
        snprintf(source, sizeof(source), "%s (did not answer)", list[0].from);
    } else if (pick >= 0) {
        changed = strcmp(current, list[pick].addr) != 0;
        g_strlcpy(current, list[pick].addr, sizeof(current));
        g_strlcpy(source, list[pick].from, sizeof(source));
    } else {
        changed = false;
    }
    fprintf(stderr, "xbl-server: using %s (%s)\n", current, source);
    qemu_mutex_unlock(&lock);

    if (changed) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), apply_bh, NULL);
    }
}

static void *look_thread(void *opaque)
{
    look();
    qemu_mutex_lock(&lock);
    looking = false;
    first_done = true;
    qemu_cond_broadcast(&done_cond);
    qemu_mutex_unlock(&lock);
    return NULL;
}

static void init_once(void)
{
    if (inited) {
        return;
    }
    inited = true;
    qemu_mutex_init(&lock);
    qemu_cond_init(&done_cond);
    if (read_saved(current, sizeof(current))) {
        g_strlcpy(source, "last good address", sizeof(source));
    } else {
        g_strlcpy(current,
                  env_or("XEMU_XBL_SERVER_BUILTIN", XBL_SERVER_BUILTIN),
                  sizeof(current));
        g_strlcpy(source, "built in", sizeof(source));
    }
}

static void begin_look(void)
{
    QemuThread thread;

    looking = true;
    last_look_ms = g_get_monotonic_time() / 1000;
    qemu_thread_create(&thread, "xbl-server", look_thread, NULL,
                       QEMU_THREAD_DETACHED);
}

const char *xemu_xbl_server(void)
{
    static char copy[INET_ADDRSTRLEN];

    init_once();
    qemu_mutex_lock(&lock);
    g_strlcpy(copy, current, sizeof(copy));
    qemu_mutex_unlock(&lock);
    return copy;
}

const char *xemu_xbl_server_source(void)
{
    static char copy[96];

    init_once();
    qemu_mutex_lock(&lock);
    g_strlcpy(copy, source, sizeof(copy));
    qemu_mutex_unlock(&lock);
    return copy;
}

void xemu_xbl_server_start(void)
{
    init_once();
    qemu_mutex_lock(&lock);
    if (!started) {
        started = true;
        begin_look();
    }
    qemu_mutex_unlock(&lock);
}

bool xemu_xbl_server_wait(int ms)
{
    int64_t end = g_get_monotonic_time() / 1000 + ms;
    bool done;

    init_once();
    qemu_mutex_lock(&lock);
    while (started && !first_done) {
        int64_t left = end - g_get_monotonic_time() / 1000;
        if (left <= 0) {
            break;
        }
        qemu_cond_timedwait(&done_cond, &lock, left);
    }
    done = first_done;
    qemu_mutex_unlock(&lock);
    return done;
}

void xemu_xbl_server_recheck(void)
{
    int64_t now = g_get_monotonic_time() / 1000;

    init_once();
    qemu_mutex_lock(&lock);
    if (started && !looking && now - last_look_ms >= XBL_RECHECK_GAP_MS) {
        fprintf(stderr, "xbl-server: %s stopped answering, looking again\n",
                current);
        begin_look();
    }
    qemu_mutex_unlock(&lock);
}

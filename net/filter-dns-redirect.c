/*
 * DNS redirect filter
 *
 * Sends every DNS query made by the guest to a fixed server, regardless of
 * the DNS server the guest is configured with. Replies are rewritten so they
 * appear to come from the server the guest originally asked.
 *
 * With 'fallback' set, the Xbox Live names XB Online serves are also checked:
 * when the reply for one of them carries another address (a VPN, ISP or router
 * answering DNS itself) or no reply arrives in time, the filter answers the
 * guest from a built-in list instead (see black-arrow-online/xbl-hosts.txt).
 * XEMU_DNS_FALLBACK_TEST_NAME adds up to four more names (comma separated),
 * to test with names the console looks up by itself.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "net/filter.h"
#include "net/net.h"
#include "net/eth.h"
#include "net/checksum.h"
#include "qapi/error.h"
#include "qemu/atomic.h"
#include "qemu/iov.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "ui/xemu-net.h"
#include "ui/xemu-xbcheck.h"

#define TYPE_FILTER_DNS_REDIRECT "filter-dns-redirect"
OBJECT_DECLARE_SIMPLE_TYPE(DnsRedirectState, FILTER_DNS_REDIRECT)

#define DNS_PORT 53
#define DNS_HDR_LEN 12
#define DNS_REDIRECT_NUM_FLOWS 64

/* How long the server gets to answer an Xbox Live name before xemu does */
#define DNS_FALLBACK_TIMEOUT_MS 1500
/* How long late replies to a query xemu answered are dropped */
#define DNS_FALLBACK_HOLD_MS 10000
#define DNS_FALLBACK_NUM_QUERIES 16
#define DNS_FALLBACK_TTL 60

/* Names XB Online's DNS server answers, all with its own address */
static const char *dns_fallback_names[] = {
    "macs.xboxlive.com", "as.xboxlive.com", "tgs.xboxlive.com",
    "xds.xboxlive.com",  "macs.xbox.com",
    /* XEMU_DNS_FALLBACK_TEST_NAME, to test with any names */
    NULL, NULL, NULL, NULL,
};
#define DNS_FALLBACK_NUM_BUILTIN 5
static const uint8_t dns_fallback_addr[4] = { 159, 223, 51, 240 };

typedef struct DnsRedirectFlow {
    uint16_t port; /* guest source port, network order (0 = unused) */
    uint32_t addr; /* server the guest addressed, network order */
} DnsRedirectFlow;

typedef enum DnsFallbackState {
    DNS_FALLBACK_FREE,
    DNS_FALLBACK_WAITING,  /* sent to the server, no good reply yet */
    DNS_FALLBACK_ANSWERED, /* xemu answered, drop what the server sends */
} DnsFallbackState;

/* An Xbox Live query from the guest, kept until the server answers it */
typedef struct DnsFallbackQuery {
    DnsFallbackState state;
    uint16_t port; /* guest source port, network order */
    uint16_t id;   /* DNS transaction id */
    int name;      /* index into dns_fallback_names */
    int64_t deadline;
    uint8_t guest_mac[ETH_ALEN];
    uint8_t peer_mac[ETH_ALEN];
    uint32_t guest_addr; /* network order */
    uint32_t asked_addr; /* network order */
    uint8_t question[260];
    size_t question_len;
} DnsFallbackQuery;

struct DnsRedirectState {
    NetFilterState parent_obj;

    char *server;
    uint32_t server_addr; /* network order */
    DnsRedirectFlow flows[DNS_REDIRECT_NUM_FLOWS];
    unsigned int next_flow;

    bool fallback;
    QEMUTimer *fallback_timer;
    DnsFallbackQuery queries[DNS_FALLBACK_NUM_QUERIES];
    unsigned int next_query;
};

/* What happened to a packet */
typedef enum DnsRedirectResult {
    DNS_REDIRECT_PASS,     /* untouched */
    DNS_REDIRECT_MODIFIED, /* rewritten in place */
    DNS_REDIRECT_CONSUMED, /* replaced by an answer from the built-in list */
} DnsRedirectResult;

/* Read by the XB Online check from another thread */
static int dns_fallback_count;
static int dns_fallback_last_name;
static int dns_fallback_last_timeout;

bool xemu_net_dns_fallback_used(int *count, const char **name,
                                bool *timed_out)
{
    int n = qatomic_read(&dns_fallback_count);

    if (n == 0) {
        return false;
    }
    *count = n;
    *name = dns_fallback_names[qatomic_read(&dns_fallback_last_name)];
    *timed_out = qatomic_read(&dns_fallback_last_timeout);
    return true;
}

static void dns_redirect_remember(DnsRedirectState *s, uint16_t port,
                                  uint32_t addr)
{
    for (int i = 0; i < DNS_REDIRECT_NUM_FLOWS; i++) {
        if (s->flows[i].port == port) {
            s->flows[i].addr = addr;
            return;
        }
    }

    s->flows[s->next_flow].port = port;
    s->flows[s->next_flow].addr = addr;
    s->next_flow = (s->next_flow + 1) % DNS_REDIRECT_NUM_FLOWS;
}

static bool dns_redirect_lookup(DnsRedirectState *s, uint16_t port,
                                uint32_t *addr)
{
    for (int i = 0; i < DNS_REDIRECT_NUM_FLOWS; i++) {
        if (s->flows[i].port == port) {
            *addr = s->flows[i].addr;
            return true;
        }
    }

    return false;
}

/*
 * Decode the first question name of a DNS message. Returns the offset just
 * past the name, or 0 if it is malformed.
 */
static size_t dns_redirect_question_name(const uint8_t *dns, size_t len,
                                         char *name, size_t name_len)
{
    size_t pos = DNS_HDR_LEN, out = 0;

    while (pos < len && dns[pos] != 0) {
        if (dns[pos] >= 64 || pos + 1 + dns[pos] > len ||
            out + dns[pos] + 2 > name_len) {
            name[0] = '\0';
            return 0;
        }
        if (out) {
            name[out++] = '.';
        }
        memcpy(&name[out], &dns[pos + 1], dns[pos]);
        out += dns[pos];
        pos += 1 + dns[pos];
    }
    name[out] = '\0';
    return pos < len ? pos + 1 : 0;
}

/* Decode the first question name of a DNS message for logging */
static void dns_redirect_log_query(DnsRedirectState *s, const uint8_t *dns,
                                   size_t len, uint32_t asked_addr)
{
    char name[256];
    struct in_addr asked = { .s_addr = asked_addr };

    dns_redirect_question_name(dns, len, name, sizeof(name));
    fprintf(stderr, "dns-redirect: query '%s' sent to %s (console asked %s)\n",
            name, s->server, inet_ntoa(asked));
}

static DnsFallbackQuery *dns_fallback_find(DnsRedirectState *s, uint16_t port,
                                           uint16_t id)
{
    for (int i = 0; i < DNS_FALLBACK_NUM_QUERIES; i++) {
        DnsFallbackQuery *q = &s->queries[i];
        if (q->state != DNS_FALLBACK_FREE && q->port == port && q->id == id) {
            return q;
        }
    }
    return NULL;
}

static void dns_fallback_arm_timer(DnsRedirectState *s)
{
    int64_t next = INT64_MAX;

    for (int i = 0; i < DNS_FALLBACK_NUM_QUERIES; i++) {
        if (s->queries[i].state != DNS_FALLBACK_FREE) {
            next = MIN(next, s->queries[i].deadline);
        }
    }
    if (next == INT64_MAX) {
        timer_del(s->fallback_timer);
    } else {
        timer_mod(s->fallback_timer, next);
    }
}

/* Note an A query for an Xbox Live name so its reply can be checked */
static void dns_fallback_track(DnsRedirectState *s, const uint8_t *frame,
                               const struct ip_header *ip, const udp_header *udp,
                               const uint8_t *dns, size_t len)
{
    char name[256];
    size_t end = dns_redirect_question_name(dns, len, name, sizeof(name));
    uint16_t id, port = lduw_he_p(&udp->uh_sport);
    DnsFallbackQuery *q;
    int match = -1;

    if (!end || end + 4 > len || lduw_be_p(&dns[4]) != 1 ||
        lduw_be_p(&dns[end]) != 1 || lduw_be_p(&dns[end + 2]) != 1) {
        return; /* not a single A/IN question */
    }
    for (int i = 0; i < ARRAY_SIZE(dns_fallback_names); i++) {
        if (dns_fallback_names[i] &&
            g_ascii_strcasecmp(name, dns_fallback_names[i]) == 0) {
            match = i;
            break;
        }
    }
    if (match < 0) {
        return;
    }

    id = lduw_be_p(dns);
    q = dns_fallback_find(s, port, id);
    if (q) {
        return; /* a retry: keep the first deadline */
    }

    q = &s->queries[s->next_query];
    s->next_query = (s->next_query + 1) % DNS_FALLBACK_NUM_QUERIES;
    q->state = DNS_FALLBACK_WAITING;
    q->port = port;
    q->id = id;
    q->name = match;
    q->deadline =
        qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + DNS_FALLBACK_TIMEOUT_MS;
    memcpy(q->guest_mac, PKT_GET_ETH_HDR(frame)->h_source, ETH_ALEN);
    memcpy(q->peer_mac, PKT_GET_ETH_HDR(frame)->h_dest, ETH_ALEN);
    memcpy(&q->guest_addr, &ip->ip_src, sizeof(q->guest_addr));
    memcpy(&q->asked_addr, &ip->ip_dst, sizeof(q->asked_addr));
    q->question_len = end + 4 - DNS_HDR_LEN;
    memcpy(q->question, dns + DNS_HDR_LEN, q->question_len);
    dns_fallback_arm_timer(s);
}

/* True if a reply carries XB Online's address as an A record */
static bool dns_fallback_reply_ok(const uint8_t *dns, size_t len,
                                  uint32_t *first_addr)
{
    char name[256];
    size_t o = dns_redirect_question_name(dns, len, name, sizeof(name));
    int answers;
    bool found = false;

    *first_addr = 0;
    if (!o || len < DNS_HDR_LEN || (dns[3] & 0x0f) != 0) {
        return false;
    }
    o += 4;
    answers = lduw_be_p(&dns[6]);
    while (answers-- > 0 && o < len) {
        uint16_t type, rdlen;
        if ((dns[o] & 0xc0) == 0xc0) {
            o += 2;
        } else {
            while (o < len && dns[o]) {
                o += dns[o] + 1;
            }
            o++;
        }
        if (o + 10 > len) {
            break;
        }
        type = lduw_be_p(&dns[o]);
        rdlen = lduw_be_p(&dns[o + 8]);
        o += 10;
        if (o + rdlen > len) {
            break;
        }
        if (type == 1 && rdlen == 4) {
            if (!*first_addr) {
                memcpy(first_addr, &dns[o], 4);
            }
            if (memcmp(&dns[o], dns_fallback_addr, 4) == 0) {
                found = true;
            }
        }
        o += rdlen;
    }
    return found;
}

/* Send the guest an answer for q from the built-in list */
static void dns_fallback_answer(DnsRedirectState *s, DnsFallbackQuery *q)
{
    NetFilterState *nf = NETFILTER(s);
    size_t dns_len = DNS_HDR_LEN + q->question_len + 16;
    size_t size = sizeof(struct eth_header) + sizeof(struct ip_header) +
                  sizeof(udp_header) + dns_len;
    g_autofree uint8_t *buf = g_malloc0(size);
    struct eth_header *eth = PKT_GET_ETH_HDR(buf);
    struct ip_header *ip = (struct ip_header *)(eth + 1);
    udp_header *udp = (udp_header *)(ip + 1);
    uint8_t *dns = (uint8_t *)(udp + 1), *ans;
    struct iovec iov = { .iov_base = buf, .iov_len = size };

    memcpy(eth->h_dest, q->guest_mac, ETH_ALEN);
    memcpy(eth->h_source, q->peer_mac, ETH_ALEN);
    stw_be_p(&eth->h_proto, ETH_P_IP);

    ip->ip_ver_len = (IP_HEADER_VERSION_4 << 4) | (sizeof(*ip) / 4);
    stw_be_p(&ip->ip_len, size - sizeof(*eth));
    ip->ip_ttl = 64;
    ip->ip_p = IP_PROTO_UDP;
    memcpy(&ip->ip_src, &q->asked_addr, sizeof(q->asked_addr));
    memcpy(&ip->ip_dst, &q->guest_addr, sizeof(q->guest_addr));

    stw_be_p(&udp->uh_sport, DNS_PORT);
    stw_he_p(&udp->uh_dport, q->port);
    stw_be_p(&udp->uh_ulen, sizeof(*udp) + dns_len);

    stw_be_p(&dns[0], q->id);
    stw_be_p(&dns[2], 0x8580); /* response, authoritative, recursion */
    stw_be_p(&dns[4], 1);      /* one question */
    stw_be_p(&dns[6], 1);      /* one answer */
    memcpy(dns + DNS_HDR_LEN, q->question, q->question_len);
    ans = dns + DNS_HDR_LEN + q->question_len;
    stw_be_p(&ans[0], 0xc000 | DNS_HDR_LEN); /* name: the question's */
    stw_be_p(&ans[2], 1);                    /* A */
    stw_be_p(&ans[4], 1);                    /* IN */
    stl_be_p(&ans[6], DNS_FALLBACK_TTL);
    stw_be_p(&ans[10], 4);
    memcpy(&ans[12], dns_fallback_addr, 4);

    net_checksum_calculate(buf, size, CSUM_IP | CSUM_UDP);

    q->state = DNS_FALLBACK_ANSWERED;
    q->deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + DNS_FALLBACK_HOLD_MS;
    qatomic_set(&dns_fallback_last_name, q->name);
    qatomic_inc(&dns_fallback_count);

    if (nf->netdev) {
        qemu_netfilter_pass_to_next(nf->netdev, 0, &iov, 1, nf);
    }
    xemu_xbcheck_note_dns_fallback();
}

static void dns_fallback_timer_cb(void *opaque)
{
    DnsRedirectState *s = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    for (int i = 0; i < DNS_FALLBACK_NUM_QUERIES; i++) {
        DnsFallbackQuery *q = &s->queries[i];
        if (q->state == DNS_FALLBACK_FREE || q->deadline > now) {
            continue;
        }
        if (q->state == DNS_FALLBACK_ANSWERED) {
            q->state = DNS_FALLBACK_FREE;
            continue;
        }
        fprintf(stderr,
                "dns-fallback: %s got no answer from %s in %d ms, console "
                "told %u.%u.%u.%u from the built-in XB Online list\n",
                dns_fallback_names[q->name], s->server, DNS_FALLBACK_TIMEOUT_MS,
                dns_fallback_addr[0], dns_fallback_addr[1],
                dns_fallback_addr[2], dns_fallback_addr[3]);
        qatomic_set(&dns_fallback_last_timeout, 1);
        dns_fallback_answer(s, q);
    }
    dns_fallback_arm_timer(s);
}

/*
 * Check a reply for a tracked Xbox Live query. 'from' is its source: a reply
 * from another address than the server comes from something on the network
 * answering DNS itself, and the console drops it, so it is replaced too.
 */
static DnsRedirectResult dns_fallback_check_reply(DnsRedirectState *s,
                                                  uint16_t port, uint32_t from,
                                                  const uint8_t *dns,
                                                  size_t len)
{
    DnsFallbackQuery *q;
    uint32_t got;
    char from_str[INET_ADDRSTRLEN], got_str[INET_ADDRSTRLEN] = "no address";

    if (len < DNS_HDR_LEN ||
        !(q = dns_fallback_find(s, port, lduw_be_p(dns)))) {
        return DNS_REDIRECT_PASS;
    }
    if (q->state == DNS_FALLBACK_ANSWERED) {
        return DNS_REDIRECT_CONSUMED; /* late reply, the console has one */
    }
    if (dns_fallback_reply_ok(dns, len, &got) && from == s->server_addr) {
        q->state = DNS_FALLBACK_FREE;
        dns_fallback_arm_timer(s);
        return DNS_REDIRECT_PASS;
    }

    inet_ntop(AF_INET, &from, from_str, sizeof(from_str));
    if (got) {
        inet_ntop(AF_INET, &got, got_str, sizeof(got_str));
    }
    fprintf(stderr,
            "dns-fallback: %s was answered %s (rcode %d) by %s%s, the network "
            "changes DNS answers; console told %u.%u.%u.%u from the built-in "
            "XB Online list\n",
            dns_fallback_names[q->name], got_str, dns[3] & 0x0f, from_str,
            from == s->server_addr ? "" : " instead of the server",
            dns_fallback_addr[0], dns_fallback_addr[1], dns_fallback_addr[2],
            dns_fallback_addr[3]);
    qatomic_set(&dns_fallback_last_timeout, 0);
    dns_fallback_answer(s, q);
    dns_fallback_arm_timer(s);
    return DNS_REDIRECT_CONSUMED;
}

static DnsRedirectResult dns_redirect_rewrite(DnsRedirectState *s, uint8_t *buf,
                                              size_t size, bool from_guest)
{
    struct ip_header *ip;
    udp_header *udp;
    size_t ip_hdr_len, dns_len;
    uint8_t *dns;
    uint32_t addr;

    if (size < sizeof(struct eth_header) + sizeof(struct ip_header) ||
        lduw_be_p(&PKT_GET_ETH_HDR(buf)->h_proto) != ETH_P_IP) {
        return DNS_REDIRECT_PASS;
    }

    ip = (struct ip_header *)(buf + sizeof(struct eth_header));
    ip_hdr_len = IP_HDR_GET_LEN(ip);
    if (IP_HEADER_VERSION(ip) != IP_HEADER_VERSION_4 ||
        ip->ip_p != IP_PROTO_UDP || IP4_IS_FRAGMENT(ip) ||
        ip_hdr_len != sizeof(struct ip_header) ||
        size < sizeof(struct eth_header) + ip_hdr_len + sizeof(udp_header)) {
        return DNS_REDIRECT_PASS;
    }

    udp = (udp_header *)((uint8_t *)ip + ip_hdr_len);
    dns = (uint8_t *)(udp + 1);
    dns_len = MIN((size_t)(buf + size - dns),
                  MAX(lduw_be_p(&udp->uh_ulen), sizeof(udp_header)) -
                      sizeof(udp_header));

    if (from_guest) {
        if (lduw_be_p(&udp->uh_dport) != DNS_PORT) {
            return DNS_REDIRECT_PASS;
        }
        if (s->fallback) {
            dns_fallback_track(s, buf, ip, udp, dns, dns_len);
        }
        memcpy(&addr, &ip->ip_dst, sizeof(addr));
        if (addr == s->server_addr) {
            return DNS_REDIRECT_PASS;
        }
        dns_redirect_remember(s, lduw_he_p(&udp->uh_sport), addr);
        dns_redirect_log_query(s, dns, dns_len, addr);
        memcpy(&ip->ip_dst, &s->server_addr, sizeof(addr));
    } else {
        if (lduw_be_p(&udp->uh_sport) != DNS_PORT) {
            return DNS_REDIRECT_PASS;
        }
        memcpy(&addr, &ip->ip_src, sizeof(addr));
        if (s->fallback &&
            dns_fallback_check_reply(s, lduw_he_p(&udp->uh_dport), addr, dns,
                                     dns_len) == DNS_REDIRECT_CONSUMED) {
            return DNS_REDIRECT_CONSUMED;
        }
        if (addr != s->server_addr) {
            return DNS_REDIRECT_PASS;
        }
        if (!dns_redirect_lookup(s, lduw_he_p(&udp->uh_dport), &addr)) {
            return DNS_REDIRECT_PASS;
        }
        memcpy(&ip->ip_src, &addr, sizeof(addr));
        fprintf(stderr, "dns-redirect: reply received from %s\n", s->server);
    }

    net_checksum_calculate(buf, size, CSUM_IP | CSUM_UDP);
    return DNS_REDIRECT_MODIFIED;
}

static ssize_t dns_redirect_receive_iov(NetFilterState *nf,
                                        NetClientState *sender, unsigned flags,
                                        const struct iovec *iov, int iovcnt,
                                        NetPacketSent *sent_cb)
{
    DnsRedirectState *s = FILTER_DNS_REDIRECT(nf);
    size_t size = iov_size(iov, iovcnt);
    g_autofree uint8_t *buf = g_malloc(size);
    struct iovec out = { .iov_base = buf, .iov_len = size };

    iov_to_buf(iov, iovcnt, 0, buf, size);
    switch (dns_redirect_rewrite(s, buf, size, sender != nf->netdev)) {
    case DNS_REDIRECT_PASS:
        return 0;
    case DNS_REDIRECT_CONSUMED:
        return size;
    case DNS_REDIRECT_MODIFIED:
        break;
    }

    qemu_netfilter_pass_to_next(sender, flags, &out, 1, nf);
    return size;
}

static void dns_redirect_setup(NetFilterState *nf, Error **errp)
{
    DnsRedirectState *s = FILTER_DNS_REDIRECT(nf);
    struct in_addr addr;

    if (!s->server || !inet_aton(s->server, &addr)) {
        error_setg(errp, "filter-dns-redirect: 'server' must be an IPv4 "
                         "address");
        return;
    }

    s->server_addr = addr.s_addr;
    if (s->fallback && g_getenv("XEMU_DNS_FALLBACK_TEST_NAME")) {
        g_auto(GStrv) names =
            g_strsplit(g_getenv("XEMU_DNS_FALLBACK_TEST_NAME"), ",", 0);
        int i = DNS_FALLBACK_NUM_BUILTIN;
        for (char **n = names; *n && i < ARRAY_SIZE(dns_fallback_names);
             n++) {
            if (**n && !dns_fallback_names[i]) {
                dns_fallback_names[i++] = g_strdup(*n);
                fprintf(stderr, "dns-fallback: test name %s added\n", *n);
            }
        }
    }
    s->fallback_timer =
        timer_new_ms(QEMU_CLOCK_REALTIME, dns_fallback_timer_cb, s);
}

static char *dns_redirect_get_server(Object *obj, Error **errp)
{
    DnsRedirectState *s = FILTER_DNS_REDIRECT(obj);

    return g_strdup(s->server);
}

static void dns_redirect_set_server(Object *obj, const char *value,
                                    Error **errp)
{
    DnsRedirectState *s = FILTER_DNS_REDIRECT(obj);

    g_free(s->server);
    s->server = g_strdup(value);
}

static bool dns_redirect_get_fallback(Object *obj, Error **errp)
{
    return FILTER_DNS_REDIRECT(obj)->fallback;
}

static void dns_redirect_set_fallback(Object *obj, bool value, Error **errp)
{
    FILTER_DNS_REDIRECT(obj)->fallback = value;
}

static void dns_redirect_fini(Object *obj)
{
    DnsRedirectState *s = FILTER_DNS_REDIRECT(obj);

    if (s->fallback_timer) {
        timer_free(s->fallback_timer);
    }
    g_free(s->server);
}

static void dns_redirect_class_init(ObjectClass *oc, const void *data)
{
    NetFilterClass *nfc = NETFILTER_CLASS(oc);

    object_class_property_add_str(oc, "server", dns_redirect_get_server,
                                  dns_redirect_set_server);
    object_class_property_add_bool(oc, "fallback", dns_redirect_get_fallback,
                                   dns_redirect_set_fallback);

    nfc->setup = dns_redirect_setup;
    nfc->receive_iov = dns_redirect_receive_iov;
}

static const TypeInfo dns_redirect_info = {
    .name = TYPE_FILTER_DNS_REDIRECT,
    .parent = TYPE_NETFILTER,
    .class_init = dns_redirect_class_init,
    .instance_finalize = dns_redirect_fini,
    .instance_size = sizeof(DnsRedirectState),
};

static void register_types(void)
{
    type_register_static(&dns_redirect_info);
}

type_init(register_types);

/*
 * DNS redirect filter
 *
 * Sends every DNS query made by the guest to a fixed server, regardless of
 * the DNS server the guest is configured with. Replies are rewritten so they
 * appear to come from the server the guest originally asked.
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
#include "qemu/iov.h"
#include "qemu/sockets.h"
#include "qom/object.h"

#define TYPE_FILTER_DNS_REDIRECT "filter-dns-redirect"
OBJECT_DECLARE_SIMPLE_TYPE(DnsRedirectState, FILTER_DNS_REDIRECT)

#define DNS_PORT 53
#define DNS_REDIRECT_NUM_FLOWS 64

typedef struct DnsRedirectFlow {
    uint16_t port; /* guest source port, network order (0 = unused) */
    uint32_t addr; /* server the guest addressed, network order */
} DnsRedirectFlow;

struct DnsRedirectState {
    NetFilterState parent_obj;

    char *server;
    uint32_t server_addr; /* network order */
    DnsRedirectFlow flows[DNS_REDIRECT_NUM_FLOWS];
    unsigned int next_flow;
};

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

/* Decode the first question name of a DNS message for logging */
static void dns_redirect_log_query(DnsRedirectState *s, const uint8_t *dns,
                                   size_t len, uint32_t asked_addr)
{
    char name[256];
    size_t pos = 12, out = 0;
    struct in_addr asked = { .s_addr = asked_addr };

    while (pos < len && dns[pos] != 0 && dns[pos] < 64 &&
           pos + 1 + dns[pos] <= len && out + dns[pos] + 2 < sizeof(name)) {
        if (out) {
            name[out++] = '.';
        }
        memcpy(&name[out], &dns[pos + 1], dns[pos]);
        out += dns[pos];
        pos += 1 + dns[pos];
    }
    name[out] = '\0';

    fprintf(stderr, "dns-redirect: query '%s' sent to %s (console asked %s)\n",
            name, s->server, inet_ntoa(asked));
}

/* Returns true if the packet was modified */
static bool dns_redirect_rewrite(DnsRedirectState *s, uint8_t *buf, size_t size,
                                 bool from_guest)
{
    struct ip_header *ip;
    udp_header *udp;
    size_t ip_hdr_len;
    uint32_t addr;

    if (size < sizeof(struct eth_header) + sizeof(struct ip_header) ||
        lduw_be_p(&PKT_GET_ETH_HDR(buf)->h_proto) != ETH_P_IP) {
        return false;
    }

    ip = (struct ip_header *)(buf + sizeof(struct eth_header));
    ip_hdr_len = IP_HDR_GET_LEN(ip);
    if (IP_HEADER_VERSION(ip) != IP_HEADER_VERSION_4 ||
        ip->ip_p != IP_PROTO_UDP || IP4_IS_FRAGMENT(ip) ||
        ip_hdr_len != sizeof(struct ip_header) ||
        size < sizeof(struct eth_header) + ip_hdr_len + sizeof(udp_header)) {
        return false;
    }

    udp = (udp_header *)((uint8_t *)ip + ip_hdr_len);

    if (from_guest) {
        if (lduw_be_p(&udp->uh_dport) != DNS_PORT) {
            return false;
        }
        memcpy(&addr, &ip->ip_dst, sizeof(addr));
        if (addr == s->server_addr) {
            return false;
        }
        dns_redirect_remember(s, lduw_he_p(&udp->uh_sport), addr);
        dns_redirect_log_query(s, (uint8_t *)(udp + 1),
                               buf + size - (uint8_t *)(udp + 1), addr);
        memcpy(&ip->ip_dst, &s->server_addr, sizeof(addr));
    } else {
        if (lduw_be_p(&udp->uh_sport) != DNS_PORT) {
            return false;
        }
        memcpy(&addr, &ip->ip_src, sizeof(addr));
        if (addr != s->server_addr ||
            !dns_redirect_lookup(s, lduw_he_p(&udp->uh_dport), &addr)) {
            return false;
        }
        memcpy(&ip->ip_src, &addr, sizeof(addr));
        fprintf(stderr, "dns-redirect: reply received from %s\n", s->server);
    }

    net_checksum_calculate(buf, size, CSUM_IP | CSUM_UDP);
    return true;
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
    if (!dns_redirect_rewrite(s, buf, size, sender != nf->netdev)) {
        return 0;
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

static void dns_redirect_fini(Object *obj)
{
    DnsRedirectState *s = FILTER_DNS_REDIRECT(obj);

    g_free(s->server);
}

static void dns_redirect_class_init(ObjectClass *oc, const void *data)
{
    NetFilterClass *nfc = NETFILTER_CLASS(oc);

    object_class_property_add_str(oc, "server", dns_redirect_get_server,
                                  dns_redirect_set_server);

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

/*
 * xemu Network Management
 *
 * Wrapper functions to configure network settings at runtime.
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "xemu-net.h"
#include "xemu-settings.h"

#include "qemu/osdep.h"
#include "qapi/qapi-commands-net.h"
#include "qemu/sockets.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "monitor/qdev.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "net/net.h"
#include "net/hub.h"
#include "net/slirp.h"
#include <libslirp.h>
#if defined(_WIN32)
#include <pcap/pcap.h>
#endif
#include "xemu-notifications.h"

static const char *id = "xemu-netdev";
static const char *id_hubport = "xemu-netdev-hubport";
static const char *id_dns_redirect = "xemu-dns-redirect";
static const char *id_pcap = "xemu-net-pcap";

static const char *get_dns_redirect_server(void)
{
    switch (g_config.net.dns_redirect) {
    case CONFIG_NET_DNS_REDIRECT_INSIGNIA:
        return "46.101.64.175";
    case CONFIG_NET_DNS_REDIRECT_XB_ONLINE:
        return "159.223.51.240";
    case CONFIG_NET_DNS_REDIRECT_CUSTOM:
        return g_config.net.dns_redirect_custom;
    default:
        return NULL;
    }
}

/*
 * Address of this computer on its local network: the source address used for
 * traffic to the internet. No packet is sent to find it.
 */
static bool get_lan_address(uint32_t *addr)
{
    struct sockaddr_in remote = { .sin_family = AF_INET,
                                  .sin_port = htons(53) };
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    bool ok;

    int fd = qemu_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    remote.sin_addr.s_addr = htonl(0x08080808);
    ok = connect(fd, (struct sockaddr *)&remote, sizeof(remote)) == 0 &&
         getsockname(fd, (struct sockaddr *)&local, &len) == 0;
    close(fd);
    if (!ok) {
        return false;
    }

    *addr = ntohl(local.sin_addr.s_addr);
    /* Not usable: unset, loopback and self-assigned addresses */
    return *addr != 0 && (*addr >> 24) != 127 && (*addr >> 16) != 0xa9fe;
}

/*
 * NAT network in which the console has the address this computer has on its
 * local network.
 *
 * Xbox Live gives players behind the same router each other's own address
 * instead of the public one. With the stock 10.0.2.15 nobody could reach a
 * game hosted here; with the computer's address they arrive at this computer
 * and the forwarded ports take them to the console.
 *
 * Addresses inside the NAT network never leave this computer, so it is kept
 * as small as possible: the console, the NAT gateway and nothing else.
 */
static bool get_lan_nat_network(char **net, char **host, char **dns,
                                struct in_addr *guest)
{
    uint32_t addr;

    if (!get_lan_address(&addr)) {
        return false;
    }

    for (int prefix = 30; prefix >= 24; prefix--) {
        uint32_t size = 1u << (32 - prefix);
        uint32_t base = addr & ~(size - 1);
        uint32_t gateway, resolver;

        if (addr == base || addr == base + size - 1) {
            continue; /* network or broadcast address at this size */
        }
        gateway = base + 1 != addr ? base + 1 : base + 2;
        if (size > 4) {
            resolver = gateway + 1 != addr ? gateway + 1 : gateway + 2;
        } else {
            resolver = 0x01010101; /* no room: use a public resolver */
        }

        guest->s_addr = htonl(addr);
        *net = g_strdup_printf("%u.%u.%u.%u/%d", base >> 24,
                               (base >> 16) & 255, (base >> 8) & 255,
                               base & 255, prefix);
        *host = g_strdup_printf("%u.%u.%u.%u", gateway >> 24,
                                (gateway >> 16) & 255, (gateway >> 8) & 255,
                                gateway & 255);
        *dns = g_strdup_printf("%u.%u.%u.%u", resolver >> 24,
                               (resolver >> 16) & 255, (resolver >> 8) & 255,
                               resolver & 255);
        return true;
    }
    return false;
}

void xemu_net_enable(void)
{
    Error *local_err = NULL;
    struct in_addr nat_guest_addr = { .s_addr = 0 };

    inet_aton("10.0.2.15", &nat_guest_addr);

    NetClientState *nc = qemu_find_netdev(id);
    if (nc != NULL) {
        return;
    }

    // Create the netdev
    QDict *qdict;
    if (g_config.net.backend == CONFIG_NET_BACKEND_NAT) {
        qdict = qdict_new();
        qdict_put_str(qdict, "id",   id);
        qdict_put_str(qdict, "type", "user");
        if (g_config.net.nat.lan_address) {
            char *net, *host, *dns;
            if (get_lan_nat_network(&net, &host, &dns, &nat_guest_addr)) {
                qdict_put_str(qdict, "net", net);
                qdict_put_str(qdict, "host", host);
                qdict_put_str(qdict, "dns", dns);
                qdict_put_str(qdict, "dhcpstart", inet_ntoa(nat_guest_addr));
                fprintf(stderr, "nat: console uses this computer's address "
                                "%s (network %s, gateway %s)\n",
                        inet_ntoa(nat_guest_addr), net, host);
                g_free(net);
                g_free(host);
                g_free(dns);
            } else {
                fprintf(stderr, "nat: no local network address found, "
                                "console uses 10.0.2.15\n");
            }
        }
    } else if (g_config.net.backend == CONFIG_NET_BACKEND_UDP) {
        qdict = qdict_new();
        qdict_put_str(qdict, "id",        id);
        qdict_put_str(qdict, "type",      "socket");
        qdict_put_str(qdict, "udp",       g_config.net.udp.remote_addr);
        qdict_put_str(qdict, "localaddr", g_config.net.udp.bind_addr);
    } else if (g_config.net.backend == CONFIG_NET_BACKEND_PCAP) {
#if defined(_WIN32)
        if (pcap_load_library()) {
            return;
        }
#endif
        qdict = qdict_new();
        qdict_put_str(qdict, "id",        id);
        qdict_put_str(qdict, "type",      "pcap");
        qdict_put_str(qdict, "ifname",    g_config.net.pcap.netif);
    } else {
        // Unsupported backend type
        return;
    }

    QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("netdev"), qdict, &error_abort);
    qobject_unref(qdict);
    netdev_add(opts, &local_err);
    if (local_err) {
        qemu_opts_del(opts);
        // error_propagate(errp, local_err);
        xemu_queue_error_message(error_get_pretty(local_err));
        error_report_err(local_err);
        return;
    }

    // Create the hubport
    qdict = qdict_new();
    qdict_put_str(qdict, "id",     id_hubport);
    qdict_put_str(qdict, "type",   "hubport");
    qdict_put_int(qdict, "hubid",  0);
    qdict_put_str(qdict, "netdev", id);
    opts = qemu_opts_from_qdict(qemu_find_opts("netdev"), qdict, &error_abort);
    qobject_unref(qdict);
    netdev_add(opts, &local_err);
    if (local_err) {
        qemu_opts_del(opts);
        // error_propagate(errp, local_err);
        xemu_queue_error_message(error_get_pretty(local_err));
        error_report_err(local_err);
        return;
    }

    if (g_config.net.backend == CONFIG_NET_BACKEND_NAT) {
        void *s = slirp_get_state_from_netdev(id);
        assert(s != NULL);

        struct in_addr host_addr = { .s_addr = INADDR_ANY };
        struct in_addr guest_addr = nat_guest_addr;

        for (int i = 0; i < g_config.net.nat.forward_ports_count; i++) {
            bool is_udp = g_config.net.nat.forward_ports[i].protocol ==
                          CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_UDP;
            int host_port = g_config.net.nat.forward_ports[i].host;
            int guest_port = g_config.net.nat.forward_ports[i].guest;

            if (slirp_add_hostfwd(s, is_udp, host_addr, host_port, guest_addr,
                                  guest_port) < 0) {
                error_setg(&local_err,
                           "Could not set host forwarding rule "
                           "%d -> %d (%s)\n",
                           host_port, guest_port, is_udp ? "udp" : "tcp");
                xemu_queue_error_message(error_get_pretty(local_err));
                break;
            }

        }
    }

    const char *dns_server = get_dns_redirect_server();
    if (!local_err && dns_server) {
        object_new_with_props("filter-dns-redirect", object_get_objects_root(),
                              id_dns_redirect, &local_err,
                              "netdev", id,
                              "queue", "all",
                              "server", dns_server,
                              NULL);
        if (local_err) {
            xemu_queue_error_message(error_get_pretty(local_err));
        }
    }

    /* Diagnostic: record all console traffic when XEMU_NET_PCAP names a file */
    const char *pcap_path = g_getenv("XEMU_NET_PCAP");
    if (!local_err && pcap_path && pcap_path[0]) {
        object_new_with_props("filter-dump", object_get_objects_root(),
                              id_pcap, &local_err,
                              "netdev", id,
                              "file", pcap_path,
                              NULL);
        if (local_err) {
            xemu_queue_error_message(error_get_pretty(local_err));
        }
    }

    if (local_err) {
        xemu_net_disable();
    }

    qmp_set_link("nvnet.0", true, NULL);
    g_config.net.enable = true;
}

static void remove_netdev(const char *name)
{
    NetClientState *nc;
    QemuOpts *opts;

    nc = qemu_find_netdev(name);
    if (!nc) {
        // error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
        //           "Device '%s' not found", name);
        return;
    }

    opts = qemu_opts_find(qemu_find_opts_err("netdev", NULL), name);
    if (!opts) {
        // error_setg(errp, "Device '%s' is not a netdev", name);
        return;
    }
    qemu_opts_del(opts);
    qemu_del_net_client(nc);
}

static void clear_slirp_port_forwards(void)
{
    void *s = slirp_get_state_from_netdev(id);
    if (!s) {
        return;
    }

    struct in_addr host_addr = { .s_addr = INADDR_ANY };
    for (int i = 0; i < g_config.net.nat.forward_ports_count; i++) {
        slirp_remove_hostfwd(s,
                             g_config.net.nat.forward_ports[i].protocol ==
                                 CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_UDP,
                             host_addr,
                             g_config.net.nat.forward_ports[i].host);
    }
}

void xemu_net_disable(void)
{
    if (g_config.net.backend == CONFIG_NET_BACKEND_NAT) {
        clear_slirp_port_forwards();
    }

    Object *dns_redirect =
        object_resolve_path_component(object_get_objects_root(),
                                      id_dns_redirect);
    if (dns_redirect) {
        object_unparent(dns_redirect);
    }

    Object *pcap = object_resolve_path_component(object_get_objects_root(),
                                                 id_pcap);
    if (pcap) {
        object_unparent(pcap);
    }

    remove_netdev(id);
    remove_netdev(id_hubport);

    qmp_set_link("nvnet.0", false, NULL);
    g_config.net.enable = false;
}

int xemu_net_is_enabled(void)
{
    NetClientState *nc;
    nc = qemu_find_netdev(id);
    g_config.net.enable = (nc != NULL);
    return g_config.net.enable;
}

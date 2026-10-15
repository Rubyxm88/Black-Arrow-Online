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

#ifndef XEMU_NETWORK_H
#define XEMU_NETWORK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void xemu_net_enable(void);
void xemu_net_disable(void);
int xemu_net_is_enabled(void);

/* DNS server the console's lookups are sent to, or NULL if not redirected */
const char *xemu_net_dns_redirect_server(void);

/*
 * Address this computer uses for traffic to the internet, which the console
 * takes in NAT mode. False if there is none.
 */
bool xemu_net_lan_address(uint32_t *addr);

#ifdef __cplusplus
}
#endif

#endif

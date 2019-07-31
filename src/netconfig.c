/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2019  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <arpa/inet.h>
#include <linux/rtnetlink.h>
#include <linux/if_arp.h>
#include <linux/in.h>

#include <ell/ell.h>

#include "src/iwd.h"
#include "src/netdev.h"
#include "src/station.h"
#include "src/common.h"
#include "src/network.h"
#include "src/rtnlutil.h"
#include "src/resolve.h"
#include "src/netconfig.h"

struct netconfig {
	uint32_t ifindex;
	enum station_state station_state;
	struct l_dhcp_client *dhcp_client;
	struct l_queue *ifaddr_list;
	bool ipv4_is_static:1;
};

struct netconfig_ifaddr {
	uint8_t proto;
	uint8_t family;
	uint8_t prefix_len;
	char *ip;
	char *broadcast;
};

static struct l_netlink *rtnl;
static struct l_queue *netconfig_list;

static void do_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	l_info("%s%s", prefix, str);
}

static void netconfig_ifaddr_destroy(void *data)
{
	struct netconfig_ifaddr *ifaddr = data;

	l_free(ifaddr->ip);
	l_free(ifaddr->broadcast);

	l_free(ifaddr);
}

static void netconfig_destroy(void *data)
{
	struct netconfig *netconfig = data;

	l_dhcp_client_destroy(netconfig->dhcp_client);

	l_queue_destroy(netconfig->ifaddr_list, netconfig_ifaddr_destroy);

	l_free(netconfig);
}

static bool netconfig_match(const void *a, const void *b)
{
	const struct netconfig *netconfig = a;
	uint32_t ifindex = L_PTR_TO_UINT(b);

	if (netconfig->ifindex == ifindex)
		return true;

	return false;
}

static struct netconfig *netconfig_find(uint32_t ifindex)
{
	const struct l_queue_entry *entry;

	for (entry = l_queue_get_entries(netconfig_list); entry;
							entry = entry->next) {
		struct netconfig *netconfig = entry->data;

		if (netconfig->ifindex != ifindex)
			continue;

		return netconfig;
	}

	return NULL;
}

static struct l_settings *netconfig_get_connected_network_settings(
						struct netconfig *netconfig)
{
	struct station *station;
	const struct network *network;

	station = station_find(netconfig->ifindex);
	if (!station)
		return NULL;

	network = station_get_connected_network(station);
	if (!network)
		return NULL;

	return network_get_settings(network);
}

static struct netconfig_ifaddr *netconfig_ipv4_get_ifaddr(
						struct netconfig *netconfig,
						uint8_t proto)
{
	const struct l_dhcp_lease *lease;
	const struct l_settings *settings;
	struct netconfig_ifaddr *ifaddr;
	struct in_addr in_addr;
	char *netmask;
	char *ip;

	switch (proto) {
	case RTPROT_STATIC:
		settings = netconfig_get_connected_network_settings(netconfig);
		if (!settings)
			return NULL;

		ip = l_settings_get_string(settings, "IPv4", "ip");
		if (!ip)
			return NULL;

		netconfig->ipv4_is_static = true;

		ifaddr = l_new(struct netconfig_ifaddr, 1);
		ifaddr->ip = ip;

		netmask = l_settings_get_string(settings, "IPv4", "netmask");
		if (netmask && inet_pton(AF_INET, netmask, &in_addr) > 0)
			ifaddr->prefix_len = __builtin_popcountl(
						L_BE32_TO_CPU(in_addr.s_addr));
		else
			ifaddr->prefix_len = 24;

		l_free(netmask);

		ifaddr->broadcast = l_settings_get_string(settings, "IPv4",
								"broadcast");
		ifaddr->family = AF_INET;

		return ifaddr;

	case RTPROT_DHCP:
		lease = l_dhcp_client_get_lease(netconfig->dhcp_client);
		if (!lease)
			return NULL;

		ip = l_dhcp_lease_get_address(lease);
		if (!ip)
			return NULL;

		ifaddr = l_new(struct netconfig_ifaddr, 1);
		ifaddr->ip = ip;

		netmask = l_dhcp_lease_get_netmask(lease);

		if (netmask && inet_pton(AF_INET, netmask, &in_addr) > 0)
			ifaddr->prefix_len = __builtin_popcountl(
						L_BE32_TO_CPU(in_addr.s_addr));
		else
			ifaddr->prefix_len = 24;

		l_free(netmask);

		ifaddr->broadcast = l_dhcp_lease_get_broadcast(lease);
		ifaddr->family = AF_INET;

		return ifaddr;
	}

	return NULL;
}

static struct netconfig_ifaddr *netconfig_ifaddr_find(
					const struct netconfig *netconfig,
					uint8_t family, uint8_t prefix_len,
					const char *ip)
{
	const struct l_queue_entry *entry;

	for (entry = l_queue_get_entries(netconfig->ifaddr_list); entry;
							entry = entry->next) {
		struct netconfig_ifaddr *ifaddr = entry->data;

		if (ifaddr->family != family)
			continue;

		if (ifaddr->prefix_len != prefix_len)
			continue;

		if (strcmp(ifaddr->ip, ip))
			continue;

		return ifaddr;
	}

	return NULL;
}

static void netconfig_ifaddr_added(struct netconfig *netconfig,
					const struct ifaddrmsg *ifa,
					uint32_t len)
{
	struct netconfig_ifaddr *ifaddr;
	char *label;

	ifaddr = l_new(struct netconfig_ifaddr, 1);
	ifaddr->family = ifa->ifa_family;
	ifaddr->prefix_len = ifa->ifa_prefixlen;

	rtnl_ifaddr_extract(ifa, len, &label, &ifaddr->ip,
							&ifaddr->broadcast);

	l_debug("%s: ifaddr %s/%u broadcast %s", label, ifaddr->ip,
					ifaddr->prefix_len, ifaddr->broadcast);
	l_free(label);

	l_queue_push_tail(netconfig->ifaddr_list, ifaddr);
}

static void netconfig_ifaddr_deleted(struct netconfig *netconfig,
					const struct ifaddrmsg *ifa,
					uint32_t len)
{
	struct netconfig_ifaddr *ifaddr;
	char *ip;

	rtnl_ifaddr_extract(ifa, len, NULL, &ip, NULL);

	ifaddr = netconfig_ifaddr_find(netconfig, ifa->ifa_family,
							ifa->ifa_prefixlen, ip);

	l_free(ip);

	if (!ifaddr)
		return;

	l_debug("ifaddr %s/%u", ifaddr->ip, ifaddr->prefix_len);

	l_queue_remove(netconfig->ifaddr_list, ifaddr);

	netconfig_ifaddr_destroy(ifaddr);
}

static void netconfig_ifaddr_notify(uint16_t type, const void *data,
						uint32_t len, void *user_data)
{
	const struct ifaddrmsg *ifa = data;
	struct netconfig *netconfig;
	uint32_t bytes;

	netconfig = netconfig_find(ifa->ifa_index);
	if (!netconfig)
		/* Ignore the interfaces which aren't managed by iwd. */
		return;

	bytes = len - NLMSG_ALIGN(sizeof(struct ifaddrmsg));

	switch (type) {
	case RTM_NEWADDR:
		netconfig_ifaddr_added(netconfig, ifa, bytes);
		break;
	case RTM_DELADDR:
		netconfig_ifaddr_deleted(netconfig, ifa, bytes);
		break;
	}
}

static void netconfig_ifaddr_cmd_cb(int error, uint16_t type,
						const void *data, uint32_t len,
						void *user_data)
{
	if (error) {
		l_error("netconfig: ifaddr command failure. "
				"Error %d: %s", error, strerror(-error));
		return;
	}

	if (type != RTM_NEWADDR)
		return;

	netconfig_ifaddr_notify(type, data, len, user_data);
}

static bool netconfig_ifaddr_remove(void *data, void *user_data)
{
	struct netconfig *netconfig = user_data;
	struct netconfig_ifaddr *ifaddr = data;

	switch (ifaddr->family) {
	case AF_INET:
		if (rtnl_ifaddr_delete(rtnl, netconfig->ifindex,
					ifaddr->prefix_len, ifaddr->ip,
					ifaddr->broadcast,
					netconfig_ifaddr_cmd_cb, NULL, NULL))
			break;

		l_error("netconfig: Failed to remove ifaddr %s from "
				"interface %u", ifaddr->ip, netconfig->ifindex);
		break;
	default:
		l_error("netconfig: Unsupported address family: %u",
								ifaddr->family);
		break;
	}

	netconfig_ifaddr_destroy(ifaddr);

	return true;
}

static bool netconfig_install_addresses(struct netconfig *netconfig,
					const struct netconfig_ifaddr *ifaddr,
					const char *gateway, char **dns)
{
	if (netconfig_ifaddr_find(netconfig, ifaddr->family, ifaddr->prefix_len,
								ifaddr->ip))
		/* The address is already installed. */
		goto gateway;

	switch (ifaddr->family) {
	case AF_INET:
		if (rtnl_ifaddr_add(rtnl, netconfig->ifindex,
					ifaddr->prefix_len, ifaddr->ip,
					ifaddr->broadcast,
					netconfig_ifaddr_cmd_cb, netconfig,
					NULL))
			break;

		l_error("netconfig: Failed to set IP %s/%u.", ifaddr->ip,
							ifaddr->prefix_len);
		return false;
	default:
		l_error("netconfig: Unsupported address family: %u",
								ifaddr->family);
		break;
	}

gateway:
	/* TODO: Add the routes. */

	resolve_add_dns(netconfig->ifindex, ifaddr->family, dns);

	return true;
}

static bool netconfig_uninstall_addresses(struct netconfig *netconfig,
					const struct netconfig_ifaddr *ifaddr,
					const char *gateway, char **dns)
{

	if (!netconfig_ifaddr_find(netconfig, ifaddr->family,
						ifaddr->prefix_len, ifaddr->ip))
		/* The address is already removed. */
		goto gateway;

	if (!netconfig_ifaddr_remove(netconfig, (void *) ifaddr)) {
		l_error("netconfig: Failed to remove IP %s/%u.", ifaddr->ip,
							ifaddr->prefix_len);
		return false;
	}

gateway:
	/* TODO: Remove the routes. */

	resolve_remove(netconfig->ifindex);

	return true;
}

static void netconfig_ipv4_dhcp_event_handler(struct l_dhcp_client *client,
						enum l_dhcp_client_event event,
						void *userdata)
{
	struct netconfig *netconfig = userdata;
	struct netconfig_ifaddr *ifaddr;

	l_debug("DHCPv4 event %d", event);

	switch (event) {
	case L_DHCP_CLIENT_EVENT_LEASE_RENEWED:
	case L_DHCP_CLIENT_EVENT_LEASE_OBTAINED:
	case L_DHCP_CLIENT_EVENT_IP_CHANGED:
		ifaddr = netconfig_ipv4_get_ifaddr(netconfig, RTPROT_DHCP);
		if (!ifaddr) {
			l_error("netconfig: Failed to obtain IP addresses from "
							"DHCPv4 lease.");
			return;
		}

		/* TODO Install address */

		netconfig_ifaddr_destroy(ifaddr);

		break;
	case L_DHCP_CLIENT_EVENT_LEASE_EXPIRED:
		ifaddr = netconfig_ipv4_get_ifaddr(netconfig, RTPROT_DHCP);
		if (!ifaddr) {
			l_error("netconfig: Failed to obtain IP addresses from "
							"DHCPv4 lease.");
			return;
		}

		/* TODO Uninstall address */

		netconfig_ifaddr_destroy(ifaddr);

		/* Fall through. */
	case L_DHCP_CLIENT_EVENT_NO_LEASE:
		/*
		 * The requested address is no longer available, try to restart
		 * the client.
		 */
		if (!l_dhcp_client_start(client))
			l_error("netconfig: Failed to re-start DHCPv4 client "
					"for interface %u", netconfig->ifindex);

		break;
	default:
		l_error("netconfig: Received unsupported DHCPv4 event: %d",
									event);
	}
}

static bool netconfig_ipv4_dhcp_create(struct netconfig *netconfig,
							struct station *station)
{
	netconfig->dhcp_client = l_dhcp_client_new(netconfig->ifindex);

	l_dhcp_client_set_address(netconfig->dhcp_client, ARPHRD_ETHER,
					netdev_get_address(
						station_get_netdev(station)),
					ETH_ALEN);

	l_dhcp_client_set_event_handler(netconfig->dhcp_client,
					netconfig_ipv4_dhcp_event_handler,
					netconfig, NULL);

	if (getenv("IWD_DHCP_DEBUG"))
		l_dhcp_client_set_debug(netconfig->dhcp_client, do_debug,
							"[DHCPv4] ", NULL);

	return true;
}

static void netconfig_ipv4_select_and_install(struct netconfig *netconfig)
{
	struct netconfig_ifaddr *ifaddr;

	ifaddr = netconfig_ipv4_get_ifaddr(netconfig, RTPROT_STATIC);
	if (ifaddr) {
		/* TODO Install address */
		netconfig_ifaddr_destroy(ifaddr);

		return;
	}

	if (netconfig->station_state == STATION_STATE_ROAMING) {
		/*
		 * TODO l_dhcp_client to try to request a
		 * previously used address.
		 *
		 * return;
		 */
	}

	if (l_dhcp_client_start(netconfig->dhcp_client))
		return;

	l_error("netconfig: Failed to start DHCPv4 client for interface %u",
							netconfig->ifindex);
}

static void netconfig_ipv4_select_and_uninstall(struct netconfig *netconfig)
{
	struct netconfig_ifaddr *ifaddr;

	ifaddr = netconfig_ipv4_get_ifaddr(netconfig, RTPROT_STATIC);
	if (ifaddr) {
		/* TODO Uninstall address */
		netconfig_ifaddr_destroy(ifaddr);

		return;
	}

	ifaddr = netconfig_ipv4_get_ifaddr(netconfig, RTPROT_DHCP);
	if (!ifaddr)
		return;

	/* TODO Uninstall address */
	netconfig_ifaddr_destroy(ifaddr);

	l_dhcp_client_stop(netconfig->dhcp_client);
}

static void netconfig_station_state_changed(enum station_state state,
								void *userdata)
{
	struct netconfig *netconfig = userdata;

	l_debug("");

	switch (state) {
	case STATION_STATE_CONNECTED:
		netconfig_ipv4_select_and_install(netconfig);

		/* TODO: IPv6 addressing */

		break;
	case STATION_STATE_DISCONNECTED:
		netconfig_ipv4_select_and_uninstall(netconfig);

		/* TODO: IPv6 addressing */

		break;
	case STATION_STATE_ROAMING:
		break;
	default:
		return;
	}

	netconfig->station_state = state;
}

bool netconfig_ifindex_add(uint32_t ifindex)
{
	struct netconfig *netconfig;
	struct station *station;

	if (!netconfig_list)
		return false;

	l_debug("Starting netconfig for interface: %d", ifindex);

	netconfig = netconfig_find(ifindex);
	if (netconfig)
		return true;

	station = station_find(ifindex);
	if (!station)
		return false;

	netconfig = l_new(struct netconfig, 1);
	netconfig->ifindex = ifindex;
	netconfig->ifaddr_list = l_queue_new();

	netconfig_ipv4_dhcp_create(netconfig, station);

	station_add_state_watch(station, netconfig_station_state_changed,
							netconfig, NULL);

	l_queue_push_tail(netconfig_list, netconfig);

	return true;
}

bool netconfig_ifindex_remove(uint32_t ifindex)
{
	struct netconfig *netconfig;

	if (!netconfig_list)
		return false;

	l_debug();

	netconfig = l_queue_remove_if(netconfig_list, netconfig_match,
							L_UINT_TO_PTR(ifindex));
	if (!netconfig)
		return false;

	netconfig_destroy(netconfig);

	return true;
}

static int netconfig_init(void)
{
	bool enabled;
	uint32_t r;

	if (netconfig_list)
		return -EALREADY;

	if (!l_settings_get_bool(iwd_get_config(), "General",
					"enable_network_config", &enabled) ||
								!enabled) {
		l_warn("netconfig: Network configuration with the IP addresses "
								"is disabled.");
		return false;
	}

	rtnl = l_netlink_new(NETLINK_ROUTE);
	if (!rtnl) {
		l_error("netconfig: Failed to open route netlink socket");
		return -EPERM;
	}

	if (getenv("IWD_RTNL_DEBUG"))
		l_netlink_set_debug(rtnl, do_debug, "[NETCONFIG RTNL] ", NULL);

	r = l_netlink_register(rtnl, RTNLGRP_IPV4_IFADDR,
					netconfig_ifaddr_notify, NULL, NULL);
	if (!r) {
		l_error("netconfig: Failed to register for RTNL link address"
							" notifications.");
		goto error;
	}

	r = rtnl_ifaddr_get(rtnl, netconfig_ifaddr_cmd_cb, NULL, NULL);
	if (!r) {
		l_error("netconfig: Failed to get addresses from RTNL link.");
		goto error;
	}

	netconfig_list = l_queue_new();

	return 0;

error:
	l_netlink_destroy(rtnl);
	rtnl = NULL;

	return r;
}

static void netconfig_exit(void)
{
	if (!netconfig_list)
		return;

	l_netlink_destroy(rtnl);
	rtnl = NULL;

	l_queue_destroy(netconfig_list, netconfig_destroy);
}

IWD_MODULE(netconfig, netconfig_init, netconfig_exit)

/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Tom Gundersen <teg@jklm.no>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <netinet/ether.h>
#include <linux/if.h>

#include "networkd.h"
#include "libudev-private.h"
#include "udev-util.h"
#include "util.h"
#include "virt.h"
#include "bus-util.h"
#include "network-internal.h"

#include "dhcp-lease-internal.h"

static int ipv4ll_address_update(Link *link, bool deprecate);
static bool ipv4ll_is_bound(sd_ipv4ll *ll);

static int link_new(Manager *manager, sd_rtnl_message *message, Link **ret) {
        _cleanup_link_free_ Link *link = NULL;
        uint16_t type;
        char *ifname;
        int r, ifindex;

        assert(manager);
        assert(manager->links);
        assert(message);
        assert(ret);

        r = sd_rtnl_message_get_type(message, &type);
        if (r < 0)
                return r;
        else if (type != RTM_NEWLINK)
                return -EINVAL;

        r = sd_rtnl_message_link_get_ifindex(message, &ifindex);
        if (r < 0)
                return r;
        else if (ifindex <= 0)
                return -EINVAL;

        r = sd_rtnl_message_read_string(message, IFLA_IFNAME, &ifname);
        if (r < 0)
                return r;

        link = new0(Link, 1);
        if (!link)
                return -ENOMEM;

        link->manager = manager;
        link->state = LINK_STATE_INITIALIZING;
        link->ifindex = ifindex;
        link->ifname = strdup(ifname);
        if (!link->ifname)
                return -ENOMEM;

        r = asprintf(&link->state_file, "/run/systemd/network/links/%"PRIu64,
                     link->ifindex);
        if (r < 0)
                return -ENOMEM;

        r = hashmap_put(manager->links, &link->ifindex, link);
        if (r < 0)
                return r;

        *ret = link;
        link = NULL;

        return 0;
}

void link_free(Link *link) {
        if (!link)
                return;

        assert(link->manager);

        sd_dhcp_client_unref(link->dhcp_client);
        sd_dhcp_lease_unref(link->dhcp_lease);

        sd_ipv4ll_unref(link->ipv4ll);

        hashmap_remove(link->manager->links, &link->ifindex);

        free(link->ifname);
        free(link->state_file);

        udev_device_unref(link->udev_device);

        free(link);
}

int link_get(Manager *m, int ifindex, Link **ret) {
        Link *link;
        uint64_t ifindex_64;

        assert(m);
        assert(m->links);
        assert(ifindex);
        assert(ret);

        ifindex_64 = ifindex;
        link = hashmap_get(m->links, &ifindex_64);
        if (!link)
                return -ENODEV;

        *ret = link;

        return 0;
}

static int link_enter_configured(Link *link) {
        assert(link);
        assert(link->state == LINK_STATE_SETTING_ROUTES);

        log_info_link(link, "link configured");

        link->state = LINK_STATE_CONFIGURED;

        link_save(link);

        return 0;
}

static void link_enter_failed(Link *link) {
        assert(link);

        log_warning_link(link, "failed");

        link->state = LINK_STATE_FAILED;

        link_save(link);
}

static int route_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(link->route_messages > 0);
        assert(link->state == LINK_STATE_SETTING_ADDRESSES ||
               link->state == LINK_STATE_SETTING_ROUTES ||
               link->state == LINK_STATE_FAILED);

        link->route_messages --;

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0 && r != -EEXIST)
                log_struct_link(LOG_WARNING, link,
                                "MESSAGE=%s: could not set route: %s",
                                link->ifname, strerror(-r),
                                "ERRNO=%d", -r,
                                NULL);

        /* we might have received an old reply after moving back to SETTING_ADDRESSES,
         * ignore it */
        if (link->route_messages == 0 && link->state == LINK_STATE_SETTING_ROUTES) {
                log_debug_link(link, "routes set");
                link_enter_configured(link);
        }

        return 1;
}

static int link_enter_set_routes(Link *link) {
        Route *rt;
        int r;

        assert(link);
        assert(link->network);
        assert(link->state == LINK_STATE_SETTING_ADDRESSES);

        link->state = LINK_STATE_SETTING_ROUTES;

        if (!link->network->static_routes && !link->dhcp_lease &&
                (!link->ipv4ll || ipv4ll_is_bound(link->ipv4ll) == false))
                return link_enter_configured(link);

        log_debug_link(link, "setting routes");

        LIST_FOREACH(static_routes, rt, link->network->static_routes) {
                r = route_configure(rt, link, &route_handler);
                if (r < 0) {
                        log_warning_link(link,
                                         "could not set routes: %s", strerror(-r));
                        link_enter_failed(link);
                        return r;
                }

                link->route_messages ++;
        }

        if (link->ipv4ll && !link->dhcp_lease) {
                _cleanup_route_free_ Route *route = NULL;
                struct in_addr addr;

                r = sd_ipv4ll_get_address(link->ipv4ll, &addr);
                if (r < 0 && r != -ENOENT) {
                        log_warning_link(link, "IPV4LL error: no address: %s",
                                        strerror(-r));
                        return r;
                }

                if (r != -ENOENT) {
                        r = route_new_dynamic(&route);
                        if (r < 0) {
                                log_error_link(link, "Could not allocate route: %s",
                                               strerror(-r));
                                return r;
                        }

                        route->family = AF_INET;
                        route->scope = RT_SCOPE_LINK;
                        route->metrics = 99;

                        r = route_configure(route, link, &route_handler);
                        if (r < 0) {
                                log_warning_link(link,
                                                 "could not set routes: %s", strerror(-r));
                                link_enter_failed(link);
                                return r;
                        }

                        link->route_messages ++;
                }
        }

        if (link->dhcp_lease) {
                _cleanup_route_free_ Route *route = NULL;
                _cleanup_route_free_ Route *route_gw = NULL;
                struct in_addr gateway;

                r = sd_dhcp_lease_get_router(link->dhcp_lease, &gateway);
                if (r < 0) {
                        log_warning_link(link, "DHCP error: no router: %s",
                                         strerror(-r));
                        return r;
                }

                r = route_new_dynamic(&route);
                if (r < 0) {
                        log_error_link(link, "Could not allocate route: %s",
                                       strerror(-r));
                        return r;
                }

                r = route_new_dynamic(&route_gw);
                if (r < 0) {
                        log_error_link(link, "Could not allocate route: %s",
                                       strerror(-r));
                        return r;
                }

                /* The dhcp netmask may mask out the gateway. Add an explicit
                 * route for the gw host so that we can route no matter the
                 * netmask or existing kernel route tables. */
                route_gw->family = AF_INET;
                route_gw->dst_addr.in = gateway;
                route_gw->dst_prefixlen = 32;
                route_gw->scope = RT_SCOPE_LINK;

                r = route_configure(route_gw, link, &route_handler);
                if (r < 0) {
                        log_warning_link(link,
                                         "could not set host route: %s", strerror(-r));
                        return r;
                }

                link->route_messages ++;

                route->family = AF_INET;
                route->in_addr.in = gateway;

                r = route_configure(route, link, &route_handler);
                if (r < 0) {
                        log_warning_link(link,
                                         "could not set routes: %s", strerror(-r));
                        link_enter_failed(link);
                        return r;
                }

                link->route_messages ++;
        }

        return 0;
}

static int route_drop_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(m);
        assert(link);
        assert(link->ifname);

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0 && r != -ENOENT)
                log_struct_link(LOG_WARNING, link,
                                "MESSAGE=%s: could not drop route: %s",
                                link->ifname, strerror(-r),
                                "ERRNO=%d", -r,
                                NULL);

        return 0;
}

static int address_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(m);
        assert(link);
        assert(link->ifname);
        assert(link->addr_messages > 0);
        assert(link->state == LINK_STATE_SETTING_ADDRESSES || link->state == LINK_STATE_FAILED);

        link->addr_messages --;

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0 && r != -EEXIST)
                log_struct_link(LOG_WARNING, link,
                                "MESSAGE=%s: could not set address: %s",
                                link->ifname, strerror(-r),
                                "ERRNO=%d", -r,
                                NULL);

        if (link->addr_messages == 0) {
                log_debug_link(link, "addresses set");
                link_enter_set_routes(link);
        }

        return 1;
}

static int link_enter_set_addresses(Link *link) {
        Address *ad;
        int r;

        assert(link);
        assert(link->network);
        assert(link->state != _LINK_STATE_INVALID);

        link->state = LINK_STATE_SETTING_ADDRESSES;

        if (!link->network->static_addresses && !link->dhcp_lease &&
                (!link->ipv4ll || ipv4ll_is_bound(link->ipv4ll) == false))
                return link_enter_set_routes(link);

        log_debug_link(link, "setting addresses");

        LIST_FOREACH(static_addresses, ad, link->network->static_addresses) {
                r = address_configure(ad, link, &address_handler);
                if (r < 0) {
                        log_warning_link(link,
                                         "could not set addresses: %s", strerror(-r));
                        link_enter_failed(link);
                        return r;
                }

                link->addr_messages ++;
        }

        if (link->ipv4ll && !link->dhcp_lease) {
                _cleanup_address_free_ Address *ll_addr = NULL;
                struct in_addr addr;

                r = sd_ipv4ll_get_address(link->ipv4ll, &addr);
                if (r < 0 && r != -ENOENT) {
                        log_warning_link(link, "IPV4LL error: no address: %s",
                                        strerror(-r));
                        return r;
                }

                if (r != -ENOENT) {
                        r = address_new_dynamic(&ll_addr);
                        if (r < 0) {
                                log_error_link(link, "Could not allocate address: %s", strerror(-r));
                                return r;
                        }

                        ll_addr->family = AF_INET;
                        ll_addr->in_addr.in = addr;
                        ll_addr->prefixlen = 16;
                        ll_addr->broadcast.s_addr = ll_addr->in_addr.in.s_addr | htonl(0xfffffffflu >> ll_addr->prefixlen);
                        ll_addr->scope = RT_SCOPE_LINK;

                        r = address_configure(ll_addr, link, &address_handler);
                        if (r < 0) {
                                log_warning_link(link,
                                         "could not set addresses: %s", strerror(-r));
                                link_enter_failed(link);
                                return r;
                        }

                        link->addr_messages ++;
                }
        }

        if (link->dhcp_lease) {
                _cleanup_address_free_ Address *address = NULL;
                struct in_addr addr;
                struct in_addr netmask;
                unsigned prefixlen;

                r = sd_dhcp_lease_get_address(link->dhcp_lease, &addr);
                if (r < 0) {
                        log_warning_link(link, "DHCP error: no address: %s",
                                         strerror(-r));
                        return r;
                }

                r = sd_dhcp_lease_get_netmask(link->dhcp_lease, &netmask);
                if (r < 0) {
                        log_warning_link(link, "DHCP error: no netmask: %s",
                                         strerror(-r));
                        return r;
                }

                prefixlen = net_netmask_to_prefixlen(&netmask);

                r = address_new_dynamic(&address);
                if (r < 0) {
                        log_error_link(link, "Could not allocate address: %s",
                                       strerror(-r));
                        return r;
                }

                address->family = AF_INET;
                address->in_addr.in = addr;
                address->prefixlen = prefixlen;
                address->broadcast.s_addr = addr.s_addr | ~netmask.s_addr;

                r = address_configure(address, link, &address_handler);
                if (r < 0) {
                        log_warning_link(link,
                                         "could not set addresses: %s", strerror(-r));
                        link_enter_failed(link);
                        return r;
                }

                link->addr_messages ++;
        }

        return 0;
}

static int address_update_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(m);
        assert(link);
        assert(link->ifname);

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0 && r != -ENOENT)
                log_struct_link(LOG_WARNING, link,
                                "MESSAGE=%s: could not update address: %s",
                                link->ifname, strerror(-r),
                                "ERRNO=%d", -r,
                                NULL);

        return 0;
}

static int address_drop_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(m);
        assert(link);
        assert(link->ifname);

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0 && r != -ENOENT)
                log_struct_link(LOG_WARNING, link,
                                "MESSAGE=%s: could not drop address: %s",
                                link->ifname, strerror(-r),
                                "ERRNO=%d", -r,
                                NULL);

        return 0;
}

static int set_hostname_handler(sd_bus *bus, sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
        int r;

        r = sd_bus_message_get_errno(m);
        if (r < 0)
                log_warning("Could not set hostname: %s", strerror(-r));

        return 1;
}

static int set_hostname(sd_bus *bus, const char *hostname) {
        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r = 0;

        assert(hostname);

        log_debug("Setting transient hostname: '%s'", hostname);

        if (!bus) { /* TODO: replace by assert when we can rely on kdbus */
                log_info("Not connected to system bus, ignoring transient hostname.");
                return 0;
        }

        r = sd_bus_message_new_method_call(
                        bus,
                        &m,
                        "org.freedesktop.hostname1",
                        "/org/freedesktop/hostname1",
                        "org.freedesktop.hostname1",
                        "SetHostname");
        if (r < 0)
                return r;

        r = sd_bus_message_append(m, "sb", hostname, false);
        if (r < 0)
                return r;

        r = sd_bus_call_async(bus, m, set_hostname_handler, NULL, 0, NULL);
        if (r < 0)
                log_error("Could not set transient hostname: %s", strerror(-r));

        return r;
}

static int set_mtu_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(m);
        assert(link);
        assert(link->ifname);

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0)
                log_struct_link(LOG_WARNING, link,
                                "MESSAGE=%s: could not set MTU: %s",
                                link->ifname, strerror(-r),
                                "ERRNO=%d", -r,
                                NULL);

        return 1;
}

static int link_set_mtu(Link *link, uint32_t mtu) {
        _cleanup_rtnl_message_unref_ sd_rtnl_message *req = NULL;
        int r;

        assert(link);
        assert(link->manager);
        assert(link->manager->rtnl);

        log_debug_link(link, "setting MTU: %" PRIu32, mtu);

        r = sd_rtnl_message_new_link(link->manager->rtnl, &req,
                                     RTM_SETLINK, link->ifindex);
        if (r < 0) {
                log_error_link(link, "Could not allocate RTM_SETLINK message");
                return r;
        }

        r = sd_rtnl_message_append_u32(req, IFLA_MTU, mtu);
        if (r < 0) {
                log_error_link(link, "Could not append MTU: %s", strerror(-r));
                return r;
        }

        r = sd_rtnl_call_async(link->manager->rtnl, req, set_mtu_handler, link, 0, NULL);
        if (r < 0) {
                log_error_link(link,
                               "Could not send rtnetlink message: %s", strerror(-r));
                return r;
        }

        return 0;
}

static int dhcp_lease_lost(Link *link) {
        _cleanup_address_free_ Address *address = NULL;
        _cleanup_route_free_ Route *route_gw = NULL;
        _cleanup_route_free_ Route *route = NULL;
        struct in_addr addr;
        struct in_addr netmask;
        struct in_addr gateway;
        unsigned prefixlen;
        int r;

        assert(link);
        assert(link->dhcp_lease);

        log_warning_link(link, "DHCP lease lost");

        r = address_new_dynamic(&address);
        if (r >= 0) {
                sd_dhcp_lease_get_address(link->dhcp_lease, &addr);
                sd_dhcp_lease_get_netmask(link->dhcp_lease, &netmask);
                sd_dhcp_lease_get_router(link->dhcp_lease, &gateway);
                prefixlen = net_netmask_to_prefixlen(&netmask);

                r = route_new_dynamic(&route_gw);
                if (r >= 0) {
                        route_gw->family = AF_INET;
                        route_gw->dst_addr.in = gateway;
                        route_gw->dst_prefixlen = 32;
                        route_gw->scope = RT_SCOPE_LINK;

                        route_drop(route_gw, link, &route_drop_handler);
                }

                r = route_new_dynamic(&route);
                if (r >= 0) {
                        route->family = AF_INET;
                        route->in_addr.in = gateway;

                        route_drop(route, link, &route_drop_handler);
                }

                address->family = AF_INET;
                address->in_addr.in = addr;
                address->prefixlen = prefixlen;

                address_drop(address, link, &address_drop_handler);
        }

        if (link->network->dhcp_mtu) {
                uint16_t mtu;

                r = sd_dhcp_lease_get_mtu(link->dhcp_lease, &mtu);
                if (r >= 0 && link->original_mtu != mtu) {
                        r = link_set_mtu(link, link->original_mtu);
                        if (r < 0) {
                                log_warning_link(link, "DHCP error: could not reset MTU");
                                link_enter_failed(link);
                                return r;
                        }
                }
        }

        if (link->network->dhcp_hostname) {
                const char *hostname = NULL;

                r = sd_dhcp_lease_get_hostname(link->dhcp_lease, &hostname);
                if (r >= 0 && hostname) {
                        r = set_hostname(link->manager->bus, "");
                        if (r < 0)
                                log_error("Failed to reset transient hostname");
                }
        }

        link->dhcp_lease = sd_dhcp_lease_unref(link->dhcp_lease);

        return 0;
}

static int dhcp_lease_acquired(sd_dhcp_client *client, Link *link) {
        sd_dhcp_lease *lease;
        struct in_addr address;
        struct in_addr netmask;
        struct in_addr gateway;
        unsigned prefixlen;
        struct in_addr *nameservers;
        size_t nameservers_size;
        int r;

        assert(client);
        assert(link);

        r = sd_dhcp_client_get_lease(client, &lease);
        if (r < 0) {
                log_warning_link(link, "DHCP error: no lease: %s",
                                 strerror(-r));
                return r;
        }

        r = sd_dhcp_lease_get_address(lease, &address);
        if (r < 0) {
                log_warning_link(link, "DHCP error: no address: %s",
                                 strerror(-r));
                return r;
        }

        r = sd_dhcp_lease_get_netmask(lease, &netmask);
        if (r < 0) {
                log_warning_link(link, "DHCP error: no netmask: %s",
                                 strerror(-r));
                return r;
        }

        prefixlen = net_netmask_to_prefixlen(&netmask);

        r = sd_dhcp_lease_get_router(lease, &gateway);
        if (r < 0) {
                log_warning_link(link, "DHCP error: no router: %s",
                                 strerror(-r));
                return r;
        }

        log_struct_link(LOG_INFO, link,
                        "MESSAGE=%s: DHCPv4 address %u.%u.%u.%u/%u via %u.%u.%u.%u",
                         link->ifname,
                         ADDRESS_FMT_VAL(address),
                         prefixlen,
                         ADDRESS_FMT_VAL(gateway),
                         "ADDRESS=%u.%u.%u.%u",
                         ADDRESS_FMT_VAL(address),
                         "PREFIXLEN=%u",
                         prefixlen,
                         "GATEWAY=%u.%u.%u.%u",
                         ADDRESS_FMT_VAL(gateway),
                         NULL);

        link->dhcp_lease = lease;

        if (link->network->dhcp_dns) {
                r = sd_dhcp_lease_get_dns(lease, &nameservers, &nameservers_size);
                if (r >= 0) {
                        r = manager_update_resolv_conf(link->manager);
                        if (r < 0)
                                log_error("Failed to update resolv.conf");
                }
        }

        if (link->network->dhcp_mtu) {
                uint16_t mtu;

                r = sd_dhcp_lease_get_mtu(lease, &mtu);
                if (r >= 0) {
                        r = link_set_mtu(link, mtu);
                        if (r < 0)
                                log_error_link(link, "Failed to set MTU "
                                               "to %" PRIu16, mtu);
                }
        }

        if (link->network->dhcp_hostname) {
                const char *hostname;

                r = sd_dhcp_lease_get_hostname(lease, &hostname);
                if (r >= 0) {
                        r = set_hostname(link->manager->bus, hostname);
                        if (r < 0)
                                log_error("Failed to set transient hostname "
                                          "to '%s'", hostname);
                }
        }

        link_enter_set_addresses(link);

        return 0;
}

static void dhcp_handler(sd_dhcp_client *client, int event, void *userdata) {
        Link *link = userdata;
        int r = 0;

        assert(link);
        assert(link->network);
        assert(link->manager);

        if (link->state == LINK_STATE_FAILED)
                return;

        switch (event) {
                case DHCP_EVENT_NO_LEASE:
                        log_debug_link(link, "IP address in use.");
                        break;
                case DHCP_EVENT_EXPIRED:
                case DHCP_EVENT_STOP:
                case DHCP_EVENT_IP_CHANGE:
                        if (link->network->dhcp_critical) {
                                log_error_link(link, "DHCPv4 connection considered system critical, "
                                               "ignoring request to reconfigure it.");
                                return;
                        }

                        if (link->dhcp_lease) {
                                r = dhcp_lease_lost(link);
                                if (r < 0) {
                                        link_enter_failed(link);
                                        return;
                                }
                        }

                        if (event == DHCP_EVENT_IP_CHANGE) {
                                r = dhcp_lease_acquired(client, link);
                                if (r < 0) {
                                        link_enter_failed(link);
                                        return;
                                }
                        }

                        if (event == DHCP_EVENT_EXPIRED && link->network->ipv4ll) {
                                if (!sd_ipv4ll_is_running(link->ipv4ll))
                                        r = sd_ipv4ll_start(link->ipv4ll);
                                else if (ipv4ll_is_bound(link->ipv4ll))
                                        r = ipv4ll_address_update(link, false);
                                if (r < 0) {
                                        link_enter_failed(link);
                                        return;
                                }
                        }

                        break;
                case DHCP_EVENT_IP_ACQUIRE:
                        r = dhcp_lease_acquired(client, link);
                        if (r < 0) {
                                link_enter_failed(link);
                                return;
                        }
                        if (link->ipv4ll) {
                                if (ipv4ll_is_bound(link->ipv4ll))
                                        r = ipv4ll_address_update(link, true);
                                else
                                        r = sd_ipv4ll_stop(link->ipv4ll);
                                if (r < 0) {
                                        link_enter_failed(link);
                                        return;
                                }
                        }
                        break;
                default:
                        if (event < 0)
                                log_warning_link(link, "DHCP error: %s", strerror(-event));
                        else
                                log_warning_link(link, "DHCP unknown event: %d", event);
                        break;
        }

        return;
}

static int ipv4ll_address_update(Link *link, bool deprecate) {
        int r;
        struct in_addr addr;

        assert(link);

        r = sd_ipv4ll_get_address(link->ipv4ll, &addr);
        if (r >= 0) {
                _cleanup_address_free_ Address *address = NULL;

                log_debug_link(link, "IPv4 link-local %s %u.%u.%u.%u",
                               deprecate ? "deprecate" : "approve",
                               ADDRESS_FMT_VAL(addr));

                r = address_new_dynamic(&address);
                if (r < 0) {
                        log_error_link(link, "Could not allocate address: %s", strerror(-r));
                        return r;
                }

                address->family = AF_INET;
                address->in_addr.in = addr;
                address->prefixlen = 16;
                address->scope = RT_SCOPE_LINK;
                address->cinfo.ifa_prefered = deprecate ? 0 : CACHE_INFO_INFINITY_LIFE_TIME;
                address->broadcast.s_addr = address->in_addr.in.s_addr | htonl(0xfffffffflu >> address->prefixlen);

                address_update(address, link, &address_update_handler);
        }

        return 0;

}

static int ipv4ll_address_lost(Link *link) {
        int r;
        struct in_addr addr;

        assert(link);

        r = sd_ipv4ll_get_address(link->ipv4ll, &addr);
        if (r >= 0) {
                _cleanup_address_free_ Address *address = NULL;
                _cleanup_route_free_ Route *route = NULL;

                log_debug_link(link, "IPv4 link-local release %u.%u.%u.%u",
                                ADDRESS_FMT_VAL(addr));

                r = address_new_dynamic(&address);
                if (r < 0) {
                        log_error_link(link, "Could not allocate address: %s", strerror(-r));
                        return r;
                }

                address->family = AF_INET;
                address->in_addr.in = addr;
                address->prefixlen = 16;
                address->scope = RT_SCOPE_LINK;

                address_drop(address, link, &address_drop_handler);

                r = route_new_dynamic(&route);
                if (r < 0) {
                        log_error_link(link, "Could not allocate route: %s",
                                       strerror(-r));
                        return r;
                }

                route->family = AF_INET;
                route->scope = RT_SCOPE_LINK;
                route->metrics = 99;

                route_drop(route, link, &route_drop_handler);
        }

        return 0;
}

static bool ipv4ll_is_bound(sd_ipv4ll *ll) {
        int r;
        struct in_addr addr;

        assert(ll);

        r = sd_ipv4ll_get_address(ll, &addr);
        if (r < 0)
                return false;
        return true;
}

static int ipv4ll_address_claimed(sd_ipv4ll *ll, Link *link) {
        struct in_addr address;
        int r;

        assert(ll);
        assert(link);

        r = sd_ipv4ll_get_address(ll, &address);
        if (r < 0)
                return r;

        log_struct_link(LOG_INFO, link,
                        "MESSAGE=%s: IPv4 link-local address %u.%u.%u.%u",
                        link->ifname,
                        ADDRESS_FMT_VAL(address),
                        NULL);

       link_enter_set_addresses(link);

       return 0;
}

static void ipv4ll_handler(sd_ipv4ll *ll, int event, void *userdata){
        Link *link = userdata;
        int r;

        assert(link);
        assert(link->network);
        assert(link->manager);

        switch(event) {
                case IPV4LL_EVENT_STOP:
                case IPV4LL_EVENT_CONFLICT:
                        r = ipv4ll_address_lost(link);
                        if (r < 0) {
                                link_enter_failed(link);
                                return;
                        }
                        break;
                case IPV4LL_EVENT_BIND:
                        r = ipv4ll_address_claimed(ll, link);
                        if (r < 0) {
                                link_enter_failed(link);
                                return;
                        }
                        break;
                default:
                        if (event < 0)
                                log_warning_link(link, "IPv4 link-local error: %s", strerror(-event));
                        else
                                log_warning_link(link, "IPv4 link-local unknown event: %d", event);
                        break;
        }
}

static int link_acquire_conf(Link *link) {
        int r;

        assert(link);
        assert(link->network);
        assert(link->manager);
        assert(link->manager->event);

        if (link->network->ipv4ll) {
                assert(link->ipv4ll);

                log_debug_link(link, "acquiring IPv4 link-local address");

                r = sd_ipv4ll_start(link->ipv4ll);
                if (r < 0)
                        return r;
        }

        if (link->network->dhcp) {
                assert(link->dhcp_client);

                log_debug_link(link, "acquiring DHCPv4 lease");

                r = sd_dhcp_client_start(link->dhcp_client);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int link_update_flags(Link *link, unsigned flags) {
        unsigned flags_added, flags_removed, generic_flags;
        bool carrier_gained, carrier_lost;
        int r;

        assert(link);

        if (link->state == LINK_STATE_FAILED)
                return 0;

        if (link->flags == flags)
                return 0;

        flags_added = (link->flags ^ flags) & flags;
        flags_removed = (link->flags ^ flags) & link->flags;
        generic_flags = ~(IFF_UP | IFF_LOWER_UP | IFF_DORMANT | IFF_DEBUG |
                          IFF_MULTICAST | IFF_BROADCAST | IFF_PROMISC |
                          IFF_NOARP | IFF_MASTER | IFF_SLAVE);

        /* consider link to have carrier when LOWER_UP and !DORMANT

           TODO: use proper operstates once we start supporting 802.1X

           see Documentation/networking/operstates.txt in the kernel sources
         */
        carrier_gained = (((flags_added & IFF_LOWER_UP) && !(flags & IFF_DORMANT)) ||
                          ((flags_removed & IFF_DORMANT) && (flags & IFF_LOWER_UP)));
        carrier_lost = ((link->flags & IFF_LOWER_UP) && !(link->flags & IFF_DORMANT)) &&
                       ((flags_removed & IFF_LOWER_UP) || (flags_added & IFF_DORMANT));

        link->flags = flags;

        if (!link->network)
                /* not currently managing this link
                   we track state changes, but don't log them
                   they will be logged if and when a network is
                   applied */
                return 0;

        if (flags_added & IFF_UP)
                log_info_link(link, "link is up");
        else if (flags_removed & IFF_UP)
                log_info_link(link, "link is down");

        if (flags_added & IFF_LOWER_UP)
                log_debug_link(link, "link is lower up");
        else if (flags_removed & IFF_LOWER_UP)
                log_debug_link(link, "link is lower down");

        if (flags_added & IFF_DORMANT)
                log_debug_link(link, "link is dormant");
        else if (flags_removed & IFF_DORMANT)
                log_debug_link(link, "link is not dormant");

        if (flags_added & IFF_DEBUG)
                log_debug_link(link, "debugging enabled in the kernel");
        else if (flags_removed & IFF_DEBUG)
                log_debug_link(link, "debugging disabled in the kernel");

        if (flags_added & IFF_MULTICAST)
                log_debug_link(link, "multicast enabled");
        else if (flags_removed & IFF_MULTICAST)
                log_debug_link(link, "multicast disabled");

        if (flags_added & IFF_BROADCAST)
                log_debug_link(link, "broadcast enabled");
        else if (flags_removed & IFF_BROADCAST)
                log_debug_link(link, "broadcast disabled");

        if (flags_added & IFF_PROMISC)
                log_debug_link(link, "promiscuous mode enabled");
        else if (flags_removed & IFF_PROMISC)
                log_debug_link(link, "promiscuous mode disabled");

        if (flags_added & IFF_NOARP)
                log_debug_link(link, "ARP protocol disabled");
        else if (flags_removed & IFF_NOARP)
                log_debug_link(link, "ARP protocol enabled");

        if (flags_added & IFF_MASTER)
                log_debug_link(link, "link is master");
        else if (flags_removed & IFF_MASTER)
                log_debug_link(link, "link is no longer master");

        if (flags_added & IFF_SLAVE)
                log_debug_link(link, "link is slave");
        else if (flags_removed & IFF_SLAVE)
                log_debug_link(link, "link is no longer slave");

        /* link flags are currently at most 18 bits, let's default to printing 20 */
        if (flags_added & generic_flags)
                log_debug_link(link, "unknown link flags gained: %#.5x (ignoring)",
                               flags_added & generic_flags);

        if (flags_removed & generic_flags)
                log_debug_link(link, "unknown link flags lost: %#.5x (ignoring)",
                               flags_removed & generic_flags);

        if (carrier_gained) {
                log_info_link(link, "gained carrier");

                if (link->network->dhcp || link->network->ipv4ll) {
                        r = link_acquire_conf(link);
                        if (r < 0) {
                                log_warning_link(link, "Could not acquire configuration: %s", strerror(-r));
                                link_enter_failed(link);
                                return r;
                        }
                }
        } else if (carrier_lost) {
                log_info_link(link, "lost carrier");

                if (link->network->dhcp) {
                        r = sd_dhcp_client_stop(link->dhcp_client);
                        if (r < 0) {
                                log_warning_link(link, "Could not stop DHCPv4 client: %s", strerror(-r));
                                link_enter_failed(link);
                                return r;
                        }
                }

                if (link->network->ipv4ll) {
                        r = sd_ipv4ll_stop(link->ipv4ll);
                        if (r < 0) {
                                log_warning_link(link, "Could not stop IPv4 link-local: %s", strerror(-r));
                                link_enter_failed(link);
                                return r;
                        }
                }
        }

        return 0;
}

static int link_up_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(link);

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r >= 0)
                link_update_flags(link, link->flags | IFF_UP);
        else
                log_struct_link(LOG_WARNING, link,
                                "MESSAGE=%s: could not bring up interface: %s",
                                link->ifname, strerror(-r),
                                "ERRNO=%d", -r,
                                NULL);
        return 1;
}

static int link_up(Link *link) {
        _cleanup_rtnl_message_unref_ sd_rtnl_message *req = NULL;
        int r;

        assert(link);
        assert(link->manager);
        assert(link->manager->rtnl);

        log_debug_link(link, "bringing link up");

        r = sd_rtnl_message_new_link(link->manager->rtnl, &req,
                                     RTM_SETLINK, link->ifindex);
        if (r < 0) {
                log_error_link(link, "Could not allocate RTM_SETLINK message");
                return r;
        }

        r = sd_rtnl_message_link_set_flags(req, IFF_UP, IFF_UP);
        if (r < 0) {
                log_error_link(link, "Could not set link flags: %s", strerror(-r));
                return r;
        }

        r = sd_rtnl_call_async(link->manager->rtnl, req, link_up_handler, link, 0, NULL);
        if (r < 0) {
                log_error_link(link,
                               "Could not send rtnetlink message: %s", strerror(-r));
                return r;
        }

        return 0;
}

static int link_enslaved(Link *link) {
        int r;

        assert(link);
        assert(link->state == LINK_STATE_ENSLAVING);
        assert(link->network);

        if (!(link->flags & IFF_UP)) {
                r = link_up(link);
                if (r < 0) {
                        link_enter_failed(link);
                        return r;
                }
        }

        if (!link->network->dhcp && !link->network->ipv4ll)
                return link_enter_set_addresses(link);

        return 0;
}

static int enslave_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(link);
        assert(link->state == LINK_STATE_ENSLAVING || link->state == LINK_STATE_FAILED);
        assert(link->network);

        link->enslaving --;

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0) {
                log_struct_link(LOG_ERR, link,
                                "MESSAGE=%s: could not enslave: %s",
                                link->ifname, strerror(-r),
                                "ERRNO=%d", -r,
                                NULL);
                link_enter_failed(link);
                return 1;
        }

        log_debug_link(link, "enslaved");

        if (link->enslaving == 0)
                link_enslaved(link);

        return 1;
}

static int link_enter_enslave(Link *link) {
        NetDev *vlan, *macvlan;
        Iterator i;
        int r;

        assert(link);
        assert(link->network);
        assert(link->state == LINK_STATE_INITIALIZING);

        link->state = LINK_STATE_ENSLAVING;

        link_save(link);

        if (!link->network->bridge && !link->network->bond &&
            hashmap_isempty(link->network->vlans) &&
            hashmap_isempty(link->network->macvlans))
                return link_enslaved(link);

        if (link->network->bridge) {
                log_struct_link(LOG_DEBUG, link,
                                "MESSAGE=%s: enslaving by '%s'",
                                link->ifname, link->network->bridge->name,
                                NETDEV(link->network->bridge),
                                NULL);

                r = netdev_enslave(link->network->bridge, link, &enslave_handler);
                if (r < 0) {
                        log_struct_link(LOG_WARNING, link,
                                        "MESSAGE=%s: could not enslave by '%s': %s",
                                        link->ifname, link->network->bridge->name, strerror(-r),
                                        NETDEV(link->network->bridge),
                                        NULL);
                        link_enter_failed(link);
                        return r;
                }

                link->enslaving ++;
        }

        if (link->network->bond) {
                log_struct_link(LOG_DEBUG, link,
                                "MESSAGE=%s: enslaving by '%s'",
                                link->ifname, link->network->bond->name,
                                NETDEV(link->network->bond),
                                NULL);

                r = netdev_enslave(link->network->bond, link, &enslave_handler);
                if (r < 0) {
                        log_struct_link(LOG_WARNING, link,
                                        "MESSAGE=%s: could not enslave by '%s': %s",
                                        link->ifname, link->network->bond->name, strerror(-r),
                                        NETDEV(link->network->bond),
                                        NULL);
                        link_enter_failed(link);
                        return r;
                }

                link->enslaving ++;
        }

        HASHMAP_FOREACH(vlan, link->network->vlans, i) {
                log_struct_link(LOG_DEBUG, link,
                                "MESSAGE=%s: enslaving by '%s'",
                                link->ifname, vlan->name, NETDEV(vlan), NULL);

                r = netdev_enslave(vlan, link, &enslave_handler);
                if (r < 0) {
                        log_struct_link(LOG_WARNING, link,
                                        "MESSAGE=%s: could not enslave by '%s': %s",
                                        link->ifname, vlan->name, strerror(-r),
                                        NETDEV(vlan), NULL);
                        link_enter_failed(link);
                        return r;
                }

                link->enslaving ++;
        }

        HASHMAP_FOREACH(macvlan, link->network->macvlans, i) {
                log_struct_link(LOG_DEBUG, link,
                                "MESSAGE=%s: enslaving by '%s'",
                                link->ifname, macvlan->name, NETDEV(macvlan), NULL);

                r = netdev_enslave(macvlan, link, &enslave_handler);
                if (r < 0) {
                        log_struct_link(LOG_WARNING, link,
                                        "MESSAGE=%s: could not enslave by '%s': %s",
                                        link->ifname, macvlan->name, strerror(-r),
                                        NETDEV(macvlan), NULL);
                        link_enter_failed(link);
                        return r;
                }

                link->enslaving ++;
        }

        return 0;
}

static int link_configure(Link *link) {
        int r;

        assert(link);
        assert(link->state == LINK_STATE_INITIALIZING);

        if (link->network->ipv4ll) {
                uint8_t seed[8];
                r = sd_ipv4ll_new(&link->ipv4ll);
                if (r < 0)
                        return r;

                if (link->udev_device) {
                        r = net_get_unique_predictable_data(link->udev_device, seed);
                        if (r >= 0) {
                                r = sd_ipv4ll_set_address_seed(link->ipv4ll, seed);
                                if (r < 0)
                                        return r;
                        }
                }

                r = sd_ipv4ll_attach_event(link->ipv4ll, NULL, 0);
                if (r < 0)
                        return r;

                r = sd_ipv4ll_set_index(link->ipv4ll, link->ifindex);
                if (r < 0)
                        return r;

                r = sd_ipv4ll_set_callback(link->ipv4ll, ipv4ll_handler, link);
                if (r < 0)
                        return r;
        }

        if (link->network->dhcp) {
                r = sd_dhcp_client_new(&link->dhcp_client);
                if (r < 0)
                        return r;

                r = sd_dhcp_client_attach_event(link->dhcp_client, NULL, 0);
                if (r < 0)
                        return r;

                r = sd_dhcp_client_set_index(link->dhcp_client, link->ifindex);
                if (r < 0)
                        return r;

                r = sd_dhcp_client_set_callback(link->dhcp_client, dhcp_handler, link);
                if (r < 0)
                        return r;

                if (link->network->dhcp_mtu) {
                        r = sd_dhcp_client_set_request_option(link->dhcp_client, 26);
                        if (r < 0)
                                return r;
                }
        }

        return link_enter_enslave(link);
}

int link_initialized(Link *link, struct udev_device *device) {
        Network *network;
        unsigned flags;
        int r;

        assert(link);
        assert(link->ifname);
        assert(link->manager);

        if (link->state != LINK_STATE_INITIALIZING)
                return 0;

        if (device)
                link->udev_device = udev_device_ref(device);

        log_debug_link(link, "link initialized");

        r = network_get(link->manager, device, link->ifname, &link->mac, &network);
        if (r < 0)
                return r == -ENOENT ? 0 : r;

        r = network_apply(link->manager, network, link);
        if (r < 0)
                return r;

        r = link_configure(link);
        if (r < 0)
                return r;

        /* re-trigger all state updates */
        flags = link->flags;
        link->flags = 0;
        r = link_update_flags(link, flags);
        if (r < 0)
                return r;

        return 0;
}

int link_add(Manager *m, sd_rtnl_message *message, Link **ret) {
        Link *link;
        _cleanup_udev_device_unref_ struct udev_device *device = NULL;
        char ifindex_str[2 + DECIMAL_STR_MAX(int)];
        int r;

        assert(m);
        assert(message);
        assert(ret);

        r = link_new(m, message, ret);
        if (r < 0)
                return r;

        link = *ret;

        log_info_link(link, "link added");

        if (detect_container(NULL) <= 0) {
                /* not in a container, udev will be around */
                sprintf(ifindex_str, "n%"PRIu64, link->ifindex);
                device = udev_device_new_from_device_id(m->udev, ifindex_str);
                if (!device) {
                        log_warning_link(link, "could not find udev device");
                        return -errno;
                }

                if (udev_device_get_is_initialized(device) <= 0)
                        /* not yet ready */
                        return 0;
        }

        r = link_initialized(link, device);
        if (r < 0)
                return r;

        return 0;
}

int link_update(Link *link, sd_rtnl_message *m) {
        unsigned flags;
        struct ether_addr mac;
        char *ifname;
        int r;

        assert(link);
        assert(link->ifname);
        assert(m);

        if (link->state == LINK_STATE_FAILED)
                return 0;

        r = sd_rtnl_message_read_string(m, IFLA_IFNAME, &ifname);
        if (r >= 0 && !streq(ifname, link->ifname)) {
                log_info_link(link, "renamed to %s", ifname);

                free(link->ifname);
                link->ifname = strdup(ifname);
                if (!link->ifname)
                        return -ENOMEM;
        }

        if (!link->original_mtu) {
                r = sd_rtnl_message_read_u16(m, IFLA_MTU, &link->original_mtu);
                if (r >= 0)
                        log_debug_link(link, "saved original MTU: %"
                                       PRIu16, link->original_mtu);
        }

        /* The kernel may broadcast NEWLINK messages without the MAC address
           set, simply ignore them. */
        r = sd_rtnl_message_read_ether_addr(m, IFLA_ADDRESS, &mac);
        if (r >= 0) {
                if (memcmp(link->mac.ether_addr_octet, mac.ether_addr_octet, ETH_ALEN)) {

                        memcpy(link->mac.ether_addr_octet, mac.ether_addr_octet, ETH_ALEN);

                        log_debug_link(link, "MAC address: "
                                       "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
                                       mac.ether_addr_octet[0],
                                       mac.ether_addr_octet[1],
                                       mac.ether_addr_octet[2],
                                       mac.ether_addr_octet[3],
                                       mac.ether_addr_octet[4],
                                       mac.ether_addr_octet[5]);

                        if (link->ipv4ll) {
                                r = sd_ipv4ll_set_mac(link->ipv4ll, &link->mac);
                                if (r < 0) {
                                        log_warning_link(link, "Could not update MAC "
                                                         "address in IPv4LL client: %s",
                                                         strerror(-r));
                                        return r;
                                }
                        }

                        if (link->dhcp_client) {
                                r = sd_dhcp_client_set_mac(link->dhcp_client, &link->mac);
                                if (r < 0) {
                                        log_warning_link(link, "Could not update MAC "
                                                         "address in DHCP client: %s",
                                                         strerror(-r));
                                        return r;
                                }
                        }
                }
        }

        r = sd_rtnl_message_link_get_flags(m, &flags);
        if (r < 0) {
                log_warning_link(link, "Could not get link flags");
                return r;
        }

        return link_update_flags(link, flags);
}

int link_save(Link *link) {
        _cleanup_free_ char *temp_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        const char *state;
        int r;

        assert(link);
        assert(link->state_file);

        state = link_state_to_string(link->state);
        assert(state);

        r = fopen_temporary(link->state_file, &f, &temp_path);
        if (r < 0)
                goto finish;

        fchmod(fileno(f), 0644);

        fprintf(f,
                "# This is private data. Do not parse.\n"
                "STATE=%s\n", state);

        if (link->dhcp_lease) {
                _cleanup_free_ char *lease_file = NULL;

                r = asprintf(&lease_file, "/run/systemd/network/leases/%"PRIu64,
                             link->ifindex);
                if (r < 0)
                        return -ENOMEM;

                r = dhcp_lease_save(link->dhcp_lease, lease_file);
                if (r < 0)
                        goto finish;

                fprintf(f, "DHCP_LEASE=%s\n", lease_file);
        }

        fflush(f);

        if (ferror(f) || rename(temp_path, link->state_file) < 0) {
                r = -errno;
                unlink(link->state_file);
                unlink(temp_path);
        }

finish:
        if (r < 0)
                log_error("Failed to save link data %s: %s", link->state_file, strerror(-r));

        return r;
}

static const char* const link_state_table[_LINK_STATE_MAX] = {
        [LINK_STATE_INITIALIZING] = "configuring",
        [LINK_STATE_ENSLAVING] = "configuring",
        [LINK_STATE_SETTING_ADDRESSES] = "configuring",
        [LINK_STATE_SETTING_ROUTES] = "configuring",
        [LINK_STATE_CONFIGURED] = "configured",
        [LINK_STATE_FAILED] = "failed",
};

DEFINE_STRING_TABLE_LOOKUP(link_state, LinkState);

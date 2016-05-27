/*
 *	IPv6 autoconf related helper functions
 *
 *	Copyright (C) 2016 SUSE Linux GmbH, Nuernberg, Germany.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *	Authors:
 *		Marius Tomaschewski <mt@suse.de>
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/time.h>
#include <ctype.h>

#include <wicked/logging.h>
#include <wicked/netinfo.h>
#include <wicked/addrconf.h>
#include <wicked/resolver.h>

#include "netinfo_priv.h"
#include "ipv6_priv.h"
#include "util_priv.h"
#include "leasefile.h"
#include "auto6.h"

#define NI_AUTO6_UPDATER_DELAY		500	/* initial delay timeout	*/
#define NI_AUTO6_UPDATER_JITTER		100	/* initial delay timeout jitter */
#define NI_AUTO6_UPDATER_TIMEOUT	500	/* step timeout	*/

static void				ni_auto6_updater_set_timer(ni_addrconf_updater_t *);
static void				ni_auto6_expire_set_timer(ni_auto6_t *, unsigned int);

struct ni_auto6 {
	ni_netdev_ref_t			device;

	struct {
		const ni_timer_t *	timer;
	} expire;
};

ni_auto6_t *
ni_auto6_new(const ni_netdev_t *dev)
{
	ni_auto6_t *auto6;

	if (!dev || !dev->link.ifindex)
		return NULL;

	auto6 = xcalloc(1, sizeof(*auto6));
	if (auto6) {
		ni_netdev_ref_set(&auto6->device, dev->name, dev->link.ifindex);
	}
	return auto6;
}

static inline void
ni_auto6_destroy(ni_auto6_t *auto6)
{
	if (auto6->expire.timer) {
		ni_timer_cancel(auto6->expire.timer);
		auto6->expire.timer = NULL;
	}
	ni_netdev_ref_destroy(&auto6->device);
}

void
ni_auto6_free(ni_auto6_t *auto6)
{
	if (auto6) {
		ni_auto6_destroy(auto6);
		free(auto6);
	}
}

ni_auto6_t *
ni_netdev_get_auto6(ni_netdev_t *dev)
{
	if (!dev->auto6)
		dev->auto6 = ni_auto6_new(dev);
	else
	if (!ni_string_eq(dev->name, dev->auto6->device.name))
		ni_netdev_ref_set_ifname(&dev->auto6->device, dev->name);
	return dev->auto6;
}

void
ni_netdev_set_auto6(ni_netdev_t *dev, ni_auto6_t *auto6)
{
	if (dev->auto6)
		ni_auto6_free(dev->auto6);
	dev->auto6 = NULL;
}

/*
 * Lease
 */
ni_bool_t
ni_auto6_is_active_lease(ni_addrconf_lease_t *lease)
{
	switch (lease->state) {
	case NI_ADDRCONF_STATE_REQUESTING:
	case NI_ADDRCONF_STATE_APPLYING:
	case NI_ADDRCONF_STATE_GRANTED:
		return TRUE;
	default:
		return FALSE;
	}
}

static ni_addrconf_lease_t *
ni_auto6_get_lease(ni_netdev_t *dev)
{
	return ni_netdev_get_lease(dev, AF_INET6, NI_ADDRCONF_AUTOCONF);
}

static ni_addrconf_lease_t *
ni_auto6_new_lease(int state, const ni_uuid_t *uuid)
{
	ni_addrconf_lease_t *lease;

	lease = ni_addrconf_lease_new(NI_ADDRCONF_AUTOCONF, AF_INET6);
	if (lease) {
		lease->state = state;
		if (uuid)
			lease->uuid = *uuid;
		else
			ni_uuid_generate(&lease->uuid);
	}
	return lease;
}

static void
ni_auto6_update_lease(ni_netdev_t *dev, ni_addrconf_lease_t *lease, unsigned int delay)
{
	ni_addrconf_updater_t *updater = lease->updater;

	if (!updater) {
		if (!(updater = ni_addrconf_updater_new_applying(lease, dev)))
			return;
		ni_timer_get_time(&updater->started);
	}

	lease->state = NI_ADDRCONF_STATE_APPLYING;
	updater->timeout    = delay;
	updater->jitter.max = NI_AUTO6_UPDATER_JITTER;
	ni_auto6_updater_set_timer(updater);
	lease->time_acquired = updater->started.tv_sec;
}

static void
ni_auto6_remove_lease(ni_netdev_t *dev, ni_addrconf_lease_t *lease)
{
	ni_addrconf_updater_t *updater;

	if (!(updater = ni_addrconf_updater_new_removing(lease, dev)))
		return;

	lease->state = NI_ADDRCONF_STATE_RELEASING;
	ni_addrconf_updater_execute(dev, lease);
}

/*
 * Lease updater
 */
static int
ni_auto6_updater_execute(ni_netdev_t *dev, ni_addrconf_lease_t *lease)
{
	ni_addrconf_updater_t *updater;
	int ret;

	if (!(updater = lease->updater))
		return -1;

	ret = ni_addrconf_updater_execute(dev, lease);
	if (ret > 0) {
		struct timeval now;

		ni_timer_get_time(&now);
		if (updater->deadline &&
		    !ni_lifetime_left(updater->deadline, &updater->started, &now)) {
			lease->state = NI_ADDRCONF_STATE_FAILED;
			ni_debug_autoip("%s: %s:%s lease apply deadline reached (state %s)",
					dev->name,
					ni_addrfamily_type_to_name(lease->family),
					ni_addrconf_type_to_name(lease->type),
					ni_addrconf_state_to_name(lease->state));
			ret = -1;
		} else {
			if (!updater->timeout)
				updater->timeout = NI_AUTO6_UPDATER_TIMEOUT;
			if (updater->timeout > NI_AUTO6_UPDATER_JITTER)
				updater->jitter.max = NI_AUTO6_UPDATER_JITTER;
			ni_auto6_updater_set_timer(updater);
		}
	}
	if (ret < 0) {
		lease->state = NI_ADDRCONF_STATE_FAILED;
		ni_addrconf_updater_free(&lease->updater);
	}
	return ret;
}

static void
ni_auto6_updater_timed_exec(void *user_data, const ni_timer_t *timer)
{
	ni_addrconf_updater_t *updater = user_data;
	ni_addrconf_lease_t *lease;
	ni_netconfig_t *nc;
	ni_netdev_t *dev;

	if (!updater || updater->timer != timer)
		return;

	updater->timer = NULL;

	if (!(nc = ni_global_state_handle(0)))
		return;

	if (!(dev = ni_netdev_by_index(nc, updater->device.index)))
		return;

	if (!(lease = ni_auto6_get_lease(dev)))
		return;

	if (lease->updater != updater)
		return;

	ni_auto6_updater_execute(dev, lease);
}

static void
ni_auto6_updater_set_timer(ni_addrconf_updater_t *updater)
{
	unsigned long timeout = 0;

	if (updater->timeout)
		timeout = ni_timeout_randomize(updater->timeout, &updater->jitter);

	if (updater->timer) {
		updater->timer = ni_timer_rearm(updater->timer, timeout);
	}
	if (!updater->timer) {
		updater->timer = ni_timer_register(timeout,
				ni_auto6_updater_timed_exec, updater);
	}
}

/*
 * Event handler hooks
 */
void
ni_auto6_on_netdev_event(ni_netdev_t *dev, ni_event_t event)
{
	ni_addrconf_lease_t *lease;

	if (!dev)
		return;

	if (!dev->ipv6 || dev->ipv6->conf.enabled == NI_TRISTATE_DISABLE) {
		ni_netdev_set_auto6(dev, NULL);
		if ((lease = ni_auto6_get_lease(dev))) {
			ni_auto6_remove_lease(dev, lease);
			ni_netdev_unset_lease(dev, AF_INET6, NI_ADDRCONF_AUTOCONF);
		}
		return;
	}

	switch (event) {
	case NI_EVENT_LINK_UP:
		break;
	case NI_EVENT_LINK_DOWN:
		break;

	case NI_EVENT_DEVICE_UP:
		break;
	case NI_EVENT_DEVICE_DOWN:
		ni_netdev_set_auto6(dev, NULL);
		if ((lease = ni_auto6_get_lease(dev))) {
			ni_auto6_remove_lease(dev, lease);
			ni_netdev_unset_lease(dev, AF_INET6, NI_ADDRCONF_AUTOCONF);
		}
		break;

	default:
		break;
	}
}

void
ni_auto6_on_prefix_event(ni_netdev_t *dev, ni_event_t event, const ni_ipv6_ra_pinfo_t *pi)
{
	if (!dev || !pi)
		return;

	switch (event) {
	case NI_EVENT_PREFIX_UPDATE:
		break;

	case NI_EVENT_PREFIX_DELETE:
		break;

	default:
		break;
	}
}

void
ni_auto6_on_address_event(ni_netdev_t *dev, ni_event_t event, const ni_address_t *ap)
{
	if (!dev || !ap || ap->family != AF_INET6)
		return;

	switch (event) {
	case NI_EVENT_ADDRESS_UPDATE:
		break;

	case NI_EVENT_ADDRESS_DELETE:
		break;

	default:
		break;
	}
}

static ni_bool_t
ni_auto6_lease_rdnss_update(ni_netdev_t *dev, ni_addrconf_lease_t *lease)
{
	ni_string_array_t *old, cur = NI_STRING_ARRAY_INIT;
	ni_ipv6_ra_rdnss_t *rdnss;
	ni_bool_t changed = FALSE;

	if (!dev || !dev->ipv6 || !lease)
		return changed;

	if (!lease->resolver && !(lease->resolver = ni_resolver_info_new()))
		return changed;

	/*
	 * rdnss servers received with lifetime of 0 are not in the list,
	 * so we put unexpired servers and report if something changed.
	 */
	old = &lease->resolver->dns_servers;
	for (rdnss = dev->ipv6->radv.rdnss; rdnss; rdnss = rdnss->next) {
		const char *ptr;
		unsigned int i;

		ptr = ni_sockaddr_print(&rdnss->server);
		if (ni_string_empty(ptr))
			continue;

		if ((i = (unsigned int)ni_string_array_index(old, ptr)) != -1U)
			ni_string_array_remove_index(old, i);
		else
			changed = TRUE; /* a server to add */

		ni_string_array_append(&cur, ptr);
	}

	if (old->count)
		changed = TRUE;		/* servers to drop */

	ni_string_array_move(old, &cur);

	return changed;
}

static ni_bool_t
ni_auto6_lease_dnssl_update(ni_netdev_t *dev, ni_addrconf_lease_t *lease)
{
	ni_string_array_t *old, cur = NI_STRING_ARRAY_INIT;
	ni_ipv6_ra_dnssl_t *dnssl;
	ni_bool_t changed = FALSE;

	if (!dev || !dev->ipv6 || !lease)
		return changed;

	if (!lease->resolver && !(lease->resolver = ni_resolver_info_new()))
		return changed;

	/*
	 * rdnss servers received with lifetime of 0 are not in the list,
	 * so we put unexpired domains and report if something changed.
	 */
	old = &lease->resolver->dns_search;
	for (dnssl = dev->ipv6->radv.dnssl; dnssl; dnssl = dnssl->next) {
		const char *ptr;
		unsigned int i;

		ptr = dnssl->domain;
		if (ni_string_empty(ptr))
			continue;

		if ((i = (unsigned int)ni_string_array_index(old, ptr)) != -1U)
			ni_string_array_remove_index(old, i);
		else
			changed = TRUE; /* a domain to add */

		ni_string_array_append(&cur, ptr);
	}

	if (old->count)
		changed = TRUE;		/* domains to drop */

	ni_string_array_move(old, &cur);

	return changed;
}

void
ni_auto6_on_nduseropt_events(ni_netdev_t *dev, ni_event_t event)
{
	ni_addrconf_lease_t *lease;
	unsigned int lifetime;
	struct timeval now;
	ni_bool_t changed;

	if (!dev)
		return;

	if (!(lease = ni_auto6_get_lease(dev))) {
		if ((lease = ni_auto6_new_lease(NI_ADDRCONF_STATE_GRANTED, NULL))) {
			ni_debug_verbose(NI_LOG_DEBUG2, NI_TRACE_IPV6|NI_TRACE_AUTOIP,
				"%s: create %s:%s lease in state %s", dev->name,
				ni_addrfamily_type_to_name(lease->family),
				ni_addrconf_type_to_name(lease->type),
				ni_addrconf_state_to_name(lease->state));
			ni_netdev_set_lease(dev, lease);
		} else {
			ni_warn("%s: failed to create a %s:%s lease: %m", dev->name,
					ni_addrfamily_type_to_name(AF_INET6),
					ni_addrconf_type_to_name(NI_ADDRCONF_AUTOCONF));
			return;
		}
	}

	switch (event) {
	case NI_EVENT_RDNSS_UPDATE:
	case NI_EVENT_DNSSL_UPDATE:
		ni_timer_get_time(&now);
		lifetime = ni_ipv6_ra_info_expire(&dev->ipv6->radv, &now);
		ni_auto6_expire_set_timer(ni_netdev_get_auto6(dev), lifetime);
		/* we expire both, so also update both in the lease */
		if (ni_auto6_lease_rdnss_update(dev, lease))
			changed = TRUE;
		if (ni_auto6_lease_dnssl_update(dev, lease))
			changed = TRUE;
		break;

	default:
		return;
	}

	if (changed) {
		ni_debug_verbose(NI_LOG_DEBUG2, NI_TRACE_IPV6|NI_TRACE_AUTOIP,
			"%s: update %s:%s lease in state %s", dev->name,
			ni_addrfamily_type_to_name(lease->family),
			ni_addrconf_type_to_name(lease->type),
			ni_addrconf_state_to_name(lease->state));

		/* delay, as there may be more events to process ... */
		ni_auto6_update_lease(dev, lease, NI_AUTO6_UPDATER_DELAY);
	}
}

/*
 * Expire handling
 */
static void
ni_auto6_expire_update_lease(ni_netdev_t *dev)
{
	ni_addrconf_lease_t *lease;
	ni_bool_t changed = FALSE;

	if (!dev || !(lease = ni_auto6_get_lease(dev)))
		return;

	switch (lease->state) {
	case NI_ADDRCONF_STATE_NONE:
	case NI_ADDRCONF_STATE_RELEASING:
	case NI_ADDRCONF_STATE_RELEASED:
		return;

	default:
		break;
	}

	if (dev->ipv6 && (dev->ipv6->radv.rdnss || dev->ipv6->radv.dnssl)) {
		if (ni_auto6_lease_rdnss_update(dev, lease))
			changed = TRUE;
		if (ni_auto6_lease_dnssl_update(dev, lease))
			changed = TRUE;

		if (changed)
			ni_auto6_update_lease(dev, lease, NI_AUTO6_UPDATER_DELAY);
	} else {
		/* drop to avoid empty granted lease */
		ni_auto6_remove_lease(dev, lease);
		ni_netdev_unset_lease(dev, AF_INET6, NI_ADDRCONF_AUTOCONF);
	}
}

static void
ni_auto6_expire_timeout(void *user_data, const ni_timer_t *timer)
{
	ni_auto6_t *auto6 = user_data;;
	unsigned int lifetime;
	ni_netconfig_t *nc;
	ni_netdev_t *dev;
	struct timeval now;

	if (!auto6 || auto6->expire.timer != timer)
		return;

	auto6->expire.timer = NULL;

	if (!(nc = ni_global_state_handle(0)))
		return;

	if (!(dev = ni_netdev_by_index(nc, auto6->device.index)))
		return;

	if (!dev->ipv6)
		return;

	ni_timer_get_time(&now);
	lifetime = ni_ipv6_ra_info_expire(&dev->ipv6->radv, &now);
	ni_auto6_expire_set_timer(ni_netdev_get_auto6(dev), lifetime);
	ni_auto6_expire_update_lease(dev);
}

static void
ni_auto6_expire_set_timer(ni_auto6_t *auto6, unsigned int lifetime)
{
	unsigned long timeout;

	if (!auto6 || lifetime == NI_LIFETIME_EXPIRED || lifetime == NI_LIFETIME_INFINITE)
		return;

	timeout = lifetime * 1000;
	if (auto6->expire.timer) {
		auto6->expire.timer = ni_timer_rearm(auto6->expire.timer, timeout);
	}
	if (!auto6->expire.timer) {
		auto6->expire.timer = ni_timer_register(timeout, ni_auto6_expire_timeout, auto6);
	}
}


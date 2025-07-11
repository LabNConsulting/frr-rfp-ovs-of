// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IS-IS Rout(e)ing protocol - isis_zebra.c
 *
 * Copyright (C) 2001,2002   Sampo Saaristo
 *                           Tampere University of Technology
 *                           Institute of Communications Engineering
 * Copyright (C) 2013-2015   Christian Franke <chris@opensourcerouting.org>
 */

#include <zebra.h>

#include "frrevent.h"
#include "command.h"
#include "memory.h"
#include "log.h"
#include "lib_errors.h"
#include "if.h"
#include "network.h"
#include "prefix.h"
#include "zclient.h"
#include "stream.h"
#include "linklist.h"
#include "nexthop.h"
#include "vrf.h"
#include "libfrr.h"
#include "bfd.h"
#include "link_state.h"

#include "isisd/isis_constants.h"
#include "isisd/isis_common.h"
#include "isisd/isis_flags.h"
#include "isisd/isis_misc.h"
#include "isisd/isis_circuit.h"
#include "isisd/isisd.h"
#include "isisd/isis_circuit.h"
#include "isisd/isis_csm.h"
#include "isisd/isis_lsp.h"
#include "isisd/isis_spf.h"
#include "isisd/isis_spf_private.h"
#include "isisd/isis_route.h"
#include "isisd/isis_zebra.h"
#include "isisd/isis_adjacency.h"
#include "isisd/isis_te.h"
#include "isisd/isis_sr.h"
#include "isisd/isis_ldp_sync.h"

struct zclient *isis_zclient;
static struct zclient *zclient_sync;

/* Router-id update message from zebra. */
static int isis_router_id_update_zebra(ZAPI_CALLBACK_ARGS)
{
	struct isis_area *area;
	struct listnode *node;
	struct prefix router_id;
	struct isis *isis = NULL;

	isis = isis_lookup_by_vrfid(vrf_id);

	if (isis == NULL) {
		return -1;
	}

	zebra_router_id_update_read(zclient->ibuf, &router_id);
	if (isis->router_id == router_id.u.prefix4.s_addr)
		return 0;

	isis->router_id = router_id.u.prefix4.s_addr;
	for (ALL_LIST_ELEMENTS_RO(isis->area_list, node, area))
		if (listcount(area->area_addrs) > 0)
			lsp_regenerate_schedule(area, area->is_type, 0);

	return 0;
}

static int isis_zebra_if_address_add(ZAPI_CALLBACK_ARGS)
{
	struct isis_circuit *circuit;
	struct connected *c;

	c = zebra_interface_address_read(ZEBRA_INTERFACE_ADDRESS_ADD,
					 zclient->ibuf, vrf_id);

	if (c == NULL)
		return 0;

#ifdef EXTREME_DEBUG
	if (c->address->family == AF_INET)
		zlog_debug("connected IP address %pFX", c->address);
	if (c->address->family == AF_INET6)
		zlog_debug("connected IPv6 address %pFX", c->address);
#endif /* EXTREME_DEBUG */

	if (if_is_operative(c->ifp)) {
		circuit = circuit_scan_by_ifp(c->ifp);
		if (circuit)
			isis_circuit_add_addr(circuit, c);
	}

	sr_if_addr_update(c->ifp);

	return 0;
}

static int isis_zebra_if_address_del(ZAPI_CALLBACK_ARGS)
{
	struct isis_circuit *circuit;
	struct connected *c;

	c = zebra_interface_address_read(ZEBRA_INTERFACE_ADDRESS_DELETE,
					 zclient->ibuf, vrf_id);

	if (c == NULL)
		return 0;

#ifdef EXTREME_DEBUG
	if (c->address->family == AF_INET)
		zlog_debug("disconnected IP address %pFX", c->address);
	if (c->address->family == AF_INET6)
		zlog_debug("disconnected IPv6 address %pFX", c->address);
#endif /* EXTREME_DEBUG */

	if (if_is_operative(c->ifp)) {
		circuit = circuit_scan_by_ifp(c->ifp);
		if (circuit)
			isis_circuit_del_addr(circuit, c);
	}

	sr_if_addr_update(c->ifp);

	connected_free(&c);

	return 0;
}

static int isis_zebra_link_params(ZAPI_CALLBACK_ARGS)
{
	struct interface *ifp;
	bool changed = false;

	ifp = zebra_interface_link_params_read(zclient->ibuf, vrf_id, &changed);

	if (ifp == NULL || !changed)
		return 0;

	/* Update TE TLV */
	isis_mpls_te_update(ifp);

	return 0;
}

enum isis_zebra_nexthop_type {
	ISIS_NEXTHOP_MAIN = 0,
	ISIS_NEXTHOP_BACKUP,
};

static int isis_zebra_add_nexthops(struct isis *isis, struct list *nexthops,
				   struct zapi_nexthop zapi_nexthops[],
				   enum isis_zebra_nexthop_type type,
				   bool mpls_lsp, uint8_t backup_nhs)
{
	struct isis_nexthop *nexthop;
	struct listnode *node;
	int count = 0;

	/* Nexthops */
	for (ALL_LIST_ELEMENTS_RO(nexthops, node, nexthop)) {
		struct zapi_nexthop *api_nh;

		if (count >= MULTIPATH_NUM)
			break;
		api_nh = &zapi_nexthops[count];
		if (fabricd)
			SET_FLAG(api_nh->flags, ZAPI_NEXTHOP_FLAG_ONLINK);
		api_nh->vrf_id = isis->vrf_id;

		switch (nexthop->family) {
		case AF_INET:
			/* FIXME: can it be ? */
			if (nexthop->ip.ipv4.s_addr != INADDR_ANY) {
				api_nh->type = NEXTHOP_TYPE_IPV4_IFINDEX;
				api_nh->gate.ipv4 = nexthop->ip.ipv4;
			} else {
				api_nh->type = NEXTHOP_TYPE_IFINDEX;
			}
			break;
		case AF_INET6:
			if (!IN6_IS_ADDR_LINKLOCAL(&nexthop->ip.ipv6)
			    && !IN6_IS_ADDR_UNSPECIFIED(&nexthop->ip.ipv6)) {
				continue;
			}
			api_nh->gate.ipv6 = nexthop->ip.ipv6;
			api_nh->type = NEXTHOP_TYPE_IPV6_IFINDEX;
			break;
		default:
			flog_err(EC_LIB_DEVELOPMENT,
				 "%s: unknown address family [%d]", __func__,
				 nexthop->family);
			exit(1);
		}

		api_nh->ifindex = nexthop->ifindex;

		/* Add MPLS label(s). */
		if (nexthop->label_stack) {
			api_nh->label_num = nexthop->label_stack->num_labels;
			memcpy(api_nh->labels, nexthop->label_stack->label,
			       sizeof(mpls_label_t) * api_nh->label_num);
		} else if (nexthop->sr.present) {
			api_nh->label_num = 1;
			api_nh->labels[0] = nexthop->sr.label;
		} else if (mpls_lsp) {
			switch (type) {
			case ISIS_NEXTHOP_MAIN:
				/*
				 * Do not use non-SR enabled nexthops to prevent
				 * broken LSPs from being formed.
				 */
				continue;
			case ISIS_NEXTHOP_BACKUP:
				/*
				 * This is necessary because zebra requires
				 * the nexthops of MPLS LSPs to be labeled.
				 */
				api_nh->label_num = 1;
				api_nh->labels[0] = MPLS_LABEL_IMPLICIT_NULL;
				break;
			}
		}

		/* Backup nexthop handling. */
		if (backup_nhs) {
			SET_FLAG(api_nh->flags, ZAPI_NEXTHOP_FLAG_HAS_BACKUP);
			/*
			 * If the backup has multiple nexthops, all of them
			 * protect the same primary nexthop since ECMP routes
			 * have no backups.
			 */
			api_nh->backup_num = backup_nhs;
			for (int i = 0; i < backup_nhs; i++)
				api_nh->backup_idx[i] = i;
		}
		count++;
	}

	return count;
}

void isis_zebra_route_add_route(struct isis *isis, struct prefix *prefix,
				struct prefix_ipv6 *src_p,
				struct isis_route_info *route_info)
{
	struct zapi_route api;
	int count = 0;

	if (isis_zclient->sock < 0)
		return;

	/* Uninstall the route if it doesn't have any valid nexthop. */
	if (list_isempty(route_info->nexthops)) {
		isis_zebra_route_del_route(isis, prefix, src_p, route_info);
		return;
	}

	memset(&api, 0, sizeof(api));
	api.vrf_id = isis->vrf_id;
	api.type = PROTO_TYPE;
	api.safi = SAFI_UNICAST;
	api.prefix = *prefix;
	if (src_p && src_p->prefixlen) {
		api.src_prefix = *src_p;
		SET_FLAG(api.message, ZAPI_MESSAGE_SRCPFX);
	}
	SET_FLAG(api.message, ZAPI_MESSAGE_NEXTHOP);
	SET_FLAG(api.message, ZAPI_MESSAGE_METRIC);
	api.metric = route_info->cost;

	/* Add backup nexthops first. */
	if (route_info->backup) {
		count = isis_zebra_add_nexthops(
			isis, route_info->backup->nexthops, api.backup_nexthops,
			ISIS_NEXTHOP_BACKUP, false, 0);
		if (count > 0) {
			SET_FLAG(api.message, ZAPI_MESSAGE_BACKUP_NEXTHOPS);
			api.backup_nexthop_num = count;
		}
	}

	/* Add primary nexthops. */
	count = isis_zebra_add_nexthops(isis, route_info->nexthops,
					api.nexthops, ISIS_NEXTHOP_MAIN, false,
					count);
	if (!count)
		return;
	api.nexthop_num = count;

	zclient_route_send(ZEBRA_ROUTE_ADD, isis_zclient, &api);
}

void isis_zebra_route_del_route(struct isis *isis,
				struct prefix *prefix,
				struct prefix_ipv6 *src_p,
				struct isis_route_info *route_info)
{
	struct zapi_route api;

	if (isis_zclient->sock < 0)
		return;

	if (!CHECK_FLAG(route_info->flag, ISIS_ROUTE_FLAG_ZEBRA_SYNCED))
		return;

	memset(&api, 0, sizeof(api));
	api.vrf_id = isis->vrf_id;
	api.type = PROTO_TYPE;
	api.safi = SAFI_UNICAST;
	api.prefix = *prefix;
	if (src_p && src_p->prefixlen) {
		api.src_prefix = *src_p;
		SET_FLAG(api.message, ZAPI_MESSAGE_SRCPFX);
	}

	zclient_route_send(ZEBRA_ROUTE_DELETE, isis_zclient, &api);
}

/**
 * Install Prefix-SID label entry in the forwarding plane through Zebra.
 *
 * @param area		IS-IS area
 * @param prefix	Route prefix
 * @param rinfo		Route information
 * @param psid		Prefix-SID information
 */
void isis_zebra_prefix_sid_install(struct isis_area *area,
				   struct prefix *prefix,
				   struct isis_sr_psid_info *psid)
{
	struct zapi_labels zl;
	int count = 0;

	sr_debug("ISIS-Sr (%s): update label %u for prefix %pFX algorithm %u",
		 area->area_tag, psid->label, prefix, psid->algorithm);

	/* Prepare message. */
	memset(&zl, 0, sizeof(zl));
	zl.type = ZEBRA_LSP_ISIS_SR;
	zl.local_label = psid->label;

	/* Local routes don't have any nexthop and require special handling. */
	if (list_isempty(psid->nexthops)) {
		struct zapi_nexthop *znh;
		struct interface *ifp;

		ifp = if_lookup_by_name("lo", VRF_DEFAULT);
		if (!ifp) {
			zlog_warn(
				"%s: couldn't install Prefix-SID %pFX: loopback interface not found",
				__func__, prefix);
			return;
		}

		znh = &zl.nexthops[zl.nexthop_num++];
		znh->type = NEXTHOP_TYPE_IFINDEX;
		znh->ifindex = ifp->ifindex;
		znh->label_num = 1;
		znh->labels[0] = MPLS_LABEL_IMPLICIT_NULL;
	} else {
		/* Add backup nexthops first. */
		if (psid->nexthops_backup) {
			count = isis_zebra_add_nexthops(
				area->isis, psid->nexthops_backup,
				zl.backup_nexthops, ISIS_NEXTHOP_BACKUP, true,
				0);
			if (count > 0) {
				SET_FLAG(zl.message, ZAPI_LABELS_HAS_BACKUPS);
				zl.backup_nexthop_num = count;
			}
		}

		/* Add primary nexthops. */
		count = isis_zebra_add_nexthops(area->isis, psid->nexthops,
						zl.nexthops, ISIS_NEXTHOP_MAIN,
						true, count);
		if (!count)
			return;
		zl.nexthop_num = count;
	}

	/* Send message to zebra. */
	(void)zebra_send_mpls_labels(isis_zclient, ZEBRA_MPLS_LABELS_REPLACE, &zl);
}

/**
 * Uninstall Prefix-SID label entry from the forwarding plane through Zebra.
 *
 * @param area		IS-IS area
 * @param prefix	Route prefix
 * @param rinfo		Route information
 * @param psid		Prefix-SID information
 */
void isis_zebra_prefix_sid_uninstall(struct isis_area *area,
				     struct prefix *prefix,
				     struct isis_route_info *rinfo,
				     struct isis_sr_psid_info *psid)
{
	struct zapi_labels zl;

	sr_debug("ISIS-Sr (%s): delete label %u for prefix %pFX algorithm %u",
		 area->area_tag, psid->label, prefix, psid->algorithm);

	/* Prepare message. */
	memset(&zl, 0, sizeof(zl));
	zl.type = ZEBRA_LSP_ISIS_SR;
	zl.local_label = psid->label;

	/* Send message to zebra. */
	(void)zebra_send_mpls_labels(isis_zclient, ZEBRA_MPLS_LABELS_DELETE, &zl);
}

/**
 * Send (LAN)-Adjacency-SID to ZEBRA for installation or deletion.
 *
 * @param cmd	ZEBRA_MPLS_LABELS_ADD or ZEBRA_ROUTE_DELETE
 * @param sra	Segment Routing Adjacency-SID
 */
void isis_zebra_send_adjacency_sid(int cmd, const struct sr_adjacency *sra)
{
	struct isis *isis = sra->adj->circuit->area->isis;
	struct zapi_labels zl;
	struct zapi_nexthop *znh;

	if (cmd != ZEBRA_MPLS_LABELS_ADD && cmd != ZEBRA_MPLS_LABELS_DELETE) {
		flog_warn(EC_LIB_DEVELOPMENT, "%s: wrong ZEBRA command",
			  __func__);
		return;
	}

	sr_debug("  |- %s label %u for interface %s",
		 cmd == ZEBRA_MPLS_LABELS_ADD ? "Add" : "Delete",
		 sra->input_label, sra->adj->circuit->interface->name);

	memset(&zl, 0, sizeof(zl));
	zl.type = ZEBRA_LSP_ISIS_SR;
	zl.local_label = sra->input_label;
	zl.nexthop_num = 1;
	znh = &zl.nexthops[0];
	znh->gate = sra->nexthop.address;
	znh->type = (sra->nexthop.family == AF_INET)
			    ? NEXTHOP_TYPE_IPV4_IFINDEX
			    : NEXTHOP_TYPE_IPV6_IFINDEX;
	znh->ifindex = sra->adj->circuit->interface->ifindex;
	znh->label_num = 1;
	znh->labels[0] = MPLS_LABEL_IMPLICIT_NULL;

	/* Set backup nexthops. */
	if (sra->type == ISIS_SR_ADJ_BACKUP) {
		int count;

		count = isis_zebra_add_nexthops(isis, sra->backup_nexthops,
						zl.backup_nexthops,
						ISIS_NEXTHOP_BACKUP, true, 0);
		if (count > 0) {
			SET_FLAG(zl.message, ZAPI_LABELS_HAS_BACKUPS);
			zl.backup_nexthop_num = count;

			SET_FLAG(znh->flags, ZAPI_NEXTHOP_FLAG_HAS_BACKUP);
			znh->backup_num = count;
			for (int i = 0; i < count; i++)
				znh->backup_idx[i] = i;
		}
	}

	(void)zebra_send_mpls_labels(isis_zclient, cmd, &zl);
}

static int isis_zebra_read(ZAPI_CALLBACK_ARGS)
{
	struct zapi_route api;
	struct isis *isis = NULL;

	isis = isis_lookup_by_vrfid(vrf_id);

	if (isis == NULL)
		return -1;

	if (zapi_route_decode(zclient->ibuf, &api) < 0)
		return -1;

	if (api.prefix.family == AF_INET6
	    && IN6_IS_ADDR_LINKLOCAL(&api.prefix.u.prefix6))
		return 0;

	/*
	 * Avoid advertising a false default reachability. (A default
	 * route installed by IS-IS gets redistributed from zebra back
	 * into IS-IS causing us to start advertising default reachabity
	 * without this check)
	 */
	if (api.prefix.prefixlen == 0
	    && api.src_prefix.prefixlen == 0
	    && api.type == PROTO_TYPE) {
		cmd = ZEBRA_REDISTRIBUTE_ROUTE_DEL;
	}

	if (cmd == ZEBRA_REDISTRIBUTE_ROUTE_ADD)
		isis_redist_add(isis, api.type, &api.prefix, &api.src_prefix,
				api.distance, api.metric, api.tag, api.instance);
	else
		isis_redist_delete(isis, api.type, &api.prefix, &api.src_prefix,
				   api.instance);

	return 0;
}

int isis_distribute_list_update(int routetype)
{
	return 0;
}

void isis_zebra_redistribute_set(afi_t afi, int type, vrf_id_t vrf_id,
				 uint16_t tableid)
{
	if (type == DEFAULT_ROUTE)
		zclient_redistribute_default(ZEBRA_REDISTRIBUTE_DEFAULT_ADD,
					     isis_zclient, afi, vrf_id);
	else
		zclient_redistribute(ZEBRA_REDISTRIBUTE_ADD, isis_zclient, afi, type,
				     tableid, vrf_id);
}

void isis_zebra_redistribute_unset(afi_t afi, int type, vrf_id_t vrf_id,
				   uint16_t tableid)
{
	if (type == DEFAULT_ROUTE)
		zclient_redistribute_default(ZEBRA_REDISTRIBUTE_DEFAULT_DELETE,
					     isis_zclient, afi, vrf_id);
	else
		zclient_redistribute(ZEBRA_REDISTRIBUTE_DELETE, isis_zclient, afi,
				     type, tableid, vrf_id);
}

/**
 * Register RLFA with LDP.
 */
int isis_zebra_rlfa_register(struct isis_spftree *spftree, struct rlfa *rlfa)
{
	struct isis_area *area = spftree->area;
	struct zapi_rlfa_request zr = {};
	int ret;

	if (!isis_zclient)
		return 0;

	zr.igp.vrf_id = area->isis->vrf_id;
	zr.igp.protocol = ZEBRA_ROUTE_ISIS;
	strlcpy(zr.igp.isis.area_tag, area->area_tag,
		sizeof(zr.igp.isis.area_tag));
	zr.igp.isis.spf.tree_id = spftree->tree_id;
	zr.igp.isis.spf.level = spftree->level;
	zr.igp.isis.spf.run_id = spftree->runcount;
	zr.destination = rlfa->prefix;
	zr.pq_address = rlfa->pq_address;

	zlog_debug("ISIS-LFA: registering RLFA %pFX@%pI4 with LDP",
		   &rlfa->prefix, &rlfa->pq_address);

	ret = zclient_send_opaque_unicast(isis_zclient, LDP_RLFA_REGISTER,
					  ZEBRA_ROUTE_LDP, 0, 0,
					  (const uint8_t *)&zr, sizeof(zr));
	if (ret == ZCLIENT_SEND_FAILURE) {
		zlog_warn("ISIS-LFA: failed to register RLFA with LDP");
		return -1;
	}

	return 0;
}

/**
 * Unregister all RLFAs from the given SPF tree with LDP.
 */
void isis_zebra_rlfa_unregister_all(struct isis_spftree *spftree)
{
	struct isis_area *area = spftree->area;
	struct zapi_rlfa_igp igp = {};
	int ret;

	if (!isis_zclient || spftree->type != SPF_TYPE_FORWARD ||
	    CHECK_FLAG(spftree->flags, F_SPFTREE_NO_ADJACENCIES))
		return;

	if (IS_DEBUG_LFA)
		zlog_debug("ISIS-LFA: unregistering all RLFAs with LDP");

	igp.vrf_id = area->isis->vrf_id;
	igp.protocol = ZEBRA_ROUTE_ISIS;
	strlcpy(igp.isis.area_tag, area->area_tag, sizeof(igp.isis.area_tag));
	igp.isis.spf.tree_id = spftree->tree_id;
	igp.isis.spf.level = spftree->level;
	igp.isis.spf.run_id = spftree->runcount;

	ret = zclient_send_opaque_unicast(isis_zclient, LDP_RLFA_UNREGISTER_ALL,
					  ZEBRA_ROUTE_LDP, 0, 0,
					  (const uint8_t *)&igp, sizeof(igp));
	if (ret == ZCLIENT_SEND_FAILURE)
		zlog_warn("ISIS-LFA: failed to unregister RLFA with LDP");
}

/* Label Manager Functions */

/**
 * Check if Label Manager is Ready or not.
 *
 * @return	True if Label Manager is ready, False otherwise
 */
bool isis_zebra_label_manager_ready(void)
{
	return (zclient_sync->sock > 0);
}

/**
 * Request Label Range to the Label Manager.
 *
 * @param base		base label of the label range to request
 * @param chunk_size	size of the label range to request
 *
 * @return 	0 on success, -1 on failure
 */
int isis_zebra_request_label_range(uint32_t base, uint32_t chunk_size)
{
	int ret;
	uint32_t start, end;

	if (zclient_sync->sock < 0)
		return -1;

	ret = lm_get_label_chunk(zclient_sync, 0, base, chunk_size, &start,
				 &end);
	if (ret < 0) {
		zlog_warn("%s: error getting label range!", __func__);
		return -1;
	}

	return 0;
}

/**
 * Request an End.X SID for an IS-IS adjacency.
 *
 * @param adj	   IS-IS Adjacency
 */
void isis_zebra_request_srv6_sid_endx(struct isis_adjacency *adj)
{
	struct isis_circuit *circuit = adj->circuit;
	struct isis_area *area = circuit->area;
	struct in6_addr nexthop;
	struct srv6_sid_ctx ctx = {};
	struct in6_addr sid_value = {};
	bool ret;

	if (!area || !area->srv6db.srv6_locator)
		return;

	/* Determine nexthop IP address */
	if (!circuit->ipv6_router || !adj->ll_ipv6_count)
		return;

	nexthop = adj->ll_ipv6_addrs[0];

	ctx.behavior = ZEBRA_SEG6_LOCAL_ACTION_END_X;
	ctx.nh6 = nexthop;
	ctx.ifindex = circuit->interface->ifindex;
	ret = isis_zebra_request_srv6_sid(&ctx, &sid_value,
					  area->srv6db.config.srv6_locator_name);
	if (!ret) {
		zlog_err("%s: not allocated new End.X SID for IS-IS area %s",
			 __func__, area->area_tag);
		return;
	}
}

static void request_srv6_sids(struct isis_area *area)
{
	struct srv6_sid_ctx ctx = {};
	struct in6_addr sid_value = {};
	struct listnode *node;
	struct isis_adjacency *adj;
	bool ret;

	if (!area || !area->srv6db.config.enabled || !area->srv6db.srv6_locator)
		return;

	sr_debug("Requesting SRv6 SIDs for IS-IS area %s", area->area_tag);

	/* Request new SRv6 End SID */
	ctx.behavior = ZEBRA_SEG6_LOCAL_ACTION_END;
	ret = isis_zebra_request_srv6_sid(&ctx, &sid_value,
					  area->srv6db.config.srv6_locator_name);
	if (!ret) {
		zlog_err("%s: not allocated new End SID for IS-IS area %s",
			 __func__, area->area_tag);
		return;
	}

	/* Create SRv6 End.X SIDs from existing IS-IS Adjacencies */
	for (ALL_LIST_ELEMENTS_RO(area->adjacency_list, node, adj)) {
		if (adj->ll_ipv6_count > 0)
			isis_zebra_request_srv6_sid_endx(adj);
	}
}

/**
 * Release Label Range to the Label Manager.
 *
 * @param start		start of label range to release
 * @param end		end of label range to release
 *
 * @return		0 on success, -1 otherwise
 */
int isis_zebra_release_label_range(uint32_t start, uint32_t end)
{
	int ret;

	if (zclient_sync->sock < 0)
		return -1;

	ret = lm_release_label_chunk(zclient_sync, start, end);
	if (ret < 0) {
		zlog_warn("%s: error releasing label range!", __func__);
		return -1;
	}

	return 0;
}

/**
 * Connect to the Label Manager.
 *
 * @return	0 on success, -1 otherwise
 */
int isis_zebra_label_manager_connect(void)
{
	/* Connect to label manager. */
	if (zclient_socket_connect(zclient_sync) < 0) {
		zlog_warn("%s: failed connecting synchronous zclient!",
			  __func__);
		return -1;
	}
	/* make socket non-blocking */
	set_nonblocking(zclient_sync->sock);

	/* Send hello to notify zebra this is a synchronous client */
	if (zclient_send_hello(zclient_sync) == ZCLIENT_SEND_FAILURE) {
		zlog_warn("%s: failed sending hello for synchronous zclient!",
			  __func__);
		close(zclient_sync->sock);
		zclient_sync->sock = -1;
		return -1;
	}

	/* Connect to label manager */
	if (lm_label_manager_connect(zclient_sync, 0) != 0) {
		zlog_warn("%s: failed connecting to label manager!", __func__);
		if (zclient_sync->sock > 0) {
			close(zclient_sync->sock);
			zclient_sync->sock = -1;
		}
		return -1;
	}

	sr_debug("ISIS-Sr: Successfully connected to the Label Manager");

	return 0;
}

void isis_zebra_vrf_register(struct isis *isis)
{
	if (!isis_zclient || isis_zclient->sock < 0 || !isis)
		return;

	if (isis->vrf_id != VRF_UNKNOWN) {
		if (IS_DEBUG_EVENTS)
			zlog_debug("%s: Register VRF %s id %u", __func__,
				   isis->name, isis->vrf_id);
		zclient_send_reg_requests(isis_zclient, isis->vrf_id);
	}
}

void isis_zebra_vrf_deregister(struct isis *isis)
{
	if (!isis_zclient || isis_zclient->sock < 0 || !isis)
		return;

	if (isis->vrf_id != VRF_UNKNOWN) {
		if (IS_DEBUG_EVENTS)
			zlog_debug("%s: Deregister VRF %s id %u", __func__,
				   isis->name, isis->vrf_id);
		zclient_send_dereg_requests(isis_zclient, isis->vrf_id);
	}
}

static void isis_zebra_connected(struct zclient *zclient)
{
	zclient_send_reg_requests(zclient, VRF_DEFAULT);
	zclient_register_opaque(zclient, LDP_RLFA_LABELS);
	zclient_register_opaque(zclient, LDP_IGP_SYNC_IF_STATE_UPDATE);
	zclient_register_opaque(zclient, LDP_IGP_SYNC_ANNOUNCE_UPDATE);
	bfd_client_sendmsg(zclient, ZEBRA_BFD_CLIENT_REGISTER, VRF_DEFAULT);
	isis_srv6_locators_request();
}

/**
 * Register / unregister Link State ZAPI Opaque Message
 *
 * @param up	True to register, false to unregister
 *
 * @return	0 if success, -1 otherwise
 */
int isis_zebra_ls_register(bool up)
{
	int rc;

	if (up)
		rc = ls_register(isis_zclient, true);
	else
		rc = ls_unregister(isis_zclient, true);

	return rc;
}

/*
 * opaque messages between processes
 */
static int isis_opaque_msg_handler(ZAPI_CALLBACK_ARGS)
{
	struct stream *s;
	struct zapi_opaque_msg info;
	struct zapi_opaque_reg_info dst;
	struct ldp_igp_sync_if_state state;
	struct ldp_igp_sync_announce announce;
	struct zapi_rlfa_response rlfa;
	int ret = 0;

	s = zclient->ibuf;
	if (zclient_opaque_decode(s, &info) != 0)
		return -1;

	switch (info.type) {
	case LINK_STATE_SYNC:
		dst.proto = info.src_proto;
		dst.instance = info.src_instance;
		dst.session_id = info.src_session_id;
		dst.type = LINK_STATE_SYNC;
		ret = isis_te_sync_ted(dst);
		break;
	case LDP_IGP_SYNC_IF_STATE_UPDATE:
		STREAM_GET(&state, s, sizeof(state));
		ret = isis_ldp_sync_state_update(state);
		break;
	case LDP_IGP_SYNC_ANNOUNCE_UPDATE:
		STREAM_GET(&announce, s, sizeof(announce));
		ret = isis_ldp_sync_announce_update(announce);
		break;
	case LDP_RLFA_LABELS:
		STREAM_GET(&rlfa, s, sizeof(rlfa));
		isis_rlfa_process_ldp_response(&rlfa);
		break;
	default:
		break;
	}

stream_failure:

	return ret;
}

static int isis_zebra_client_close_notify(ZAPI_CALLBACK_ARGS)
{
	int ret = 0;

	struct zapi_client_close_info info;

	if (zapi_client_close_notify_decode(zclient->ibuf, &info) < 0)
		return -1;

	isis_ldp_sync_handle_client_close(&info);
	isis_ldp_rlfa_handle_client_close(&info);

	return ret;
}

/**
 * Send SRv6 SID to ZEBRA for installation or deletion.
 *
 * @param cmd		ZEBRA_ROUTE_ADD or ZEBRA_ROUTE_DELETE
 * @param sid		SRv6 SID to install or delete
 * @param prefixlen	Prefix length
 * @param oif		Outgoing interface
 * @param action	SID action
 * @param context	SID context
 */
static void isis_zebra_send_localsid(int cmd, const struct in6_addr *sid,
				     uint16_t prefixlen, ifindex_t oif,
				     enum seg6local_action_t action,
				     const struct seg6local_context *context)
{
	struct prefix_ipv6 p = {};
	struct zapi_route api = {};
	struct zapi_nexthop *znh;

	if (cmd != ZEBRA_ROUTE_ADD && cmd != ZEBRA_ROUTE_DELETE) {
		flog_warn(EC_LIB_DEVELOPMENT, "%s: wrong ZEBRA command",
			  __func__);
		return;
	}

	if (prefixlen > IPV6_MAX_BITLEN) {
		flog_warn(EC_LIB_DEVELOPMENT, "%s: wrong prefixlen %u",
			  __func__, prefixlen);
		return;
	}

	sr_debug("  |- %s SRv6 SID %pI6 behavior %s",
		 cmd == ZEBRA_ROUTE_ADD ? "Add" : "Delete", sid,
		 seg6local_action2str(action));

	p.family = AF_INET6;
	p.prefixlen = prefixlen;
	p.prefix = *sid;

	api.vrf_id = VRF_DEFAULT;
	api.type = PROTO_TYPE;
	api.instance = 0;
	api.safi = SAFI_UNICAST;
	memcpy(&api.prefix, &p, sizeof(p));

	if (cmd == ZEBRA_ROUTE_DELETE)
		return (void)zclient_route_send(ZEBRA_ROUTE_DELETE, isis_zclient,
						&api);

	SET_FLAG(api.flags, ZEBRA_FLAG_ALLOW_RECURSION);
	SET_FLAG(api.message, ZAPI_MESSAGE_NEXTHOP);

	znh = &api.nexthops[0];

	memset(znh, 0, sizeof(*znh));

	znh->type = NEXTHOP_TYPE_IFINDEX;
	znh->ifindex = oif;
	SET_FLAG(znh->flags, ZAPI_NEXTHOP_FLAG_SEG6LOCAL);
	znh->seg6local_action = action;
	memcpy(&znh->seg6local_ctx, context, sizeof(struct seg6local_context));

	api.nexthop_num = 1;

	zclient_route_send(ZEBRA_ROUTE_ADD, isis_zclient, &api);
}

/**
 * Install SRv6 SID in the forwarding plane through Zebra.
 *
 * @param area		IS-IS area
 * @param sid		SRv6 SID
 */
void isis_zebra_srv6_sid_install(struct isis_area *area,
				 struct isis_srv6_sid *sid)
{
	enum seg6local_action_t action = ZEBRA_SEG6_LOCAL_ACTION_UNSPEC;
	uint16_t prefixlen = IPV6_MAX_BITLEN;
	struct seg6local_context ctx = {};
	struct interface *ifp;

	if (!area || !sid)
		return;

	sr_debug("ISIS-SRv6 (%s): setting SRv6 SID %pI6", area->area_tag,
		 &sid->sid);

	switch (sid->behavior) {
	case SRV6_ENDPOINT_BEHAVIOR_END:
		action = ZEBRA_SEG6_LOCAL_ACTION_END;
		prefixlen = IPV6_MAX_BITLEN;
		break;
	case SRV6_ENDPOINT_BEHAVIOR_END_PSP:
		action = ZEBRA_SEG6_LOCAL_ACTION_END;
		prefixlen = IPV6_MAX_BITLEN;
		SET_SRV6_FLV_OP(ctx.flv.flv_ops, ZEBRA_SEG6_LOCAL_FLV_OP_PSP);
		break;
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID:
		action = ZEBRA_SEG6_LOCAL_ACTION_END;
		prefixlen = sid->locator->block_bits_length +
			    sid->locator->node_bits_length;
		SET_SRV6_FLV_OP(ctx.flv.flv_ops,
				ZEBRA_SEG6_LOCAL_FLV_OP_NEXT_CSID);
		ctx.flv.lcblock_len = sid->locator->block_bits_length;
		ctx.flv.lcnode_func_len = sid->locator->node_bits_length;
		break;
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID_PSP:
		action = ZEBRA_SEG6_LOCAL_ACTION_END;
		prefixlen = sid->locator->block_bits_length + sid->locator->node_bits_length;
		SET_SRV6_FLV_OP(ctx.flv.flv_ops, ZEBRA_SEG6_LOCAL_FLV_OP_NEXT_CSID);
		SET_SRV6_FLV_OP(ctx.flv.flv_ops, ZEBRA_SEG6_LOCAL_FLV_OP_PSP);
		ctx.flv.lcblock_len = sid->locator->block_bits_length;
		ctx.flv.lcnode_func_len = sid->locator->node_bits_length;
		break;
	case SRV6_ENDPOINT_BEHAVIOR_END_X:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID:
	case SRV6_ENDPOINT_BEHAVIOR_RESERVED:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT6:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT4:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT46:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT6_USID:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT4_USID:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT46_USID:
	case SRV6_ENDPOINT_BEHAVIOR_OPAQUE:
	case SRV6_ENDPOINT_BEHAVIOR_END_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_RED:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_NEXT_CSID:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_RED_NEXT_CSID:
	default:
		zlog_err(
			"ISIS-SRv6 (%s): unsupported SRv6 endpoint behavior %u",
			area->area_tag, sid->behavior);
		return;
	}

	/* Attach the SID to the SRv6 interface */
	ifp = if_lookup_by_name(area->srv6db.config.srv6_ifname, VRF_DEFAULT);
	if (!ifp) {
		zlog_warn(
			"Failed to install SRv6 SID %pI6: %s interface not found",
			&sid->sid, area->srv6db.config.srv6_ifname);
		return;
	}

	/* Send the SID to zebra */
	isis_zebra_send_localsid(ZEBRA_ROUTE_ADD, &sid->sid, prefixlen,
				 ifp->ifindex, action, &ctx);
}

/**
 * Uninstall SRv6 SID from the forwarding plane through Zebra.
 *
 * @param area		IS-IS area
 * @param sid		SRv6 SID
 */
void isis_zebra_srv6_sid_uninstall(struct isis_area *area,
				   struct isis_srv6_sid *sid)
{
	enum seg6local_action_t action = ZEBRA_SEG6_LOCAL_ACTION_UNSPEC;
	struct interface *ifp;
	uint16_t prefixlen = IPV6_MAX_BITLEN;

	if (!area || !sid)
		return;

	sr_debug("ISIS-SRv6 (%s): delete SID %pI6", area->area_tag, &sid->sid);

	switch (sid->behavior) {
	case SRV6_ENDPOINT_BEHAVIOR_END:
	case SRV6_ENDPOINT_BEHAVIOR_END_PSP:
		prefixlen = IPV6_MAX_BITLEN;
		break;
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID:
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID_PSP:
		prefixlen = sid->locator->block_bits_length +
			    sid->locator->node_bits_length;
		break;
	case SRV6_ENDPOINT_BEHAVIOR_RESERVED:
	case SRV6_ENDPOINT_BEHAVIOR_END_X:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT6:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT4:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT46:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT6_USID:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT4_USID:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT46_USID:
	case SRV6_ENDPOINT_BEHAVIOR_OPAQUE:
	case SRV6_ENDPOINT_BEHAVIOR_END_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_RED:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_NEXT_CSID:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_RED_NEXT_CSID:
	default:
		zlog_err(
			"ISIS-SRv6 (%s): unsupported SRv6 endpoint behavior %u",
			area->area_tag, sid->behavior);
		return;
	}

	/* The SID is attached to the SRv6 interface */
	ifp = if_lookup_by_name(area->srv6db.config.srv6_ifname, VRF_DEFAULT);
	if (!ifp) {
		zlog_warn("%s interface not found: nothing to uninstall",
			  area->srv6db.config.srv6_ifname);
		return;
	}

	/* Send delete request to zebra */
	isis_zebra_send_localsid(ZEBRA_ROUTE_DELETE, &sid->sid, prefixlen,
				 ifp->ifindex, action, NULL);
}

void isis_zebra_srv6_adj_sid_install(struct srv6_adjacency *sra)
{
	enum seg6local_action_t action = ZEBRA_SEG6_LOCAL_ACTION_UNSPEC;
	struct seg6local_context ctx = {};
	uint16_t prefixlen = IPV6_MAX_BITLEN;
	struct interface *ifp;
	struct isis_circuit *circuit;
	struct isis_area *area;

	if (!sra)
		return;

	circuit = sra->adj->circuit;
	area = circuit->area;

	sr_debug("ISIS-SRv6 (%s): setting adjacency SID %pI6", area->area_tag,
		 &sra->sid);

	switch (sra->behavior) {
	case SRV6_ENDPOINT_BEHAVIOR_END_X:
		action = ZEBRA_SEG6_LOCAL_ACTION_END_X;
		prefixlen = IPV6_MAX_BITLEN;
		ctx.nh6 = sra->nexthop;
		break;
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID:
		action = ZEBRA_SEG6_LOCAL_ACTION_END_X;
		prefixlen = sra->locator->block_bits_length +
			    sra->locator->node_bits_length +
			    sra->locator->function_bits_length;
		ctx.nh6 = sra->nexthop;
		SET_SRV6_FLV_OP(ctx.flv.flv_ops,
				ZEBRA_SEG6_LOCAL_FLV_OP_NEXT_CSID);
		ctx.flv.lcblock_len = sra->locator->block_bits_length;
		ctx.flv.lcnode_func_len = sra->locator->node_bits_length;
		break;
	case SRV6_ENDPOINT_BEHAVIOR_RESERVED:
	case SRV6_ENDPOINT_BEHAVIOR_END:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT6:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT4:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT46:
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT6_USID:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT4_USID:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT46_USID:
	case SRV6_ENDPOINT_BEHAVIOR_OPAQUE:
	case SRV6_ENDPOINT_BEHAVIOR_END_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_RED:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_NEXT_CSID:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_RED_NEXT_CSID:
	default:
		zlog_err(
			"ISIS-SRv6 (%s): unsupported SRv6 endpoint behavior %u",
			area->area_tag, sra->behavior);
		return;
	}

	ifp = sra->adj->circuit->interface;

	isis_zebra_send_localsid(ZEBRA_ROUTE_ADD, &sra->sid, prefixlen,
				 ifp->ifindex, action, &ctx);
}

void isis_zebra_srv6_adj_sid_uninstall(struct srv6_adjacency *sra)
{
	enum seg6local_action_t action = ZEBRA_SEG6_LOCAL_ACTION_UNSPEC;
	struct interface *ifp;
	uint16_t prefixlen = IPV6_MAX_BITLEN;
	struct isis_circuit *circuit;
	struct isis_area *area;

	if (!sra)
		return;

	circuit = sra->adj->circuit;
	area = circuit->area;

	switch (sra->behavior) {
	case SRV6_ENDPOINT_BEHAVIOR_END_X:
		prefixlen = IPV6_MAX_BITLEN;
		break;
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID:
		prefixlen = sra->locator->block_bits_length +
			    sra->locator->node_bits_length +
			    sra->locator->function_bits_length;
		break;
	case SRV6_ENDPOINT_BEHAVIOR_RESERVED:
	case SRV6_ENDPOINT_BEHAVIOR_END:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT6:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT4:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT46:
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT6_USID:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT4_USID:
	case SRV6_ENDPOINT_BEHAVIOR_END_DT46_USID:
	case SRV6_ENDPOINT_BEHAVIOR_OPAQUE:
	case SRV6_ENDPOINT_BEHAVIOR_END_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_NEXT_CSID_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID_PSP:
	case SRV6_ENDPOINT_BEHAVIOR_END_X_NEXT_CSID_PSP_USD:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_RED:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_NEXT_CSID:
	case SRV6_ENDPOINT_BEHAVIOR_END_B6_ENCAPS_RED_NEXT_CSID:
	default:
		zlog_err(
			"ISIS-SRv6 (%s): unsupported SRv6 endpoint behavior %u",
			area->area_tag, sra->behavior);
		return;
	}

	ifp = sra->adj->circuit->interface;

	sr_debug("ISIS-SRv6 (%s): delete End.X SID %pI6", area->area_tag,
		 &sra->sid);

	isis_zebra_send_localsid(ZEBRA_ROUTE_DELETE, &sra->sid, prefixlen,
				 ifp->ifindex, action, NULL);
}

/**
 * Internal function to process an SRv6 locator
 *
 * @param locator The locator to be processed
 */
static int isis_zebra_process_srv6_locator_internal(struct srv6_locator *locator)
{
	struct isis *isis = isis_lookup_by_vrfid(VRF_DEFAULT);
	struct isis_area *area;
	struct listnode *node;

	if (!isis || !locator)
		return -1;

	zlog_info("%s: Received SRv6 locator %s %pFX, loc-block-len=%u, loc-node-len=%u func-len=%u, arg-len=%u",
		  __func__, locator->name, &locator->prefix,
		  locator->block_bits_length, locator->node_bits_length,
		  locator->function_bits_length, locator->argument_bits_length);

	/* Walk through all areas of the ISIS instance */
	for (ALL_LIST_ELEMENTS_RO(isis->area_list, node, area)) {
		/*
		 * Check if the IS-IS area is configured to use the received
		 * locator
		 */
		if (strncmp(area->srv6db.config.srv6_locator_name, locator->name,
			    sizeof(area->srv6db.config.srv6_locator_name)) != 0) {
			zlog_err("%s: SRv6 Locator name unmatch %s:%s",
				 __func__, area->srv6db.config.srv6_locator_name,
				 locator->name);
			continue;
		}

		sr_debug("SRv6 locator (locator %s, prefix %pFX) set for IS-IS area %s",
			 locator->name, &locator->prefix, area->area_tag);

		/* Store the locator in the IS-IS area */
		area->srv6db.srv6_locator = srv6_locator_alloc(locator->name);
		srv6_locator_copy(area->srv6db.srv6_locator, locator);

		/* Request SIDs from the locator */
		request_srv6_sids(area);
	}

	return 0;
}

/**
 * Callback to process an SRv6 locator received from SRv6 Manager (zebra).
 *
 * @result 0 on success, -1 otherwise
 */
static int isis_zebra_process_srv6_locator_add(ZAPI_CALLBACK_ARGS)
{
	struct isis *isis = isis_lookup_by_vrfid(VRF_DEFAULT);
	struct srv6_locator loc = {};

	if (!isis)
		return -1;

	/* Decode the SRv6 locator */
	if (zapi_srv6_locator_decode(zclient->ibuf, &loc) < 0)
		return -1;

	return isis_zebra_process_srv6_locator_internal(&loc);
}

/**
 * Callback to process a notification from SRv6 Manager (zebra) of an SRv6
 * locator deleted.
 *
 * @result 0 on success, -1 otherwise
 */
static int isis_zebra_process_srv6_locator_delete(ZAPI_CALLBACK_ARGS)
{
	struct isis *isis = isis_lookup_by_vrfid(VRF_DEFAULT);
	struct srv6_locator loc = {};
	struct isis_area *area;
	struct listnode *node, *nnode;
	struct srv6_locator_chunk *chunk;
	struct isis_srv6_sid *sid;
	struct srv6_adjacency *sra;

	if (!isis)
		return -1;

	/* Decode the received zebra message */
	if (zapi_srv6_locator_decode(zclient->ibuf, &loc) < 0)
		return -1;

	sr_debug(
		"SRv6 locator deleted in zebra: name %s, "
		"prefix %pFX, block_len %u, node_len %u, func_len %u, arg_len %u",
		loc.name, &loc.prefix, loc.block_bits_length,
		loc.node_bits_length, loc.function_bits_length,
		loc.argument_bits_length);

	/* Walk through all areas of the ISIS instance */
	for (ALL_LIST_ELEMENTS_RO(isis->area_list, node, area)) {
		if (strncmp(area->srv6db.config.srv6_locator_name, loc.name,
			    sizeof(area->srv6db.config.srv6_locator_name)) != 0)
			continue;

		/* Delete SRv6 SIDs */
		for (ALL_LIST_ELEMENTS(area->srv6db.srv6_sids, node, nnode,
				       sid)) {

			sr_debug(
				"Deleting SRv6 SID (locator %s, sid %pI6) from IS-IS area %s",
				area->srv6db.config.srv6_locator_name,
				&sid->sid, area->area_tag);

			/* Uninstall the SRv6 SID from the forwarding plane
			 * through Zebra */
			isis_zebra_srv6_sid_uninstall(area, sid);

			listnode_delete(area->srv6db.srv6_sids, sid);
			isis_srv6_sid_free(sid);
		}

		/* Uninstall all local Adjacency-SIDs. */
		for (ALL_LIST_ELEMENTS(area->srv6db.srv6_endx_sids, node, nnode,
				       sra))
			srv6_endx_sid_del(sra);

		/* Free the SRv6 locator chunks */
		for (ALL_LIST_ELEMENTS(area->srv6db.srv6_locator_chunks, node,
				       nnode, chunk)) {
			if (prefix_match((struct prefix *)&loc.prefix,
					 (struct prefix *)&chunk->prefix)) {
				listnode_delete(
					area->srv6db.srv6_locator_chunks,
					chunk);
				srv6_locator_chunk_free(&chunk);
			}
		}

		srv6_locator_free(area->srv6db.srv6_locator);
		area->srv6db.srv6_locator = NULL;

		/* Regenerate LSPs to advertise that the locator no longer
		 * exists */
		lsp_regenerate_schedule(area, area->is_type, 0);
	}

	return 0;
}

/**
 * Request an SRv6 locator chunk to the SRv6 Manager (zebra) asynchronously.
 *
 * @param locator_name Name of SRv6 locator
 *
 * @result 0 on success, -1 otherwise
 */
int isis_zebra_srv6_manager_get_locator_chunk(const char *name)
{
	return srv6_manager_get_locator_chunk(isis_zclient, name);
}


/**
 * Release an SRv6 locator chunk.
 *
 * @param locator_name Name of SRv6 locator
 *
 * @result 0 on success, -1 otherwise
 */
int isis_zebra_srv6_manager_release_locator_chunk(const char *name)
{
	return srv6_manager_release_locator_chunk(isis_zclient, name);
}

/**
 * Ask the SRv6 Manager (zebra) about a specific locator
 *
 * @param name Locator name
 * @return 0 on success, -1 otherwise
 */
int isis_zebra_srv6_manager_get_locator(const char *name)
{
	if (!name)
		return -1;

	/*
	 * Send the Get Locator request to the SRv6 Manager and return the
	 * result
	 */
	return srv6_manager_get_locator(isis_zclient, name);
}

/**
 * Ask the SRv6 Manager (zebra) to allocate a SID.
 *
 * Optionally, it is possible to provide an IPv6 address (sid_value parameter).
 *
 * When sid_value is provided, the SRv6 Manager allocates the requested SID
 * address, if the request can be satisfied (explicit allocation).
 *
 * When sid_value is not provided, the SRv6 Manager allocates any available SID
 * from the provided locator (dynamic allocation).
 *
 * @param ctx Context to be associated with the request SID
 * @param sid_value IPv6 address to be associated with the requested SID (optional)
 * @param locator_name Name of the locator from which the SID must be allocated
 */
bool isis_zebra_request_srv6_sid(const struct srv6_sid_ctx *ctx,
				 struct in6_addr *sid_value,
				 const char *locator_name)
{
	int ret;

	if (!ctx || !locator_name)
		return false;

	/*
	 * Send the Get SRv6 SID request to the SRv6 Manager and check the
	 * result
	 */
	ret = srv6_manager_get_sid(isis_zclient, ctx, sid_value, locator_name, NULL, false);
	if (ret < 0) {
		zlog_warn("%s: error getting SRv6 SID!", __func__);
		return false;
	}

	return true;
}

/**
 * Ask the SRv6 Manager (zebra) to release a previously allocated SID.
 *
 * This function is used to tell the SRv6 Manager that IS-IS no longer intends
 * to use the SID.
 *
 * @param ctx Context to be associated with the SID to be released
 * @param locator_name Parent locator of the SID
 */
void isis_zebra_release_srv6_sid(const struct srv6_sid_ctx *ctx, const char *locator_name)
{
	int ret;

	if (!ctx || !locator_name)
		return;

	/*
	 * Send the Release SRv6 SID request to the SRv6 Manager and check the
	 * result
	 */
	ret = srv6_manager_release_sid(isis_zclient, ctx, locator_name, false);
	if (ret < 0) {
		zlog_warn("%s: error releasing SRv6 SID!", __func__);
		return;
	}
}

static int isis_zebra_srv6_sid_notify(ZAPI_CALLBACK_ARGS)
{
	struct isis *isis = isis_lookup_by_vrfid(VRF_DEFAULT);
	struct srv6_sid_ctx ctx;
	struct in6_addr sid_addr;
	enum zapi_srv6_sid_notify note;
	uint32_t sid_func;
	struct isis_area *area;
	struct listnode *node, *nnode, *n;
	char buf[256];
	struct srv6_locator *locator;
	struct prefix_ipv6 tmp_prefix;
	struct srv6_adjacency *sra;
	enum srv6_endpoint_behavior_codepoint behavior;
	struct isis_srv6_sid *sid;
	struct isis_adjacency *adj;

	if (!isis)
		return -1;

	/* Decode the received notification message */
	if (!zapi_srv6_sid_notify_decode(zclient->ibuf, &ctx, &sid_addr,
					 &sid_func, NULL, &note, NULL)) {
		zlog_err("%s : error in msg decode", __func__);
		return -1;
	}

	sr_debug("%s: received SRv6 SID notify: ctx %s sid_value %pI6 sid_func %u note %s",
		 __func__, srv6_sid_ctx2str(buf, sizeof(buf), &ctx), &sid_addr,
		 sid_func, zapi_srv6_sid_notify2str(note));

	for (ALL_LIST_ELEMENTS_RO(isis->area_list, node, area)) {
		if (!area->srv6db.config.enabled || !area->srv6db.srv6_locator)
			continue;

		locator = area->srv6db.srv6_locator;

		/* Verify that the received SID belongs to the configured locator */
		if (note == ZAPI_SRV6_SID_ALLOCATED) {
			tmp_prefix.family = AF_INET6;
			tmp_prefix.prefixlen = IPV6_MAX_BITLEN;
			tmp_prefix.prefix = sid_addr;

			if (!prefix_match((struct prefix *)&locator->prefix,
					  (struct prefix *)&tmp_prefix)) {
				sr_debug("%s : ignoring SRv6 SID notify: locator (area %s) does not match",
					 __func__, area->area_tag);
				continue;
			}
		}

		/* Handle notification */
		switch (note) {
		case ZAPI_SRV6_SID_ALLOCATED:
			sr_debug("SRv6 SID %pI6 %s ALLOCATED", &sid_addr,
				 srv6_sid_ctx2str(buf, sizeof(buf), &ctx));

			if (ctx.behavior == ZEBRA_SEG6_LOCAL_ACTION_END) {
				/* Remove old End SIDs, if any */
				for (ALL_LIST_ELEMENTS(area->srv6db.srv6_sids,
						       node, nnode, sid)) {
					isis_zebra_srv6_sid_uninstall(area, sid);
					listnode_delete(area->srv6db.srv6_sids,
							sid);
					isis_srv6_sid_free(sid);
				}

				/* Allocate new SRv6 End SID */
				behavior = SRV6_BEHAVIOR_CODEPOINT(SRV6_ENDPOINT_BEHAVIOR_END,
								   locator->flags);
				sid = isis_srv6_sid_alloc(area,
							  area->srv6db
								  .srv6_locator,
							  behavior, &sid_addr);
				if (!sid) {
					zlog_warn("%s: isis_srv6_sid_alloc failed",
						  __func__);
					return -1;
				}

				/*
				 * Install the new SRv6 End SID in the forwarding plane through
				 * Zebra
				 */
				isis_zebra_srv6_sid_install(area, sid);

				/* Store the SID */
				listnode_add(area->srv6db.srv6_sids, sid);

			} else if (ctx.behavior ==
				   ZEBRA_SEG6_LOCAL_ACTION_END_X) {
				for (ALL_LIST_ELEMENTS_RO(area->adjacency_list,
							  n, adj)) {
					/* Check if the End.X SID is for this adjacecny */
					if (adj->ll_ipv6_count == 0 ||
					    memcmp(&adj->ll_ipv6_addrs[0],
						   &ctx.nh6,
						   sizeof(struct in6_addr)) != 0)
						continue;

					/* Remove old End.X SIDs, if any */
					for (ALL_LIST_ELEMENTS(adj->srv6_endx_sids,
							       node, nnode, sra))
						srv6_endx_sid_del(sra);

					/* Allocate new End.X SID for the adjacency */
					srv6_endx_sid_add_single(adj, false,
								 NULL,
								 &sid_addr);
				}
			} else {
				zlog_warn("%s: unsupported behavior %u",
					  __func__, ctx.behavior);
				return -1;
			}
			break;
		case ZAPI_SRV6_SID_RELEASED:
			sr_debug("SRv6 SID %pI6 %s: RELEASED", &sid_addr,
				 srv6_sid_ctx2str(buf, sizeof(buf), &ctx));
			break;
		case ZAPI_SRV6_SID_FAIL_ALLOC:
			sr_debug("SRv6 SID %pI6 %s: Failed to allocate",
				 &sid_addr,
				 srv6_sid_ctx2str(buf, sizeof(buf), &ctx));

			/* Error will be logged by zebra module */
			break;
		case ZAPI_SRV6_SID_FAIL_RELEASE:
			zlog_warn("%s: SRv6 SID %pI6 %s failure to release",
				  __func__, &sid_addr,
				  srv6_sid_ctx2str(buf, sizeof(buf), &ctx));

			/* Error will be logged by zebra module */
			break;
		}

		/* Regenerate LSPs to advertise the new locator and the SID */
		lsp_regenerate_schedule(area, area->is_type, 0);
	}

	return 0;
}

static zclient_handler *const isis_handlers[] = {
	[ZEBRA_ROUTER_ID_UPDATE] = isis_router_id_update_zebra,
	[ZEBRA_INTERFACE_ADDRESS_ADD] = isis_zebra_if_address_add,
	[ZEBRA_INTERFACE_ADDRESS_DELETE] = isis_zebra_if_address_del,
	[ZEBRA_INTERFACE_LINK_PARAMS] = isis_zebra_link_params,
	[ZEBRA_REDISTRIBUTE_ROUTE_ADD] = isis_zebra_read,
	[ZEBRA_REDISTRIBUTE_ROUTE_DEL] = isis_zebra_read,

	[ZEBRA_OPAQUE_MESSAGE] = isis_opaque_msg_handler,

	[ZEBRA_CLIENT_CLOSE_NOTIFY] = isis_zebra_client_close_notify,

	[ZEBRA_SRV6_LOCATOR_ADD] = isis_zebra_process_srv6_locator_add,
	[ZEBRA_SRV6_LOCATOR_DELETE] = isis_zebra_process_srv6_locator_delete,
	[ZEBRA_SRV6_SID_NOTIFY] = isis_zebra_srv6_sid_notify,
};

void isis_zebra_init(struct event_loop *mst, int instance)
{
	/* Initialize asynchronous zclient. */
	isis_zclient = zclient_new(mst, &zclient_options_default, isis_handlers,
			      array_size(isis_handlers));
	zclient_init(isis_zclient, PROTO_TYPE, 0, &isisd_privs);
	isis_zclient->zebra_connected = isis_zebra_connected;

	/* Initialize special zclient for synchronous message exchanges. */
	zclient_sync = zclient_new(mst, &zclient_options_sync, NULL, 0);
	zclient_sync->sock = -1;
	zclient_sync->redist_default = ZEBRA_ROUTE_ISIS;
	zclient_sync->instance = instance;
	/*
	 * session_id must be different from default value (0) to distinguish
	 * the asynchronous socket from the synchronous one
	 */
	zclient_sync->session_id = 1;
	zclient_sync->privs = &isisd_privs;
}

void isis_zebra_stop(void)
{
	zclient_unregister_opaque(isis_zclient, LDP_RLFA_LABELS);
	zclient_unregister_opaque(isis_zclient, LDP_IGP_SYNC_IF_STATE_UPDATE);
	zclient_unregister_opaque(isis_zclient, LDP_IGP_SYNC_ANNOUNCE_UPDATE);
	zclient_stop(zclient_sync);
	zclient_free(zclient_sync);
	zclient_stop(isis_zclient);
	zclient_free(isis_zclient);
}

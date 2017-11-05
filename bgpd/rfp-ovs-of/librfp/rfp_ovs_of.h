/*
 *
 * Copyright 2015-2017, LabN Consulting, L.L.C.
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

/* Sample header file */
#ifndef _RFP_OFS_OF_H
#define _RFP_OFS_OF_H

#define RFP_OVS_OF_NO_CONTROLLER        /* disable OVS learning */

struct rfp_ovs_of_out {
	uint16_t vid;
	uint32_t port;
#define RFP_OFS_OF_NO_QUEUE_ID UINT32_MAX
	uint32_t queue_id;
};

#ifndef _QUAGGA_BGP_RFAPI_H /* for ovs code only */
struct lswitch *lswitch;
extern void rfp_ovs_set_sw_features(struct lswitch *lswitch,
				    unsigned long long int datapath_id);

extern int rfp_ovs_of_mac(int add, struct lswitch *lswitch,
			  unsigned long long int datapath_id, uint16_t vid,
			  const struct eth_addr *mac, ovs_be32 ipv4_src,
			  const struct in6_addr *ipv6_src, uint32_t port,
			  uint32_t lifetime);

#define RFP_OFS_OF_MAX_PORTS   256      /* for now */

extern int rfp_ovs_of_lookup(struct lswitch *lswitch,
			     unsigned long long int datapath_id, uint16_t vid,
			     const struct eth_addr *mac, ovs_be32 ipv4_src,
			     const struct in6_addr *ipv6_src, uint32_t in_port,
			     uint32_t port_list_size,
			     struct rfp_ovs_of_out out_list[]);

/* from prefix.h */
# define ETHER_ADDR_LEN 6
struct ethaddr {
	u_char octet[ETHER_ADDR_LEN];
} __packed;

/* from rfapi.h */
#define RFAPI_INFINITE_LIFETIME         0xFFFFFFFF
struct rfapi_ip_addr {
	uint8_t addr_family; /* AF_INET | AF_INET6 */
	union {
		struct in_addr v4;  /* in network order */
		struct in6_addr v6; /* in network order */
	} addr;
};

struct rfapi_ip_prefix {
	uint8_t length;
	uint8_t cost;
	struct rfapi_ip_addr prefix;
};
#else
#define OFPP_NONE  0xffff
#define OFPP_FLOOD 0xfffb
typedef uint16_t ofp_port_t;
#endif

extern void *rfp_switch_open(void *parent, struct sockaddr_storage *ss,
			     void *data);
extern int rfp_switch_close(void *rfd);
extern int rfp_mac_add_drop(void *parent, void *rfd, int add,
			    const struct ethaddr *mac,
			    struct rfapi_ip_prefix *prefix,
			    unsigned long long int datapath_id, int use_vlans,
			    uint16_t vid, uint32_t port, uint32_t lifetime);

extern int rfp_mac_lookup(void *parent, void *rfd, const struct ethaddr *mac,
			  struct rfapi_ip_addr *target,
			  unsigned long long int datapath_id, int use_vlans,
			  uint16_t vid, uint32_t in_port,
			  uint32_t port_list_size,
			  struct rfp_ovs_of_out port_list[]);
extern int rfp_get_ports_by_group(void *parent, void *rfd,
				  unsigned long long int datapath_id,
				  int use_vlans, uint16_t vid, uint32_t in_port,
				  uint32_t port_list_size,
				  struct rfp_ovs_of_out port_list[]);
extern int rfp_get_ports_by_datapath_id(void *parent,
					unsigned long long int datapath_id,
					int use_vlans, uint32_t port_list_size,
					struct rfp_ovs_of_out port_list[]);
extern void rfp_get_l2_group_str_by_pnum(char *buf, void *parent,
					 unsigned long long int datapath_id,
					 int use_vlans, uint32_t port);

typedef enum {
	RFP_FLOW_MODE_SPECIFIC,
	RFP_FLOW_MODE_WILDCARDS
} rfp_flow_mode_t;

extern rfp_flow_mode_t rfp_get_flow_mode(void *parent);

extern int rfp_get_modes(void *parent, unsigned long long int datapath_id,
			 rfp_flow_mode_t *flow_mode, int *use_vlans);
extern void rfp_output(void *vty, const char *string);
#endif /* _RFP_OFS_OF_H */

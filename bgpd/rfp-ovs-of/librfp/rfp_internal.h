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
#ifndef _RFP_INTERNAL_H
#define _RFP_INTERNAL_H

#include "rfp_ovs_of.h"

extern int rfp_ovs_of_main(int argc, char *argv[]);
extern int rfp_ovs_of_listen(void *vooi, uint16_t port);
extern int rfp_ovs_of_close_all(void *vooi);
extern int rfp_ovs_of_switch_close(void *vooi, void *rfd, int reason);
extern const char *rfp_ovs_of_set_loglevel(const char *ovs_lvl);
extern void *rfp_ovs_of_init(void *parent, int argc, char *argv[],
			     const char *ovs_lvl);
extern void rfp_ovs_of_destroy(void *vooi);
extern void rfp_ovs_of_reset_all(void *vooi);
extern int rfp_ovs_of_reset_all_by_ss(void *vooi, struct sockaddr_storage *ss);
extern int rfp_ovs_of_reset_all_by_dpid(void *vooi,
					unsigned long long int dpid);
extern void rfp_ovs_of_reset_flows(void *vooi);
extern int rfp_ovs_of_reset_flow_by_ss(void *vooi, struct sockaddr_storage *ss);
extern int rfp_ovs_of_reset_flow_by_dpid(void *vooi,
					 unsigned long long int dpid);
typedef void(rfp_fd_cb_t)(void *vooi, int fd, void *data);
extern void rfp_fd_listen(void *parent, int fd, rfp_fd_cb_t *cb, void *data);
extern int rfp_fd_close(void *parent, int fd);
extern void rfp_ovs_of_show_switches(void *vty, void *vooi,
				     unsigned long long int dpid);
#endif /* _RFP_INTERNAL_H */

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
//#define RFP_OVS_OF_TEST_UN_TT 1
#include "zebra.h"
#include "rfp.h"
#include "rfp_internal.h"
#include "rfapi.h"
#include "command.h"
#include "linklist.h"
#include "memory.h"
#include <stdlib.h>

#include "bgp_rfapi_cfg.h"      /* really RFAPI internal */
#include "bgp_ecommunity.h" /* for rfp_get_l2_group_str_by_pnum */

DEFINE_MGROUP(RFP, "rfp")
DEFINE_MTYPE(RFP, RFP_GENERIC, "Generic OF RFP pointer")

extern int
rfapiCliGetRfapiIpAddr (struct vty *, const char *, struct rfapi_ip_addr *);

struct rfp_thread_list
{
  struct rfp_thread_list *next;
  int fd;
  struct thread *thread;
  rfp_fd_cb_t *cb;
  void *data;
  void *parent;
};

/*
 * Group config used two ways.  
 * 1) to indicate use of wildcards and VIDs
 *     the LNI of this setting is always the non-VID form
 * 2) to indicate vid+dipd used by specific ports
 */
typedef enum
{
  RFP_GRP_CFG_INIT,
  RFP_GRP_CFG_ANY,              /* used on get calls only */
  RFP_GRP_CFG_DPID,
  RFP_GRP_CFG_VID,
  RFP_GRP_CFG_MODES
} rfp_group_config_type;

static const char *rfp_group_config_names[] = {
  "Init",
  "",                           /* for internal use only */
  "DPID",
  "VLAN ID",
  "Configuration"
};

struct rfp_group_config
{
  rfp_group_config_type type;
  unsigned long long int datapath_id;   /* DPID */
  union
  {
    int vid;                    /* -1 = not set | ignore */
    struct
    {
      int use_vlans;            /* -1 = no config [default] */
      int use_wildcards;        /* -1 = no config [default] */
    } modes;
  } u;
};

struct rfp_instance_t
{
  struct rfapi_rfp_cfg rfapi_config;
  struct rfapi_rfp_cb_methods rfapi_callbacks;
  struct thread_master *master;
  uint32_t config_var;
  void *ooi;                    /* ovs of instance */
  struct list *listen_ports;    /* <val>s are port numbers */
  struct rfp_thread_list *threads;
  /* config items */
  int use_vlans;                /* rfp wide for now, per l2 group in future? */
  int use_wildcards;            /* rfp wide for now, per nve in future? */
  int loglevel;
};

typedef struct
{
  const char *cmd;
  int ncmp;                     /* #chars for unambigious check */
  const char *ovs;
} rfp_log_levels_t;

static const rfp_log_levels_t rfp_log_levels[] = {
  {"off", 1, "OFF"},            /* config default */
  {"emergency", 2, "EMER"},
  {"error", 2, "ERR"},
  {"warning", 1, "WARN"},
  {"info", 1, "INFO"},
  {"debug", 1, "DBG"},
  {NULL, 0, NULL}               /* end marker */
};

#define RFP_INIT_LOGLEVEL 0     /* non-zero for debugging */

#define RFI_ACTIVE(rfi)                 \
    ((rfi)!=NULL && (rfi)->listen_ports != NULL)
#define USE_VLANS(rfi,uv)        \
    ((uv) > 0 || ((uv) == -1 && (rfi)->use_vlans))
#define OF_CONFIG_STR "OpenFlow related configuration\n"
#define OF_SHOW_STR   "OpenFlow information\n"

struct rfp_instance_t global_rfi;       /* dynamically allocate in future */

/***********************************************************************
 *			utility functions
 ***********************************************************************/
static int
rfp_do_reset_all (struct rfp_instance_t *rfi)
{
  int ret = CMD_SUCCESS;
  struct listnode *node;
  void *data;
  rfp_ovs_of_close_all (rfi->ooi);
  rfp_ovs_of_reset_all (rfi->ooi);
  for (ALL_LIST_ELEMENTS_RO (rfi->listen_ports, node, data))
    {
      uint16_t port = (uint16_t) (((uintptr_t) data) & 0xffff);
      if (rfp_ovs_of_listen (rfi->ooi, port))
        ret = CMD_WARNING;
    }
  return ret;
}

#define RFP_VLAN_MASK 0xfff
static uint32_t
rfp_get_lni (struct rfp_instance_t *rfi, 
             unsigned long long int datapath_id, 
             int use_vlans,     /* -1 = use default */
             uint16_t vid)
{
  uint32_t lni = 0;             /* 24-bit logical network ID */
  int count;
  count = sizeof (datapath_id) / sizeof (lni);
  while (--count > 0)
    {
      lni += (uint32_t) (datapath_id & 0xffffffff);
      datapath_id >>= 32;
    }
  lni = 0xffffff & ((lni & 0xff) + (lni >> 8));
  if (USE_VLANS (rfi, use_vlans))
    lni = 0xffffff & ((lni << 12) + (vid & RFP_VLAN_MASK));
  return lni;
}

rfp_flow_mode_t
rfp_get_flow_mode (void *parent)
{
  struct rfp_instance_t *rfi = parent;
  if (rfi->use_wildcards)
    return RFP_FLOW_MODE_WILDCARDS;
  else
    return RFP_FLOW_MODE_SPECIFIC;
}

static int
rfp_group_config_search_cb (void *criteria, void *rfp_cfg_group)
{
  rfp_group_config_type type = (rfp_group_config_type) criteria;
  struct rfp_group_config *rgc = rfp_cfg_group;
  if (rgc != NULL && rgc->type == type)
    return 0;
  return ENOENT;                /* not a match */
}

int
rfp_get_modes (void *parent,
               unsigned long long int datapath_id,
               rfp_flow_mode_t * flow_mode, int *use_vlans)
{
  struct rfp_instance_t *rfi = parent;
  int ret = 0;
  struct rfp_group_config *rgc;

  if (parent == NULL)
    return 0;

  rgc = rfapi_rfp_get_l2_group_config_ptr_lni (rfi,
                                               rfp_get_lni (rfi, datapath_id,
                                                            0, 0),
                                               (void *) RFP_GRP_CFG_MODES,
                                               rfp_group_config_search_cb);
  /* use default values */
  if (flow_mode)
    *flow_mode = rfp_get_flow_mode (rfi);
  if (use_vlans)
    *use_vlans = -1;

  if (rgc && rgc->type == RFP_GRP_CFG_MODES)
    {
      if (flow_mode && rgc->u.modes.use_wildcards != -1)
        {
          if (rgc->u.modes.use_wildcards)
            *flow_mode = RFP_FLOW_MODE_WILDCARDS;
          else
            *flow_mode = RFP_FLOW_MODE_SPECIFIC;
          ret++;
        }
      if (use_vlans && rgc->u.modes.use_vlans != -1)
        {
          *use_vlans = rgc->u.modes.use_vlans;
          ret++;
        }
    }
  else
    zlog_debug ("%s: %016llx using defaults: flow_mode=%d, use_vlans=-1",
                __func__, datapath_id, rfp_get_flow_mode (rfi));

  return ret;
}

static void
rfp_reset_group_config_type (struct rfp_group_config *rgc)
{
  /* check ofr consistency and reset type, based on settings */
  if (rgc == NULL)
    return;
  switch (rgc->type)
    {
    case RFP_GRP_CFG_INIT:
      break;                    /* nothing to clear */
    case RFP_GRP_CFG_MODES:
      if (rgc->u.modes.use_vlans == -1 && rgc->u.modes.use_wildcards == -1)
        rgc->type = RFP_GRP_CFG_DPID;
      /* intentional fall through */
    case RFP_GRP_CFG_DPID:
    case RFP_GRP_CFG_VID:
      if (rgc->datapath_id == 0)
        rgc->type = RFP_GRP_CFG_INIT;
      break;
    default:
      zlog_warn ("%s: unexpected config type (%d)!", __func__, rgc->type);
      break;
    }
  return;
}


static void
rfp_set_group_config_type (struct rfp_group_config *rgc,
                           rfp_group_config_type type)
{
  if (rgc == NULL)
    return;
  switch (type)
    {
    case RFP_GRP_CFG_DPID:
      if (rgc->type == RFP_GRP_CFG_MODES)
        type = rgc->type;       /* leave MODES type */
      break;
    case RFP_GRP_CFG_VID:
      rgc->u.vid = -1;
      break;
    case RFP_GRP_CFG_MODES:
      rgc->u.modes.use_vlans = -1;
      rgc->u.modes.use_wildcards = -1;
      break;
    default:
      zlog_warn ("%s: unexpected config type (%d)!", __func__, rgc->type);
      break;
    }
  rgc->type = type;
  return;
}

static struct rfp_group_config *
rfp_get_group_config_vty (struct rfp_instance_t *rfi,
                          struct vty *vty, rfp_group_config_type type)
{
  struct rfp_group_config *rgc;

  rgc = rfapi_rfp_get_group_config_ptr_vty (rfi, RFAPI_RFP_CFG_GROUP_L2, vty);
  if (rgc == NULL)
    {
      rgc = rfapi_rfp_init_group_config_ptr_vty (rfi, RFAPI_RFP_CFG_GROUP_L2,
                                                 vty, sizeof (*rgc));
      if (type != RFP_GRP_CFG_ANY)
        rfp_set_group_config_type (rgc, type);
    }
  else if (type != RFP_GRP_CFG_ANY &&
           type != rgc->type &&
           rgc->type != RFP_GRP_CFG_INIT && rgc->type != RFP_GRP_CFG_DPID)
    {
      /* check for incompatible types */
      vty_out (vty,
               "Unable to change configuration. Group is of type %s.%s",
               rfp_group_config_names[rgc->type], VTY_NEWLINE);
      return NULL;
    }
  return rgc;
}

static int
rfp_get_loglevel_index (const char *cmd)
{
  int ret = 0;

  while (rfp_log_levels[ret].ncmp != 0 &&
         strncmp (cmd, rfp_log_levels[ret].cmd,
                  rfp_log_levels[ret].ncmp) != 0)
    ret++;
  if (rfp_log_levels[ret].ncmp == 0)
    ret = -1;                   /* no match */
  return ret;
}

static const char *
rfp_get_loglevel_cmd (int index)
{
  if (index < 0)
    return NULL;
  return rfp_log_levels[index].cmd;
}

static const char *
rfp_get_loglevel_ovs (int index)
{
  if (index < 0)
    return NULL;
  return rfp_log_levels[index].ovs;
}


/***********************************************************************
 *			CLI/CONFIG
 ***********************************************************************/
extern void
rfp_output(void *vvty, const char *string)
{
  struct vty *vty = vvty;
  if (vty == NULL)
    return;
  if (string != NULL)
    vty_out(vty, string);
  vty_out (vty, "%s", VTY_NEWLINE);
}

DEFUN (vnc_rfp_loglevel,
       vnc_rfp_loglevel_cmd,
       "vnc openflow log-level (off|emergency|error|warning|info|debug)",
       VNC_CONFIG_STR
       OF_CONFIG_STR
       "Set the log level of OVS OpenFlow\n"
       "OVS OpenFlow logging disabled [default]\n"
       "Log emergency level information         (Level 1)\n"
       "Log error level, and lower, information (Level 2)\n"
       "Log warnings and lower levels           (Level 3)\n"
       "Log general, and lower, information     (Level 4)\n"
       "Log all available information           (Level 5)\n")
{
  int ll;
  const char *error;
  struct rfp_instance_t *rfi = NULL;
  rfi = rfapi_get_rfp_start_val (vty->index);   /* index=bgp for BGP_NODE */

  ll = rfp_get_loglevel_index (argv[0]);
  if (ll < 0)
    {
      vty_out (vty, "%s isn't a recognized logging level.%s",
               argv[0], VTY_NEWLINE);
      return CMD_WARNING;
    }
  error = rfp_ovs_of_set_loglevel (rfp_get_loglevel_ovs (ll));
  if (error != NULL)
    {
      vty_out (vty, "Error setting OpenFlow log level: %s%s",
               error, VTY_NEWLINE);
      return CMD_WARNING;
    }
  rfi->loglevel = ll;
  return CMD_SUCCESS;
}

DEFUN (vnc_show_rfp_listen_ports,
       vnc_show_rfp_listen_ports_cmd,
       "show vnc rfp-listen-ports",
       SHOW_STR RFAPI_SHOW_STR "Display RFP listening ports\n")
{
  int lc = 0;
  struct listnode *node;
  void *data;
  struct rfp_instance_t *rfi = NULL;
  rfi = rfapi_get_rfp_start_val (bgp_get_default ());   /* assume 1 instance for now */
  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  if (rfi->listen_ports != NULL)
    lc = listcount (rfi->listen_ports);
  if (lc == 0)
    {
      vty_out (vty, "Not listening for RFP/OpenFlow messages.%s",
               VTY_NEWLINE);
      return CMD_WARNING;
    }

  vty_out (vty, "Configured for RFP/OpenFlow messages on port%s   ",
           (lc == 1 ? ": " : "s:"));
  for (ALL_LIST_ELEMENTS_RO (rfi->listen_ports, node, data))
    {
      vty_out (vty, "%u ", (uint16_t) ((uintptr_t) data) & 0xffff);
    }
  vty_out (vty, "%s", VTY_NEWLINE);
  return CMD_SUCCESS;
}

ALIAS (vnc_show_rfp_listen_ports,
       vnc_show_of_listen_ports_cmd,
       "show vnc openflow listen-ports",
       SHOW_STR
       RFAPI_SHOW_STR
       OF_SHOW_STR
       "Display OpenFlow listening ports\n")
DEFUN (vnc_listen_ports,
       vnc_listen_ports_cmd,
       "vnc rfp-listen-ports .PORTLIST",
       VNC_CONFIG_STR
       "Configure RFP listen-ports\n"
       "Space separated list of TCP port numbers <0-65535>, 0 = disable RFP\n")
{
  int ret = CMD_SUCCESS;
  struct list *pl = list_new ();
  int disable = 0;
  struct rfp_instance_t *rfi = NULL;
  rfi = rfapi_get_rfp_start_val (vty->index);   /* index=bgp for BGP_NODE */

  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!pl)
    {
      vty_out (vty, "Out of memory%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (argc > 16)
    {
      vty_out (vty, "Only first 16 ports will be added%s", VTY_NEWLINE);
      argc = 16;
      return CMD_WARNING;
    }

  for (; argc; --argc, ++argv)
    {
      int pnum;

      VTY_GET_INTEGER_RANGE ("Port number", pnum, argv[0], 0, 65535);
      if (pnum == 0)
        disable = 1;            /* RFP is to be disabled */
      else
        listnode_add (pl, (void *) (uintptr_t) pnum);
    }

  /* save new port list */
  rfp_ovs_of_close_all (rfi->ooi);
  if (rfi->listen_ports)
    {
      list_delete (rfi->listen_ports);
      rfi->listen_ports = NULL;
    }
  if (disable)
    {
      list_delete (pl);
    }
  else
    {
      struct listnode *node;
      void *data;
      rfi->listen_ports = pl;
      for (ALL_LIST_ELEMENTS_RO (rfi->listen_ports, node, data))
        {
          uint16_t port = (uint16_t) (((uintptr_t) data) & 0xffff);
          if (rfp_ovs_of_listen (rfi->ooi, port))
            ret = CMD_WARNING;
        }
    }

  return ret;
}

ALIAS (vnc_listen_ports,
       vnc_of_listen_ports_cmd,
       "vnc openflow listen-ports .PORTLIST",
       VNC_CONFIG_STR
       OF_CONFIG_STR
       "Configure OpenFlow listen-ports (6633 OpenFlow 1.0 only)\n"
       "Space separated list of TCP port numbers <0-65535>, 0 = disable OpenFlow\n")
/* for compatibility with regression scripts */
  DEFUN (vnc_rfp_updated_responses,
       vnc_rfp_updated_responses_cmd,
       "vnc rfp-updated-responses (on|off)",
       VNC_CONFIG_STR
       "Control generation of updated RFP responses\n"
       "Enable updated RFP responses\n" "Disable updated RFP responses\n")
{
  vty_out (vty, "Command not supported%s", VTY_NEWLINE);
  return CMD_WARNING;
#if 0
  struct bgp *bgp;
  struct rfp_cli *rci;

  bgp = vty->index;
  rci = rfapi_get_rfp_start_val_by_bgp (bgp);

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!rci)
    {
      vty_out (vty, "VNC not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (argv[0][1] == 'f')
    {
      rci->rfp_cfg.use_updated_response = 0;
    }
  else
    {
      rci->rfp_cfg.use_updated_response = 1;
    }

  rfapi_rfp_set_configuration (rci, &rci->rfp_cfg);
  return CMD_SUCCESS;
#endif
}

/* for compatibility with regression scripts */
DEFUN (vnc_rfp_removal_responses,
       vnc_rfp_removal_responses_cmd,
       "vnc rfp-removal-responses (on|off)",
       VNC_CONFIG_STR
       "Control generation of updated RFP response-removal messages\n"
       "Enable updated RFP response-removal messages\n"
       "Disable updated RFP response removal messages\n")
{
  vty_out (vty, "Command not supported%s", VTY_NEWLINE);
  return CMD_WARNING;
#if 0
  struct bgp *bgp;
  struct rfp_cli *rci;

  bgp = vty->index;
  rci = rfapi_get_rfp_start_val_by_bgp (bgp);

  if (!bgp)
    {
      vty_out (vty, "No BGP process is configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (!rci)
    {
      vty_out (vty, "VNC not configured%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (argv[0][1] == 'f')
    {
      rci->rfp_cfg.use_removes = 0;
    }
  else
    {
      rci->rfp_cfg.use_removes = 1;
    }
  rfapi_rfp_set_configuration (rci, &rci->rfp_cfg);

  return CMD_SUCCESS;
#endif
}

DEFUN (vnc_show_vlan_mode,
       vnc_show_vlan_mode_cmd,
       "show vnc openflow vlan-mode",
       SHOW_STR RFAPI_SHOW_STR OF_SHOW_STR "Display OpenFlow VLAN mode\n")
{
  struct rfp_instance_t *rfi = NULL;
  rfi = rfapi_get_rfp_start_val (bgp_get_default ());   /* assume 1 instance for now */
  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  vty_out (vty, "VLAN tags are %s.%s",
           (rfi->use_vlans ?
            "mapped into network ID (last 12 bits)" :
            "ignored (default)"), VTY_NEWLINE);
  return CMD_SUCCESS;
}

DEFUN (vnc_vlan_mode,
       vnc_vlan_mode_cmd,
       "vnc openflow vlan-mode (tagged|untagged)",
       VNC_CONFIG_STR
       OF_CONFIG_STR
       "Configure OpenFlow VLAN mode\n"
       "Network ID based on VID\n" "Ignore VLAN tags (default)\n")
{
  int ret = CMD_SUCCESS;
  struct rfp_instance_t *rfi = NULL;
  int new;
  rfi = rfapi_get_rfp_start_val (vty->index);   /* index=bgp for BGP_NODE */

  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  new = (*argv[0] == 't');      /* tagged */
  if (rfi->use_vlans != new && RFI_ACTIVE (rfi))
    {
      /* config change need to reset all connections */
      vty_out (vty, "VLAN mode changed, resetting all running OF state%s",
               VTY_NEWLINE);
      ret = rfp_do_reset_all (rfi);
    }
  rfi->use_vlans = new;
  return ret;
}

DEFUN (vnc_show_flow_mode,
       vnc_show_flow_mode_cmd,
       "show vnc openflow flow-mode",
       SHOW_STR
       RFAPI_SHOW_STR OF_SHOW_STR "Display OpenFlow flow entry mode\n")
{
  struct rfp_instance_t *rfi = NULL;
  rfi = rfapi_get_rfp_start_val (bgp_get_default ());   /* assume 1 instance for now */
  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  vty_out (vty, "OpenFlow flow entries are %s.%s",
           (rfi->use_wildcards ?
            "use wildcards" : "flow specific (default)"), VTY_NEWLINE);
  return CMD_SUCCESS;
}

DEFUN (vnc_flow_mode,
       vnc_flow_mode_cmd,
       "vnc openflow flow-mode (with-wildcards|flow-specific)",
       VNC_CONFIG_STR
       OF_CONFIG_STR
       "Configure OpenFlow flow table entry mode\n"
       "Use wildcards\n" "Use flow specific entries (default)\n")
{
  int ret = CMD_SUCCESS;
  struct rfp_instance_t *rfi = NULL;
  int new;
  rfi = rfapi_get_rfp_start_val (vty->index);   /* index=bgp for BGP_NODE */

  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  new = (*argv[0] == 'w');      /* with-wildcards */
  if (rfi->use_wildcards != new && RFI_ACTIVE (rfi))
    {
      /* config change need to reset all connections */
      vty_out (vty, "Flow mode changed for new connections.%s", VTY_NEWLINE);
      vty_out (vty,
               "Use 'clear openflow connections all' to reset existing NVEs/switches.%s",
               VTY_NEWLINE);
    }
  rfi->use_wildcards = new;
  return ret;
}

static int
vnc_l2_group_of_dpid_common (struct vty *vty,
                             int argc,
                             const char **argv, rfp_group_config_type type)
{
  VTY_DECLVAR_CONTEXT_SUB(rfapi_l2_group_cfg, rfg);
  struct rfp_instance_t *rfi = NULL;
  struct rfp_group_config *rgc;
  unsigned long long int datapath_id;
  char *e;
  uint16_t vid = 0;

  rfi = rfapi_get_rfp_start_val (vty->index);   /* index=bgp for BGP_NODE */
  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  datapath_id = strtoll (argv[0], &e, 16);      /* force hex */
  if (datapath_id == 0)
    {
      vty_out (vty, "Datapath identifier can't be zero%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (argc == 2)
    {
      if (argv[1][0] != 'u')    /* untagged = 0 */
        VTY_GET_INTEGER_RANGE ("VLAN ID", vid, argv[1], 1, 4094);
      assert (type == RFP_GRP_CFG_ANY);
      type = RFP_GRP_CFG_VID;
    }

  rgc = rfp_get_group_config_vty (rfi, vty, type);
  if (rgc == NULL)
    return CMD_WARNING;

  if (type == RFP_GRP_CFG_ANY)
    {                           /* just dpid change */
      if (rgc->type == RFP_GRP_CFG_VID)
        {                       /* use old vid */
          type = rgc->type;
          vid = rgc->u.vid;
        }
      else
        type = RFP_GRP_CFG_DPID;
    }

  if (RFI_ACTIVE (rfi) && rgc->datapath_id != 0 &&
      (rgc->type != type ||
       rgc->datapath_id != datapath_id ||
       (type == RFP_GRP_CFG_VID && rgc->u.vid != vid)))
    vty_out (vty,
             "Active NVEs may not be impacted by change.%s"
             "Use 'clear openflow connections' to update.%s",
             VTY_NEWLINE, VTY_NEWLINE);

  if (rgc->type != type)
    rfp_set_group_config_type (rgc, type);
  rgc->datapath_id = datapath_id;
  if (type == RFP_GRP_CFG_VID)
    {
      rgc->u.vid = vid;
      rfg->logical_net_id = rfp_get_lni (rfi, datapath_id, 1, vid);
    }
  else
    rfg->logical_net_id = rfp_get_lni (rfi, datapath_id, 0, -1);

  return CMD_SUCCESS;
}

DEFUN (vnc_l2_group_of_dpid_config,
       vnc_l2_group_of_dpid_config_cmd,
       "openflow dpid DPID configuration",
       OF_CONFIG_STR
       "set associated OpenFlow DPID information\n"
       "DPID in hexadecimal\n"
       "Identify l2-group as containing OpenFlow configuration\n")
{
  return vnc_l2_group_of_dpid_common (vty, argc, argv, RFP_GRP_CFG_MODES);
}

DEFUN (vnc_l2_group_of_dpid_vpid,
       vnc_l2_group_of_dpid_vpid_cmd,
       "openflow dpid DPID vid <1-4094>",
       OF_CONFIG_STR
       "set associated OpenFlow DPID information\n"
       "DPID in hexadecimal\n"
       "Identify l2-group as containing VLAN ID configuration\n"
       "VLAN ID value <1-4094>\n")
{
  return vnc_l2_group_of_dpid_common (vty, argc, argv, RFP_GRP_CFG_ANY);
}

ALIAS (vnc_l2_group_of_dpid_vpid,
       vnc_l2_group_of_dpid_untagged_cmd,
       "openflow dpid DPID vid (untagged|)",
       OF_CONFIG_STR
       "set associated OpenFlow DPID information\n"
       "DPID in hexadecimal\n"
       "Identify l2-group as containing VLAN ID configuration\n"
       "Untagged\n")
ALIAS (vnc_l2_group_of_dpid_vpid,
       vnc_l2_group_of_dpid_cmd,
       "openflow dpid DPID",
       OF_CONFIG_STR
       "set associated OpenFlow DPID and VLAN ID\n"
       "DPID in hexadecimal\n")
DEFUN (vnc_l2_group_of_no_dpid,
       vnc_l2_group_of_no_dpid_cmd,
       "no openflow dpid",
       NO_STR
       OF_CONFIG_STR
       "Remove associated OpenFlow DPID and, when present, VLAN ID\n")
{
  struct rfp_instance_t *rfi = NULL;
  struct rfp_group_config *rgc;

  rfi = rfapi_get_rfp_start_val (vty->index);   /* index=bgp for BGP_NODE */
  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* get saved pointer */
  rgc = rfp_get_group_config_vty (rfi, vty, RFP_GRP_CFG_ANY);
  if (rgc == NULL)
    return CMD_WARNING;
  rgc->datapath_id = 0;
  rfp_reset_group_config_type (rgc);

  return CMD_SUCCESS;
}

DEFUN (vnc_l2_group_of_vlan_mode,
       vnc_l2_group_of_vlan_mode_cmd,
       "openflow vlan-mode (tagged|untagged)",
       OF_CONFIG_STR
       "Configure group OpenFlow vlan-mode\n"
       "Network ID based on VID\n" "Ignore VLAN tags\n")
{
  struct rfp_instance_t *rfi = NULL;
  struct rfp_group_config *rgc;
  rfp_group_config_type type = RFP_GRP_CFG_MODES;
  int new;

  rfi = rfapi_get_rfp_start_val (vty->index);   /* index=bgp for BGP_NODE */
  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  new = (*argv[0] == 't');      /* tagged */

  rgc = rfp_get_group_config_vty (rfi, vty, type);
  if (rgc == NULL)
    return CMD_WARNING;

  if (RFI_ACTIVE (rfi) &&
      rgc->type == type && rgc->u.modes.use_vlans != -1
      && rgc->u.modes.use_vlans != new)
    vty_out (vty,
             "Active NVEs may not be impacted by change.%s"
             "Use 'clear openflow connections' to update.%s",
             VTY_NEWLINE, VTY_NEWLINE);

  if (rgc->type != type)
    rfp_set_group_config_type (rgc, type);
  rgc->u.modes.use_vlans = new;

  return CMD_SUCCESS;
}

DEFUN (vnc_l2_group_of_no_vlan_mode,
       vnc_l2_group_of_no_vlan_mode_cmd,
       "no openflow vlan-mode",
       NO_STR OF_CONFIG_STR "Remove group OpenFlow vlan-mode\n")
{
  struct rfp_instance_t *rfi = NULL;
  struct rfp_group_config *rgc;
  rfp_group_config_type type = RFP_GRP_CFG_MODES;

  rfi = rfapi_get_rfp_start_val (vty->index);   /* index=bgp for BGP_NODE */
  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  rgc = rfp_get_group_config_vty (rfi, vty, RFP_GRP_CFG_ANY);
  if (rgc == NULL)
    return CMD_WARNING;

  if (rgc->type != type)        /* nothing to do */
    return CMD_SUCCESS;

  if (RFI_ACTIVE (rfi) && (rgc->type != type || rgc->u.modes.use_vlans != -1))
    vty_out (vty,
             "Active NVEs may not be impacted by change.%s"
             "Use 'clear openflow connections' to update.%s",
             VTY_NEWLINE, VTY_NEWLINE);

  rgc->u.modes.use_vlans = -1;
  rfp_reset_group_config_type (rgc);

  return CMD_SUCCESS;
}

DEFUN (vnc_l2_group_of_flow_mode,
       vnc_l2_group_of_flow_mode_cmd,
       "openflow flow-mode (with-wildcards|flow-specific)",
       OF_CONFIG_STR
       "Configure group OpenFlow flow table entry mode\n"
       "Use wildcards\n" "Use flow specific entries\n")
{
  struct rfp_instance_t *rfi = NULL;
  struct rfp_group_config *rgc;
  rfp_group_config_type type = RFP_GRP_CFG_MODES;
  int new;

  rfi = rfapi_get_rfp_start_val (vty->index);   /* index=bgp for BGP_NODE */
  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  new = (*argv[0] == 'w');      /* with-wildcards */

  rgc = rfp_get_group_config_vty (rfi, vty, type);
  if (rgc == NULL)
    return CMD_WARNING;

  if (RFI_ACTIVE (rfi) &&
      rgc->type == type && rgc->u.modes.use_wildcards != -1
      && rgc->u.modes.use_wildcards != new)
    vty_out (vty,
             "Active NVEs may not be impacted by change.%s"
             "Use 'clear openflow connections' to update.%s",
             VTY_NEWLINE, VTY_NEWLINE);

  if (rgc->type != type)
    rfp_set_group_config_type (rgc, type);
  rgc->u.modes.use_wildcards = new;

  return CMD_SUCCESS;
}

DEFUN (vnc_l2_group_of_no_flow_mode,
       vnc_l2_group_of_no_flow_mode_cmd,
       "no openflow flow-mode",
       NO_STR OF_CONFIG_STR "Remove group OpenFlow flow table entry mode\n")
{
  struct rfp_instance_t *rfi = NULL;
  struct rfp_group_config *rgc;
  rfp_group_config_type type = RFP_GRP_CFG_MODES;

  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  rfi = rfapi_get_rfp_start_val (vty->index);   /* index=bgp for BGP_NODE */
  rgc = rfp_get_group_config_vty (rfi, vty, RFP_GRP_CFG_ANY);
  if (rgc == NULL)
    return CMD_WARNING;

  if (rgc->type != type)        /* nothing to do */
    return CMD_SUCCESS;

  if (RFI_ACTIVE (rfi) &&
      (rgc->type != type || rgc->u.modes.use_wildcards != -1))
    vty_out (vty,
             "Active NVEs may not be impacted by change.%s"
             "Use 'clear openflow connections' to update.%s",
             VTY_NEWLINE, VTY_NEWLINE);

  rgc->u.modes.use_wildcards = -1;
  rfp_reset_group_config_type (rgc);

  return CMD_SUCCESS;
}

static int
rfp_ovs_of_handle_reset(struct vty *vty,  
                        int argc,
                        const char **argv,
                        int nve) 
{
  struct rfp_instance_t *rfi = NULL;
  int connections;
  struct rfapi_ip_addr un;

  rfi = rfapi_get_rfp_start_val (bgp_get_default ());   /* assume 1 instance for now */
  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  connections = (*(argv[0]) == 'c');
  if (argc == 1)
    {                           /* all */
      if (connections)
        return rfp_do_reset_all (rfi);
      /* else */
      rfp_ovs_of_reset_flows (rfi->ooi);
      return CMD_SUCCESS;
    }
  /* nve */
  if (nve && !rfapiCliGetRfapiIpAddr (vty, argv[1], &un))
    {
      int rc;
      union
      {
        struct sockaddr_storage ss;
        struct sockaddr_in      sin;
        struct sockaddr_in6     sin6;
      } u;  
      u.ss.ss_family = un.addr_family;
      if (u.ss.ss_family == AF_INET)
        {
          u.sin.sin_addr.s_addr = un.addr.v4.s_addr;
        }
      else if (u.ss.ss_family == AF_INET6)
        {
          u.sin6.sin6_addr = un.addr.v6;
        }
      if (connections)
        rc = rfp_ovs_of_reset_all_by_ss (rfi->ooi, &u.ss);
      else
        rc = rfp_ovs_of_reset_flow_by_ss (rfi->ooi, &u.ss);
      vty_out (vty, "Cleared %d NVEs%s", rc, VTY_NEWLINE);
      return CMD_SUCCESS;
    }
  /* dpid */
  if (!nve) 
    {
      int rc;
      unsigned long long int datapath_id;
      char *e;
      datapath_id = strtoll (argv[1], &e, 16);
      if (connections)
        rc = rfp_ovs_of_reset_all_by_dpid (rfi->ooi, datapath_id);
      else
        rc = rfp_ovs_of_reset_flow_by_dpid (rfi->ooi, datapath_id);
      vty_out (vty, "Cleared %d NVEs%s", rc, VTY_NEWLINE);
      return CMD_SUCCESS;
    }
  return CMD_WARNING;
}

/* reset all OF switch state */
DEFUN (vnc_rfp_of_reset,
       vnc_rfp_of_reset_cmd,
       "clear openflow (connections|flows) nve (A.B.C.D|X:X::X:X)",
       "Clear stored data\n"
       "VNC Information\n"
       "Remove OpenFlow connection\n"
       "Remove OpenFlow flow state\n"
       "Remove state for one switch (using NVE UN address)\n")
{
  return rfp_ovs_of_handle_reset(vty, argc, argv, 1);
}

ALIAS (vnc_rfp_of_reset,
       vnc_rfp_of_reset_all_cmd,
       "clear openflow (connections|flows) all",
       "Clear stored data\n"
       "VNC Openflow Information\n"
       "Remove OpenFlow connection\n"
       "Remove OpenFlow flow state\n"
       "Remove state for all switches (NVEs)\n")

DEFUN (vnc_rfp_of_reset_dpid,
       vnc_rfp_of_reset_dpid_cmd,
       "clear openflow (connections|flows) dpid DPID",
       "Clear stored data\n"
       "VNC Openflow Information\n"
       "Remove OpenFlow connection\n"
       "Remove OpenFlow flow state\n"
       "Remove state for one switch (using OpenFlow DPID)\n")
{
  return rfp_ovs_of_handle_reset(vty, argc, argv, 0);
}

DEFUN (vnc_show_of_helper,
       vnc_show_of_helper_cmd,
       "show vnc openflow helper DPID .VIDLIST",
       SHOW_STR
       RFAPI_SHOW_STR
       OF_SHOW_STR
       "Helper to show l2-group configuration values\n"
       "DPID in hexadecimal\n"
       "Space separated list of VIDs <0-4094> (0=untagged)\n")
{
  struct rfp_instance_t *rfi = NULL;
  unsigned long long int datapath_id;
  char *e;
  rfi = rfapi_get_rfp_start_val (bgp_get_default ());   /* assume 1 instance for now */
  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  datapath_id = strtoll (argv[0], &e, 16);      /* force hex */
  argc--;
  argv++;

  vty_out (vty, "By default, VLAN tags are %s.%s%s",
           (rfi->use_vlans ?
            "mapped into network ID (last 12 bits)" :
            "ignored"), VTY_NEWLINE, VTY_NEWLINE);
  vty_out (vty, "%-16s %-8s --> %s%s",
           "OpenFlow DIPD", "VLAN ID",
           "l2-group logical-network-id", VTY_NEWLINE);
  for (; argc; --argc, ++argv)
    {
      uint16_t vid;
      uint32_t lni;
      VTY_GET_INTEGER_RANGE ("VLAN ID", vid, argv[0], 0, 4094);
      lni = rfp_get_lni (rfi, datapath_id, -1, vid);
      vty_out (vty, "%016llx %-8d --> %d (0x%x)%s",
               datapath_id, vid, lni, lni, VTY_NEWLINE);
    }

  return CMD_SUCCESS;
}

DEFUN (vnc_show_of_switches,
       vnc_show_of_switches_cmd,
       "show vnc openflow switches",
       SHOW_STR
       RFAPI_SHOW_STR
       OF_SHOW_STR
       "Display openflow switches information\n")
{
  struct rfp_instance_t *rfi = NULL;
  unsigned long long int datapath_id = 0;
  char *e;

  rfi = rfapi_get_rfp_start_val (bgp_get_default ());   /* assume 1 instance for now */
  if (!rfi)
    {
      vty_out (vty, "OpenFlow not running%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  if (argc)
    datapath_id = strtoll (argv[0], &e, 16);      /* force hex */
  rfp_ovs_of_show_switches (vty, rfi->ooi, datapath_id);
  return CMD_SUCCESS;
}
ALIAS (vnc_show_of_switches,
       vnc_show_of_switches_dpid_cmd,
       "show vnc openflow switches DPID",
       SHOW_STR
       RFAPI_SHOW_STR
       OF_SHOW_STR
       "Display openflow switches information"  
       "ID of switch to disply\n")

static void
rfp_vty_install ()
{
  static int installed = 0;
  if (installed)                /* do this only once */
    return;
  installed = 1;
  /* example of new cli command */

  install_element (BGP_NODE, &vnc_rfp_loglevel_cmd);
  install_element (VIEW_NODE, &vnc_show_rfp_listen_ports_cmd);
  install_element (BGP_NODE, &vnc_listen_ports_cmd);
  install_element (VIEW_NODE, &vnc_show_of_listen_ports_cmd);
  install_element (BGP_NODE, &vnc_of_listen_ports_cmd);
  install_element (VIEW_NODE, &vnc_show_vlan_mode_cmd);
  install_element (BGP_NODE, &vnc_vlan_mode_cmd);
  install_element (VIEW_NODE, &vnc_show_flow_mode_cmd);
  install_element (BGP_NODE, &vnc_flow_mode_cmd);

  install_element (ENABLE_NODE, &vnc_rfp_of_reset_cmd);
  install_element (ENABLE_NODE, &vnc_rfp_of_reset_all_cmd);
  install_element (ENABLE_NODE, &vnc_rfp_of_reset_dpid_cmd);

  install_element (VIEW_NODE, &vnc_show_of_helper_cmd);

  install_element (VIEW_NODE, &vnc_show_of_switches_cmd);
  install_element (VIEW_NODE, &vnc_show_of_switches_dpid_cmd);

  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_of_dpid_config_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_of_dpid_vpid_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_of_dpid_untagged_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_of_dpid_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_of_no_dpid_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_of_vlan_mode_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_of_no_vlan_mode_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_of_flow_mode_cmd);
  install_element (BGP_VNC_L2_GROUP_NODE, &vnc_l2_group_of_no_flow_mode_cmd);

  /* for compatibility with regression scripts */
  install_element (BGP_NODE, &vnc_rfp_updated_responses_cmd);
  install_element (BGP_NODE, &vnc_rfp_removal_responses_cmd);
}

/***********************************************************************
 * RFAPI Callbacks
 **********************************************************************/

/*------------------------------------------
 * rfp_response_cb
 *
 * Callbacks of this type are used to provide asynchronous 
 * route updates from RFAPI to the RFP client.
 *
 * response_cb
 *	called to notify the rfp client that a next hop list
 *	that has previously been provided in response to an
 *	rfapi_query call has been updated. Deleted routes are indicated
 *	with lifetime==RFAPI_REMOVE_RESPONSE_LIFETIME.
 *
 *	By default, the routes an NVE receives via this callback include
 *	its own routes (that it has registered). However, these may be
 *	filtered out if the global BGP_VNC_CONFIG_FILTER_SELF_FROM_RSP
 *	flag is set.
 *
 * input: 
 *	next_hops	a list of possible next hops.
 *			This is a linked list allocated within the
 *			rfapi. The response_cb callback function is responsible
 *			for freeing this memory via rfapi_free_next_hop_list()
 *			in order to avoid memory leaks.
 *
 *	userdata	value (cookie) originally specified in call to
 *			rfapi_open()
 *
 *------------------------------------------*/
static void
rfp_response_cb (struct rfapi_next_hop_entry *next_hops, void *userdata)
{
  /* 
   * Identify NVE based on userdata, which is a value passed 
   * to RFAPI in the rfapi_open call 
   */

  /* process list of next_hops */

  /* free next hops */
  rfapi_free_next_hop_list (next_hops);
  return;
}

/*------------------------------------------
 * rfp_local_cb
 *
 * Callbacks of this type are used to provide asynchronous 
 * route updates from RFAPI to the RFP client.
 *
 * local_cb
 *	called to notify the rfp client that a local route
 *	has been added or deleted. Deleted routes are indicated
 *	with lifetime==RFAPI_REMOVE_RESPONSE_LIFETIME.
 *
 * input: 
 *	next_hops	a list of possible next hops.
 *			This is a linked list allocated within the
 *			rfapi. The local_cb callback function is responsible
 *			for freeing this memory via rfapi_free_next_hop_list()
 *			in order to avoid memory leaks.
 *
 *	userdata	value (cookie) originally specified in call to
 *			rfapi_open()
 *
 *------------------------------------------*/
static void
rfp_local_cb (struct rfapi_next_hop_entry *next_hops, void *userdata)
{
  /* 
   * Identify NVE based on userdata, which is a value passed 
   * to RFAPI in the rfapi_open call 
   */

  /* process list of local next_hops */

  /* free next hops */
  rfapi_free_next_hop_list (next_hops);
  return;
}

/*------------------------------------------
 * rfp_close_cb 
 *
 * Callbacks used to provide asynchronous 
 * notification that an rfapi_handle was invalidated
 *
 * input: 
 *	pHandle		Firmerly valid rfapi_handle returned to
 *			client via rfapi_open().
 *
 *	reason		EIDRM	handle administratively closed (clear nve ...)
 *			ESTALE	handle invalidated by configuration change
 *
 *------------------------------------------*/
static void
rfp_close_cb (rfapi_handle pHandle, int reason)
{
  /* close / invalidate NVE with the pHandle returned by the rfapi_open call */

  struct rfp_instance_t *rfi = &global_rfi;     /* single instance for now */
  rfp_ovs_of_switch_close (rfi->ooi, (void *) pHandle, reason);
  return;
}

/*------------------------------------------
 * rfp_cfg_write_cb
 *
 * This callback is used to generate output for any config parameters
 * that may supported by RFP  via RFP defined vty commands at the bgp 
 * level.  See loglevel as an example.
 *
 * input: 
 *    vty           -- quagga vty context
 *    rfp_start_val -- value returned by rfp_start
 *
 * output:
 *    to vty, rfp related configuration
 *
 * return value: 
 *    lines written
--------------------------------------------*/
static int
rfp_cfg_write_cb (struct vty *vty, void *rfp_start_val)
{
  struct rfp_instance_t *rfi = rfp_start_val;
  int write = 0;
  struct listnode *node;
  void *data;
  uint16_t port;

  if (rfi->loglevel)
    {
      vty_out (vty, " vnc openflow log-level %s%s",
               rfp_get_loglevel_cmd (rfi->loglevel), VTY_NEWLINE);
      write++;
    }
  if (rfi->use_vlans)
    {
      vty_out (vty, " vnc openflow vlan-mode tagged%s", VTY_NEWLINE);
      write++;
    }
  if (rfi->use_wildcards)
    {
      vty_out (vty, " vnc openflow flow-mode with-wildcards%s", VTY_NEWLINE);
      write++;
    }
  if (rfi->listen_ports != NULL && !list_isempty (rfi->listen_ports))
    {
      vty_out (vty, " vnc openflow listen-ports ");
      for (ALL_LIST_ELEMENTS_RO (rfi->listen_ports, node, data))
        {
          port = (uint16_t) (((uintptr_t) data) & 0xffff);
          vty_out (vty, "%hu ", port);
        }
      vty_out (vty, "%s", VTY_NEWLINE);
      write++;
    }
  return write;
}

/*------------------------------------------
 * rfp_cfg_group_write_cb 
 *
 * This callback is used to generate output for any config parameters
 * that may supported by RFP via RFP defined vty commands at the 
 * L2 or NVE level.  See loglevel as an example.
 *
 * input: 
 *    vty              quagga vty context
 *    rfp_start_val    value returned by rfp_start
 *    type             group type
 *    name             group name
 *    rfp_cfg_group    Pointer to configuration structure 
 *
 * output:
 *    to vty, rfp related configuration
 *
 * return value: 
 *    lines written
--------------------------------------------*/
static int
rfp_cfg_group_write_cb (struct vty *vty,
                        void *rfp_start_val,
                        rfapi_rfp_cfg_group_type type,
                        const char *name, void *rfp_cfg_group)
{
  struct rfp_instance_t *rfi = rfp_start_val;
  int write = 0;
  struct rfp_group_config *rgc = rfp_cfg_group;

  if (type != RFAPI_RFP_CFG_GROUP_L2 || !vty || !rfi || !rgc)
    return 0;
  if (rgc->type == RFP_GRP_CFG_INIT)
    {
      zlog_debug ("%s: config in init state!", __func__);
      return 0;
    }
  if (rgc->datapath_id != 0)
    {
      vty_out (vty, "   #logical-network-id set from openflow dpid%s",
               VTY_NEWLINE);
      write++;
      vty_out (vty, "   openflow dpid %016llx", rgc->datapath_id);
      write++;
    }
  switch (rgc->type)
    {
    case RFP_GRP_CFG_DPID:
      if (rgc->datapath_id != 0)
        vty_out (vty, "%s", VTY_NEWLINE);
      break;
    case RFP_GRP_CFG_VID:
      if (rgc->datapath_id != 0 && rgc->u.vid != -1)
        {
          if (rgc->u.vid)
            vty_out (vty, " vid %d%s", rgc->u.vid, VTY_NEWLINE);
          else
            vty_out (vty, " vid untagged%s", VTY_NEWLINE);
        }
      break;
    case RFP_GRP_CFG_MODES:
      if (rgc->datapath_id != 0)
        vty_out (vty, " configuration%s", VTY_NEWLINE);
      if (rgc->u.modes.use_vlans != -1)
        {
          vty_out (vty, "   openflow vlan-mode %stagged%s",
                   (rgc->u.modes.use_vlans ? "" : "un"), VTY_NEWLINE);
          write++;
        }
      if (rgc->u.modes.use_wildcards != -1)
        {
          vty_out (vty, "   openflow flow-mode %s%s",
                   (rgc->u.
                    modes.use_wildcards ? "with-wildcards" : "flow-specific"),
                   VTY_NEWLINE);
          write++;
        }
      break;
    default:
      zlog_warn ("%s: unexpected config type (%d)!", __func__, rgc->type);
      break;
    }
  return write;
}

/***********************************************************************
 * RFAPI required functions
 **********************************************************************/

/*------------------------------------------
 * rfp_start
 *
 * This function will start the RFP code
 *
 * input: 
 *    master    quagga thread_master to tie into bgpd threads
 * 
 * output:
 *    cfgp      Pointer to rfapi_rfp_cfg (null = use defaults), 
 *              copied by caller, updated via rfp_set_configuration
 *    cbmp      Pointer to rfapi_rfp_cb_methods, may be null
 *              copied by caller, updated via rfapi_rfp_set_cb_methods
 *
 * return value: 
 *    rfp_start_val rfp returned value passed on rfp_stop and rfp_cfg_write
 * 
--------------------------------------------*/
static const char *synth_argv[] = {
  "rfp_ovs_of",                 /* not counted in argc */
  "ptcp:"
};

static int synth_argc = 1;

void *
rfp_start (struct thread_master *master,
           struct rfapi_rfp_cfg **cfgp, struct rfapi_rfp_cb_methods **cbmp)
{
  memset (&global_rfi, 0, sizeof (struct rfp_instance_t));
  global_rfi.master = master;   /* for BGPD threads */

  global_rfi.loglevel = RFP_INIT_LOGLEVEL;
  global_rfi.ooi = rfp_ovs_of_init (&global_rfi, synth_argc,
                                    (char **)synth_argv,
                                    rfp_get_loglevel_ovs
                                    (global_rfi.loglevel));
  /* initilize struct rfapi_rfp_cfg, see rfapi.h */
  global_rfi.rfapi_config.download_type = RFAPI_RFP_DOWNLOAD_PARTIAL;   /* default=partial */
  global_rfi.rfapi_config.ftd_advertisement_interval =
    RFAPI_RFP_CFG_DEFAULT_FTD_ADVERTISEMENT_INTERVAL;
  global_rfi.rfapi_config.holddown_factor = 0;  /*default: RFAPI_RFP_CFG_DEFAULT_HOLDDOWN_FACTOR */
  global_rfi.rfapi_config.use_updated_response = 0;     /* 0=no */
  global_rfi.rfapi_config.use_removes = 0;      /* 0=no */

  /* initilize structrfapi_rfp_cb_methods , see rfapi.h */
  global_rfi.rfapi_callbacks.cfg_cb = rfp_cfg_write_cb;
  global_rfi.rfapi_callbacks.cfg_group_cb = rfp_cfg_group_write_cb;
  /* group config comming */
  global_rfi.rfapi_callbacks.response_cb = rfp_response_cb;
  global_rfi.rfapi_callbacks.local_cb = rfp_local_cb;
  global_rfi.rfapi_callbacks.close_cb = rfp_close_cb;

  if (cfgp != NULL)
    *cfgp = &global_rfi.rfapi_config;
  if (cbmp != NULL)
    *cbmp = &global_rfi.rfapi_callbacks;
  rfp_vty_install ();
  return &global_rfi;
}

/*------------------------------------------
 * rfp_stop
 *
 * This function is called on shutdown to trigger RFP cleanup
 *
 * input: 
 *    none
 *
 * output:
 *    none
 *
 * return value: 
 *    rfp_start_val
--------------------------------------------*/
void
rfp_stop (void *rfp_start_val)
{
  struct rfp_instance_t *rfi = rfp_start_val;

  assert (rfp_start_val != NULL);

  rfp_ovs_of_destroy (rfi->ooi);
  rfi->ooi = NULL;
  if (rfi->listen_ports)        /* should be emptied by destroy */
    list_delete (rfi->listen_ports);
}

/* TO BE REMOVED */
void
rfp_clear_vnc_nve_all (void)
{
  return;
}


/***********************************************************************
 *			FD related
 ***********************************************************************/
#if 0
static struct rfp_thread_list *
rfp_find_thread_by_fd (struct rfp_instance_t *rfi, int fd)
{
  struct rfp_thread_list *this;
  if (rfi == NULL)
    return NULL;
  this = rfi->threads;
  while (this != NULL && this->fd != fd)
    this = this->next;
  return this;
}
#endif

static int
rfp_fd_accept (struct thread *thread)
{
  struct rfp_instance_t *rfi;
  int fd;
  struct rfp_thread_list *thread_list;

  fd = THREAD_FD (thread);
  thread_list = THREAD_ARG (thread);
  rfi = thread_list->parent;
  assert (thread_list != NULL);
  assert (fd == thread_list->fd);
  assert (rfi != NULL);
  thread_list->thread =
    thread_add_read (rfi->master, rfp_fd_accept, thread_list, fd);
  (thread_list->cb) (rfi->ooi, fd, thread_list->data);
  return 1;
}

void
rfp_fd_listen (void *parent, int fd, rfp_fd_cb_t * cb, void *data)
{
  struct rfp_instance_t *rfi = parent;
  struct rfp_thread_list *thread_list;
  assert (parent != NULL);
  assert (cb != NULL);

  thread_list = XCALLOC (MTYPE_RFP_GENERIC, sizeof (*thread_list));
  thread_list->fd = fd;
  thread_list->cb = cb;
  thread_list->data = data;
  thread_list->parent = rfi;
  thread_list->next = rfi->threads;
  rfi->threads = thread_list;
  thread_list->thread =
    thread_add_read (rfi->master, rfp_fd_accept, thread_list, fd);
}

int
rfp_fd_close (void *parent, int fd)
{
  struct rfp_instance_t *rfi = parent;
  struct rfp_thread_list **threadp, *this;

  if (rfi == NULL || rfi->threads == NULL)
    return 0;
  threadp = &rfi->threads;
  while (*threadp != NULL && (*threadp)->fd != fd)
    threadp = &(*threadp)->next;
  if (*threadp == NULL)
    return 0;
  this = *threadp;
  *threadp = this->next;
  thread_cancel (this->thread);
  XFREE (MTYPE_RFP_GENERIC, this);
  return 1;
}

/***********************************************************************
 *			switch add/drop related
 ***********************************************************************/

void *
rfp_switch_open (void *parent, struct sockaddr_storage *ss, void *data)
{
  struct rfp_instance_t *rfi = parent;
  void *rfd = NULL;
  struct rfapi_ip_addr vn;
  struct rfapi_ip_addr un;
  uint32_t response_lifetime;
#define RFP_OVS_OF_DEFAULT_UN_TT
#ifdef RFP_OVS_OF_DEFAULT_UN_TT
  struct rfapi_un_option uo;
  
  memset (&uo, 0, sizeof (uo));
  uo.type = RFAPI_UN_OPTION_TYPE_TUNNELTYPE;
  uo.v.tunnel.type = BGP_ENCAP_TYPE_MPLS;
#endif
  memset (&vn, 0, sizeof (struct rfapi_ip_addr));
  memset (&un, 0, sizeof (struct rfapi_ip_addr));
  vn.addr_family = ss->ss_family;
  un.addr_family = ss->ss_family;
  if (ss->ss_family == AF_INET)
    {
      const struct sockaddr_in *sin = (const struct sockaddr_in *) ss;
      vn.addr.v4.s_addr = 0x01 + (((uint32_t) sin->sin_port) << 16);
      un.addr.v4.s_addr = sin->sin_addr.s_addr;
    }
  else if (ss->ss_family == AF_INET6)
    {
      const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *) ss;
      vn.addr.v6.s6_addr16[7] = sin6->sin6_port;
      un.addr.v6 = sin6->sin6_addr;
    }
  if (rfapi_open (rfi, &vn, &un,
#ifdef RFP_OVS_OF_DEFAULT_UN_TT
                  &uo,
#else
                  NULL,
#endif
                  &response_lifetime, data, &rfd))
    return NULL;
  return rfd;
}

int
rfp_switch_close (void *rfd)
{
  return rfapi_close (rfd);
}

/***********************************************************************
 *			MAC add/drop related
 ***********************************************************************/

int
rfp_mac_add_drop (void *parent,
                  void *rfd,
                  int add,
                  const struct ethaddr *mac,
                  struct rfapi_ip_prefix *prefix,
                  unsigned long long int datapath_id,
                  int use_vlans,
                  uint16_t vid, uint32_t port, uint32_t lifetime)
{
  rfapi_register_action action;
  struct rfapi_vn_option vo;
  struct rfapi_l2address_option *l2o = NULL;
#ifdef RFP_OVS_OF_TEST_UN_TT
  struct rfapi_un_option uo;
#endif

  action = (add ? RFAPI_REGISTER_ADD : RFAPI_REGISTER_WITHDRAW);

#if 0                           /* broken */
  if (vid == 0)                 /* 0 = no VID */
    use_vlans = 0;
#endif

  memset (&vo, 0, sizeof (vo));
  vo.type = RFAPI_VN_OPTION_TYPE_L2ADDR;
  l2o = &vo.v.l2addr;
  l2o->macaddr = *mac;
  l2o->label = port & 0xffff;
  l2o->logical_net_id = rfp_get_lni (parent, datapath_id, use_vlans, vid);
  l2o->local_nve_id = (uint8_t) (l2o->logical_net_id);
  l2o->tag_id = vid;

#ifdef RFP_OVS_OF_TEST_UN_TT
  memset (&uo, 0, sizeof (uo));
  uo.type = RFAPI_UN_OPTION_TYPE_TUNNELTYPE;
#if 0                           /* unknown TT not working */
  uo.v.tunnel.type = 0x5226;
  uo.v.tunnel.bytes = 2;
  *((uint16_t *) & uo.v.tunnel.bgpinfo) = (uint16_t) (port & 0xffff);
#else
# if 0                          /* test IP_IN_IP */
  uo.v.tunnel.type = BGP_ENCAP_TYPE_IP_IN_IP;
  uo.v.tunnel.bgpinfo.ip_ip.valid_subtlvs = BGP_TEA_SUBTLV_PROTO_TYPE;
  uo.v.tunnel.bgpinfo.ip_ip.st_proto.proto = (uint16_t) (port & 0xffff);
# else
  uo.v.tunnel.type = BGP_ENCAP_TYPE_MPLS;
#endif
#endif

#endif /* RFP_OVS_OF_TEST_UN_TT */
  return rfapi_register (rfd, prefix, lifetime,
#ifdef RFP_OVS_OF_TEST_UN_TT
                         &uo,
#else
                         NULL,
#endif
                         &vo, action);
}

int
rfp_mac_lookup (void *parent,
                void *rfd,
                const struct ethaddr * mac,
                struct rfapi_ip_addr * target,
                unsigned long long int datapath_id,
                int use_vlans, uint16_t vid, uint32_t in_port,
                uint32_t port_list_size, 
                struct rfp_ovs_of_out port_list[]) /* return */
{
  int pcount = 0;
  uint16_t port = OFPP_FLOOD;
  uint16_t out_vid = vid;
  int ret;
  struct rfapi_l2address_option l2o;
  struct rfapi_next_hop_entry *nhe;
  memset (&l2o, 0, sizeof (l2o));

  l2o.macaddr = *mac;
  l2o.logical_net_id = rfp_get_lni (parent, datapath_id, use_vlans, vid);
  l2o.local_nve_id = (uint8_t) (l2o.logical_net_id);
  l2o.label = in_port & 0xffff;
  l2o.tag_id = vid;

  ret = rfapi_query (rfd, target, &l2o, &nhe);
  if (nhe != NULL && nhe->vn_options != NULL &&
      nhe->vn_options->type == RFAPI_VN_OPTION_TYPE_L2ADDR)
    {
      struct rfapi_l2address_option *l2o = &nhe->vn_options->v.l2addr;
      port = (uint16_t) (l2o->label);
      out_vid = l2o->tag_id;
#ifdef RFP_OVS_OF_TEST_UN_TT    /* for dev testing only */
#if 0                           /* unknown TT not working */
      if (nhe->un_options != NULL &&
          nhe->un_options->type == RFAPI_UN_OPTION_TYPE_TUNNELTYPE &&
          nhe->un_options->v.tunnel.type == 0x5226 &&
          nhe->un_options->v.tunnel.bytes >= 2)
        {
          uint16_t nport;
          nport = *((uint16_t *) & nhe->un_options->v.tunnel.bgpinfo);
          zlog_debug ("%s: port=%d, nport=%d", __func__, port, nport);
          port = nport;
        }
#else
      if (nhe->un_options != NULL &&
          nhe->un_options->type == RFAPI_UN_OPTION_TYPE_TUNNELTYPE &&
          nhe->un_options->v.tunnel.type == BGP_ENCAP_TYPE_IP_IN_IP &&
          (nhe->un_options->v.tunnel.bgpinfo.ip_ip.valid_subtlvs &
           BGP_TEA_SUBTLV_PROTO_TYPE))
        {
          uint16_t nport;
          nport = nhe->un_options->v.tunnel.bgpinfo.ip_ip.st_proto.proto;
          zlog_debug ("%s: port=%d, nport=%d", __func__, port, nport);
          port = nport;
        }
#endif
#endif /* RFP_OVS_OF_TEST_UN_TT */
      if (in_port != port || vid != out_vid) /* break loops! */
        port_list[pcount].port = port;
      else
        port_list[pcount].port = OFPP_NONE;
      port_list[pcount++].vid  = out_vid;
        
      zlog_debug ("%s: pcount=%d, in=%u, out=%u/%u", __func__, 
                  pcount, vid, out_vid, port);
      rfapi_free_next_hop_list (nhe);
    }
  return pcount;
}

/***********************************************************************
 *			L2-group ports related
 ***********************************************************************/

/* 
 *  NOTE: QUICK HACK!!!
 *  This function uses bgp internals -- perhaps add to rfapi
 */
static int
rfp_get_ports_by_single_group (void *parent,
                               void *rfd,
                               unsigned long long int datapath_id,
                               int use_vlans,
                               uint16_t vid,
                               uint32_t in_port,
                               uint32_t port_list_size,
                               struct rfp_ovs_of_out port_list[])
{
  int pcount = 0;
  struct list *ll;
  struct bgp *bgp;

  in_port &= 0xffff;
  bgp = bgp_get_default ();     /* assume 1 instance for now */
  ll = bgp_rfapi_get_labellist_by_lni_label (bgp,
                                             rfp_get_lni (parent, datapath_id,
                                                          use_vlans, vid),
                                             in_port);
  if (ll != NULL)
    {
      struct listnode *lnode;
      void *data;
      for (ALL_LIST_ELEMENTS_RO (ll, lnode, data))
        if (((uint32_t) ((uintptr_t) data)) != in_port)
          {                     /* match! */
            if (pcount >= (int) port_list_size)
              return pcount;
            port_list[pcount].vid    = vid;
            port_list[pcount++].port = (uint32_t) ((uintptr_t) data);
          }
      zlog_debug ("%s: have labellist, pcount=%d, last=%u/%u",
                  __func__, pcount, 
                  port_list[pcount - 1].vid, port_list[pcount - 1].port);
    }
  else
    zlog_debug ("%s: no labellist, %016llx, vid=%d",
                __func__, datapath_id, vid);
  return pcount;
}

int
rfp_get_ports_by_group (void *parent,
                        void *rfd,
                        unsigned long long int datapath_id,
                        int use_vlans,
                        uint16_t vid,
                        uint32_t in_port,
                        uint32_t port_list_size,
                        struct rfp_ovs_of_out port_list[])
{
  int pcount = 0;
  struct bgp *bgp;
  struct rfapi_l2_group_cfg *rfg;
  struct ecommunity *rt_import_list;
  struct listnode *node;
  uint32_t vmask = 0;       /* all bits */
  uint32_t mask = 0;        /* all bits */

  in_port &= 0xffff;
  bgp = bgp_get_default ();     /* assume 1 instance for now */
  rfg = bgp_rfapi_get_group_by_lni_label (bgp,
                                          rfp_get_lni (parent, datapath_id,
                                                       use_vlans, vid),
                                          in_port);
  if (rfg == NULL || rfg->rt_import_list == NULL) 
    return rfp_get_ports_by_single_group(parent, rfd, datapath_id, use_vlans, vid,
                                         in_port, port_list_size, port_list);
                                         
  rt_import_list = rfg->rt_import_list;
  if (use_vlans)
    vmask = RFP_VLAN_MASK;
  mask = ~vmask;
  
  for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->l2_groups, node, rfg))
    if (rt_import_list == rfg->rt_import_list ||
        (rfg->rt_import_list &&
         ecommunity_cmp (rt_import_list, rfg->rt_import_list)))
      {
        struct listnode *lnode;
        void *data;
        uint16_t out_vid = (uint16_t)(rfg->logical_net_id & vmask);
        for (ALL_LIST_ELEMENTS_RO (rfg->labels, lnode, data))
          {
            uint32_t out_port;
            if (pcount >= (int) port_list_size)
              return pcount;
            out_port = (uint32_t) ((uintptr_t) data);
            if (in_port != out_port || vid != out_vid)
              {
                port_list[pcount].vid  = (uint16_t)(rfg->logical_net_id & vmask); 
                port_list[pcount].port = (uint32_t) ((uintptr_t) data);
                zlog_debug ("%s: #%d %u/%u from %s",
                            __func__, pcount, 
                            port_list[pcount].vid, port_list[pcount].port,
                            rfg->name);
                pcount++;
              }
          }
      }
  if (pcount == 0)
    zlog_debug ("%s: no labellist, %016llx, vid=%d",
                __func__, datapath_id, vid);
  return pcount;
}


/*
 * find l2-group configed ports for a particular DPID, 
 * ignoring VID and in port 
 */
int
rfp_get_ports_by_datapath_id (void *parent,
                              unsigned long long int datapath_id,
                              int use_vlans,
                              uint32_t port_list_size, 
                              struct rfp_ovs_of_out port_list[])
{
  struct rfp_instance_t *rfi;
  struct bgp *bgp;
  int pcount = 0;
  bgp = bgp_get_default ();     /* assume 1 instance for now */
  rfi = parent;

  if (bgp->rfapi_cfg->l2_groups != NULL)
    {
      struct rfapi_l2_group_cfg *rfg;
      struct listnode *node;
      uint32_t vmask = 0;       /* all bits */
      uint32_t mask = 0;        /* all bits */
      uint32_t logical_net_id;

      if (USE_VLANS (rfi, use_vlans))
        vmask = RFP_VLAN_MASK;
      mask = ~vmask;
      logical_net_id = rfp_get_lni (parent, datapath_id, use_vlans, 0) & mask;

      for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->l2_groups, node, rfg))
        if ((rfg->logical_net_id & mask) == logical_net_id)
          {
            struct listnode *lnode;
            void *data;
            for (ALL_LIST_ELEMENTS_RO (rfg->labels, lnode, data))
              {
                if (pcount >= (int) port_list_size)
                  return pcount;
                port_list[pcount].vid  = (uint16_t)(rfg->logical_net_id & vmask); 
                port_list[pcount].port = (uint32_t) ((uintptr_t) data);
                zlog_debug ("%s: #%d %u/%u",
                            __func__, pcount, 
                            port_list[pcount].vid, port_list[pcount].port);
                pcount++;
              }
          }
    }
  if (pcount == 0)
    {                           /* no matching l2 groups / ports found */
      zlog_debug ("%s: NO labellist, pcount=%d", __func__, pcount);
    }
  return pcount;
}

/*
 * find l2-group configed ports for a particular DPID & port, 
 * and create related output string
 */
extern void
rfp_get_l2_group_str_by_pnum (char *buf,
                              void *parent,
                              unsigned long long int datapath_id,
                              int use_vlans, 
                              uint32_t pnum)
{
  struct rfp_instance_t *rfi;
  struct bgp *bgp;
  uint32_t pcount = 0;
  bgp = bgp_get_default ();     /* assume 1 instance for now */
  rfi = parent;
  if (buf != NULL)
    buf[0] = '\0';
  
  if (bgp->rfapi_cfg->l2_groups != NULL)
    {
      struct rfapi_l2_group_cfg *rfg;
      struct listnode *node;
      uint32_t mask = 0;        /* all bits */
      uint32_t logical_net_id;

      if (USE_VLANS (rfi, use_vlans))
        mask = RFP_VLAN_MASK;
      mask = ~mask;
      logical_net_id = rfp_get_lni (parent, datapath_id, use_vlans, 0) & mask;
      for (ALL_LIST_ELEMENTS_RO (bgp->rfapi_cfg->l2_groups, node, rfg))
        if ((rfg->logical_net_id & mask) == logical_net_id)
          {
            struct listnode *lnode;
            void *data;
            for (ALL_LIST_ELEMENTS_RO (rfg->labels, lnode, data)) 
              if (pcount++ == pnum)
                  {
                    uint32_t vlan = rfg->logical_net_id & ~mask;
                    if (vlan) 
                      buf += sprintf(buf,"VLAN: %-4d  Group: %-20s",
                                     vlan, rfg->name);
                    else
                      buf += sprintf(buf,"Untagged    Group: %-20s",
                                     rfg->name);
                    
                    buf += sprintf(buf," RT:");
                    if (rfg->rt_import_list && rfg->rt_export_list &&
                        ecommunity_cmp (rfg->rt_import_list, rfg->rt_export_list))
                      {
                        char *b = ecommunity_ecom2str (rfg->rt_import_list,
                                                       ECOMMUNITY_FORMAT_ROUTE_MAP, 0);
                        buf += sprintf(buf," %s", b);
                        XFREE (MTYPE_ECOMMUNITY_STR, b);
                      }
                    else
                      {
                        char *i = NULL, *e = NULL;
                        
                        if (rfg->rt_import_list)
                          {
                            i = ecommunity_ecom2str (rfg->rt_import_list,
                                                     ECOMMUNITY_FORMAT_ROUTE_MAP, 0);
                            buf += sprintf(buf," import %s", i);
                            XFREE (MTYPE_ECOMMUNITY_STR, i);
                          }
                        if (rfg->rt_export_list)
                          {
                            e =  ecommunity_ecom2str (rfg->rt_export_list,
                                                      ECOMMUNITY_FORMAT_ROUTE_MAP, 0);
                            buf += sprintf(buf," export %s", e);
                            XFREE (MTYPE_ECOMMUNITY_STR, e);
                          }
                        if (i == NULL && e == NULL) 
                            buf += sprintf(buf," NONE");
                      }
                  }
          }
    }
}

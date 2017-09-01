/*
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2015 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* derrived from https://github.com/openvswitch/ovs/blob/master/utilities/ovs-testcontroller.c */

#include <config.h>

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../ovs/lib/command-line.h"
#include "../ovs/lib/compiler.h"
#include "../ovs/lib/daemon.h"
#include "../ovs/lib/fatal-signal.h"
#include "../ovs/lib/learning-switch.h"
#include "../ovs/lib/ofp-parse.h"
#include "../ovs/lib/ofp-version-opt.h"
#include "../ovs/lib/ofpbuf.h"
#include "../ovs/include/openflow/openflow.h"
#include "../ovs/lib/poll-loop.h"
#include "../ovs/lib/rconn.h"
#include "../ovs/lib/simap.h"
#include "../ovs/lib/stream-ssl.h"
#include "../ovs/lib/timeval.h"
#include "../ovs/lib/unixctl.h"
#include "../ovs/lib/util.h"
#include "../ovs/include/openvswitch/vconn.h"
#include "../ovs/include/openvswitch/vlog.h"
#include "../ovs/lib/socket-util.h"
#include "../ovs/lib/ofp-util.h"
#include "../ovs/lib/vconn-provider.h"

#include "rfp_internal.h"

VLOG_DEFINE_THIS_MODULE (controller);

#define MAX_SWITCHES 16
#define MAX_LISTENERS 16

struct ports_
{
  struct ports_ *next;
  uint32_t port;
};

struct switch_
{
  struct switch_ *next;
  struct lswitch *lswitch;
  unsigned long long int dpid;
  int fd;
  void *rfd;
  struct sockaddr_storage ss;
  struct ports_ *default_group_ports; /* contains ports with no config */
  int l2_group_ports_count;
  struct rfp_ovs_of_out l2_group_ports[RFP_OFS_OF_MAX_PORTS];
  rfp_flow_mode_t flow_mode;    /* settable on init only */
  int use_vlans;
};

/* The log messages here could actually be useful in debugging, so keep the
 * rate limit relatively high. */
static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT (30, 100);

/* -H, --hub: Learn the ports on which MAC addresses appear? */
static bool learn_macs = true;

/* -n, --noflow: Set up flows?  (If not, every packet is processed at the
 * controller.) */
static bool set_up_flows = true;

/* -N, --normal: Use "NORMAL" action instead of explicit port? */
static bool action_normal = false;

/* -w, --wildcard: 0 to disable wildcard flow entries, an OFPFW10_* bitmask to
 * enable specific wildcards, or UINT32_MAX to use the default wildcards. */
static uint32_t wildcards = 0;

/* --max-idle: Maximum idle time, in seconds, before flows expire. */
static int max_idle = 60;

/* --mute: If true, accept connections from switches but do not reply to any
 * of their messages (for debugging fail-open mode). */
static bool mute = false;

/* -q, --queue: default OpenFlow queue, none if UINT32_MAX. */
static uint32_t default_queue = UINT32_MAX;

/* -Q, --port-queue: map from port name to port number. */
static struct simap port_queues = SIMAP_INITIALIZER (&port_queues);

/* --with-flows: Flows to send to switch. */
static struct ofputil_flow_mod *default_flows;
static size_t n_default_flows;
static enum ofputil_protocol usable_protocols;

/* --unixctl: Name of unixctl socket, or null to use the default. */
/* default is of form /usr/local/var/run/openvswitch/rfp_ovs_of.<pid>.ctl  */
static char *unixctl_path = NULL;

static void new_switch (struct switch_ *, struct vconn *);
static void parse_options (int argc, char *argv[]);
OVS_NO_RETURN static void usage (void);

#if 0                           /* unused/old */
int
rfp_ovs_of_main (int argc, char *argv[])
{
  struct unixctl_server *unixctl;
  struct switch_ switches[MAX_SWITCHES];
  struct pvconn *listeners[MAX_LISTENERS];
  int n_switches, n_listeners;
  int retval;
  int i;

  ovs_cmdl_proctitle_init (argc, argv);
  set_program_name (argv[0]);
  parse_options (argc, argv);
  fatal_ignore_sigpipe ();

  //    daemon_become_new_user(false);

  if (argc - optind < 1)
    {
      ovs_fatal (0, "at least one vconn argument required; "
                 "use --help for usage");
    }

  n_switches = n_listeners = 0;
  for (i = optind; i < argc; i++)
    {
      const char *name = argv[i];
      struct vconn *vconn;

      retval = vconn_open (name, get_allowed_ofp_versions (), DSCP_DEFAULT,
                           &vconn);
      if (!retval)
        {
          if (n_switches >= MAX_SWITCHES)
            {
              ovs_fatal (0, "max %d switch connections", n_switches);
            }
          new_switch (&switches[n_switches++], vconn);
          continue;
        }
      else if (retval == EAFNOSUPPORT)
        {
          struct pvconn *pvconn;
          retval = pvconn_open (name, get_allowed_ofp_versions (),
                                DSCP_DEFAULT, &pvconn);
          if (!retval)
            {
              if (n_listeners >= MAX_LISTENERS)
                {
                  ovs_fatal (0, "max %d passive connections", n_listeners);
                }
              listeners[n_listeners++] = pvconn;
            }
        }
      if (retval)
        {
          VLOG_ERR ("%s: connect: %s", name, ovs_strerror (retval));
        }
    }
  if (n_switches == 0 && n_listeners == 0)
    {
      ovs_fatal (0, "no active or passive switch connections");
    }

  //daemonize_start(false);

  retval = unixctl_server_create (unixctl_path, &unixctl);
  if (retval)
    {
      exit (EXIT_FAILURE);
    }

  //daemonize_complete();

  while (n_switches > 0 || n_listeners > 0)
    {
      /* Accept connections on listening vconns. */
      for (i = 0; i < n_listeners && n_switches < MAX_SWITCHES;)
        {
          struct vconn *new_vconn;

          retval = pvconn_accept (listeners[i], &new_vconn);
          if (!retval || retval == EAGAIN)
            {
              if (!retval)
                {
                  new_switch (&switches[n_switches++], new_vconn);
                }
              i++;
            }
          else
            {
              pvconn_close (listeners[i]);
              listeners[i] = listeners[--n_listeners];
            }
        }

      /* Do some switching work.  . */
      for (i = 0; i < n_switches;)
        {
          struct switch_ *this = &switches[i];
          lswitch_run (this->lswitch);
          if (lswitch_is_alive (this->lswitch))
            {
              i++;
            }
          else
            {
              lswitch_destroy (this->lswitch);
              switches[i] = switches[--n_switches];
            }
        }

      unixctl_server_run (unixctl);

      /* Wait for something to happen. */
      if (n_switches < MAX_SWITCHES)
        {
          for (i = 0; i < n_listeners; i++)
            {
              pvconn_wait (listeners[i]);
            }
        }
      for (i = 0; i < n_switches; i++)
        {
          struct switch_ *sw = &switches[i];
          lswitch_wait (sw->lswitch);
        }
      unixctl_server_wait (unixctl);
      poll_block ();
    }

  return 0;
}
#endif 

static int32_t
flow_mode_to_wildcards (rfp_flow_mode_t flow_mode)
{
  int32_t ret = wildcards; /* for compat */ ;
  if (flow_mode == RFP_FLOW_MODE_SPECIFIC)
    ret = 0;                    /* see lswitch_create */
  if (flow_mode == RFP_FLOW_MODE_WILDCARDS)
    ret = UINT32_MAX;           /* see lswitch_create */
  return ret;
}

static void
new_switch (struct switch_ *sw, struct vconn *vconn)
{
  struct lswitch_config cfg;
  struct rconn *rconn;
  char *c;

  rconn = rconn_create (60, 0, DSCP_DEFAULT, get_allowed_ofp_versions ());
  rconn_connect_unreliably (rconn, vconn, NULL);

  cfg.mode = (action_normal ? LSW_NORMAL
              : learn_macs ? LSW_LEARN : LSW_FLOOD);
  cfg.wildcards = flow_mode_to_wildcards (sw->flow_mode);
  cfg.max_idle = set_up_flows ? max_idle : -1;
  cfg.default_flows = default_flows;
  cfg.n_default_flows = n_default_flows;
  cfg.usable_protocols = usable_protocols;
  cfg.default_queue = default_queue;
  cfg.port_queues = &port_queues;
  cfg.mute = mute;
  sw->lswitch = lswitch_create (rconn, &cfg);
  sw->fd = vconn_get_stream_fd (vconn);

  /* skip tcp: */
  for (c = vconn->name; c != NULL && *c != 0 && *c++ != ':';);
  inet_parse_active (c, 0, &sw->ss);
}

static void
add_port_queue (char *s)
{
  char *save_ptr = NULL;
  char *port_name;
  char *queue_id;

  port_name = strtok_r (s, ":", &save_ptr);
  queue_id = strtok_r (NULL, "", &save_ptr);
  if (!queue_id)
    {
      ovs_fatal (0, "argument to -Q or --port-queue should take the form "
                 "\"<port-name>:<queue-id>\"");
    }

  if (!simap_put (&port_queues, port_name, atoi (queue_id)))
    {
      ovs_fatal (0, "<port-name> arguments for -Q or --port-queue must "
                 "be unique");
    }
}

static void
parse_options (int argc, char *argv[])
{
  enum
  {
    OPT_MAX_IDLE = UCHAR_MAX + 1,
    OPT_PEER_CA_CERT,
    OPT_MUTE,
    OPT_WITH_FLOWS,
    OPT_UNIXCTL,
    VLOG_OPTION_ENUMS,
    DAEMON_OPTION_ENUMS,
    OFP_VERSION_OPTION_ENUMS
  };
  static const struct option long_options[] = {
    {"hub", no_argument, NULL, 'H'},
    {"noflow", no_argument, NULL, 'n'},
    {"normal", no_argument, NULL, 'N'},
    {"wildcards", optional_argument, NULL, 'w'},
    {"max-idle", required_argument, NULL, OPT_MAX_IDLE},
    {"mute", no_argument, NULL, OPT_MUTE},
    {"queue", required_argument, NULL, 'q'},
    {"port-queue", required_argument, NULL, 'Q'},
    {"with-flows", required_argument, NULL, OPT_WITH_FLOWS},
    {"unixctl", required_argument, NULL, OPT_UNIXCTL},
    {"help", no_argument, NULL, 'h'},
    DAEMON_LONG_OPTIONS,
    OFP_VERSION_LONG_OPTIONS,
    VLOG_LONG_OPTIONS,
    STREAM_SSL_LONG_OPTIONS,
    {"peer-ca-cert", required_argument, NULL, OPT_PEER_CA_CERT},
    {NULL, 0, NULL, 0},
  };
  char *short_options = ovs_cmdl_long_options_to_short_options (long_options);

  optind = 1;                   /* reset getopt */
  for (;;)
    {
      int indexptr;
      char *error;
      int c;
      extern int optind;
      c = getopt_long (argc, argv, short_options, long_options, &indexptr);
      if (c == -1)
        {
          break;
        }

      switch (c)
        {
        case 'H':
          learn_macs = false;
          break;

        case 'n':
          set_up_flows = false;
          break;

        case OPT_MUTE:
          mute = true;
          break;

        case 'N':
          action_normal = true;
          break;

        case 'w':
          wildcards = optarg ? strtol (optarg, NULL, 16) : UINT32_MAX;
          break;

        case OPT_MAX_IDLE:
          if (!strcmp (optarg, "permanent"))
            {
              max_idle = OFP_FLOW_PERMANENT;
            }
          else
            {
              max_idle = atoi (optarg);
              if (max_idle < 1 || max_idle > 65535)
                {
                  ovs_fatal (0, "--max-idle argument must be between 1 and "
                             "65535 or the word 'permanent'");
                }
            }
          break;

        case 'q':
          default_queue = atoi (optarg);
          break;

        case 'Q':
          add_port_queue (optarg);
          break;

        case OPT_WITH_FLOWS:
          error = parse_ofp_flow_mod_file (optarg, OFPFC_ADD, &default_flows,
                                           &n_default_flows,
                                           &usable_protocols);
          if (error)
            {
              ovs_fatal (0, "%s", error);
            }
          break;

        case OPT_UNIXCTL:
          unixctl_path = optarg;
          break;

        case 'h':
          usage ();

          VLOG_OPTION_HANDLERS OFP_VERSION_OPTION_HANDLERS
            DAEMON_OPTION_HANDLERS STREAM_SSL_OPTION_HANDLERS case
            OPT_PEER_CA_CERT:stream_ssl_set_peer_ca_cert_file (optarg);
          break;

        case '?':
          exit (EXIT_FAILURE);

        default:
          abort ();
        }
    }
  free (short_options);

  if (!simap_is_empty (&port_queues) || default_queue != UINT32_MAX)
    {
      if (action_normal)
        {
          ovs_error (0, "queue IDs are incompatible with -N or --normal; "
                     "not using OFPP_NORMAL");
          action_normal = false;
        }

      if (!learn_macs)
        {
          ovs_error (0, "queue IDs are incompatible with -H or --hub; "
                     "not acting as hub");
          learn_macs = true;
        }
    }
}

static void
usage (void)
{
  printf ("%s: OpenFlow controller\n"
          "usage: %s [OPTIONS] METHOD\n"
          "where METHOD is any OpenFlow connection method.\n",
          program_name, program_name);
  vconn_usage (true, true, false);
  daemon_usage ();
  ofp_version_usage ();
  vlog_usage ();
  printf ("\nOther options:\n"
          "  -H, --hub               act as hub instead of learning switch\n"
          "  -n, --noflow            pass traffic, but don't add flows\n"
          "  --max-idle=SECS         max idle time for new flows\n"
          "  -N, --normal            use OFPP_NORMAL action\n"
          "  -w, --wildcards[=MASK]  wildcard (specified) bits in flows\n"
          "  -q, --queue=QUEUE-ID    OpenFlow queue ID to use for output\n"
          "  -Q PORT-NAME:QUEUE-ID   use QUEUE-ID for frames from PORT-NAME\n"
          "  --with-flows FILE       use the flows from FILE\n"
          "  --unixctl=SOCKET        override default control socket name\n"
          "  -h, --help              display this help message\n"
          "  -V, --version           display version information\n");
  exit (EXIT_SUCCESS);
}

/***********************************************************************
 *			support for RFP
 ***********************************************************************/

struct ovs_of_info
{
  void *parent;
  struct unixctl_server *unixctl;
  int unixfd;
  struct switch_ *switches;
  struct pvconn *listeners[MAX_LISTENERS];
  int n_switches, n_listeners;
};

static struct ovs_of_info *global_ooi = NULL;   /* make list in future */

static int
rfp_ss_same (struct sockaddr_storage *a, struct sockaddr_storage *b)
{
  if (a->ss_family != b->ss_family)
    return 0;
  if (a->ss_family == AF_INET &&
      ((const struct sockaddr_in *) a)->sin_addr.s_addr ==
      ((const struct sockaddr_in *) b)->sin_addr.s_addr)
    return 1;
  if (a->ss_family == AF_INET6)
    {
      const struct sockaddr_in6 *sin6a = (const struct sockaddr_in6 *) a;
      const struct sockaddr_in6 *sin6b = (const struct sockaddr_in6 *) b;
      return (memcmp (&sin6a->sin6_addr, &sin6b->sin6_addr,
                      sizeof (sin6a->sin6_addr)) == 0);
    }
  return 0;
}

static int
rfp_ooi_and_switch_by_lswitch (struct lswitch *lswitch,
                               struct ovs_of_info **ooip,
                               struct switch_ **swp)
{
  struct switch_ *sw;
  if (lswitch == NULL || ooip == NULL || swp == NULL || global_ooi == NULL)
    return 1;
  *ooip = global_ooi;
  sw = global_ooi->switches;
  while (sw != NULL && sw->lswitch != lswitch)
    sw = sw->next;
  *swp = sw;
  return (sw == NULL);
}

static void
rfp_ooi_switch_add (struct ovs_of_info *ooi, struct switch_ *sw)
{
  sw->next = ooi->switches;
  ooi->switches = sw;
  ooi->n_switches++;
}

static void
rfp_ooi_switch_drop (struct ovs_of_info *ooi, struct switch_ *sw)
{
  struct switch_ **this = &ooi->switches;

  while (*this != NULL && *this != sw)
    this = &(*this)->next;
  if (*this == NULL)
    VLOG_ERR ("Switch not found in list!");
  else
    *this = sw->next;
  ooi->n_switches--;
}

static struct switch_ *
rfp_ooi_switch_drop_by_rfd (struct ovs_of_info *ooi, void *rfd)
{
  struct switch_ *sw;
  struct switch_ **this = &ooi->switches;

  while (*this != NULL && (*this)->rfd != rfd)
    this = &(*this)->next;
  sw = *this;
  if (*this == NULL)
    VLOG_ERR ("Switch not found by rfd!");
  else
    *this = sw->next;
  ooi->n_switches--;
  return sw;
}


static void
rfp_ovs_of_fd_unix (void *vooi, int fd, void *data)
{
  struct ovs_of_info *ooi = vooi;
  if (ooi->unixctl != NULL && fd == ooi->unixfd)
    {
      unixctl_server_run (ooi->unixctl);
    }
  else
    {
      VLOG_ERR ("Unexpected unix fd %d != %d", fd, ooi->unixfd);
    }
}

static void
rfp_ovs_of_clear_flows (struct ovs_of_info *ooi, struct switch_ *sw)
{
  lswitch_init_flows (sw->lswitch);
  // TBD -- local flow tracking
}

static void
rfp_ovs_of_reset_switch (struct ovs_of_info *ooi, struct switch_ *sw)
{
  rfp_fd_close (ooi->parent, sw->fd);
  //rfp_ovs_of_clear_flows(ooi, sw); maybe
  rfp_switch_close (sw->rfd);
  lswitch_destroy (sw->lswitch);
  rfp_ooi_switch_drop (ooi, sw);
  free (sw);
}

static void
rfp_ovs_of_fd_switch (void *vooi, int fd, void *data)
{
  struct ovs_of_info *ooi = vooi;
  struct switch_ *sw = data;

  /* Do some switching work. */
  if (lswitch_is_alive (sw->lswitch) && lswitch_is_connected (sw->lswitch))
    lswitch_run (sw->lswitch);
  if (!lswitch_is_alive (sw->lswitch))
    {
      rfp_ovs_of_reset_switch (ooi, sw);
    }
}

static struct switch_ *
rfp_ovs_of_switch_add (struct ovs_of_info *ooi, struct vconn *vconn)
{
  struct switch_ *sw;
  sw = xcalloc (1, sizeof (*sw));
  assert (sw != NULL);
  sw->l2_group_ports_count = -1;        /* init pending DPID, 1st register */
  sw->use_vlans = -1;           /* may be set once dpid learned */
  sw->flow_mode = rfp_get_flow_mode (ooi->parent);
  new_switch (sw, vconn);
  sw->rfd = rfp_switch_open (ooi->parent, &sw->ss, (void *) sw);
  if (sw->rfd == NULL)
    {                           /* not configured */
      lswitch_destroy (sw->lswitch);
      free (sw);
      return NULL;
    }
  rfp_ooi_switch_add (ooi, sw);
  rfp_fd_listen (ooi->parent, sw->fd, rfp_ovs_of_fd_switch, (void *) sw);
  return sw;
}

static void
rfp_ovs_of_fd_listener (void *vooi, int fd, void *data)
{
  struct ovs_of_info *ooi = vooi;
  struct pvconn *pvconn = data;

  if (pvconn == NULL)
    {
      VLOG_ERR ("pvconn missing for listener fd = %d", fd);
    }
  else
    {
      struct vconn *new_vconn;
      int retval;
      retval = pvconn_accept (pvconn, &new_vconn);
      if (!retval || retval == EAGAIN)
        {
          if (!retval)
            {
              rfp_ovs_of_switch_add (ooi, new_vconn);
            }
        }
      else
        {
          assert ("Got unexpected fd close" == 0);      /* humm, tbd? */
        }
    }
}

int
rfp_ovs_of_listen (void *vooi, uint16_t port)
{
  struct ovs_of_info *ooi = vooi;
  int retval;
  struct vconn *vconn;
  uint32_t allowed_versions;
  char name[20];
  sprintf (name, "ptcp:%u", port);
  
  allowed_versions = (port == OFP_OLD_PORT ?
                      (1u << OFP10_VERSION) : /* force OF1.0 for old port */
                      get_allowed_ofp_versions ());
  retval = vconn_open (name, allowed_versions,
                       DSCP_DEFAULT, &vconn);
  if (!retval)
    {
      rfp_ovs_of_switch_add (ooi, vconn);
    }
  else if (retval == EAFNOSUPPORT)
    {
      struct pvconn *pvconn;
      retval = pvconn_open (name, allowed_versions,
                            DSCP_DEFAULT, &pvconn);
      if (!retval)
        {
          if (ooi->n_listeners >= MAX_LISTENERS)
            {
              ovs_fatal (0, "max %d passive connections", ooi->n_listeners);
            }
          ooi->listeners[ooi->n_listeners++] = pvconn;
          rfp_fd_listen (ooi->parent, pvconn_get_stream_fd (pvconn),
                         rfp_ovs_of_fd_listener, (void *) pvconn);
        }
    }
  if (retval)
    {
      VLOG_ERR ("%s: connect: %s", name, ovs_strerror (retval));
    }
  return retval;
}

int
rfp_ovs_of_close_all (void *vooi)
{
  struct ovs_of_info *ooi = vooi;
  int i;

  if (ooi == NULL)
    return -1;
  for (i = 0; i < ooi->n_listeners; i++)
    {
      rfp_fd_close (ooi->parent, pvconn_get_stream_fd (ooi->listeners[i]));
      pvconn_close (ooi->listeners[i]);
      ooi->listeners[i] = NULL;
    }
  ooi->n_listeners = 0;
  return i;
}

const char *
rfp_ovs_of_set_loglevel (const char *ovs_lvl)
{
  return vlog_set_levels_from_string (ovs_lvl);
}

void *
rfp_ovs_of_init (void *parent, int argc, char *argv[], const char *ovs_lvl)
{
  struct ovs_of_info *ooi;
  int i, retval;

  ooi = xcalloc (1, sizeof (*ooi));
  assert (ooi != NULL);

  ooi->parent = parent;
  ovs_cmdl_proctitle_init (argc, argv);
  set_program_name (argv[0]);
  parse_options (argc, argv);
  rfp_ovs_of_set_loglevel (ovs_lvl);
  for (i = optind; i < argc; i++)
    {
      //const char *name = argv[i];
     //rfp_ovs_of_listen (ooi, name); /* no longer used */
    }

  retval = unixctl_server_create (unixctl_path, &ooi->unixctl);
  if (retval)
    {
      free (ooi);
      return NULL;
    }
  //  set_allowed_ofp_versions("OpenFlow10"); 
  ooi->unixfd = unixctl_get_stream_fd (ooi->unixctl);
  rfp_fd_listen (ooi->parent, ooi->unixfd, rfp_ovs_of_fd_unix, NULL);
  //fatal_ignore_sigpipe();
  global_ooi = ooi;
  return ooi;
}

void
rfp_ovs_of_destroy (void *vooi)
{
  struct ovs_of_info *ooi = vooi;

  if (ooi->unixctl != NULL)
    {
      rfp_fd_close (ooi->parent, ooi->unixfd);
      unixctl_server_destroy (ooi->unixctl);
      ooi->unixctl = NULL;
    }

  if (ooi == NULL)
    return;
  rfp_ovs_of_close_all (ooi);
  /* TBD cleanup switches */
  if (ooi == NULL)
    return;
  free (ooi);
}

void
rfp_ovs_of_reset_all (void *vooi)
{
  struct ovs_of_info *ooi = vooi;

  if (ooi == NULL)
    return;

  while (ooi->switches != NULL)
    rfp_ovs_of_reset_switch (ooi, ooi->switches);
}

int
rfp_ovs_of_reset_all_by_ss (void *vooi, struct sockaddr_storage *ss)
{
  struct ovs_of_info *ooi = vooi;
  struct switch_ *sw, *next;
  int ret = 0;

  if (ooi == NULL)
    return ret;

  for (sw = ooi->switches; sw != NULL; sw = next)
    {
      next = sw->next;
      if (rfp_ss_same (ss, &sw->ss))
        {
          rfp_ovs_of_reset_switch (ooi, sw);
          ret++;
        }
    }
  return ret;
}

int
rfp_ovs_of_reset_all_by_dpid (void *vooi, unsigned long long int dpid)
{
  struct ovs_of_info *ooi = vooi;
  struct switch_ *sw, *next;
  int ret = 0;

  if (ooi == NULL)
    return ret;

  for (sw = ooi->switches; sw != NULL; sw = next)
    {
      next = sw->next;
      if (sw->dpid == dpid)
        {
          rfp_ovs_of_reset_switch (ooi, sw);
          ret++;
        }
    }
  return ret;
}

void
rfp_ovs_of_reset_flows (void *vooi)
{
  struct ovs_of_info *ooi = vooi;
  struct switch_ *sw;

  if (ooi == NULL)
    return;

  for (sw = ooi->switches; sw != NULL; sw = sw->next)
    rfp_ovs_of_clear_flows (ooi, sw);
}

int
rfp_ovs_of_reset_flow_by_ss (void *vooi, struct sockaddr_storage *ss)
{
  struct ovs_of_info *ooi = vooi;
  struct switch_ *sw;
  int ret = 0;

  if (ooi == NULL)
    return ret;

  for (sw = ooi->switches; sw != NULL; sw = sw->next)
    if (rfp_ss_same (ss, &sw->ss))
      {
        rfp_ovs_of_clear_flows (ooi, sw);
        ret++;
      }
  return ret;
}

int
rfp_ovs_of_reset_flow_by_dpid (void *vooi, unsigned long long int dpid)
{
  struct ovs_of_info *ooi = vooi;
  struct switch_ *sw;
  int ret = 0;

  if (ooi == NULL)
    return ret;

  for (sw = ooi->switches; sw != NULL; sw = sw->next)
    if (sw->dpid == dpid)
      {
        rfp_ovs_of_clear_flows (ooi, sw);
        ret++;
      }
  return ret;
}


/***********************************************************************
 *		default group support
 ***********************************************************************/
static int
rfp_ovs_of_port_add_drop (struct ovs_of_info *ooi,
                          struct switch_ *sw,
                          int add,
                          unsigned long long int datapath_id, uint32_t port)
{
  struct ports_ **pl;           /* list */
  struct ports_ *pp;            /* pointer */
  int found;
  int dcount = 0;

  pl = &(sw->default_group_ports);
  /* 1st see if port present */
  while (*pl && (*pl)->port != port)
    {
      pl = &(*pl)->next;
      dcount++;
    }
  if (!add)
    {                           /* drop */
      if (*pl == NULL)
        return 1;               /* not found */
      /* drop it */
      pp = *pl;
      *pl = pp->next;
      free (pp);
      VLOG_INFO_RL (&rl, "%016llx: dropped port %" PRIu16 " from default",
                    datapath_id, port);
      return 0;
    }
  /* is add */
  if (*pl != NULL)
    return 1;                   /* already listed */

  if (sw->l2_group_ports_count < 0)
    {                           /* needs init */
      sw->l2_group_ports_count =
        rfp_get_ports_by_datapath_id (ooi->parent, datapath_id,
                                      sw->use_vlans,
                                      RFP_OFS_OF_MAX_PORTS,
                                      sw->l2_group_ports);
      VLOG_INFO_RL (&rl, "%016llx: initl2_group_ports, count=%d, last=%d",
                    datapath_id, sw->l2_group_ports_count,
                    (sw->l2_group_ports_count ?
                     (int)sw->l2_group_ports[sw->l2_group_ports_count - 1].port : -1));
    }
  found = sw->l2_group_ports_count;
  //LB TBD: VID
  while (found > 0 && sw->l2_group_ports[found - 1].port != port)
    found--;
  if (found)
    {
      VLOG_DBG ("%016llx: not adding %d, is in configured group",
                datapath_id, port);
      return 2;                 /* not default */
    }

  VLOG_INFO_RL (&rl, "%016llx: adding default port #%d, %d",
                datapath_id, ++dcount, port);

  pp = xcalloc (1, sizeof (*pp));
  assert (pp != NULL);
  pp->port = port;
  pp->next = sw->default_group_ports;
  sw->default_group_ports = pp;
  return 0;
}

/* only clone if in_port is in list */
static int
rfp_ovs_of_maybe_clone_default_group_list (struct ovs_of_info *ooi,
                                           struct switch_ *sw,
                                           uint32_t in_port,
                                           uint32_t port_list_size,
                                           struct rfp_ovs_of_out *port_list,
                                           uint16_t vid)
{
  uint32_t count = 0;
  struct ports_ *pp;            /* pointer */
  int in_list = 0;
  pp = sw->default_group_ports;
  while (count < port_list_size && pp != NULL)
    {
      if (pp->port != in_port)
        {
          port_list->vid  = vid;
          port_list->port = pp->port;
          port_list++;
          count++;
        }
      else
        in_list++;
      pp = pp->next;
    }
  if (in_list == 0)
    count = 0;

  VLOG_INFO_RL (&rl, "%016llx: cloned %d, in=%u/%u, in_list=%d",
     sw->dpid, count, vid, in_port, in_list);

  return count;
}

/***********************************************************************
 *		Glue bewteen OF and RFAPI
 ***********************************************************************/
void
rfp_ovs_set_sw_features (struct lswitch *lswitch,
                         unsigned long long int datapath_id)
{
  struct ovs_of_info *ooi;
  struct switch_ *sw;
  rfp_flow_mode_t flow_mode;
  int use_vlans;
  if (rfp_ooi_and_switch_by_lswitch (lswitch, &ooi, &sw))
    {
      VLOG_ERR ("lswitch not found!");
      return;
    }
  if (sw->dpid == 0)
    sw->dpid = datapath_id;
  if (rfp_get_modes (ooi->parent, datapath_id, &flow_mode, &use_vlans))
    {
      VLOG_DBG
        ("%016llx: modes: OLD: vlans=%d, flow=%d, NEW: vlans=%d, flow=%d",
         datapath_id, sw->use_vlans, sw->flow_mode, use_vlans, flow_mode);
      if (sw->flow_mode != flow_mode)
        {
          sw->flow_mode = flow_mode;
          lswitch_set_wildcards (sw->lswitch,
                                 flow_mode_to_wildcards (flow_mode));
        }
      sw->use_vlans = use_vlans;
    }
}

int
rfp_ovs_of_mac (int add,
                struct lswitch *lswitch,
                unsigned long long int datapath_id,
                uint16_t vid,
                const struct eth_addr *mac,
                ovs_be32 ipv4_src,
                const struct in6_addr *ipv6_src,
                uint32_t port, uint32_t lifetime)
{
  struct ovs_of_info *ooi;
  struct switch_ *sw;
  struct ethaddr rfpmac;
  struct rfapi_ip_prefix prefix;
  VLOG_INFO_RL (&rl, "%016llx: vid=%d " ETH_ADDR_FMT " is on "
                "port %" PRIu16 " state=%s, life=0x%x", datapath_id, vid,
                ETH_ADDR_ARGS (*mac), port, (add ? "ADD" : "DROP"), lifetime);
  if (rfp_ooi_and_switch_by_lswitch (lswitch, &ooi, &sw))
    {
      VLOG_ERR ("lswitch not found!");
      return 0;
    }

  memcpy (&rfpmac, mac, ETHER_ADDR_LEN);

  /* default */
  memset (&prefix, 0, sizeof (prefix));
  prefix.length = 32;
  prefix.cost = 100;
  prefix.prefix.addr_family = AF_INET;
  if (ipv4_src != 0)
    {
      prefix.prefix.addr.v4.s_addr = ipv4_src;
    }
  else if (0)
    {
      prefix.length = 128;
      prefix.prefix.addr_family = AF_INET6;
      prefix.prefix.addr.v6 = *ipv6_src;
    }

  if (lifetime == RFAPI_INFINITE_LIFETIME)      /* is interface */
    rfp_ovs_of_port_add_drop (ooi, sw, add, datapath_id, port);

  rfp_mac_add_drop (ooi->parent, sw->rfd, add,
                    &rfpmac, &prefix, datapath_id,
                    sw->use_vlans, vid, port, lifetime);

  /* catch and ignore zero macs */
  return 0;
}

int
rfp_ovs_of_lookup (struct lswitch *lswitch,
                   unsigned long long int datapath_id,
                   uint16_t vid,
                   const struct eth_addr *dmac,
                   ovs_be32 ipv4_src,
                   const struct in6_addr *ipv6_src,
                   uint32_t in_port,
                   uint32_t port_list_size, 
                   struct rfp_ovs_of_out port_list[])
{
  int pcount = 0;
  struct ovs_of_info *ooi;
  struct switch_ *sw;
  struct ethaddr rfpmac;
  struct rfapi_ip_addr addr;

  VLOG_DBG ("%016llx: looking up " ETH_ADDR_FMT " in on "
            "port %" PRIu16, datapath_id, ETH_ADDR_ARGS (*dmac), in_port);

  /* Drop frames for reserved multicast addresses. */
  if (eth_addr_is_reserved (*dmac))
    {
      port_list[pcount].vid    = vid;
      port_list[pcount++].port = OFPP_NONE;
      return pcount;
    }

  if (rfp_ooi_and_switch_by_lswitch (lswitch, &ooi, &sw))
    {
      VLOG_ERR ("lswitch not found!");
      port_list[pcount].vid    = vid;
      port_list[pcount++].port = OFPP_NONE;
      return pcount;
    }

  if ((dmac->ea[0] & 1) == 0)
    {                           /* unicast */
      memcpy (&rfpmac, dmac, ETHER_ADDR_LEN);
      memset (&addr, 0, sizeof (addr));
      addr.addr_family = AF_INET;       /* default */
      if (ipv4_src != 0)
        {
          addr.addr.v4.s_addr = ipv4_src;
        }
      else if (0)
        {
          addr.addr_family = AF_INET6;
          addr.addr.v6 = *ipv6_src;
        }
      pcount = rfp_mac_lookup (ooi->parent, sw->rfd,
                                 &rfpmac, &addr, datapath_id,
                                 sw->use_vlans, vid, in_port,
                                 port_list_size,
                                 port_list);
    }
  if (pcount == 0)              /* flooding when using l2 groups */
    pcount = rfp_get_ports_by_group (ooi->parent, sw->rfd, datapath_id,
                                     sw->use_vlans, vid,
                                     in_port, port_list_size, port_list);
  if (pcount == 0 &&            /* clone default list? */
      (!sw->use_vlans ||        /* ignore vid if not in vid mode */
       (sw->use_vlans && vid == 0))) /* if vid mod and untagged */
    pcount = rfp_ovs_of_maybe_clone_default_group_list (ooi, sw, in_port,
                                                        port_list_size,
                                                        port_list, vid);
  if (pcount == 0)              /* still 0 = drop */
    {
      port_list[pcount].vid    = vid;
      port_list[pcount++].port = OFPP_NONE;
    }
  VLOG_INFO_RL (&rl,
                "%016llx: vid=%d " ETH_ADDR_FMT " port %" PRIu16 " --> %u/%u",
                datapath_id, vid, ETH_ADDR_ARGS (*dmac), in_port,
                port_list[0].vid, port_list[0].port);
  if (pcount > 1)
    {
      int i = 1;
      while (i++ < pcount)
        VLOG_INFO_RL (&rl,
                      "%016llx: vid=%d " ETH_ADDR_FMT " port %" PRIu16
                      " --> %u/%u, #%d", datapath_id, vid, ETH_ADDR_ARGS (*dmac),
                      in_port, port_list[(i - 1)].vid, port_list[(i - 1)].port, i);
    }
  return pcount;
}

int
rfp_ovs_of_switch_close (void *vooi, void *rfd, int reason)
{
  struct ovs_of_info *ooi = vooi;
  struct switch_ *sw;

  sw = rfp_ooi_switch_drop_by_rfd (ooi, rfd);
  if (sw == NULL)
    return -1;
  rfp_fd_close (ooi->parent, sw->fd);
  lswitch_destroy (sw->lswitch);
  free (sw);
  return 0;
}

void 
rfp_ovs_of_show_switches(void *vty, void *vooi, unsigned long long int dpid)
{
  struct ovs_of_info *ooi = vooi;
  struct switch_ *sw;
  if (ooi == NULL || vty == NULL)
    return;
  for (sw = ooi->switches; sw != NULL; sw = sw->next)
    if (dpid == 0 || dpid == sw->dpid)
      {
        char tmp[4096];
        sprintf(tmp,"Switch DPID:\t\t%016llx\t(FD=%-3d RFD=0x%lx)", 
                sw->dpid, sw->fd, (long unsigned int)sw->rfd);
        rfp_output(vty, tmp);
        sprintf(tmp,"VLANS:\t\t%s", (sw->use_vlans ? "Used" : "Ignored"));
        rfp_output(vty, tmp);
        sprintf(tmp,"Flowsmode:\t%s", (sw->flow_mode == RFP_FLOW_MODE_WILDCARDS ? 
                                  "Wildcarded" : "Specific"));
        rfp_output(vty, tmp);
        if (sw->default_group_ports == NULL)
          {
            sprintf(tmp,"Default group ports:\tEmpty");
            rfp_output(vty, tmp);
          } 
        else 
          {
            struct ports_ *port = sw->default_group_ports;
            sprintf(tmp,"Default group ports:\t%u", port->port);
            rfp_output(vty, tmp);
            while (port->next != NULL) 
              {
                port = port->next;
                sprintf(tmp,"                   \t%u", port->port);
                rfp_output(vty, tmp);
              }
          }
        if (sw->l2_group_ports_count == 0)
          {
            sprintf(tmp,"L2 group ports:\tEmpty");
            rfp_output(vty, tmp);
          }
        else 
          {
            int i = 0;
            char tmp2[256];
            rfp_get_l2_group_str_by_pnum(tmp2, ooi->parent, 
                                         sw->dpid, sw->use_vlans,
                                         i);
            sprintf(tmp,"L2 group ports:\t%u\t%s", sw->l2_group_ports[i].port, tmp2);
            rfp_output(vty, tmp);
            while (++i < sw->l2_group_ports_count) 
              {
                rfp_get_l2_group_str_by_pnum(tmp2, ooi->parent, 
                                             sw->dpid, sw->use_vlans,
                                             i);
                sprintf(tmp,"              \t%u\t%s", sw->l2_group_ports[i].port, tmp2);
                rfp_output(vty, tmp);
              }
          }
        if (sw->next)
          rfp_output(vty, NULL);
      }
}

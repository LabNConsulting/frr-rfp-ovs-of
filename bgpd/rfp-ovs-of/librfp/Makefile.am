#
# This file has been modified by LabN Consulting, L.L.C.
#
#
## Process this file with automake to produce Makefile.in.

top_srcdir=$(abs_top_srcdir)
top_builddir=$(top_srcdir)

if ENABLE_BGP_VNC
BGP_DIR=$(top_srcdir)/bgpd
BGP_VNC_RFAPI_INC=-I$(top_srcdir)/lib -I$(BGP_DIR) -I$(BGP_DIR)/rfapi

#set/link ovs to your ovs source tree
#obtain tree via 'git clone https://github.com/openvswitch/ovs.git'
OVS_REPO=https://github.com/openvswitch/ovs.git
OVS_REPO_BRANCH=remotes/origin/branch-2.5
OVS=../ovs
OVS_PATCH=$(top_srcdir)/$(LIBRFP)/ovs.patch
OVS_PATCHED_FILES= \
	$(OVS)/lib/learning-switch.c \
	$(OVS)/lib/learning-switch.h \
	$(OVS)/lib/stream-fd.c \
	$(OVS)/lib/stream-fd.h \
	$(OVS)/lib/unixctl.c \
	$(OVS)/lib/unixctl.h \
	$(OVS)/lib/vconn-provider.h \
	$(OVS)/lib/vconn-stream.c 

BGP_VNC_RFP_INCDIR=$(OVS)/include
BGP_VNC_RFP_LIBDIR=$(OVS)/lib
BGP_VNC_RFP_LIB=libdummy.a librfp.a
BGP_VNC_RFP_INC=-I$(BGP_VNC_RFP_INCDIR) -I$(BGP_VNC_RFP_LIBDIR) -I$(OVS)
BGP_VNC_RFP_DEFS=-std=gnu99 -DRFP_OVS_OF
BGP_VNC_RFP_HD= \
	$(BGP_VNC_RFP_LIBDIR)/aes128.h \
	$(BGP_VNC_RFP_LIBDIR)/async-append.h \
	$(BGP_VNC_RFP_LIBDIR)/backtrace.h \
	$(BGP_VNC_RFP_LIBDIR)/bfd.h \
	$(BGP_VNC_RFP_LIBDIR)/bitmap.h \
	$(BGP_VNC_RFP_LIBDIR)/bundle.h \
	$(BGP_VNC_RFP_LIBDIR)/byte-order.h \
	$(BGP_VNC_RFP_LIBDIR)/byteq.h \
	$(BGP_VNC_RFP_LIBDIR)/cfm.h \
	$(BGP_VNC_RFP_LIBDIR)/classifier.h \
	$(BGP_VNC_RFP_LIBDIR)/classifier-private.h \
	$(BGP_VNC_RFP_LIBDIR)/cmap.h \
	$(BGP_VNC_RFP_LIBDIR)/command-line.h \
	$(BGP_VNC_RFP_LIBDIR)/compiler.h \
	$(BGP_VNC_RFP_LIBDIR)/connectivity.h \
	$(BGP_VNC_RFP_LIBDIR)/coverage.h \
	$(BGP_VNC_RFP_LIBDIR)/crc32c.h \
	$(BGP_VNC_RFP_LIBDIR)/csum.h \
	$(BGP_VNC_RFP_LIBDIR)/daemon.h \
	$(BGP_VNC_RFP_LIBDIR)/daemon-private.h \
	$(BGP_VNC_RFP_LIBDIR)/db-ctl-base.h \
	$(BGP_VNC_RFP_LIBDIR)/dhcp.h \
	$(BGP_VNC_RFP_LIBDIR)/dhparams.h \
	$(BGP_VNC_RFP_LIBDIR)/dirs.h \
	$(BGP_VNC_RFP_LIBDIR)/dpctl.h \
	$(BGP_VNC_RFP_LIBDIR)/dpif.h \
	$(BGP_VNC_RFP_LIBDIR)/dpif-netdev.h \
	$(BGP_VNC_RFP_LIBDIR)/dpif-netlink.h \
	$(BGP_VNC_RFP_LIBDIR)/dpif-provider.h \
	$(BGP_VNC_RFP_LIBDIR)/dp-packet.h \
	$(BGP_VNC_RFP_LIBDIR)/dummy.h \
	$(BGP_VNC_RFP_LIBDIR)/dynamic-string.h \
	$(BGP_VNC_RFP_LIBDIR)/entropy.h \
	$(BGP_VNC_RFP_LIBDIR)/fatal-signal.h \
	$(BGP_VNC_RFP_LIBDIR)/fat-rwlock.h \
	$(BGP_VNC_RFP_LIBDIR)/flow.h \
	$(BGP_VNC_RFP_LIBDIR)/geneve.h \
	$(BGP_VNC_RFP_LIBDIR)/guarded-list.h \
	$(BGP_VNC_RFP_LIBDIR)/hash.h \
	$(BGP_VNC_RFP_LIBDIR)/heap.h \
	$(BGP_VNC_RFP_LIBDIR)/hindex.h \
	$(BGP_VNC_RFP_LIBDIR)/hmap.h \
	$(BGP_VNC_RFP_LIBDIR)/hmapx.h \
	$(BGP_VNC_RFP_LIBDIR)/id-pool.h \
	$(BGP_VNC_RFP_LIBDIR)/if-notifier.h \
	$(BGP_VNC_RFP_LIBDIR)/jhash.h \
	$(BGP_VNC_RFP_LIBDIR)/json.h \
	$(BGP_VNC_RFP_LIBDIR)/jsonrpc.h \
	$(BGP_VNC_RFP_LIBDIR)/lacp.h \
	$(BGP_VNC_RFP_LIBDIR)/latch.h \
	$(BGP_VNC_RFP_LIBDIR)/learn.h \
	$(BGP_VNC_RFP_LIBDIR)/learning-switch.h \
	$(BGP_VNC_RFP_LIBDIR)/list.h \
	$(BGP_VNC_RFP_LIBDIR)/lockfile.h \
	$(BGP_VNC_RFP_LIBDIR)/mac-learning.h \
	$(BGP_VNC_RFP_LIBDIR)/match.h \
	$(BGP_VNC_RFP_LIBDIR)/mcast-snooping.h \
	$(BGP_VNC_RFP_LIBDIR)/memory.h \
	$(BGP_VNC_RFP_LIBDIR)/meta-flow.h \
	$(BGP_VNC_RFP_LIBDIR)/multipath.h \
	$(BGP_VNC_RFP_LIBDIR)/netdev-dpdk.h \
	$(BGP_VNC_RFP_LIBDIR)/netdev.h \
	$(BGP_VNC_RFP_LIBDIR)/netdev-linux.h \
	$(BGP_VNC_RFP_LIBDIR)/netdev-provider.h \
	$(BGP_VNC_RFP_LIBDIR)/netdev-vport.h \
	$(BGP_VNC_RFP_LIBDIR)/netflow.h \
	$(BGP_VNC_RFP_LIBDIR)/netlink.h \
	$(BGP_VNC_RFP_LIBDIR)/netlink-notifier.h \
	$(BGP_VNC_RFP_LIBDIR)/netlink-protocol.h \
	$(BGP_VNC_RFP_LIBDIR)/netlink-socket.h \
	$(BGP_VNC_RFP_LIBDIR)/nx-match.h \
	$(BGP_VNC_RFP_LIBDIR)/odp-execute.h \
	$(BGP_VNC_RFP_LIBDIR)/odp-util.h \
	$(BGP_VNC_RFP_LIBDIR)/ofp-actions.h \
	$(BGP_VNC_RFP_LIBDIR)/ofpbuf.h \
	$(BGP_VNC_RFP_LIBDIR)/ofp-errors.h \
	$(BGP_VNC_RFP_LIBDIR)/ofp-msgs.h \
	$(BGP_VNC_RFP_LIBDIR)/ofp-parse.h \
	$(BGP_VNC_RFP_LIBDIR)/ofp-print.h \
	$(BGP_VNC_RFP_LIBDIR)/ofp-util.h \
	$(BGP_VNC_RFP_LIBDIR)/ofp-version-opt.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic-c11.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic-clang.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic-flag-gcc4.7+.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic-gcc4.7+.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic-gcc4+.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic-i586.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic-locked.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic-msvc.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic-pthreads.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic-x86_64.h \
	$(BGP_VNC_RFP_LIBDIR)/ovsdb-data.h \
	$(BGP_VNC_RFP_LIBDIR)/ovsdb-error.h \
	$(BGP_VNC_RFP_LIBDIR)/ovsdb-idl.h \
	$(BGP_VNC_RFP_LIBDIR)/ovsdb-idl-provider.h \
	$(BGP_VNC_RFP_LIBDIR)/ovsdb-parser.h \
	$(BGP_VNC_RFP_LIBDIR)/ovsdb-types.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-lldp.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-numa.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-rcu.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-router.h \
	$(BGP_VNC_RFP_LIBDIR)/ovs-thread.h \
	$(BGP_VNC_RFP_LIBDIR)/packets.h \
	$(BGP_VNC_RFP_LIBDIR)/pcap-file.h \
	$(BGP_VNC_RFP_LIBDIR)/perf-counter.h \
	$(BGP_VNC_RFP_LIBDIR)/poll-loop.h \
	$(BGP_VNC_RFP_LIBDIR)/process.h \
	$(BGP_VNC_RFP_LIBDIR)/pvector.h \
	$(BGP_VNC_RFP_LIBDIR)/random.h \
	$(BGP_VNC_RFP_LIBDIR)/rconn.h \
	$(BGP_VNC_RFP_LIBDIR)/rculist.h \
	$(BGP_VNC_RFP_LIBDIR)/reconnect.h \
	$(BGP_VNC_RFP_LIBDIR)/route-table.h \
	$(BGP_VNC_RFP_LIBDIR)/rstp-common.h \
	$(BGP_VNC_RFP_LIBDIR)/rstp.h \
	$(BGP_VNC_RFP_LIBDIR)/rstp-state-machines.h \
	$(BGP_VNC_RFP_LIBDIR)/rtbsd.h \
	$(BGP_VNC_RFP_LIBDIR)/rtnetlink.h \
	$(BGP_VNC_RFP_LIBDIR)/sat-math.h \
	$(BGP_VNC_RFP_LIBDIR)/seq.h \
	$(BGP_VNC_RFP_LIBDIR)/sflow_api.h \
	$(BGP_VNC_RFP_LIBDIR)/sflow.h \
	$(BGP_VNC_RFP_LIBDIR)/sha1.h \
	$(BGP_VNC_RFP_LIBDIR)/shash.h \
	$(BGP_VNC_RFP_LIBDIR)/signals.h \
	$(BGP_VNC_RFP_LIBDIR)/simap.h \
	$(BGP_VNC_RFP_LIBDIR)/smap.h \
	$(BGP_VNC_RFP_LIBDIR)/socket-util.h \
	$(BGP_VNC_RFP_LIBDIR)/sort.h \
	$(BGP_VNC_RFP_LIBDIR)/sset.h \
	$(BGP_VNC_RFP_LIBDIR)/stdio.h \
	$(BGP_VNC_RFP_LIBDIR)/stp.h \
	$(BGP_VNC_RFP_LIBDIR)/stream-fd.h \
	$(BGP_VNC_RFP_LIBDIR)/stream.h \
	$(BGP_VNC_RFP_LIBDIR)/stream-provider.h \
	$(BGP_VNC_RFP_LIBDIR)/stream-ssl.h \
	$(BGP_VNC_RFP_LIBDIR)/string.h \
	$(BGP_VNC_RFP_LIBDIR)/svec.h \
	$(BGP_VNC_RFP_LIBDIR)/syslog-direct.h \
	$(BGP_VNC_RFP_LIBDIR)/syslog-libc.h \
	$(BGP_VNC_RFP_LIBDIR)/syslog-provider.h \
	$(BGP_VNC_RFP_LIBDIR)/table.h \
	$(BGP_VNC_RFP_LIBDIR)/timer.h \
	$(BGP_VNC_RFP_LIBDIR)/timeval.h \
	$(BGP_VNC_RFP_LIBDIR)/tnl-neigh-cache.h \
	$(BGP_VNC_RFP_LIBDIR)/tnl-ports.h \
	$(BGP_VNC_RFP_LIBDIR)/tun-metadata.h \
	$(BGP_VNC_RFP_LIBDIR)/type-props.h \
	$(BGP_VNC_RFP_LIBDIR)/unaligned.h \
	$(BGP_VNC_RFP_LIBDIR)/unicode.h \
	$(BGP_VNC_RFP_LIBDIR)/unixctl.h \
	$(BGP_VNC_RFP_LIBDIR)/util.h \
	$(BGP_VNC_RFP_LIBDIR)/uuid.h \
	$(BGP_VNC_RFP_LIBDIR)/valgrind.h \
	$(BGP_VNC_RFP_LIBDIR)/vconn-provider.h \
	$(BGP_VNC_RFP_LIBDIR)/vlan-bitmap.h \
	$(BGP_VNC_RFP_LIBDIR)/vlandev.h \
	$(BGP_VNC_RFP_LIBDIR)/vswitch-idl.h \
	$(BGP_VNC_RFP_LIBDIR)/ct-dpif.h \
	$(BGP_VNC_RFP_LIBDIR)/netlink-conntrack.h

librfp_a_SOURCES = \
	$(BGP_VNC_RFP_LIBDIR)/aes128.c \
	$(BGP_VNC_RFP_LIBDIR)/async-append-aio.c \
	$(BGP_VNC_RFP_LIBDIR)/backtrace.c \
	$(BGP_VNC_RFP_LIBDIR)/bfd.c \
	$(BGP_VNC_RFP_LIBDIR)/bundle.c \
	$(BGP_VNC_RFP_LIBDIR)/byteq.c \
	$(BGP_VNC_RFP_LIBDIR)/cfm.c \
	$(BGP_VNC_RFP_LIBDIR)/classifier.c \
	$(BGP_VNC_RFP_LIBDIR)/cmap.c \
	$(BGP_VNC_RFP_LIBDIR)/command-line.c \
	$(BGP_VNC_RFP_LIBDIR)/connectivity.c \
	$(BGP_VNC_RFP_LIBDIR)/coverage.c \
	$(BGP_VNC_RFP_LIBDIR)/crc32c.c \
	$(BGP_VNC_RFP_LIBDIR)/csum.c \
	$(BGP_VNC_RFP_LIBDIR)/daemon.c \
	$(BGP_VNC_RFP_LIBDIR)/daemon-unix.c \
	$(BGP_VNC_RFP_LIBDIR)/db-ctl-base.c \
	$(BGP_VNC_RFP_LIBDIR)/dhparams.c \
	$(BGP_VNC_RFP_LIBDIR)/dirs.c \
	$(BGP_VNC_RFP_LIBDIR)/dpctl.c \
	$(BGP_VNC_RFP_LIBDIR)/dpif-netdev.c \
	$(BGP_VNC_RFP_LIBDIR)/dpif-netlink.c \
	$(BGP_VNC_RFP_LIBDIR)/dpif.c \
	$(BGP_VNC_RFP_LIBDIR)/dp-packet.c \
	$(BGP_VNC_RFP_LIBDIR)/dummy.c \
	$(BGP_VNC_RFP_LIBDIR)/dynamic-string.c \
	$(BGP_VNC_RFP_LIBDIR)/entropy.c \
	$(BGP_VNC_RFP_LIBDIR)/fatal-signal.c \
	$(BGP_VNC_RFP_LIBDIR)/fat-rwlock.c \
	$(BGP_VNC_RFP_LIBDIR)/flow.c \
	$(BGP_VNC_RFP_LIBDIR)/guarded-list.c \
	$(BGP_VNC_RFP_LIBDIR)/hash.c \
	$(BGP_VNC_RFP_LIBDIR)/heap.c \
	$(BGP_VNC_RFP_LIBDIR)/hindex.c \
	$(BGP_VNC_RFP_LIBDIR)/hmap.c \
	$(BGP_VNC_RFP_LIBDIR)/hmapx.c \
	$(BGP_VNC_RFP_LIBDIR)/id-pool.c \
	$(BGP_VNC_RFP_LIBDIR)/if-notifier.c \
	$(BGP_VNC_RFP_LIBDIR)/jhash.c \
	$(BGP_VNC_RFP_LIBDIR)/json.c \
	$(BGP_VNC_RFP_LIBDIR)/jsonrpc.c \
	$(BGP_VNC_RFP_LIBDIR)/lacp.c \
	$(BGP_VNC_RFP_LIBDIR)/latch-unix.c \
	$(BGP_VNC_RFP_LIBDIR)/learning-switch.c \
	$(BGP_VNC_RFP_LIBDIR)/learn.c \
	$(BGP_VNC_RFP_LIBDIR)/lockfile.c \
	$(BGP_VNC_RFP_LIBDIR)/mac-learning.c \
	$(BGP_VNC_RFP_LIBDIR)/match.c \
	$(BGP_VNC_RFP_LIBDIR)/mcast-snooping.c \
	$(BGP_VNC_RFP_LIBDIR)/memory.c \
	$(BGP_VNC_RFP_LIBDIR)/meta-flow.c \
	$(BGP_VNC_RFP_LIBDIR)/multipath.c \
	$(BGP_VNC_RFP_LIBDIR)/netdev-dummy.c \
	$(BGP_VNC_RFP_LIBDIR)/netdev-linux.c \
	$(BGP_VNC_RFP_LIBDIR)/netdev.c \
	$(BGP_VNC_RFP_LIBDIR)/netdev-vport.c \
	$(BGP_VNC_RFP_LIBDIR)/netlink-notifier.c \
	$(BGP_VNC_RFP_LIBDIR)/netlink.c \
	$(BGP_VNC_RFP_LIBDIR)/netlink-socket.c \
	$(BGP_VNC_RFP_LIBDIR)/nx-match.c \
	$(BGP_VNC_RFP_LIBDIR)/odp-execute.c \
	$(BGP_VNC_RFP_LIBDIR)/odp-util.c \
	$(BGP_VNC_RFP_LIBDIR)/ofp-actions.c \
	$(BGP_VNC_RFP_LIBDIR)/ofpbuf.c \
	$(BGP_VNC_RFP_LIBDIR)/ofp-errors.c \
	$(BGP_VNC_RFP_LIBDIR)/ofp-msgs.c \
	$(BGP_VNC_RFP_LIBDIR)/ofp-parse.c \
	$(BGP_VNC_RFP_LIBDIR)/ofp-print.c \
	$(BGP_VNC_RFP_LIBDIR)/ofp-util.c \
	$(BGP_VNC_RFP_LIBDIR)/ofp-version-opt.c \
	$(BGP_VNC_RFP_LIBDIR)/ovs-atomic-locked.c \
	$(BGP_VNC_RFP_LIBDIR)/ovsdb-data.c \
	$(BGP_VNC_RFP_LIBDIR)/ovsdb-error.c \
	$(BGP_VNC_RFP_LIBDIR)/ovsdb-idl.c \
	$(BGP_VNC_RFP_LIBDIR)/ovsdb-parser.c \
	$(BGP_VNC_RFP_LIBDIR)/ovsdb-types.c \
	$(BGP_VNC_RFP_LIBDIR)/ovs-lldp.c \
	$(BGP_VNC_RFP_LIBDIR)/ovs-numa.c \
	$(BGP_VNC_RFP_LIBDIR)/ovs-rcu.c \
	$(BGP_VNC_RFP_LIBDIR)/ovs-router.c \
	$(BGP_VNC_RFP_LIBDIR)/ovs-thread.c \
	$(BGP_VNC_RFP_LIBDIR)/packets.c \
	$(BGP_VNC_RFP_LIBDIR)/pcap-file.c \
	$(BGP_VNC_RFP_LIBDIR)/perf-counter.c \
	$(BGP_VNC_RFP_LIBDIR)/poll-loop.c \
	$(BGP_VNC_RFP_LIBDIR)/process.c \
	$(BGP_VNC_RFP_LIBDIR)/pvector.c \
	$(BGP_VNC_RFP_LIBDIR)/random.c \
	$(BGP_VNC_RFP_LIBDIR)/rconn.c \
	$(BGP_VNC_RFP_LIBDIR)/reconnect.c \
	$(BGP_VNC_RFP_LIBDIR)/route-table.c \
	$(BGP_VNC_RFP_LIBDIR)/rstp.c \
	$(BGP_VNC_RFP_LIBDIR)/rstp-state-machines.c \
	$(BGP_VNC_RFP_LIBDIR)/rtnetlink.c \
	$(BGP_VNC_RFP_LIBDIR)/seq.c \
	$(BGP_VNC_RFP_LIBDIR)/sha1.c \
	$(BGP_VNC_RFP_LIBDIR)/shash.c \
	$(BGP_VNC_RFP_LIBDIR)/signals.c \
	$(BGP_VNC_RFP_LIBDIR)/simap.c \
	$(BGP_VNC_RFP_LIBDIR)/smap.c \
	$(BGP_VNC_RFP_LIBDIR)/socket-util.c \
	$(BGP_VNC_RFP_LIBDIR)/socket-util-unix.c \
	$(BGP_VNC_RFP_LIBDIR)/sort.c \
	$(BGP_VNC_RFP_LIBDIR)/sset.c \
	$(BGP_VNC_RFP_LIBDIR)/stdio.c \
	$(BGP_VNC_RFP_LIBDIR)/stp.c \
	$(BGP_VNC_RFP_LIBDIR)/stream-fd.c \
	$(BGP_VNC_RFP_LIBDIR)/stream.c \
	$(BGP_VNC_RFP_LIBDIR)/stream-ssl.c \
	$(BGP_VNC_RFP_LIBDIR)/stream-tcp.c \
	$(BGP_VNC_RFP_LIBDIR)/stream-unix.c \
	$(BGP_VNC_RFP_LIBDIR)/string.c \
	$(BGP_VNC_RFP_LIBDIR)/svec.c \
	$(BGP_VNC_RFP_LIBDIR)/syslog-direct.c \
	$(BGP_VNC_RFP_LIBDIR)/syslog-libc.c \
	$(BGP_VNC_RFP_LIBDIR)/table.c \
	$(BGP_VNC_RFP_LIBDIR)/timer.c \
	$(BGP_VNC_RFP_LIBDIR)/timeval.c \
	$(BGP_VNC_RFP_LIBDIR)/tnl-neigh-cache.c \
	$(BGP_VNC_RFP_LIBDIR)/tnl-ports.c \
	$(BGP_VNC_RFP_LIBDIR)/token-bucket.c \
	$(BGP_VNC_RFP_LIBDIR)/tun-metadata.c \
	$(BGP_VNC_RFP_LIBDIR)/unicode.c \
	$(BGP_VNC_RFP_LIBDIR)/unixctl.c \
	$(BGP_VNC_RFP_LIBDIR)/util.c \
	$(BGP_VNC_RFP_LIBDIR)/uuid.c \
	$(BGP_VNC_RFP_LIBDIR)/vconn.c \
	$(BGP_VNC_RFP_LIBDIR)/vconn-stream.c \
	$(BGP_VNC_RFP_LIBDIR)/vlan-bitmap.c \
	$(BGP_VNC_RFP_LIBDIR)/vlandev.c \
	$(BGP_VNC_RFP_LIBDIR)/vlog.c \
	$(BGP_VNC_RFP_LIBDIR)/vswitch-idl.c \
	$(BGP_VNC_RFP_LIBDIR)/ct-dpif.c \
	$(BGP_VNC_RFP_LIBDIR)/netlink-conntrack.c \
	rfp_ovs_of.c \
	rfp_ovs_testcontroller.c

#	$(BGP_VNC_RFP_LIBDIR)/stream-ssl.c 

librfp_a_INCLUDES = \
        rfp.h \
        rfp_internal.h

libdummy_a_SOURCES =

libdummy_a_DEPENDENCIES = $(OVS) 

$(OVS_PATCHED_FILES): $(OVS) $(OVS_PATCH)
	@echo "Updating $(OVS) based on $(OVS_PATCH)"
	@cd $(OVS) ; git reset --hard ; patch -p 1 < $(OVS_PATCH)

$(OVS):
	@if [ ! -e $(OVS) ] ; then \
		echo "Downloading, modifying and compiling $(OVS)" ; \
		pushd . ; \
		cd `dirname $(OVS)` ; \
		git clone $(OVS_REPO) `basename $(OVS)` ; \
		cd `basename $(OVS)` ; \
		git checkout -t $(OVS_REPO_BRANCH) ; \
		for i in `egrep -rl '(json_|route_table_init|set_nonblocking|#include..config.)' *` ; do \
			echo "Modifying $$i"; \
			(echo 'g,#include <config.*h>,s,,#include "$(OVS)/config.h",' ; \
			 echo "g,route_table_init,s,,ovs_route_table_init,g" ; \
			 echo "g,set_nonblocking,s,,ovs_set_nonblocking,g" ; \
			 echo "g,json_,s,,ovs_json_,g" ; \
			 echo "wq") | ed - $$i  ; \
		done ; \
		git commit -a -m "automated rfp_ovs_of ed changes" ; \
		patch -p 1 < $(OVS_PATCH) ; \
		autoreconf ; libtoolize; automake --add-missing ; \
		./configure ; $(MAKE) -k ;\
		popd ; \
		egrep '(CAPNG_LDADD|SSL_LIBS|S..LIBS)' ../ovs/config.status | awk -F\" '{print $$(NF-1)}' > rfp_ld_flags ; \
	fi

else
BGP_VNC_RFAPI_INC=
BGP_VNC_RFAPI_SRC=
BGP_VNC_RFAPI_HD=
BGP_VNC_RFP_LIB=
BGP_VNC_RFP_INC=
BGP_VNC_RFP_DEFS=
BGP_VNC_RFP_HD=
endif

AM_CPPFLAGS = -I. -I$(top_srcdir)/lib -I$(top_srcdir) \
	 $(BGP_VNC_RFP_INC) $(BGP_VNC_RFAPI_INC)

DEFS = @DEFS@ -DSYSCONFDIR=\"$(sysconfdir)/\" $(BGP_VNC_RFP_DEFS)
INSTALL_SDATA=@INSTALL@ -m 600
DEFAULT_INCLUDES =

AM_CFLAGS = $(PICFLAGS)
AM_LDFLAGS = $(PILDFLAGS)

noinst_LIBRARIES = $(BGP_VNC_RFP_LIB)
noinst_HEADERS = \
	$(BGP_VNC_RFAPI_HD) $(BGP_VNC_RFP_HD) $(librfp_a_INCLUDES)




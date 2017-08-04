#
# This file has been modified by LabN Consulting, L.L.C.
#
#
## Process this file with automake to produce Makefile.in.

module_LTLIBRARIES =
noinst_HEADERS =

if ENABLE_BGP_VNC
BGP_VNC_RFAPI_INC=-I$(top_srcdir)/bgpd/rfapi

module_LTLIBRARIES += bgpd_rfp_example.la

bgpd_rfp_example_la_SOURCES = rfp_example.c
bgpd_rfp_example_la_LDFLAGS = -avoid-version -module -shared -export-dynamic
bgpd_rfp_example_la_LIBADD = ../../../lib/libfrrsnmp.la

noinst_HEADERS += \
	rfp.h \
	rfp_internal.h 

else
BGP_VNC_RFAPI_INC=
endif

AM_CPPFLAGS =  -I$(top_srcdir) -I$(top_srcdir)/lib \
	-I$(top_builddir) -I$(top_builddir)/lib \
	$(BGP_VNC_RFAPI_INC)
DEFS = @DEFS@ -DSYSCONFDIR=\"$(sysconfdir)/\"
INSTALL_SDATA=@INSTALL@ -m 600

AM_CFLAGS = $(PICFLAGS)
AM_LDFLAGS = $(PILDFLAGS)

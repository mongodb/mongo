# $Id$

PACKAGE=wiredtiger
AC_INIT(WiredTiger, [__EDIT_WIREDTIGER_VERSION__],
	[support@wiredtiger.com],
	wt-__EDIT_WIREDTIGER_VERSION__)

AC_CONFIG_SRCDIR([../dist/RELEASE])
AC_CONFIG_HEADERS([wiredtiger_config.h:config.hin])
AC_CONFIG_AUX_DIR(gnu-support, aclocal)

AM_INIT_AUTOMAKE

AC_PROG_CC
AC_PROG_RANLIB
AC_PROG_INSTALL

# Local macros.
AM_TYPES
AM_OPTIONS

AC_CONFIG_FILES([
	Makefile
	wiredtiger.h:../inc_posix/wiredtiger.in
	wt_internal.h:../inc_posix/wt_internal.in])
AC_OUTPUT

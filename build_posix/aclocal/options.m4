# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

# Optional configuration.
AC_DEFUN([AM_OPTIONS], [

AH_TEMPLATE(HAVE_DEBUG, [Define to 1 if configuring debugging build.])
AC_MSG_CHECKING(if --enable-debug option specified)
AC_ARG_ENABLE(debug,
	[AC_HELP_STRING([--enable-debug],
	    [Configure a debug version.])], r=set, r=notset)
case "$r" in
set)	AC_DEFINE(HAVE_DEBUG)
	db_cv_enable_debug=yes;;
notset)	db_cv_enable_debug=no;;
esac
AC_MSG_RESULT($db_cv_enable_debug)

AH_TEMPLATE(HAVE_STATISTICS, [Define to 1 if configuring statistics support.])
AC_MSG_CHECKING(if --disable-statistics option specified)
AC_ARG_ENABLE(statistics,
	AC_HELP_STRING([--disable-statistics],
	    [Do not configure statistics support.]), r=set, r=notset)
case "$r" in
set)	db_cv_disable_statistics=yes;;
notset)	AC_DEFINE(HAVE_STATISTICS)
	db_cv_disable_statistics=no;;
esac
AC_MSG_RESULT($db_cv_disable_statistics)

])

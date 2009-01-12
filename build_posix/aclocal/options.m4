# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

# Optional configuration.
AC_DEFUN([AM_OPTIONS], [

AH_TEMPLATE(HAVE_DIAGNOSTIC, [Define to 1 for diagnostic tests.])
AC_MSG_CHECKING(if --enable-diagnostic option specified)
AC_ARG_ENABLE(diagnostic,
	[AC_HELP_STRING([--enable-diagnostic],
	    [Configure for diagnostic tests.])], r=set, r=notset)
case "$r" in
set)	AC_DEFINE(HAVE_DIAGNOSTIC)
	db_cv_enable_diagnostic=yes;;
notset)	db_cv_enable_diagnostic=no;;
esac
AC_MSG_RESULT($db_cv_enable_diagnostic)

AH_TEMPLATE(HAVE_DIAGNOSTIC_MEMORY, [Define to 1 for memory tracking output.])
AC_MSG_CHECKING(if --enable-diagnostic_memory option specified)
AC_ARG_ENABLE(diagnostic_memory,
	[AC_HELP_STRING([--enable-diagnostic_memory],
	    [Configure for memory tracking output.])], r=set, r=notset)
case "$r" in
set)	AC_DEFINE(HAVE_DIAGNOSTIC_MEMORY)
	db_cv_enable_diagnostic_memory=yes;;
notset)	db_cv_enable_diagnostic_memory=no;;
esac
AC_MSG_RESULT($db_cv_enable_diagnostic_memory)

AH_TEMPLATE(HAVE_STATISTICS, [Define to 1 for statistics support.])
AC_MSG_CHECKING(if --disable-statistics option specified)
AC_ARG_ENABLE(statistics,
	AC_HELP_STRING([--disable-statistics],
	    [Do not configure for statistics support.]), r=set, r=notset)
case "$r" in
set)	db_cv_disable_statistics=yes;;
notset)	AC_DEFINE(HAVE_STATISTICS)
	db_cv_disable_statistics=no;;
esac
AC_MSG_RESULT($db_cv_disable_statistics)

])

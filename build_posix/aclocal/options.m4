# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

# Optional configuration.
AC_DEFUN([AM_OPTIONS], [

AC_MSG_CHECKING(if --enable-debug option specified)
AC_ARG_ENABLE(debug,
	[AC_HELP_STRING([--enable-debug],
	    [Configure for debug symbols.])], r=set, r=notset)
case "$r" in
set)	db_cv_enable_debug=yes;;
notset)	db_cv_enable_debug=no;;
esac
AC_MSG_RESULT($db_cv_enable_debug)

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

AH_TEMPLATE(HAVE_SMALLBUILD, [Define to 1 for a small build.])
AC_MSG_CHECKING(if --enable-smallbuild option specified)
AC_ARG_ENABLE(smallbuild,
	[AC_HELP_STRING([--enable-smallbuild],
	    [Configure for a small build.])], r=set, r=notset)
case "$r" in
set)	AC_DEFINE(HAVE_SMALLBUILD)
	db_cv_enable_smallbuild=yes;;
notset)	db_cv_enable_smallbuild=no;;
esac
AC_MSG_RESULT($db_cv_enable_smallbuild)

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

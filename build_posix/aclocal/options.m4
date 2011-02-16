# Copyright (c) 2008-2011 WiredTiger Software.
#	All rights reserved.

# Optional configuration.
AC_DEFUN([AM_OPTIONS], [

AH_TEMPLATE(HAVE_ATTACH, [Define to 1 to pause for debugger attach on failure.])
AC_MSG_CHECKING(if --enable-attach option specified)
AC_ARG_ENABLE(attach,
	[AC_HELP_STRING([--enable-attach],
	    [Configure for debugger attach on failure.])], r=$enableval, r=no)
case "$r" in
no)	db_cv_enable_attach=no;;
*)	AC_DEFINE(HAVE_ATTACH)
	db_cv_enable_attach=yes;;
esac
AC_MSG_RESULT($db_cv_enable_attach)

AC_MSG_CHECKING(if --enable-debug option specified)
AC_ARG_ENABLE(debug,
	[AC_HELP_STRING([--enable-debug],
	    [Configure for debug symbols.])], r=$enableval, r=no)
case "$r" in
no)	db_cv_enable_debug=no;;
*)	db_cv_enable_debug=yes;;
esac
AC_MSG_RESULT($db_cv_enable_debug)

AH_TEMPLATE(HAVE_DIAGNOSTIC, [Define to 1 for diagnostic tests.])
AC_MSG_CHECKING(if --enable-diagnostic option specified)
AC_ARG_ENABLE(diagnostic,
	[AC_HELP_STRING([--enable-diagnostic],
	    [Configure for diagnostic tests.])], r=$enableval, r=no)
case "$r" in
no)	db_cv_enable_diagnostic=no;;
*)	AC_DEFINE(HAVE_DIAGNOSTIC)
	db_cv_enable_diagnostic=yes;;
esac
AC_MSG_RESULT($db_cv_enable_diagnostic)

AH_TEMPLATE(HAVE_VERBOSE, [Define to 1 to support the Env.verbose_set method.])
AC_MSG_CHECKING(if --enable-verbose option specified)
AC_ARG_ENABLE(verbose,
	[AC_HELP_STRING([--enable-verbose],
	    [Configure for Env.verbose_set method.])], r=$enableval, r=yes)
case "$r" in
no)	db_cv_enable_verbose=no;;
*)	AC_DEFINE(HAVE_VERBOSE)
	db_cv_enable_verbose=yes;;
esac
AC_MSG_RESULT($db_cv_enable_verbose)

])

# $Id$

PACKAGE=wiredtiger
AC_INIT(WiredTiger, [__EDIT_WIREDTIGER_VERSION__],
	[support@wiredtiger.com],
	wt-__EDIT_WIREDTIGER_VERSION__)

AC_CONFIG_SRCDIR([../dist/RELEASE])
AC_CONFIG_HEADERS([wiredtiger_config.h:config.hin])
AC_CONFIG_AUX_DIR(gnu-support, aclocal)

AC_SUBST(WIREDTIGER_VERSION_MAJOR)
WIREDTIGER_VERSION_MAJOR="__EDIT_WIREDTIGER_VERSION_MAJOR__"
AC_SUBST(WIREDTIGER_VERSION_MINOR)
WIREDTIGER_VERSION_MINOR="__EDIT_WIREDTIGER_VERSION_MINOR__"
AC_SUBST(WIREDTIGER_VERSION_PATCH)
WIREDTIGER_VERSION_PATCH="__EDIT_WIREDTIGER_VERSION_PATCH__"
AC_SUBST(WIREDTIGER_VERSION_STRING)
WIREDTIGER_VERSION_STRING='"__EDIT_WIREDTIGER_VERSION_STRING__"'
AC_SUBST(WIREDTIGER_VERSION_UNIQUE_NAME)

AM_INIT_AUTOMAKE

AM_OPTIONS

AC_PROG_CC(cc gcc)

if test "$GCC" = "yes"; then
	# Automake configures gcc's CFLAGS to be -g -O2.
	# 
	# If we're using gcc, change the optimization to -O3, unless we're
	# doing a small build, in which case we use -Os.
	if test "$db_cv_enable_smallbuild" = "yes"; then
		CFLAGS=`echo "$CFLAGS" | sed 's/-O./-Os /g'`
	else
		CFLAGS=`echo "$CFLAGS" | sed 's/-O./-O3 /g'`
	fi

	# The Solaris gcc compiler gets the additional -pthreads flag.
	if test "`uname -s`" = "SunOS"; then
		CFLAGS="$CFLAGS -pthreads"
	fi
else
	# Automake configures non-gcc compiler CFLAGS to be -g.
	#
	# If not using gcc, use -O (which is specifically correct for Solaris,
	# the -O flag configures the compiler to use the "default" optimization).
	CFLAGS=`echo "$CFLAGS" | sed 's/-g/-O/g'`

	# The Solaris native compiler gets the additional -mt flag.
	if test "`uname -s`" = "SunOS"; then
		CFLAGS="$CFLAGS -mt"
	fi
fi

# A debug build turns on debugging symbols -- pretty much everybody uses -g.
# Leave optimization alone, although that can make debugging more difficult.
if test "$db_cv_enable_debug" = "yes"; then
	CFLAGS=`echo "$CFLAGS" | sed 's/-g//g'`
	CFLAGS="$CFLAGS -g"
fi

AM_TYPES

AC_PROG_RANLIB
AC_PROG_INSTALL

AC_CHECK_LIB(pthread, pthread_create)
AC_CHECK_LIB(rt, sched_yield)
AC_CHECK_FUNCS(fcntl pthread_yield)

AC_CONFIG_FILES([
	Makefile
	wiredtiger.h:../inc_posix/wiredtiger.in
	wt_internal.h:../inc_posix/wt_internal.in])
AC_OUTPUT

# Copyright (c) 2008-2011 WiredTiger Software.
#	All rights reserved.

PACKAGE=wiredtiger
AC_INIT(WiredTiger, m4_normalize(m4_include([aclocal/version.m4])),
	[support@wiredtiger.com])

m4_include([aclocal/version-set.m4])

AC_CONFIG_AUX_DIR([gnu-support])
AC_CONFIG_MACRO_DIR([aclocal])
AC_CONFIG_SRCDIR([../dist/RELEASE])

LT_INIT([dlopen])
AC_SUBST([LIBTOOL_DEPS])

AM_INIT_AUTOMAKE
AM_OPTIONS

AC_CONFIG_HEADERS([wiredtiger_config.h:config.hin])

AC_PROG_CC(cc gcc)
AC_PROG_CXX(c++ g++)

if test "$GCC" = "yes"; then
	# Automake configures gcc's CFLAGS to be -g -O2.
	# 
	# If we're using gcc, change the optimization to -O3, unless we're
	# doing a small build, in which case we use -Os.
	if test "$db_cv_enable_debug" = "yes"; then
		# don't touch for debugging
		:
	elif test "$db_cv_enable_smallbuild" = "yes"; then
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
AM_CONDITIONAL(DEBUG, test "$db_cv_enable_debug" = "yes")

# Python API
if test "$db_cv_enable_python" = "yes"; then
	AM_PATH_PYTHON([2.5])
        AC_PATH_PROG([SWIG], [swig])
	if test -z "$SWIG" ; then
                AC_MSG_ERROR(
		    [Cannot find 'swig'. Download it from http://www.swig.org])
	fi
fi

AM_TYPES

AC_PROG_INSTALL

AC_CHECK_LIB(pthread, pthread_create)
AC_CHECK_LIB(dl, dlopen)
AC_CHECK_LIB(rt, sched_yield)
AC_CHECK_FUNCS([fcntl pthread_yieldNO])

AC_C_BIGENDIAN

AC_CONFIG_FILES([
	Makefile
	wiredtiger.h:../src/include/wiredtiger.in
	wt_internal.h:../src/include/wt_internal.in
	wiredtiger-${VERSION}.pc:wiredtiger.pc.in
	])
AC_OUTPUT

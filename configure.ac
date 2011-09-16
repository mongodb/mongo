# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.

PACKAGE=wiredtiger
AC_INIT(WiredTiger, m4_normalize(m4_include([build_posix/aclocal/version.m4])),
	[support@wiredtiger.com])

m4_include([build_posix/aclocal/version-set.m4])

AC_CONFIG_AUX_DIR([build_posix/gnu-support])
AC_CONFIG_MACRO_DIR([build_posix/aclocal])
AC_CONFIG_SRCDIR([dist/RELEASE])

AM_INIT_AUTOMAKE
AM_OPTIONS

define([AC_LIBTOOL_LANG_CXX_CONFIG], [:])dnl
define([AC_LIBTOOL_LANG_F77_CONFIG], [:])dnl
LT_INIT([pic-only])
AC_SUBST([LIBTOOL_DEPS])

AC_PROG_CC(cc gcc)
# AC_PROG_CXX(c++ g++)

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
	# the -O flag configures the compiler to use "default" optimization).
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

AM_CONDITIONAL([DEBUG], [test "$db_cv_enable_debug" = "yes"])

# Python API
if test "$db_cv_enable_python" = "yes"; then
	AM_PATH_PYTHON([2.6])
        AC_PATH_PROG([SWIG], [swig], [swig])
fi

AM_TYPES

AC_PROG_INSTALL

AC_CHECK_LIB(pthread, pthread_create)
AC_CHECK_LIB(dl, dlopen)
AC_CHECK_LIB(rt, sched_yield)
AC_CHECK_FUNCS([fcntl])
AC_SYS_LARGEFILE

AC_C_BIGENDIAN

# Output files
AC_CONFIG_HEADERS([wiredtiger_config.h:build_posix/config.hin])

AC_CONFIG_LINKS([
	test/format/s_dumpcmp:test/format/s_dumpcmp
])

AC_CONFIG_FILES([
	bench/tcbench/Makefile
	examples/c/Makefile
	ext/compressors/bzip2_compress/Makefile
	ext/compressors/nop_compress/Makefile
	lang/python/Makefile
	test/format/Makefile
	test/salvage/Makefile
	test/thread/Makefile
])

AC_CONFIG_FILES([
	Makefile
	wiredtiger.h:src/include/wiredtiger.in
	wt_internal.h:src/include/wt_internal.in
	wiredtiger.pc:build_posix/wiredtiger.pc.in
])
AC_OUTPUT

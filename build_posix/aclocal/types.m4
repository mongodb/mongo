# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

# AM_SIGNED_TYPES, AM_UNSIGNED_TYPES --
#	Search standard type names for something of the same size and
#	signed-ness as the type we want to declare.
#
# $1 AC_SUBST variable
# $2 typedef name
# $3 number of bytes
AC_DEFUN([AM_SIGNED_TYPES], [
	case "$3" in
	"$ac_cv_sizeof_int")
		$1="typedef int $2;";;
	"$ac_cv_sizeof_char")
		$1="typedef char $2;";;
	"$ac_cv_sizeof_short")
		$1="typedef short $2;";;
	"$ac_cv_sizeof_long")
		$1="typedef long $2;";;
	"$ac_cv_sizeof_long_long")
		$1="typedef long long $2;";;
	*)
		AC_MSG_ERROR([No signed $3-byte type found]);;
	esac])
])
AC_DEFUN([AM_UNSIGNED_TYPES], [
	case "$3" in
	"$ac_cv_sizeof_unsigned_int")
		$1="typedef unsigned int $2;";;
	"$ac_cv_sizeof_unsigned_char")
		$1="typedef unsigned char $2;";;
	"$ac_cv_sizeof_unsigned_short")
		$1="typedef unsigned short $2;";;
	"$ac_cv_sizeof_unsigned_long")
		$1="typedef unsigned long $2;";;
	"$ac_cv_sizeof_unsigned_long_long")
		$1="typedef unsigned long long $2;";;
	*)
		AC_MSG_ERROR([No unsigned $3-byte type found]);;
	esac])
])
# AM_TYPES --
#	Create any missing types.
AC_DEFUN([AM_TYPES], [
	# Basic list of include files that might have types.  We also use
	# as the list of includes directly included by wiredtiger.h.
	std_includes="
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>"
	AC_SUBST(wiredtiger_includes_decl)
	wiredtiger_includes_decl="$std_includes"

	# Look for variable-sized type names, and if we don't find them,
	# create our own.
	AC_SUBST(u_char_decl)
	AC_CHECK_TYPE(u_char,,
	    [u_char_decl="typedef unsigned char u_char;"], $std_includes)
	AC_SUBST(u_short_decl)
	AC_CHECK_TYPE(u_short,,
	    [u_short_decl="typedef unsigned short u_short;"], $std_includes)
	AC_SUBST(u_int_decl)
	AC_CHECK_TYPE(u_int,,
	    [u_int_decl="typedef unsigned int u_int;"], $std_includes)
	AC_SUBST(u_long_decl)
	AC_CHECK_TYPE(u_long,,
	    [u_long_decl="typedef unsigned long u_long;"], $std_includes)
	AC_SUBST(u_quad_decl)
	AC_CHECK_TYPE(u_quad,,
	    [u_quad_decl="typedef unsigned long long u_quad;"], $std_includes)

	# Look for fixed-size type names, and if we don't find them, create
	# our own.
	#
	# First, figure out the sizes of the standard types.
	AC_CHECK_SIZEOF(char,, $std_includes)
	AC_CHECK_SIZEOF(unsigned char,, $std_includes)
	AC_CHECK_SIZEOF(short,, $std_includes)
	AC_CHECK_SIZEOF(unsigned short,, $std_includes)
	AC_CHECK_SIZEOF(int,, $std_includes)
	AC_CHECK_SIZEOF(unsigned int,, $std_includes)
	AC_CHECK_SIZEOF(long,, $std_includes)
	AC_CHECK_SIZEOF(unsigned long,, $std_includes)
	AC_CHECK_SIZEOF(long long,, $std_includes)
	AC_CHECK_SIZEOF(unsigned long long,, $std_includes)
	AC_CHECK_SIZEOF(char *,, $std_includes)

	# Second, check for the types we really want, and if we don't find
	# them, search for something of the same size and signed-ness.
	AC_SUBST(u_int8_decl)
	AC_CHECK_TYPE(u_int8_t,, [
	    AM_UNSIGNED_TYPES(u_int8_decl, u_int8_t, 1)], $std_includes)
	AC_SUBST(int8_decl)
	AC_CHECK_TYPE(int8_t,, [
	    AM_SIGNED_TYPES(int8_decl, int8_t, 1)], $std_includes)
	AC_SUBST(u_int16_decl)
	AC_CHECK_TYPE(u_int16_t,, [
	    AM_UNSIGNED_TYPES(u_int16_decl, u_int16_t, 2)], $std_includes)
	AC_SUBST(int16_decl)
	AC_CHECK_TYPE(int16_t,, [
	    AM_SIGNED_TYPES(int16_decl, int16_t, 2)], $std_includes)
	AC_SUBST(u_int32_decl)
	AC_CHECK_TYPE(u_int32_t,, [
	    AM_UNSIGNED_TYPES(u_int32_decl, u_int32_t, 4)], $std_includes)
	AC_SUBST(int32_decl)
	AC_CHECK_TYPE(int32_t,, [
	    AM_SIGNED_TYPES(int32_decl, int32_t, 4)], $std_includes)
	AC_SUBST(u_int64_decl)
	AC_CHECK_TYPE(u_int64_t,, [
	    AM_UNSIGNED_TYPES(u_int64_decl, u_int64_t, 8)], $std_includes)
	AC_SUBST(int64_decl)
	AC_CHECK_TYPE(int64_t,, [
	    AM_SIGNED_TYPES(int64_decl, int64_t, 8)], $std_includes)

	# We additionally require FILE, off_t, pid_t, size_t, ssize_t,
	# time_t, uintmax_t and uintptr_t.
	AC_SUBST(FILE_t_decl)
	AC_CHECK_TYPE(FILE *,, AC_MSG_ERROR([No FILE type.]), $std_includes)
	AC_SUBST(off_t_decl)
	AC_CHECK_TYPE(off_t,, AC_MSG_ERROR([No off_t type.]), $std_includes)
	AC_SUBST(pid_t_decl)
	AC_CHECK_TYPE(pid_t,, AC_MSG_ERROR([No pid_t type.]), $std_includes)
	AC_SUBST(size_t_decl)
	AC_CHECK_TYPE(size_t,, AC_MSG_ERROR([No size_t type.]), $std_includes)
	AC_SUBST(ssize_t_decl)
	AC_CHECK_TYPE(ssize_t,, AC_MSG_ERROR([No size_t type.]), $std_includes)
	AC_SUBST(time_t_decl)
	AC_CHECK_TYPE(time_t,, AC_MSG_ERROR([No time_t type.]), $std_includes)

	# Some systems don't have a uintmax_t type (for example, FreeBSD 6.2.
	# In this case, use an unsigned long long.
	AC_SUBST(uintmax_t_decl)
	AC_CHECK_TYPE(uintmax_t,, [AC_CHECK_TYPE(unsigned long long,
	    [uintmax_t_decl="typedef unsigned long long uintmax_t;"],
	    [uintmax_t_decl="typedef unsigned long uintmax_t;"],
	    $std_includes)])

	AC_SUBST(uintptr_t_decl)
	AC_CHECK_TYPE(uintptr_t,,
	    AC_MSG_ERROR([No uintptr_t type.]), $std_includes)
])

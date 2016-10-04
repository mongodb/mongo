# AM_TYPES --
#	Check for missing types, create substitutes where we can.
AC_DEFUN([AM_TYPES], [
	# Basic list of include files that might have types.  We also use
	# as the list of includes directly included by wiredtiger.h.
	std_includes="
#include <sys/types.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>"
	AC_SUBST(wiredtiger_includes_decl)
	wiredtiger_includes_decl="$std_includes"

	# We require FILE, pid_t, size_t, ssize_t, time_t, uintmax_t
	# and uintptr_t.
	AC_SUBST(FILE_t_decl)
	AC_CHECK_TYPE(FILE *,, AC_MSG_ERROR([No FILE type.]), $std_includes)
	AC_SUBST(pid_t_decl)
	AC_CHECK_TYPE(pid_t,, AC_MSG_ERROR([No pid_t type.]), $std_includes)
	AC_SUBST(size_t_decl)
	AC_CHECK_TYPE(size_t,, AC_MSG_ERROR([No size_t type.]), $std_includes)
	AC_SUBST(ssize_t_decl)
	AC_CHECK_TYPE(ssize_t,, AC_MSG_ERROR([No size_t type.]), $std_includes)
	AC_SUBST(time_t_decl)
	AC_CHECK_TYPE(time_t,, AC_MSG_ERROR([No time_t type.]), $std_includes)

	# We require off_t, but use a local version for portability to Windows
	# where it's 4B, not 8B.
	AC_SUBST(off_t_decl)
	AC_CHECK_TYPE(off_t,
	    [off_t_decl="typedef off_t wt_off_t;"],
	    [AC_MSG_ERROR([No off_t type.])],
	    $std_includes)

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

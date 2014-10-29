# ===========================================================================
#        http://www.gnu.org/software/autoconf-archive/ax_prog_jar.html
# ===========================================================================
#
# SYNOPSIS
#
#   AX_PROG_JAR
#
# DESCRIPTION
#
#   AX_PROG_JAR tests for an existing jar program. It uses the environment
#   variable JAR then tests in sequence various common jar programs.
#
#   If you want to force a specific compiler:
#
#   - at the configure.in level, set JAR=yourcompiler before calling
#   AX_PROG_JAR
#
#   - at the configure level, setenv JAR
#
#   You can use the JAR variable in your Makefile.in, with @JAR@.
#
#   Note: This macro depends on the autoconf M4 macros for Java programs. It
#   is VERY IMPORTANT that you download that whole set, some macros depend
#   on other. Unfortunately, the autoconf archive does not support the
#   concept of set of macros, so I had to break it for submission.
#
#   The general documentation of those macros, as well as the sample
#   configure.in, is included in the AX_PROG_JAVA macro.
#
# LICENSE
#
#   Copyright (c) 2008 Egon Willighagen <e.willighagen@science.ru.nl>
#
#   Copying and distribution of this file, with or without modification, are
#   permitted in any medium without royalty provided the copyright notice
#   and this notice are preserved. This file is offered as-is, without any
#   warranty.

#serial 6

AU_ALIAS([AC_PROG_JAR], [AX_PROG_JAR])
AC_DEFUN([AX_PROG_JAR],[
AC_REQUIRE([AC_EXEEXT])dnl
if test "x$JAVAPREFIX" = x; then
        test "x$JAR" = x && AC_CHECK_PROGS(JAR, jar$EXEEXT)
else
        test "x$JAR" = x && AC_CHECK_PROGS(JAR, jar, $JAVAPREFIX)
fi
test "x$JAR" = x && AC_MSG_ERROR([no acceptable jar program found in \$PATH])
AC_PROVIDE([$0])dnl
])

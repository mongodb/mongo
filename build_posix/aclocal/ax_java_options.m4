# ===========================================================================
#      http://www.gnu.org/software/autoconf-archive/ax_java_options.html
# ===========================================================================
#
# SYNOPSIS
#
#   AX_JAVA_OPTIONS
#
# DESCRIPTION
#
#   AX_JAVA_OPTIONS adds configure command line options used for Java m4
#   macros. This Macro is optional.
#
#   Note: This is part of the set of autoconf M4 macros for Java programs.
#   It is VERY IMPORTANT that you download the whole set, some macros depend
#   on other. Unfortunately, the autoconf archive does not support the
#   concept of set of macros, so I had to break it for submission. The
#   general documentation, as well as the sample configure.in, is included
#   in the AX_PROG_JAVA macro.
#
# LICENSE
#
#   Copyright (c) 2008 Devin Weaver <ktohg@tritarget.com>
#
#   Copying and distribution of this file, with or without modification, are
#   permitted in any medium without royalty provided the copyright notice
#   and this notice are preserved. This file is offered as-is, without any
#   warranty.

#serial 6

AU_ALIAS([AC_JAVA_OPTIONS], [AX_JAVA_OPTIONS])
AC_DEFUN([AX_JAVA_OPTIONS],[
AC_ARG_WITH(java-prefix,
                        [  --with-java-prefix=PFX  prefix where Java runtime is installed (optional)])
AC_ARG_WITH(javac-flags,
                        [  --with-javac-flags=FLAGS flags to pass to the Java compiler (optional)])
AC_ARG_WITH(java-flags,
                        [  --with-java-flags=FLAGS flags to pass to the Java VM (optional)])
JAVAPREFIX=$with_java_prefix
JAVACFLAGS=$with_javac_flags
JAVAFLAGS=$with_java_flags
AC_SUBST(JAVAPREFIX)dnl
AC_SUBST(JAVACFLAGS)dnl
AC_SUBST(JAVAFLAGS)dnl
AC_SUBST(JAVA)dnl
AC_SUBST(JAVAC)dnl
])

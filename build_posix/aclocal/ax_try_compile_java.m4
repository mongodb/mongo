# ===========================================================================
#    http://www.gnu.org/software/autoconf-archive/ax_try_compile_java.html
# ===========================================================================
#
# SYNOPSIS
#
#   AX_TRY_COMPILE_JAVA
#
# DESCRIPTION
#
#   AX_TRY_COMPILE_JAVA attempt to compile user given source.
#
#   *Warning*: its success or failure can depend on a proper setting of the
#   CLASSPATH env. variable.
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

#serial 7

AU_ALIAS([AC_TRY_COMPILE_JAVA], [AX_TRY_COMPILE_JAVA])
AC_DEFUN([AX_TRY_COMPILE_JAVA],[
AC_REQUIRE([AX_PROG_JAVAC])dnl
cat << \EOF > Test.java
/* [#]line __oline__ "configure" */
ifelse([$1], , , [import $1;])
public class Test {
[$2]
}
EOF
if AC_TRY_COMMAND($JAVAC $JAVACFLAGS Test.java) && test -s Test.class
then
dnl Don't remove the temporary files here, so they can be examined.
  ifelse([$3], , :, [$3])
else
  echo "configure: failed program was:" >&AS_MESSAGE_LOG_FD
  cat Test.java >&AS_MESSAGE_LOG_FD
ifelse([$4], , , [  rm -fr Test*
  $4
])dnl
fi
rm -fr Test*])

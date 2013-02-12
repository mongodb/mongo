# ===========================================================================
#       http://www.gnu.org/software/autoconf-archive/ax_prog_java.html
# ===========================================================================
#
# SYNOPSIS
#
#   AX_PROG_JAVA
#
# DESCRIPTION
#
#   Here is a summary of the main macros:
#
#   AX_PROG_JAVAC: finds a Java compiler.
#
#   AX_PROG_JAVA: finds a Java virtual machine.
#
#   AX_CHECK_CLASS: finds if we have the given class (beware of CLASSPATH!).
#
#   AX_CHECK_RQRD_CLASS: finds if we have the given class and stops
#   otherwise.
#
#   AX_TRY_COMPILE_JAVA: attempt to compile user given source.
#
#   AX_TRY_RUN_JAVA: attempt to compile and run user given source.
#
#   AX_JAVA_OPTIONS: adds Java configure options.
#
#   AX_PROG_JAVA tests an existing Java virtual machine. It uses the
#   environment variable JAVA then tests in sequence various common Java
#   virtual machines. For political reasons, it starts with the free ones.
#   You *must* call [AX_PROG_JAVAC] before.
#
#   If you want to force a specific VM:
#
#   - at the configure.in level, set JAVA=yourvm before calling AX_PROG_JAVA
#
#     (but after AC_INIT)
#
#   - at the configure level, setenv JAVA
#
#   You can use the JAVA variable in your Makefile.in, with @JAVA@.
#
#   *Warning*: its success or failure can depend on a proper setting of the
#   CLASSPATH env. variable.
#
#   TODO: allow to exclude virtual machines (rationale: most Java programs
#   cannot run with some VM like kaffe).
#
#   Note: This is part of the set of autoconf M4 macros for Java programs.
#   It is VERY IMPORTANT that you download the whole set, some macros depend
#   on other. Unfortunately, the autoconf archive does not support the
#   concept of set of macros, so I had to break it for submission.
#
#   A Web page, with a link to the latest CVS snapshot is at
#   <http://www.internatif.org/bortzmeyer/autoconf-Java/>.
#
#   This is a sample configure.in Process this file with autoconf to produce
#   a configure script.
#
#     AC_INIT(UnTag.java)
#
#     dnl Checks for programs.
#     AC_CHECK_CLASSPATH
#     AX_PROG_JAVAC
#     AX_PROG_JAVA
#
#     dnl Checks for classes
#     AX_CHECK_RQRD_CLASS(org.xml.sax.Parser)
#     AX_CHECK_RQRD_CLASS(com.jclark.xml.sax.Driver)
#
#     AC_OUTPUT(Makefile)
#
# LICENSE
#
#   Copyright (c) 2008 Stephane Bortzmeyer <bortzmeyer@pasteur.fr>
#
#   This program is free software; you can redistribute it and/or modify it
#   under the terms of the GNU General Public License as published by the
#   Free Software Foundation; either version 2 of the License, or (at your
#   option) any later version.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
#   Public License for more details.
#
#   You should have received a copy of the GNU General Public License along
#   with this program. If not, see <http://www.gnu.org/licenses/>.
#
#   As a special exception, the respective Autoconf Macro's copyright owner
#   gives unlimited permission to copy, distribute and modify the configure
#   scripts that are the output of Autoconf when processing the Macro. You
#   need not follow the terms of the GNU General Public License when using
#   or distributing such scripts, even though portions of the text of the
#   Macro appear in them. The GNU General Public License (GPL) does govern
#   all other use of the material that constitutes the Autoconf Macro.
#
#   This special exception to the GPL applies to versions of the Autoconf
#   Macro released by the Autoconf Archive. When you make and distribute a
#   modified version of the Autoconf Macro, you may extend this special
#   exception to the GPL to apply to your modified version as well.

#serial 8

AU_ALIAS([AC_PROG_JAVA], [AX_PROG_JAVA])
AC_DEFUN([AX_PROG_JAVA],[
if test x$JAVAPREFIX = x; then
        test x$JAVA = x && AC_CHECK_PROGS(JAVA, kaffe java)
else
        test x$JAVA = x && AC_CHECK_PROGS(JAVA, kaffe java, $JAVAPREFIX)
fi
test x$JAVA = x && AC_MSG_ERROR([no acceptable Java virtual machine found in \$PATH])
AX_PROG_JAVA_WORKS
AC_PROVIDE([$0])dnl
])

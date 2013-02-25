# ===========================================================================
#      http://www.gnu.org/software/autoconf-archive/ax_check_junit.html
# ===========================================================================
#
# WiredTiger: Updated to use JUnit 4 call semantics.
#
# SYNOPSIS
#
#   AX_CHECK_JUNIT
#
# DESCRIPTION
#
#   AX_CHECK_JUNIT tests the availability of the Junit testing framework,
#   and set some variables for conditional compilation of the test suite by
#   automake.
#
#   If available, JUNIT is set to a command launching the text based user
#   interface of Junit, @JAVA_JUNIT@ is set to $JAVA_JUNIT and @TESTS_JUNIT@
#   is set to $TESTS_JUNIT, otherwise they are set to empty values.
#
#   You can use these variables in your Makefile.am file like this :
#
#    # Some of the following classes are built only if junit is available
#    JAVA_JUNIT  = Class1Test.java Class2Test.java AllJunitTests.java
#
#    noinst_JAVA = Example1.java Example2.java @JAVA_JUNIT@
#
#    EXTRA_JAVA  = $(JAVA_JUNIT)
#
#    TESTS_JUNIT = AllJunitTests
#
#    TESTS       = StandaloneTest1 StandaloneTest2 @TESTS_JUNIT@
#
#    EXTRA_TESTS = $(TESTS_JUNIT)
#
#    AllJunitTests :
#       echo "#! /bin/sh" > $@
#       echo "exec @JUNIT@ my.package.name.AllJunitTests" >> $@
#       chmod +x $@
#
# LICENSE
#
#   Copyright (c) 2008 Luc Maisonobe <luc@spaceroots.org>
#
#   Copying and distribution of this file, with or without modification, are
#   permitted in any medium without royalty provided the copyright notice
#   and this notice are preserved. This file is offered as-is, without any
#   warranty.

#serial 5

AU_ALIAS([AC_CHECK_JUNIT], [AX_CHECK_JUNIT])
AC_DEFUN([AX_CHECK_JUNIT],[
AC_CACHE_VAL(ac_cv_prog_JUNIT,[
AX_CHECK_CLASS(org.junit.runner.JUnitCore)
if test x"`eval 'echo $ac_cv_class_org_junit_runner_JUnitCore'`" != xno ; then
  ac_cv_prog_JUNIT='$(CLASSPATH_ENV) $(JAVA) $(JAVAFLAGS) org.junit.runner.JUnitCore'
fi])
AC_MSG_CHECKING([for junit])
if test x"`eval 'echo $ac_cv_prog_JUNIT'`" != x ; then
  JUNIT="$ac_cv_prog_JUNIT"
  JAVA_JUNIT='$(JAVA_JUNIT)'
  TESTS_JUNIT='$(TESTS_JUNIT)'
else
  JUNIT=
  JAVA_JUNIT=
  TESTS_JUNIT=
fi
AC_MSG_RESULT($JAVA_JUNIT)
AC_SUBST(JUNIT)
AC_SUBST(JAVA_JUNIT)
AC_SUBST(TESTS_JUNIT)])

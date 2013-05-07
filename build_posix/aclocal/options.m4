# Optional configuration.
AC_DEFUN([AM_OPTIONS], [

AH_TEMPLATE(HAVE_ATTACH, [Define to 1 to pause for debugger attach on failure.])
AC_MSG_CHECKING(if --enable-attach option specified)
AC_ARG_ENABLE(attach,
	[AS_HELP_STRING([--enable-attach],
	    [Configure for debugger attach on failure.])], r=$enableval, r=no)
case "$r" in
no)	wt_cv_enable_attach=no;;
*)	AC_DEFINE(HAVE_ATTACH)
	wt_cv_enable_attach=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_attach)

AC_MSG_CHECKING(if --enable-bzip2 option specified)
AC_ARG_ENABLE(bzip2,
	[AS_HELP_STRING([--enable-bzip2],
	    [Build the bzip2 compressor extension.])], r=$enableval, r=no)
case "$r" in
no)	wt_cv_enable_bzip2=no;;
*)	wt_cv_enable_bzip2=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_bzip2)
if test "$wt_cv_enable_bzip2" = "yes"; then
	AC_CHECK_HEADER(bzlib.h,,
	    [AC_MSG_ERROR([--enable-bzip2 requires bzlib.h])])
	AC_CHECK_LIB(bz2, BZ2_bzCompress,,
	    [AC_MSG_ERROR([--enable-bzip2 requires bz2 library])])
fi
AM_CONDITIONAL([BZIP2], [test "$wt_cv_enable_bzip2" = "yes"])

AC_MSG_CHECKING(if --enable-debug option specified)
AC_ARG_ENABLE(debug,
	[AS_HELP_STRING([--enable-debug],
	    [Configure for debug symbols.])], r=$enableval, r=no)
case "$r" in
no)	wt_cv_enable_debug=no;;
*)	wt_cv_enable_debug=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_debug)

AH_TEMPLATE(HAVE_DIAGNOSTIC, [Define to 1 for diagnostic tests.])
AC_MSG_CHECKING(if --enable-diagnostic option specified)
AC_ARG_ENABLE(diagnostic,
	[AS_HELP_STRING([--enable-diagnostic],
	    [Configure for diagnostic tests.])], r=$enableval, r=no)
case "$r" in
no)	wt_cv_enable_diagnostic=no;;
*)	AC_DEFINE(HAVE_DIAGNOSTIC)
	wt_cv_enable_diagnostic=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_diagnostic)

AC_MSG_CHECKING(if --enable-java option specified)
AC_ARG_ENABLE(java,
	[AS_HELP_STRING([--enable-java],
	    [Configure the Java API.])], r=$enableval, r=no)
case "$r" in
no)	wt_cv_enable_java=no;;
*)	if test "$enable_shared" = "no"; then
		AC_MSG_ERROR([--enable-java requires shared libraries])
	fi
	wt_cv_enable_java=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_java)
AM_CONDITIONAL([JAVA], [test x$wt_cv_enable_java = xyes])

AC_MSG_CHECKING(if --enable-python option specified)
AC_ARG_ENABLE(python,
	[AS_HELP_STRING([--enable-python],
	    [Configure the python API.])], r=$enableval, r=no)
case "$r" in
no)	wt_cv_enable_python=no;;
*)	if test "$enable_shared" = "no"; then
		AC_MSG_ERROR([--enable-python requires shared libraries])
	fi
	wt_cv_enable_python=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_python)
AM_CONDITIONAL([PYTHON], [test x$wt_cv_enable_python = xyes])

AC_MSG_CHECKING(if --enable-snappy option specified)
AC_ARG_ENABLE(snappy,
	[AS_HELP_STRING([--enable-snappy],
	    [Build the snappy compressor extension.])], r=$enableval, r=no)
case "$r" in
no)	wt_cv_enable_snappy=no;;
*)	wt_cv_enable_snappy=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_snappy)
if test "$wt_cv_enable_snappy" = "yes"; then
	AC_LANG_PUSH([C++])
	AC_CHECK_HEADER(snappy.h,,
	    [AC_MSG_ERROR([--enable-snappy requires snappy.h])])
	AC_LANG_POP([C++])
	AC_CHECK_LIB(snappy, snappy_compress,,
	    [AC_MSG_ERROR([--enable-snappy requires snappy library])])
fi
AM_CONDITIONAL([SNAPPY], [test "$wt_cv_enable_snappy" = "yes"])

AC_MSG_CHECKING(if --with-spinlock option specified)
AH_TEMPLATE(SPINLOCK_TYPE, [Spinlock type from mutex.h.])
AC_ARG_WITH(spinlock,
	[AS_HELP_STRING([--with-spinlock],
	    [Spinlock type (pthread_mutex or gcc).])],
	    [],
	    [with_spinlock=pthread])
case "$with_spinlock" in
pthread)	AC_DEFINE(SPINLOCK_TYPE, SPINLOCK_PTHREAD_MUTEX);;
gcc)		AC_DEFINE(SPINLOCK_TYPE, SPINLOCK_GCC);;
*)		AC_MSG_ERROR([Unknown spinlock type "$with_spinlock"]);;
esac
AC_MSG_RESULT($with_spinlock)

])

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

AH_TEMPLATE(HAVE_BUILTIN_EXTENSION_LZ4,
	    [LZ4 support automatically loaded.])
AH_TEMPLATE(HAVE_BUILTIN_EXTENSION_SNAPPY,
	    [Snappy support automatically loaded.])
AH_TEMPLATE(HAVE_BUILTIN_EXTENSION_ZLIB,
	    [Zlib support automatically loaded.])
AH_TEMPLATE(HAVE_BUILTIN_EXTENSION_ZSTD,
	    [ZSTD support automatically loaded.])
AH_TEMPLATE(HAVE_BUILTIN_EXTENSION_SODIUM,
	    [Sodium support automatically loaded.])
AC_MSG_CHECKING(if --with-builtins option specified)
AC_ARG_WITH(builtins,
	[AS_HELP_STRING([--with-builtins],
	    [builtin extension names (lz4, snappy, zlib, zstd, sodium).])],
	    [with_builtins=$withval],
	    [with_builtins=])

# Validate and setup each builtin extension library.
builtin_list=`echo "$with_builtins"|tr -s , ' '`
for builtin_i in $builtin_list; do
	case "$builtin_i" in
	lz4)	AC_DEFINE(HAVE_BUILTIN_EXTENSION_LZ4)
		wt_cv_with_builtin_extension_lz4=yes;;
	snappy)	AC_DEFINE(HAVE_BUILTIN_EXTENSION_SNAPPY)
		wt_cv_with_builtin_extension_snappy=yes;;
	zlib)	AC_DEFINE(HAVE_BUILTIN_EXTENSION_ZLIB)
		wt_cv_with_builtin_extension_zlib=yes;;
	zstd)	AC_DEFINE(HAVE_BUILTIN_EXTENSION_ZSTD)
		wt_cv_with_builtin_extension_zstd=yes;;
	sodium)	AC_DEFINE(HAVE_BUILTIN_EXTENSION_SODIUM)
		wt_cv_with_builtin_extension_sodium=yes;;
	*)	AC_MSG_ERROR([Unknown builtin extension "$builtin_i"]);;
	esac
done
AM_CONDITIONAL([HAVE_BUILTIN_EXTENSION_LZ4],
    [test "$wt_cv_with_builtin_extension_lz4" = "yes"])
AM_CONDITIONAL([HAVE_BUILTIN_EXTENSION_SNAPPY],
    [test "$wt_cv_with_builtin_extension_snappy" = "yes"])
AM_CONDITIONAL([HAVE_BUILTIN_EXTENSION_ZLIB],
    [test "$wt_cv_with_builtin_extension_zlib" = "yes"])
AM_CONDITIONAL([HAVE_BUILTIN_EXTENSION_ZSTD],
    [test "$wt_cv_with_builtin_extension_zstd" = "yes"])
AM_CONDITIONAL([HAVE_BUILTIN_EXTENSION_SODIUM],
    [test "$wt_cv_with_builtin_extension_sodium" = "yes"])
AC_MSG_RESULT($with_builtins)

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

AC_MSG_CHECKING(if --with-python-prefix option specified)
AC_ARG_WITH(python-prefix,
	[AS_HELP_STRING([--with-python-prefix=DIR],
	    [Installation prefix for Python module.])])
AC_MSG_RESULT($with_python_prefix)

AC_MSG_CHECKING(if --enable-snappy option specified)
AC_ARG_ENABLE(snappy,
	[AS_HELP_STRING([--enable-snappy],
	    [Build the snappy compressor extension.])], r=$enableval, r=no)
case "$r" in
no)	if test "$wt_cv_with_builtin_extension_snappy" = "yes"; then
		wt_cv_enable_snappy=yes
	else
		wt_cv_enable_snappy=no
	fi
	;;
*)	if test "$wt_cv_with_builtin_extension_snappy" = "yes"; then
		AC_MSG_ERROR(
		   [Only one of --enable-snappy --with-builtins=snappy allowed])
	fi
	wt_cv_enable_snappy=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_snappy)
if test "$wt_cv_enable_snappy" = "yes"; then
	AC_CHECK_HEADER(snappy-c.h,,
	    [AC_MSG_ERROR([--enable-snappy requires snappy.h])])
	AC_CHECK_LIB(snappy, snappy_compress,,
	    [AC_MSG_ERROR([--enable-snappy requires snappy library])])
fi
AM_CONDITIONAL([SNAPPY], [test "$wt_cv_enable_snappy" = "yes"])

AC_MSG_CHECKING(if --enable-lz4 option specified)
AC_ARG_ENABLE(lz4,
	[AS_HELP_STRING([--enable-lz4],
	    [Build the lz4 compressor extension.])], r=$enableval, r=no)
case "$r" in
no)	if test "$wt_cv_with_builtin_extension_lz4" = "yes"; then
		wt_cv_enable_lz4=yes
	else
		wt_cv_enable_lz4=no
	fi
	;;
*)	if test "$wt_cv_with_builtin_extension_lz4" = "yes"; then
		AC_MSG_ERROR(
		   [Only one of --enable-lz4 --with-builtins=lz4 allowed])
	fi
	wt_cv_enable_lz4=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_lz4)
if test "$wt_cv_enable_lz4" = "yes"; then
	AC_CHECK_HEADER(lz4.h,,
	    [AC_MSG_ERROR([--enable-lz4 requires lz4.h])])
	AC_CHECK_LIB(lz4, LZ4_compress_destSize,,
	    [AC_MSG_ERROR([--enable-lz4 requires lz4 library with LZ4_compress_destSize support])])
fi
AM_CONDITIONAL([LZ4], [test "$wt_cv_enable_lz4" = "yes"])

AC_MSG_CHECKING(if --enable-sodium option specified)
AC_ARG_ENABLE(sodium,
	[AS_HELP_STRING([--enable-sodium],
	    [Build the libsodium encryptor extension.])], r=$enableval, r=no)
case "$r" in
no)	if test "$wt_cv_with_builtin_extension_sodium" = "yes"; then
		wt_cv_enable_sodium=yes
	else
		wt_cv_enable_sodium=no
	fi
	;;
*)	if test "$wt_cv_with_builtin_extension_sodium" = "yes"; then
		AC_MSG_ERROR(
		   [Only one of --enable-sodium --with-builtins=sodium allowed])
	fi
	wt_cv_enable_sodium=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_sodium)
if test "$wt_cv_enable_sodium" = "yes"; then
	AC_CHECK_HEADER(sodium.h,,
	    [AC_MSG_ERROR([--enable-sodium requires sodium.h])])
	AC_CHECK_LIB(sodium, sodium_init,,
	    [AC_MSG_ERROR([--enable-sodium requires sodium library])])
fi
AM_CONDITIONAL([SODIUM], [test "$wt_cv_enable_sodium" = "yes"])

AC_MSG_CHECKING(if --enable-tcmalloc option specified)
AC_ARG_ENABLE(tcmalloc,
	[AS_HELP_STRING([--enable-tcmalloc],
	    [Build WiredTiger with tcmalloc.])], r=$enableval, r=no)
case "$r" in
no)	wt_cv_enable_tcmalloc=no;;
*)	wt_cv_enable_tcmalloc=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_tcmalloc)
if test "$wt_cv_enable_tcmalloc" = "yes"; then
	AC_CHECK_HEADER(gperftools/tcmalloc.h,,
	    [AC_MSG_ERROR([--enable-tcmalloc requires gperftools/tcmalloc.h])])
	AC_CHECK_LIB(tcmalloc, tc_calloc,,
	    [AC_MSG_ERROR([--enable-tcmalloc requires tcmalloc library])])
fi
AM_CONDITIONAL([TCMalloc], [test "$wt_cv_enable_tcmalloc" = "yes"])

AH_TEMPLATE(SPINLOCK_TYPE, [Spinlock type from mutex.h.])
AC_MSG_CHECKING(if --with-spinlock option specified)
AC_ARG_WITH(spinlock,
	[AS_HELP_STRING([--with-spinlock],
	    [Spinlock type (pthread, pthread_adaptive or gcc).])],
	    [],
	    [with_spinlock=pthread])
case "$with_spinlock" in
gcc)	AC_DEFINE(SPINLOCK_TYPE, SPINLOCK_GCC);;
pthread|pthreads)
	AC_DEFINE(SPINLOCK_TYPE, SPINLOCK_PTHREAD_MUTEX);;
pthread_adaptive|pthreads_adaptive)
	AC_DEFINE(SPINLOCK_TYPE, SPINLOCK_PTHREAD_MUTEX_ADAPTIVE);;
*)	AC_MSG_ERROR([Unknown spinlock type "$with_spinlock"]);;
esac
AC_MSG_RESULT($with_spinlock)

AC_MSG_CHECKING(if --enable-strict option specified)
AC_ARG_ENABLE(strict,
	[AS_HELP_STRING([--enable-strict],
	    [Enable strict compiler checking.])], r=$enableval, r=no)
case "$r" in
no)	wt_cv_enable_strict=no;;
*)	wt_cv_enable_strict=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_strict)

AC_MSG_CHECKING(if --enable-zlib option specified)
AC_ARG_ENABLE(zlib,
	[AS_HELP_STRING([--enable-zlib],
	    [Build the zlib compressor extension.])], r=$enableval, r=no)
case "$r" in
no)	if test "$wt_cv_with_builtin_extension_zlib" = "yes"; then
		wt_cv_enable_zlib=yes
	else
		wt_cv_enable_zlib=no
	fi
	;;
*)	if test "$wt_cv_with_builtin_extension_zlib" = "yes"; then
		AC_MSG_ERROR(
		   [Only one of --enable-zlib --with-builtins=zlib allowed])
	fi
	wt_cv_enable_zlib=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_zlib)
if test "$wt_cv_enable_zlib" = "yes"; then
	AC_CHECK_HEADER(zlib.h,,
	    [AC_MSG_ERROR([--enable-zlib requires zlib.h])])
	AC_CHECK_LIB(z, deflate,,
	    [AC_MSG_ERROR([--enable-zlib requires zlib library])])
fi
AM_CONDITIONAL([ZLIB], [test "$wt_cv_enable_zlib" = "yes"])

AC_MSG_CHECKING(if --enable-zstd option specified)
AC_ARG_ENABLE(zstd,
	[AS_HELP_STRING([--enable-zstd],
	    [Build the zstd compressor extension.])], r=$enableval, r=no)
case "$r" in
no)	if test "$wt_cv_with_builtin_extension_zstd" = "yes"; then
		wt_cv_enable_zstd=yes
	else
		wt_cv_enable_zstd=no
	fi
	;;
*)	if test "$wt_cv_with_builtin_extension_zstd" = "yes"; then
		AC_MSG_ERROR(
		   [Only one of --enable-zstd --with-builtins=zstd allowed])
	fi
	wt_cv_enable_zstd=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_zstd)
if test "$wt_cv_enable_zstd" = "yes"; then
	AC_CHECK_HEADER(zstd.h,,
	    [AC_MSG_ERROR([--enable-zstd requires zstd.h])])
	AC_CHECK_LIB(zstd, ZSTD_compress,,
	    [AC_MSG_ERROR([--enable-zstd requires Zstd library])])
fi
AM_CONDITIONAL([ZSTD], [test "$wt_cv_enable_zstd" = "yes"])

AH_TEMPLATE(HAVE_NO_CRC32_HARDWARE,
    [Define to 1 to disable any crc32 hardware support.])
AC_MSG_CHECKING(if --disable-crc32-hardware option specified)
AC_ARG_ENABLE(crc32-hardware,
	[AS_HELP_STRING([--disable-crc32-hardware],
	    [Disable any crc32 hardware support.])], r=$enableval, r=yes)
case "$r" in
no)	wt_cv_crc32_hardware=no
	AC_DEFINE(HAVE_NO_CRC32_HARDWARE)
	AC_MSG_RESULT(yes);;
*)	wt_cv_crc32_hardware=yes
	AC_MSG_RESULT(no);;
esac

AH_TEMPLATE(WT_STANDALONE_BUILD,
    [Define to 1 to support standalone build.])
AC_MSG_CHECKING(if --disable-standalone-build option specified)
AC_ARG_ENABLE(standalone-build,
       [AS_HELP_STRING([--disable-standalone-build],
           [Disable standalone build support.])], r=$enableval, r=yes)
case "$r" in
no)    wt_cv_disable_standalone_build=no
	   AC_MSG_RESULT(yes);;
*)     wt_cv_disable_standalone_build=yes
	   AC_DEFINE(WT_STANDALONE_BUILD)
	   AC_MSG_RESULT(no);;
esac

AC_MSG_CHECKING(if --enable-llvm option specified)
AC_ARG_ENABLE(llvm,
	[AS_HELP_STRING([--enable-llvm],
	    [Configure with LLVM.])], r=$enableval, r=no)
case "$r" in
no)	wt_cv_enable_llvm=no;;
*)	wt_cv_enable_llvm=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_llvm)
if test "$wt_cv_enable_llvm" = "yes"; then
	AC_CHECK_PROG(wt_cv_llvm_config, llvm-config, yes)
	if test "$wt_cv_llvm_config" != "yes"; then
		AC_MSG_ERROR([--enable-llvm requires llvm-config])
	fi
	if ! test $(llvm-config --version | grep "^8"); then
		AC_MSG_ERROR([llvm-config must be version 8])
	fi
fi
AM_CONDITIONAL([LLVM], [test x$wt_cv_enable_llvm = xyes])

AC_MSG_CHECKING(if --enable-libfuzzer option specified)
AC_ARG_ENABLE(libfuzzer,
	[AS_HELP_STRING([--enable-libfuzzer],
		[Configure with LibFuzzer.])], r=$enableval, r=no)
case "$r" in
no)	wt_cv_enable_libfuzzer=no;;
*)	wt_cv_enable_libfuzzer=yes;;
esac
AC_MSG_RESULT($wt_cv_enable_libfuzzer)
if test "$wt_cv_enable_libfuzzer" = "yes"; then
	AX_CHECK_COMPILE_FLAG([-fsanitize=fuzzer-no-link], [wt_cv_libfuzzer_works=yes])
        if test "$wt_cv_libfuzzer_works" != "yes"; then
		AC_MSG_ERROR([--enable-libfuzzer requires a Clang version that supports -fsanitize=fuzzer-no-link])
	fi
fi
AM_CONDITIONAL([LIBFUZZER], [test x$wt_cv_enable_libfuzzer = xyes])
])

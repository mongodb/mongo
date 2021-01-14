define(pkg_major, 1)
define(pkg_minor, 5)
define(pkg_extra, 0)
define(pkg_maintainer, libunwind-devel@nongnu.org)
define(mkvers, $1.$2.$3)
dnl Process this file with autoconf to produce a configure script.
AC_INIT([libunwind],[mkvers(pkg_major, pkg_minor, pkg_extra)],[pkg_maintainer])
AC_CONFIG_SRCDIR(src/mi/backtrace.c)
AC_CONFIG_AUX_DIR(config)
AC_CANONICAL_TARGET
AM_INIT_AUTOMAKE([1.6 subdir-objects])
AM_MAINTAINER_MODE
AC_CONFIG_HEADERS([include/config.h])

dnl Checks for programs.
AC_PROG_CC
AC_PROG_CXX
AC_PROG_INSTALL
AC_PROG_MAKE_SET
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])
LT_INIT
AM_PROG_AS
AM_PROG_CC_C_O

dnl Checks for libraries.
AC_CHECK_LIB(uca, __uc_get_grs)
OLD_LIBS=${LIBS}
AC_SEARCH_LIBS(dlopen, dl)
LIBS=${OLD_LIBS}
case "$ac_cv_search_dlopen" in
  -l*) DLLIB=$ac_cv_search_dlopen;;
  *) DLLIB="";;
esac

CHECK_ATOMIC_OPS

dnl Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS(asm/ptrace_offsets.h endian.h sys/endian.h execinfo.h \
		ia64intrin.h sys/uc_access.h unistd.h signal.h sys/types.h \
		sys/procfs.h sys/ptrace.h byteswap.h elf.h sys/elf.h link.h sys/link.h)

dnl Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_C_INLINE
AC_TYPE_SIZE_T
AC_CHECK_SIZEOF(off_t)

CPPFLAGS="${CPPFLAGS} -D_GNU_SOURCE"

AC_CHECK_MEMBERS([struct dl_phdr_info.dlpi_subs],,,[#include <link.h>])
AC_CHECK_TYPES([struct elf_prstatus, struct prstatus], [], [],
[$ac_includes_default
#if HAVE_SYS_PROCFS_H
# include <sys/procfs.h>
#endif
])

AC_CHECK_DECLS([PTRACE_POKEUSER, PTRACE_POKEDATA, PTRACE_SETREGSET,
PTRACE_TRACEME, PTRACE_CONT, PTRACE_SINGLESTEP,
PTRACE_SYSCALL, PT_IO, PT_GETREGS,
PT_GETFPREGS, PT_CONTINUE, PT_TRACE_ME,
PT_STEP, PT_SYSCALL], [], [],
[$ac_includes_default
#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <sys/ptrace.h>
])

dnl Checks for library functions.
AC_CHECK_FUNCS(dl_iterate_phdr dl_phdr_removals_counter dlmodinfo getunwind \
		ttrace mincore pipe2)

AC_MSG_CHECKING([if building with AltiVec])
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
#ifndef __ALTIVEC__
# error choke
#endif
]])], [use_altivec=yes],[use_altivec=no])
AM_CONDITIONAL(USE_ALTIVEC, [test x$use_altivec = xyes])
AC_MSG_RESULT([$use_altivec])

AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
#ifndef __powerpc64__
# error choke
#endif
]])], [ppc_bits=64], [ppc_bits=32])

AC_DEFUN([SET_ARCH],[
    AS_CASE([$1],
        [aarch64*],[$2=aarch64],
        [arm*],[$2=arm],
        [i?86],[$2=x86],
        [hppa*],[$2=hppa],
        [mips*],[$2=mips],
        [powerpc*],[$2=ppc$ppc_bits],
        [sh*],[$2=sh],
        [amd64],[$2=x86_64],
        [tile*],[$2=tilegx],
        [$2=$1])
]) dnl SET_ARCH

SET_ARCH([$build_cpu],[build_arch])
SET_ARCH([$host_cpu],[host_arch])
SET_ARCH([$target_cpu],[target_arch])

# Check for Android
AC_MSG_CHECKING([for Android])
android="no"
case "$host_os" in
  *android*)
    android="yes"
    AC_MSG_RESULT([yes])
    ;;
  *)
    AC_MSG_RESULT([no])
    ;;
esac

AC_ARG_ENABLE(coredump,
	AS_HELP_STRING([--enable-coredump],[building libunwind-coredump library]),,
        [AS_CASE([$host_arch], [aarch64*|arm*|mips*|sh*|x86*|tile*], [enable_coredump=yes], [enable_coredump=no])]
)

AC_MSG_CHECKING([if we should build libunwind-coredump])
AC_MSG_RESULT([$enable_coredump])

AC_ARG_ENABLE(ptrace,
	AS_HELP_STRING([--enable-ptrace],[building libunwind-ptrace library]),,
        [AC_CHECK_HEADER([sys/ptrace.h], [enable_ptrace=yes], [enable_ptrace=no])]
)

AC_MSG_CHECKING([if we should build libunwind-ptrace])
AC_MSG_RESULT([$enable_ptrace])

AC_ARG_ENABLE(setjmp,
	AS_HELP_STRING([--enable-setjmp],[building libunwind-setjmp library]),,
        [AS_IF([test x$target_arch == x$host_arch], [enable_setjmp=yes], [enable_setjmp=no])]
)

AC_ARG_ENABLE(documentation,
	AS_HELP_STRING([--disable-documentation],[Disable generating the man pages]),,
	[enable_documentation=yes])

AC_ARG_ENABLE(tests,
 AS_HELP_STRING([--disable-tests],[Disable tests build]),,
 [enable_tests=yes])

AC_ARG_ENABLE(weak-backtrace,
 AS_HELP_STRING([--disable-weak-backtrace],[Do not provide the weak 'backtrace' symbol.]),,
 [enable_weak_backtrace=yes])

AC_MSG_CHECKING([if we should build libunwind-setjmp])
AC_MSG_RESULT([$enable_setjmp])

AC_MSG_CHECKING([for build architecture])
AC_MSG_RESULT([$build_arch])
AC_MSG_CHECKING([for host architecture])
AC_MSG_RESULT([$host_arch])
AC_MSG_CHECKING([for target architecture])
AC_MSG_RESULT([$target_arch])
AC_MSG_CHECKING([for target operating system])
AC_MSG_RESULT([$target_os])

AM_CONDITIONAL(BUILD_COREDUMP, test x$enable_coredump = xyes)
AM_CONDITIONAL(BUILD_PTRACE, test x$enable_ptrace = xyes)
AM_CONDITIONAL(BUILD_SETJMP, test x$enable_setjmp = xyes)
AM_CONDITIONAL(NO_PTRACE_TEST, test x$build_arch != x$host_arch)
AM_CONDITIONAL(REMOTE_ONLY, test x$target_arch != x$host_arch)
AM_CONDITIONAL(ARCH_AARCH64, test x$target_arch = xaarch64)
AM_CONDITIONAL(ARCH_ARM, test x$target_arch = xarm)
AM_CONDITIONAL(ARCH_IA64, test x$target_arch = xia64)
AM_CONDITIONAL(ARCH_HPPA, test x$target_arch = xhppa)
AM_CONDITIONAL(ARCH_MIPS, test x$target_arch = xmips)
AM_CONDITIONAL(ARCH_X86, test x$target_arch = xx86)
AM_CONDITIONAL(ARCH_X86_64, test x$target_arch = xx86_64)
AM_CONDITIONAL(ARCH_PPC32, test x$target_arch = xppc32)
AM_CONDITIONAL(ARCH_PPC64, test x$target_arch = xppc64)
AM_CONDITIONAL(ARCH_SH, test x$target_arch = xsh)
AM_CONDITIONAL(ARCH_TILEGX, test x$target_arch = xtilegx)
AM_CONDITIONAL(ARCH_S390X, test x$target_arch = xs390x)
AM_CONDITIONAL(OS_LINUX, expr x$target_os : xlinux >/dev/null)
AM_CONDITIONAL(OS_HPUX, expr x$target_os : xhpux >/dev/null)
AM_CONDITIONAL(OS_FREEBSD, expr x$target_os : xfreebsd >/dev/null)
AM_CONDITIONAL(OS_QNX, expr x$target_os : xnto-qnx >/dev/null)
AM_CONDITIONAL(OS_SOLARIS, expr x$target_os : xsolaris >/dev/null)

AC_MSG_CHECKING([for ELF helper width])
case "${target_arch}" in
(arm|hppa|ppc32|x86|sh) use_elf32=yes; AC_MSG_RESULT([32]);;
(aarch64|ia64|ppc64|x86_64|s390x|tilegx)  use_elf64=yes; AC_MSG_RESULT([64]);;
(mips)                 use_elfxx=yes; AC_MSG_RESULT([xx]);;
*)                     AC_MSG_ERROR([Unknown ELF target: ${target_arch}])
esac
AM_CONDITIONAL(USE_ELF32, [test x$use_elf32 = xyes])
AM_CONDITIONAL(USE_ELF64, [test x$use_elf64 = xyes])
AM_CONDITIONAL(USE_ELFXX, [test x$use_elfxx = xyes])

AC_MSG_CHECKING([whether to include DWARF support])
if test x$target_arch != xia64; then
  use_dwarf=yes
else
  use_dwarf=no
fi
AM_CONDITIONAL(USE_DWARF, [test x$use_dwarf = xyes])
AC_MSG_RESULT([$use_dwarf])

if test x$target_arch = xppc64; then
        libdir='${exec_prefix}/lib64'
        AC_MSG_NOTICE([PowerPC64 detected, lib will be installed ${libdir}]);
        AC_SUBST([libdir])
fi

AC_MSG_CHECKING([whether to restrict build to remote support])
if test x$target_arch != x$host_arch; then
  CPPFLAGS="${CPPFLAGS} -DUNW_REMOTE_ONLY"
  remote_only=yes
else
  remote_only=no
fi
AC_MSG_RESULT([$remote_only])

AC_MSG_CHECKING([whether to enable debug support])
AC_ARG_ENABLE(debug,
AS_HELP_STRING([--enable-debug],[turn on debug support (slows down execution)]))
if test x$enable_debug = xyes; then
  CPPFLAGS="${CPPFLAGS} -DDEBUG"
else
  CPPFLAGS="${CPPFLAGS} -DNDEBUG"
fi
AC_MSG_RESULT([$enable_debug])

AC_MSG_CHECKING([whether to enable C++ exception support])
AC_ARG_ENABLE(cxx_exceptions,
AS_HELP_STRING([--enable-cxx-exceptions],[use libunwind to handle C++ exceptions]),,
[
# C++ exception handling doesn't work too well on x86
case $target_arch in
  x86*) enable_cxx_exceptions=no;;
  aarch64*) enable_cxx_exceptions=no;;
  arm*) enable_cxx_exceptions=no;;
  mips*) enable_cxx_exceptions=no;;
  tile*) enable_cxx_exceptions=no;;
  *) enable_cxx_exceptions=yes;;
esac
])

AM_CONDITIONAL([SUPPORT_CXX_EXCEPTIONS], [test x$enable_cxx_exceptions = xyes])
AC_MSG_RESULT([$enable_cxx_exceptions])

AC_MSG_CHECKING([whether to load .debug_frame sections])
AC_ARG_ENABLE(debug_frame,
AS_HELP_STRING([--enable-debug-frame],[Load the ".debug_frame" section if available]),, [
case "${target_arch}" in
  (arm) enable_debug_frame=yes;;
  (aarch64) enable_debug_frame=yes;;
  (*)   enable_debug_frame=no;;
esac])
if test x$enable_debug_frame = xyes; then
  AC_DEFINE([CONFIG_DEBUG_FRAME], [], [Enable Debug Frame])
fi
AC_MSG_RESULT([$enable_debug_frame])

AC_MSG_CHECKING([whether to block signals during mutex ops])
AC_ARG_ENABLE(block_signals,
AS_HELP_STRING([--enable-block-signals],[Block signals before performing mutex operations]),,
[enable_block_signals=yes])
if test x$enable_block_signals = xyes; then
  AC_DEFINE([CONFIG_BLOCK_SIGNALS], [], [Block signals before mutex operations])
fi
AC_MSG_RESULT([$enable_block_signals])

AC_MSG_CHECKING([whether to validate memory addresses before use])
AC_ARG_ENABLE(conservative_checks,
AS_HELP_STRING([--enable-conservative-checks],[Validate all memory addresses before use]),,
[enable_conservative_checks=yes])
if test x$enable_conservative_checks = xyes; then
  AC_DEFINE(CONSERVATIVE_CHECKS, 1,
	[Define to 1 if you want every memory access validated])
fi
AC_MSG_RESULT([$enable_conservative_checks])

AC_MSG_CHECKING([whether to enable msabi support])
AC_ARG_ENABLE(msabi_support,
AS_HELP_STRING([--enable-msabi-support],[Enables support for Microsoft ABI extensions]))
if test x$enable_msabi_support = xyes; then
  AC_DEFINE([CONFIG_MSABI_SUPPORT], [], [Support for Microsoft ABI extensions])
fi
AC_MSG_RESULT([$enable_msabi_support])

LIBLZMA=
AC_MSG_CHECKING([whether to support LZMA-compressed symbol tables])
AC_ARG_ENABLE(minidebuginfo,
AS_HELP_STRING([--enable-minidebuginfo], [Enables support for LZMA-compressed symbol tables]),, [enable_minidebuginfo=auto])
AC_MSG_RESULT([$enable_minidebuginfo])
if test x$enable_minidebuginfo != xno; then
   AC_CHECK_LIB([lzma], [lzma_mf_is_supported],
   [LIBLZMA=-llzma
    AC_DEFINE([HAVE_LZMA], [1], [Define if you have liblzma])
    enable_minidebuginfo=yes],
   [if test x$enable_minidebuginfo = xyes; then
      AC_MSG_FAILURE([liblzma not found])
    fi])
fi
AC_SUBST([LIBLZMA])
AM_CONDITIONAL(HAVE_LZMA, test x$enable_minidebuginfo = xyes)

LIBZ=
AC_MSG_CHECKING([whether to support ZLIB-compressed symbol tables])
AC_ARG_ENABLE(zlibdebuginfo,
AS_HELP_STRING([--enable-zlibdebuginfo], [Enables support for ZLIB-compressed symbol tables]),, [enable_zlibdebuginfo=auto])
AC_MSG_RESULT([$enable_zlibdebuginfo])
if test x$enable_zlibdebuginfo != xno; then
   AC_CHECK_LIB([z], [uncompress],
   [LIBZ=-lz
    AC_DEFINE([HAVE_ZLIB], [1], [Define if you have libz])
    enable_zlibdebuginfo=yes],
   [if test x$enable_zlibdebuginfo = xyes; then
      AC_MSG_FAILURE([libz not found])
    fi])
fi
AC_SUBST([LIBZ])
AM_CONDITIONAL(HAVE_ZLIB, test x$enable_zlibdebuginfo = xyes)

AC_MSG_CHECKING([whether to support UNW_CACHE_PER_THREAD])
AC_ARG_ENABLE([per-thread-cache],
AS_HELP_STRING([--enable-per-thread-cache], [build with support for UNW_CACHE_PER_THREAD (which imposes a hight TLS memory usage) (default: disabled)]))
AC_MSG_RESULT([$enable_per_thread_cache])
AS_IF([test x$enable_per_thread_cache = xyes], [
  LIBUNWIND___THREAD
  AS_IF([test x$libc_cv_gcc___thread = xno], [
    AC_MSG_FAILURE([UNW_CACHE_PER_THREAD requires __thread])
  ])
])

AC_MSG_CHECKING([for Intel compiler])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[#ifndef __INTEL_COMPILER
#error choke me
#endif]])],[intel_compiler=yes],[intel_compiler=no])

if test x$GCC = xyes -a x$intel_compiler != xyes; then
  CFLAGS="${CFLAGS} -fexceptions -Wall -Wsign-compare"
fi
AC_MSG_RESULT([$intel_compiler])

AC_MSG_CHECKING([if building on Solaris then define __EXTENSIONS__ macro])
if $OS_SOLARIS; then
  CFLAGS="${CFLAGS} -D__EXTENSIONS__"
  AC_MSG_RESULT([yes])
else
  AC_MSG_RESULT([no])
fi

AC_MSG_CHECKING([for QCC compiler])
AS_CASE([$CC], [qcc*|QCC*], [qcc_compiler=yes], [qcc_compiler=no])
AC_MSG_RESULT([$qcc_compiler])

if test x$intel_compiler = xyes; then
  AC_MSG_CHECKING([if linker supports -static-libcxa])
  save_LDFLAGS="$LDFLAGS"
  LDFLAGS="$LDFLAGS -static-libcxa"
  AC_LINK_IFELSE([AC_LANG_PROGRAM([[]], [[]])],[have_static_libcxa=yes],[have_static_libcxa=no])
  LDFLAGS="$save_LDFLAGS"
  if test "x$have_static_libcxa" = xyes; then
    LDFLAGS_STATIC_LIBCXA="-XCClinker -static-libcxa"
  fi
  AC_MSG_RESULT([$have_static_libcxa])
fi

if test x$qcc_compiler = xyes; then
    LDFLAGS_NOSTARTFILES="-XCClinker -Wc,-nostartfiles"
else
    LDFLAGS_NOSTARTFILES="-XCClinker -nostartfiles"
fi

if test x$GCC = xyes -a x$intel_compiler != xyes -a x$qcc_compiler != xyes -a x$android != xyes; then
  LIBCRTS="-lgcc_s"
fi

AC_MSG_CHECKING([for __builtin___clear_cache])
AC_LINK_IFELSE(
  [AC_LANG_PROGRAM([[]], [[__builtin___clear_cache(0, 0)]])],
  [have__builtin___clear_cache=yes],
  [have__builtin___clear_cache=no])
if test x$have__builtin___clear_cache = xyes; then
  AC_DEFINE([HAVE__BUILTIN___CLEAR_CACHE], [1],
            [Defined if __builtin___clear_cache() is available])
fi
AC_MSG_RESULT([$have__builtin___clear_cache])

AC_MSG_CHECKING([for __builtin_unreachable])
AC_LINK_IFELSE(
  [AC_LANG_PROGRAM([[]], [[__builtin_unreachable()]])],
  [have__builtin_unreachable=yes],
  [have__builtin_unreachable=no])
if test x$have__builtin_unreachable = xyes; then
  AC_DEFINE([HAVE__BUILTIN_UNREACHABLE], [1],
            [Defined if __builtin_unreachable() is available])
fi
AC_MSG_RESULT([$have__builtin_unreachable])

AC_MSG_CHECKING([for __sync atomics])
AC_LINK_IFELSE(
  [AC_LANG_PROGRAM([[]], [[
    __sync_bool_compare_and_swap((int *)0, 0, 1);
    __sync_fetch_and_add((int *)0, 1);
    ]])],
  [have_sync_atomics=yes],
  [have_sync_atomics=no])
if test x$have_sync_atomics = xyes; then
  AC_DEFINE([HAVE_SYNC_ATOMICS], [1],
            [Defined if __sync atomics are available])
fi
AC_MSG_RESULT([$have_sync_atomics])

CCASFLAGS="${CCASFLAGS} ${CPPFLAGS}"

arch="$target_arch"
ARCH=`echo $target_arch | tr [a-z] [A-Z]`

dnl create shell variables from the M4 macros:
PKG_MAJOR=pkg_major
PKG_MINOR=pkg_minor
PKG_EXTRA=pkg_extra
PKG_MAINTAINER=pkg_maintainer

old_LIBS="$LIBS"
LIBS=""
AC_SEARCH_LIBS(backtrace, execinfo)
LIBS="$old_LIBS"
case "$ac_cv_search_backtrace" in
  -l*) BACKTRACELIB=$ac_cv_search_backtrace;;
  *) BACKTRACELIB="";;
esac


AC_SUBST(build_arch)
AC_SUBST(target_os)
AC_SUBST(arch)
AC_SUBST(ARCH)
AC_SUBST(LDFLAGS_STATIC_LIBCXA)
AC_SUBST(LDFLAGS_NOSTARTFILES)
AC_SUBST(LIBCRTS)
AC_SUBST(PKG_MAJOR)
AC_SUBST(PKG_MINOR)
AC_SUBST(PKG_EXTRA)
AC_SUBST(PKG_MAINTAINER)
AC_SUBST(enable_cxx_exceptions)
AC_SUBST(enable_debug_frame)
AC_SUBST(DLLIB)
AC_SUBST(BACKTRACELIB)

AC_PATH_PROG([LATEX2MAN],[latex2man])
if test "x$LATEX2MAN" = "x"; then
  AC_MSG_WARN([latex2man not found. Install latex2man. Disabling docs.])
  enable_documentation="no";
fi

AM_CONDITIONAL([CONFIG_DOCS], [test x$enable_documentation = xyes])
if test "x$enable_documentation" = "xyes"; then
  AC_CONFIG_FILES(doc/Makefile doc/common.tex)
fi

AM_CONDITIONAL([CONFIG_TESTS], [test x$enable_tests = xyes])
if test "x$enable_tests" = "xyes"; then
  AC_CONFIG_FILES(tests/Makefile tests/check-namespace.sh)
fi

AM_CONDITIONAL([CONFIG_WEAK_BACKTRACE], [test "x$enable_weak_backtrace" = xyes])
AM_COND_IF([CONFIG_WEAK_BACKTRACE], [
  AC_DEFINE([CONFIG_WEAK_BACKTRACE], [1], [Define if the weak 'backtrace' symbol is provided.])
])

AC_CONFIG_FILES(Makefile src/Makefile
                include/libunwind-common.h
                include/libunwind.h include/tdep/libunwind_i.h)
AC_CONFIG_FILES(src/unwind/libunwind.pc src/coredump/libunwind-coredump.pc
                src/ptrace/libunwind-ptrace.pc src/setjmp/libunwind-setjmp.pc
                src/libunwind-generic.pc)
AC_OUTPUT

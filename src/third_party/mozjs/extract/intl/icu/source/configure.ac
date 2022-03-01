# -*-autoconf-*-
AC_COPYRIGHT([ Copyright (C) 2016 and later: Unicode, Inc. and others. License & terms of use: http://www.unicode.org/copyright.html ])
# AC_COPYRIGHT([ Copyright (c) 1999-2015, International Business Machines Corporation and others. All Rights Reserved. ])
# configure.in for ICU
# Stephen F. Booth, heavily modified by Yves and others

# NOTE: please use 'autoreconf' to rebuild, otherwise 'aclocal && autoconf'.

# Check for autoconf version
AC_PREREQ(2.69)

# Process this file with autoreconf to produce a configure script
AC_INIT([ICU],
        m4_esyscmd_s([sed -n 's/^[ 	]*#[ 	]*define[ 	]*U_ICU_VERSION[ 	]*"\([^"]*\)".*/\1/p' "./common/unicode/uvernum.h]"),
        [http://icu-project.org/bugs],
        [International Components for Unicode],
        [http://icu-project.org])

# Instruct Python to never write any byte code to the ICU source tree.
PYTHONDONTWRITEBYTECODE=1
export PYTHONDONTWRITEBYTECODE

AC_CONFIG_MACRO_DIR([config/m4])
AC_CONFIG_SRCDIR([common/unicode/utypes.h])

PACKAGE="icu"
AC_SUBST(PACKAGE)

# Use custom echo test for newline option
# Current autoconf (2.65) gives incorrect echo newline option
# for icu-config
# This may be removed later - mow (June 17, 2010)
ICU_ECHO_C= ICU_ECHO_N= ICU_ECHO_T=
case `/bin/sh -c "echo -n x"` in
-n*)
  case `/bin/sh -c "echo 'x\c'"` in
  *c*) ICU_ECHO_T=' ';;     # ECHO_T is single tab character.
  *)   ICU_ECHO_C='\c';;
  esac;;
*)
  ICU_ECHO_N='-n';;
esac
AC_SUBST(ICU_ECHO_N)
AC_SUBST(ICU_ECHO_C)
AC_SUBST(ICU_ECHO_T)

AC_MSG_CHECKING(for ICU version numbers)

# Get the ICU version from uversion.h or other headers
geticuversion() {
    [sed -n 's/^[ 	]*#[ 	]*define[ 	]*U_ICU_VERSION[ 	]*"\([^"]*\)".*/\1/p' "$@"]
}
getuversion() {
    [sed -n 's/^[ 	]*#[ 	]*define[ 	]*U_UNICODE_VERSION[ 	]*"\([^"]*\)".*/\1/p' "$@"]
}
VERSION=`geticuversion $srcdir/common/unicode/uvernum.h`
if test x"$VERSION" = x; then
   as_fn_error $? "Cannot determine ICU version number from uvernum.h header file" "$LINENO" 5
fi

#TODO: IcuBug:8502
#if test "$VERSION" != "$PACKAGE_VERSION"; then
#   AC_MSG_ERROR([configure was generated for $PACKAGE_VERSION but uvernum.h has $VERSION - please rerun autoconf])
#fi

UNICODE_VERSION=`getuversion $srcdir/common/unicode/uchar.h`
if test x"$UNICODE_VERSION" = x; then
    AC_MSG_ERROR([Cannot determine Unicode version number from uchar.h header file])
fi
# Compute a reasonable library version from the release version. This is
# very bad, but that's wanted... We want to make sure that the LIB_VERSION
# has at least a dot in it, so we'll add a .0 if needed.
#[LIB_VERSION=`echo $VERSION | sed -e 's/\.//' -e 's/^\([^.]*\)$/\1.0/'`]
LIB_VERSION=$VERSION
LIB_VERSION_MAJOR=`echo $LIB_VERSION | sed 's/\..*//'`
AC_SUBST(VERSION)
AC_SUBST(LIB_VERSION)
AC_SUBST(LIB_VERSION_MAJOR)
AC_MSG_RESULT([release $VERSION, library $LIB_VERSION, unicode version $UNICODE_VERSION])

AC_SUBST(UNICODE_VERSION)

# Determine the host system
AC_CANONICAL_HOST

AC_SUBST(CPPFLAGS)

# This variable is needed on z/OS because the C++ compiler only recognizes .C
_CXX_CXXSUFFIX=cpp
export _CXX_CXXSUFFIX

# Accumulate #defines

# CONFIG_CPPFLAGS: These are defines that are set for ICU Build time only.
# They are only needed for building ICU itself. Example: platform stuff
CONFIG_CPPFLAGS=""
# UCONFIG_CPPFLAGS: These are defines which are set for ICU build time,
# and also a notice is output that they need to be set
# for end-users of ICU also. uconfig.h.prepend is generated
# with, for example, "#define U_DISABLE_RENAMING 1"
# Example: ICU configuration stuff
UCONFIG_CPPFLAGS=""
# UCONFIG_CFLAGS: contains a copy of anything that needs to be set by end users
# such as -std
UCONFIG_CFLAGS=""

# Check whether to install icu-config
AC_ARG_ENABLE([icu-config],
    AS_HELP_STRING([--enable-icu-config], [install icu-config]),
        [case "${enableval}" in
            yes) enable_icu_config=true ;;
            no) enable_icu_config=false ;;
            *) AC_MSG_ERROR([bad value '${enableval}' for --enable-icu-config]) ;;
        esac], [enable_icu_config=true])
AC_SUBST(INSTALL_ICU_CONFIG, [$enable_icu_config])

# Check whether to build debug libraries
AC_MSG_CHECKING([whether to build debug libraries])
enabled=no
ENABLE_DEBUG=0
AC_ARG_ENABLE(debug,
    [  --enable-debug          build debug libraries and enable the U_DEBUG define [default=no]],
    [ case "${enableval}" in
         yes|"") enabled=yes; ENABLE_DEBUG=1; CONFIG_CPPFLAGS="$CONFIG_CPPFLAGS -DU_DEBUG=1" ;;
         *) ;;
    esac],
)
AC_MSG_RESULT($enabled)
AC_SUBST(ENABLE_DEBUG)

# Check whether to build release libraries
AC_MSG_CHECKING([whether to build release libraries])
enabled=yes
ENABLE_RELEASE=1
AC_ARG_ENABLE(release,
    [  --enable-release        build release libraries [default=yes]],
    [ case "${enableval}" in
         no) enabled=no; ENABLE_RELEASE=0 ;;
         *) ;;
    esac],
)
AC_MSG_RESULT($enabled)
AC_SUBST(ENABLE_RELEASE)

# Don't use the default C/CXXFLags
: ${CFLAGS=""}
: ${CXXFLAGS=""}

# Checks for compilers
AC_PROG_CC([clang gcc cc c99 c89 xlc_r xlc cl.exe icc])
# Make sure that we try clang++ first, which provides C++11 support.
# The g++ compiler is less likely to support C++11.
AC_PROG_CXX([clang++ g++ c++ gpp xlC_r xlC aCC CC cxx cc++ cl.exe icc FCC KCC RCC])

# pkg-config is needed for harfbuzz support
PKG_PROG_PKG_CONFIG([0.20])
PKG_CHECK_MODULES(ICULEHB, icu-le-hb, have_icu_le_hb=true, :)

# Ensure that if CXXFLAGS/CFLAGS were not set when calling configure, set it correctly based on (enable/disable) debug or release option
# The release mode use is the default one for autoconf
if test "$GCC" = yes; then
    if test "$CFLAGS" = ""; then
        if test "$ENABLE_DEBUG" = 1; then
            CFLAGS=-g
        fi
        if test "$ENABLE_RELEASE" = 1; then
            CFLAGS="$CFLAGS -O2"
        fi
    fi
    if test "$CXXFLAGS" = ""; then
        if test "$ENABLE_DEBUG" = 1; then
            CXXFLAGS=-g
        fi
        if test "$ENABLE_RELEASE" = 1; then
            CXXFLAGS="$CXXFLAGS -O2"
        fi
    fi
fi

AC_PROG_CPP

AC_PROG_INSTALL

AC_SUBST(cross_compiling)

dnl use the pld hack to get ac_fn_cxx_try_link defined globally and not local
AC_LANG_PUSH([C++])
AC_LINK_IFELSE([AC_LANG_PROGRAM()])
AC_LANG_POP([C++])

# make sure install is relative to srcdir - if a script
if test "$srcdir" = "."; then
  # If srcdir isn't just ., then (srcdir) is already prepended.
  if test "${ac_install_sh}" = "${INSTALL}"; then
   INSTALL="\\\$(top_srcdir)/${ac_install_sh}"
  fi
fi

#AC_CHECK_PROG(AUTOCONF, autoconf, autoconf, true)
#AC_CHECK_PROG(STRIP, strip, strip, true)

# TODO(ICU-20301): Remove fallback to Python 2.
AC_CHECK_PROGS(PYTHON, python3 "py -3" python "py")
AC_SUBST(PYTHON)

# Check for the platform make
AC_PATH_PROGS(U_MAKE, gmake gnumake, make)
AC_SUBST(U_MAKE)


AC_ARG_WITH(cross-build,
	[  --with-cross-build=dir specify an absolute path to the build directory of an ICU built for the current platform [default=no cross dir]],
	[cross_buildroot="${withval}"],
        [cross_buildroot=""])

if test "X$cross_buildroot" = "X"; then
    if test "$cross_compiling" = "yes"; then
        AC_MSG_ERROR([Error! Cross compiling but no --with-cross-build option specified - please supply the path to an executable ICU's build root])
	dnl '
    fi
else
    if test -f "${cross_buildroot}/config/icucross.mk"; then
        AC_MSG_RESULT([Using cross buildroot: $cross_buildroot])
    else
        if test -d "${cross_buildroot}"; then
            AC_MSG_ERROR([${cross_buildroot}/config/icucross.mk not found. Please build ICU in ${cross_buildroot} first.])
        else
            AC_MSG_ERROR([No such directory ${cross_buildroot} supplied as the argument to --with-cross-build. Use an absolute path.])
        fi
    fi
fi
AC_SUBST(cross_buildroot)

# Check for doxygen to generate documentation
AC_PATH_PROG(DOXYGEN,doxygen,,$PATH:/usr/local/bin:/usr/bin)

# Check that the linker is usable
ICU_PROG_LINK

# Determine the executable suffix
# We don't use AC_EXEEXT because some compilers output separate debugging
# files, which confuses the AC_EXEEXT macro.
AC_MSG_CHECKING(checking for executable suffix)
case "${host}" in
    *-*-cygwin*|*-*-mingw*)  EXEEXT=.exe ;;
    *)      EXEEXT="" ;;
esac
ac_exeext=$EXEEXT
AC_MSG_RESULT($EXEEXT)
AC_SUBST(EXEEXT)

# Determine how strict we want to be when compiling
AC_CHECK_STRICT_COMPILE

# Check if we can build and use 64-bit libraries
AC_CHECK_64BIT_LIBS
AC_SUBST(ARFLAGS)
AC_SUBST(COMPILE_LINK_ENVVAR)

# Determine the Makefile fragment
ICU_CHECK_MH_FRAG

# Checks for libraries and other host specific stuff
# On HP/UX, don't link to -lm from a shared lib because it isn't
#  PIC (at least on 10.2)
case "${host}" in
	*-*-hpux*) 	AC_CHECK_LIB(m, floor, LIB_M="-lm") ;;

	*) 		AC_CHECK_LIB(m, floor)
			LIB_M="" ;;
esac
AC_SUBST(LIB_M)

# Check whether to build shared libraries
AC_MSG_CHECKING([whether to build shared libraries])
enabled=no
AC_ARG_ENABLE(shared,
    [  --enable-shared         build shared libraries [default=yes]],
    [ case "${enableval}" in
	     yes|"") enabled=yes; ENABLE_SHARED=YES ;;
	     no);;
	     *) ;;
    esac],
    [enabled=yes; ENABLE_SHARED=YES]
)
AC_MSG_RESULT($enabled)
AC_SUBST(ENABLE_SHARED)

# Check whether to build static libraries
AC_MSG_CHECKING([whether to build static libraries])
enabled=no
AC_ARG_ENABLE(static,
    [  --enable-static         build static libraries [default=no]],
    [ case "${enableval}" in
	     yes|"") enabled=yes; ENABLE_STATIC=YES ;;
	     no) ;;
	     *) ;;
    esac],
)
AC_MSG_RESULT($enabled)
AC_SUBST(ENABLE_STATIC)

# When building release static library, there might be some optimization flags we can use
if test "$ENABLE_STATIC" = "YES"; then
    if test "$ENABLE_SHARED" != "YES"; then
        if test "$ENABLE_RELEASE" = 1; then
            AC_MSG_CHECKING([whether we can use static library optimization option])
            CHECK_STATIC_OPT_FLAG=no

            OLD_CPPFLAGS="${CPPFLAGS}"
            OLD_LDFLAGS="${LDFLAGS}"

            case "${host}" in
            *-linux*|i*86-*-*bsd*|i*86-pc-gnu)
                if test "$GCC" = yes; then
                    CPPFLAGS="${CPPFLAGS} -ffunction-sections -fdata-sections"
                    LDFLAGS="${LDFLAGS} -Wl,--gc-sections"
                fi
                ;;
            *)
                ;;
            esac

            AC_LINK_IFELSE([AC_LANG_PROGRAM([], [])], [CHECK_STATIC_OPT_FLAG=yes], [CHECK_STATIC_OPT_FLAG=no])
            AC_MSG_RESULT($CHECK_STATIC_OPT_FLAG)
            if test "$CHECK_STATIC_OPT_FLAG" = no; then
                CPPFLAGS="${OLD_CPPFLAGS}"
                LDFLAGS="${OLD_LDFLAGS}"
            fi
        fi
    fi
fi


# Check whether to enable auto cleanup of libraries
AC_MSG_CHECKING([whether to enable auto cleanup of libraries])
enabled=no
UCLN_NO_AUTO_CLEANUP=1
AC_ARG_ENABLE(auto-cleanup,
    [  --enable-auto-cleanup   enable auto cleanup of libraries [default=no]],
    [ case "${enableval}" in
         yes) enabled=yes;
         CONFIG_CPPFLAGS="$CONFIG_CPPFLAGS -DUCLN_NO_AUTO_CLEANUP=0";
         UCLN_NO_AUTO_CLEANUP=0
          ;;
         *) ;;
    esac],
)
AC_MSG_RESULT($enabled)
AC_SUBST(UCLN_NO_AUTO_CLEANUP)

# MSVC floating-point option
MSVC_RELEASE_FLAG=""
if test $enabled = yes
then
    if test $icu_cv_host_frag = mh-cygwin-msvc -o $icu_cv_host_frag = mh-msys-msvc
    then
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #if defined _MSC_VER && _MSC_VER >= 1400
    #else
    Microsoft Visual C++ < 2005
    #endif
        ]], [[]])],[MSVC_RELEASE_FLAG="/fp:precise"],[MSVC_RELEASE_FLAG="/Op"])

        CFLAGS="${CFLAGS} ${MSVC_RELEASE_FLAG}"
        CXXFLAGS="${CXXFLAGS} ${MSVC_RELEASE_FLAG}"
    fi
fi

# Check whether to enabled draft APIs
AC_MSG_CHECKING([whether to enable draft APIs])
enabled=yes
U_DEFAULT_SHOW_DRAFT=1
AC_ARG_ENABLE(draft,
    [  --enable-draft          enable draft APIs (and internal APIs) [default=yes]],
    [ case "${enableval}" in
         no) enabled=no; U_DEFAULT_SHOW_DRAFT=0;
         CONFIG_CPPFLAGS="$CONFIG_CPPFLAGS -DU_DEFAULT_SHOW_DRAFT=0"
          ;;
         *) ;;
    esac],
)
AC_MSG_RESULT($enabled)
# Make sure that we can use draft API in ICU.
if test "$U_DEFAULT_SHOW_DRAFT" = 0; then
    CONFIG_CPPFLAGS="$CONFIG_CPPFLAGS -DU_SHOW_DRAFT_API -DU_SHOW_INTERNAL_API"
fi
AC_SUBST(U_DEFAULT_SHOW_DRAFT)

AC_PROG_RANLIB

# need sed
AC_PROG_SED

# look for 'ar' the proper way
AC_CHECK_TOOL(AR, ar, false)

AC_MSG_CHECKING([whether to enable renaming of symbols])
enabled=yes
U_DISABLE_RENAMING=0
AC_ARG_ENABLE(renaming,
    [  --enable-renaming       add a version suffix to symbols [default=yes]],
    [ case "${enableval}" in
	     yes|"") enabled=yes ;;
	     no) enabled=no; U_DISABLE_RENAMING=1;
             UCONFIG_CPPFLAGS="${UCONFIG_CPPFLAGS} -DU_DISABLE_RENAMING=1"
              ;;
	     *) ;;
    esac],
)
AC_MSG_RESULT($enabled)
AC_SUBST(U_DISABLE_RENAMING)

AC_MSG_CHECKING([whether to enable function and data tracing])
enabled=no
U_ENABLE_TRACING=0
AC_ARG_ENABLE(tracing,
    [  --enable-tracing        enable function and data tracing [default=no]],
    [ case "${enableval}" in
	     yes|"") enabled=yes;
                     CONFIG_CPPFLAGS="$CONFIG_CPPFLAGS -DU_ENABLE_TRACING=1";
                     U_ENABLE_TRACING=1 ;;
	     no) enabled=no; U_ENABLE_TRACING=0 ;;
	     *) ;;
    esac],
)
AC_MSG_RESULT($enabled)
AC_SUBST(U_ENABLE_TRACING)

# check if elf.h is present.
AC_CHECK_HEADERS([elf.h])
if test "x$ac_cv_header_elf_h" = "xyes"; then
    CONFIG_CPPFLAGS="$CONFIG_CPPFLAGS -DU_HAVE_ELF_H=1";
fi

# Enable/disable plugins
AC_ARG_ENABLE(plugins,
	[  --enable-plugins        enable plugins [default=no]],
	[case "${enableval}" in
		yes) plugins=true ;;
		no)  plugins=false ;;
		*) AC_MSG_ERROR(bad value ${enableval} for --enable-plugins) ;;
		esac],
	plugins=false)
ICU_CONDITIONAL(PLUGINS, test "$plugins" = true)

if test "x$plugins" = "xtrue"; then
   UCONFIG_CPPFLAGS="$UCONFIG_CPPFLAGS -DUCONFIG_ENABLE_PLUGINS=1"
fi


U_ENABLE_DYLOAD=1
enable=yes
AC_MSG_CHECKING([whether to enable dynamic loading of plugins. Ignored if plugins disabled.])
AC_ARG_ENABLE(dyload,
    [  --disable-dyload        disable dynamic loading [default=no]],
    [ case "${enableval}" in
	     yes|"")
		     U_ENABLE_DYLOAD=1
                     enable=yes
                     ;;
	     no)
	     	 U_ENABLE_DYLOAD=0;
                 enable=no;
                 CONFIG_CPPFLAGS="$CONFIG_CPPFLAGS -DU_ENABLE_DYLOAD=0";
                 ;;
	     *) ;;
    esac],
)
AC_MSG_RESULT($enable)
AC_SUBST(U_ENABLE_DYLOAD)

if test "$enable" = "yes"; then
   AC_CHECK_HEADERS([dlfcn.h])
   #AC_MSG_RESULT($enabled)
   AC_SEARCH_LIBS([dlopen], [dl])
   AC_CHECK_FUNCS([dlopen])

   if test "x$ac_cv_func_dlopen" != xyes; then
      CONFIG_CPPFLAGS="$CONFIG_CPPFLAGS -DHAVE_DLOPEN=0"
   fi
fi

# Check for miscellanous functions.
# So, use for putil / tools only.
# Note that this will generate HAVE_GETTIMEOFDAY, not U_HAVE_GETTIMEOFDAY
AC_CHECK_FUNCS([gettimeofday])


# Check whether to use the evil rpath or not
AC_ARG_ENABLE(rpath,
    [  --enable-rpath          use rpath when linking [default is only if necessary]],
    [ case "${enableval}" in
	     yes|"") ENABLE_RPATH=YES ;;
	     no) ;;
	     *) ;;
    esac],
    [ENABLE_RPATH=NO]
)
AC_SUBST(ENABLE_RPATH)



AC_LANG_PUSH([C++])
AC_MSG_CHECKING([[if we have a C++ compiler]])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]],[[]])],[cxx_okay=yes],[cxx_okay=no])
if test $cxx_okay = yes
then
	AC_MSG_RESULT([[Good]])
else
	AC_MSG_RESULT([[no]])
	AC_MSG_ERROR([[C++ compiler $CXX does not work or no compiler found]])
fi

if [[ "$GXX" = yes ]]; then
    # if CXXFLAGS does not have a "-std=" setting, set it now to -std=c++11,
    # and check that the compiler still works.
    if ! echo "$CXXFLAGS" | grep '\-std=' >/dev/null 2>&1; then
        OLD_CXXFLAGS="${CXXFLAGS}"
        CXXFLAGS="$CXXFLAGS -std=c++11"
        AC_MSG_CHECKING([[if we have a C++11 compiler]])
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]],[[]])],[cxx11_okay=yes],[cxx11_okay=no])
        AC_MSG_RESULT($cxx11_okay)
        if [[ $cxx11_okay = yes ]]; then
            AC_MSG_NOTICE([Adding CXXFLAGS option -std=c++11])
            UCONFIG_CXXFLAGS="${UCONFIG_CXXFLAGS} -std=c++11"
        else
            CXXFLAGS="$OLD_CXXFLAGS"
        fi
        case "${host}" in
        *-*-solaris*)
            CXXFLAGS="$OLD_CXXFLAGS"
            ;;
        esac
    fi
fi

AC_LANG_POP([C++])

# Always build ICU with multi-threading support.
OLD_LIBS=${LIBS}

# For Compaq Tru64 (OSF1), we must look for pthread_attr_init
# and must do this before seaching for pthread_mutex_destroy, or
# we will pick up libpthreads.so not libpthread.so
# If this fails, then we must test for HPUX specials, before
# moving on to a more generic test

AC_CHECK_LIB(pthread, pthread_attr_init)
if test $ac_cv_lib_pthread_pthread_attr_init = yes; then
  :
else
  # Locate the right library for POSIX threads. We look for the
  # symbols in the libraries first, because on Solaris libc provides
  # pthread_create but libpthread has the real code :(
  # AIX uses libpthreads instead of libpthread, and HP/UX uses libpthread
  # FreeBSD users may need libpthread if they do not have libc_r.

  AC_SEARCH_LIBS_FIRST(pthread_mutex_destroy, pthread pthreads c_r )

  if test "$ac_cv_search_pthread_mutex_destroy" != no; then
    :
  else
    # For HP 11
    AC_CHECK_LIB(pthread, pthread_mutex_init)
    if test $ac_cv_lib_pthread_pthread_mutex_init = yes; then
      :
    fi
  fi

  AC_CHECK_FUNC(pthread_mutex_lock)

  if test $ac_cv_func_pthread_mutex_lock = yes; then
    :
  fi
fi
# Check to see if we are using CygWin with MSVC
case "${host}" in
*-pc-cygwin*|*-pc-mingw*)
  # For gcc, the thread options are set by mh-mingw/mh-cygwin
  # For msvc, the thread options are set by runConfigureICU
  :
  ;;
*-*-hpux*)
  # Add -mt because it does several nice things on newer compilers.
  case "${icu_cv_host_frag}" in
    mh-hpux-acc)
      OLD_CXXFLAGS="${CXXFLAGS}"
      CXXFLAGS="${CXXFLAGS} -mt"
      if test x"`${CXX} ${CXXFLAGS} 2>&1`" != x""; then
         CXXFLAGS="${OLD_CXXFLAGS}"
      else
         UCONFIG_CXXFLAGS="${UCONFIG_CXXFLAGS} -mt"
      fi
    ;;
  esac
  ;;
*-*-solaris*)
  case "${icu_cv_host_frag}" in
    mh-solaris)
         LIBS="${LIBS} -mt"
    ;;
  esac
  ;;
esac

AC_ARG_ENABLE(weak-threads,
	[  --enable-weak-threads   weakly reference the threading library [default=no]],
	[case "${enableval}" in
		yes)
            LIB_THREAD="${LIBS%${OLD_LIBS}}"
            LIBS=${OLD_LIBS}
            ;;
		no)  ;;
		*) AC_MSG_ERROR(bad value ${enableval} for --enable-weak-threads) ;;
		esac])
AC_SUBST(LIB_THREAD)

# Check for mmap()

# The AC_FUNC_MMAP macro doesn't work properly.  It seems to be too specific.
# Do this check instead.
HAVE_MMAP=0
AC_MSG_CHECKING([for mmap])
AC_CACHE_VAL(ac_cv_func_mmap_ok,
    [AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>], [mmap((void *)0, 0, PROT_READ, 0, 0, 0);])],[ac_cv_func_mmap_ok=yes],[ac_cv_func_mmap_ok=no])] )
AC_MSG_RESULT($ac_cv_func_mmap_ok)
if test $ac_cv_func_mmap_ok = yes
then
    HAVE_MMAP=1
else
    CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_MMAP=0"
fi
AC_SUBST(HAVE_MMAP)

AC_MSG_CHECKING([for genccode assembly])

# Check to see if genccode can generate simple assembly.
GENCCODE_ASSEMBLY=
case "${host}" in
*-linux*|*-kfreebsd*-gnu*|i*86-*-*bsd*|i*86-pc-gnu)
    if test "$GCC" = yes; then
        # We're using gcc, and the simple -a gcc command line works for genccode
        GENCCODE_ASSEMBLY="-a gcc"
    fi ;;
i*86-*-solaris*)
    if test "$GCC" = yes; then
        # When using gcc, look if we're also using GNU as.
        # When using GNU as, the simple -a gcc command line works for genccode.
        asv=`"${CC}" -print-prog-name=as 2>/dev/null`
        asv=`"${asv}" --version 2>/dev/null`
        case "X${asv}" in
        X*GNU*) GENCCODE_ASSEMBLY="-a gcc" ;;
        X*) GENCCODE_ASSEMBLY="-a sun-x86" ;;
        esac
        unset asv
    else
        GENCCODE_ASSEMBLY="-a sun-x86"
    fi ;;
sparc-*-solaris*)
    GENCCODE_ASSEMBLY="-a sun"
    ;;
ia64-*-hpux*)
# There has been some issues with building ICU data on HPUX ia64 aCC
# when using the assemble code setting below.  For now, build without
# assemble code for this platform.  This will increase the build time.   
#    GENCCODE_ASSEMBLY="-a aCC-ia64"
    ;;
esac
AC_SUBST(GENCCODE_ASSEMBLY)

AC_MSG_RESULT($GENCCODE_ASSEMBLY)

# Checks for header files
AC_CHECK_HEADERS(inttypes.h)
if test $ac_cv_header_inttypes_h = no; then
 U_HAVE_INTTYPES_H=0
 CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_INTTYPES_H=0"
else
 U_HAVE_INTTYPES_H=1
fi
if test "$CC" = ccc; then
 AC_MSG_RESULT("C compiler set to CCC ${CC}" )
 case "${host}" in
        alpha*-*-*) U_HAVE_INTTYPES_H=0;
        CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_INTTYPES_H=0"
 esac
fi

AC_SUBST(U_HAVE_INTTYPES_H)

AC_CHECK_HEADERS(dirent.h)
if test $ac_cv_header_dirent_h = no; then
 U_HAVE_DIRENT_H=0
 CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_DIRENT_H=0"
else
 U_HAVE_DIRENT_H=1
fi

AC_SUBST(U_HAVE_DIRENT_H)

# Check for endianness
AC_C_BIGENDIAN()
if test $ac_cv_c_bigendian = no; then
U_IS_BIG_ENDIAN=0
U_ENDIAN_CHAR="l"
else
U_IS_BIG_ENDIAN=1
U_ENDIAN_CHAR="b"
fi
AC_SUBST(U_IS_BIG_ENDIAN)

# Do various POSIX related checks
U_HAVE_NL_LANGINFO_CODESET=0
U_NL_LANGINFO_CODESET=-1
AC_CHECK_FUNC(nl_langinfo,[U_HAVE_NL_LANGINFO=1],[U_HAVE_NL_LANGINFO=0])
dnl AC_SUBST(U_HAVE_NL_LANGINFO)
if test $U_HAVE_NL_LANGINFO -eq 1; then
  AC_CACHE_CHECK([for nl_langinfo's argument to obtain the codeset],
  ac_cv_nl_langinfo_codeset,
  [ac_cv_nl_langinfo_codeset="unknown"
  for a in CODESET _NL_CTYPE_CODESET_NAME; do
  AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <langinfo.h>]], [[nl_langinfo($a);]])],[ac_cv_nl_langinfo_codeset="$a"; break],[])]
  done)
  if test x$ac_cv_nl_langinfo_codeset != xunknown
  then
      U_HAVE_NL_LANGINFO_CODESET=1
      U_NL_LANGINFO_CODESET=$ac_cv_nl_langinfo_codeset
      if test "x${ac_cv_nl_langinfo_codeset}" != "xCODESET"
      then
        CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DNL_LANGINFO_CODESET=${ac_cv_nl_langinfo_codeset}"
      fi
  else
      CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_NL_LANGINFO_CODESET=0"
  fi
fi
AC_SUBST(U_HAVE_NL_LANGINFO_CODESET)
AC_SUBST(U_NL_LANGINFO_CODESET)

# Namespace support checks
AC_LANG(C++)
AC_MSG_CHECKING([for namespace support])
AC_CACHE_VAL(ac_cv_namespace_ok,
    [AC_LINK_IFELSE([AC_LANG_PROGRAM([namespace x_version {void f(){}}
    namespace x = x_version;
    using namespace x_version;
    ], [f();])],[ac_cv_namespace_ok=yes],[ac_cv_namespace_ok=no])] )
AC_MSG_RESULT($ac_cv_namespace_ok)
if test $ac_cv_namespace_ok = no
then
    AC_MSG_ERROR(Namespace support is required to build ICU.)
fi

AC_MSG_CHECKING([for properly overriding new and delete])
U_OVERRIDE_CXX_ALLOCATION=0
U_HAVE_PLACEMENT_NEW=0
AC_CACHE_VAL(ac_cv_override_cxx_allocation_ok,
    [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <stdlib.h>
    class UMemory {
    public:
    void *operator new(size_t size) {return malloc(size);}
    void *operator new[](size_t size) {return malloc(size);}
    void operator delete(void *p) {free(p);}
    void operator delete[](void *p) {free(p);}
    };
    ]], [])],[ac_cv_override_cxx_allocation_ok=yes],[ac_cv_override_cxx_allocation_ok=no])] )
AC_MSG_RESULT($ac_cv_override_cxx_allocation_ok)
if test $ac_cv_override_cxx_allocation_ok = yes
then
    U_OVERRIDE_CXX_ALLOCATION=1
    AC_MSG_CHECKING([for placement new and delete])
    AC_CACHE_VAL(ac_cv_override_placement_new_ok,
        [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <stdlib.h>
        class UMemory {
        public:
        void *operator new(size_t size) {return malloc(size);}
        void *operator new[](size_t size) {return malloc(size);}
        void operator delete(void *p) {free(p);}
        void operator delete[](void *p) {free(p);}
        void * operator new(size_t, void *ptr) { return ptr; }
        void operator delete(void *, void *) {}
        };
        ]], [])],[ac_cv_override_placement_new_ok=yes],[ac_cv_override_placement_new_ok=no])] )
    AC_MSG_RESULT($ac_cv_override_placement_new_ok)
    if test $ac_cv_override_placement_new_ok = yes
    then
        U_HAVE_PLACEMENT_NEW=1
    else
        CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_PLACEMENT_NEW=0"
    fi
else
    CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_OVERRIDE_CXX_ALLOCATION=0"
fi
AC_SUBST(U_OVERRIDE_CXX_ALLOCATION)
AC_SUBST(U_HAVE_PLACEMENT_NEW)

AC_LANG(C)
AC_CHECK_FUNC(popen)
if test x$ac_cv_func_popen = xyes
then
     U_HAVE_POPEN=1
else
     CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_POPEN=0"
     U_HAVE_POPEN=0
fi
AC_SUBST(U_HAVE_POPEN)

AC_CHECK_FUNC(tzset)
U_HAVE_TZSET=0
if test x$ac_cv_func_tzset = xyes
then
    U_TZSET=tzset
    U_HAVE_TZSET=1
else
    AC_CHECK_FUNC(_tzset)
    if test x$ac_cv_func__tzset = xyes
    then
        U_TZSET=_tzset
        U_HAVE_TZSET=1
    else
      CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_TZSET=0"
    fi
fi
AC_SUBST(U_HAVE_TZSET)
AC_SUBST(U_TZSET)

U_HAVE_TZNAME=0
AC_CACHE_CHECK(for tzname,ac_cv_var_tzname,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([[#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif
#include <stdlib.h>
#include <time.h>
#ifndef tzname /* For SGI.  */
extern char *tzname[]; /* RS6000 and others reject char **tzname.  */
#endif]], [atoi(*tzname);])],[ac_cv_var_tzname=yes],[ac_cv_var_tzname=no])])
if test $ac_cv_var_tzname = yes; then
  U_TZNAME=tzname
  U_HAVE_TZNAME=1
else
  AC_CACHE_CHECK(for _tzname,ac_cv_var__tzname,
  [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <stdlib.h>
  #include <time.h>
  extern char *_tzname[];]], [atoi(*_tzname);])],[ac_cv_var__tzname=yes],[ac_cv_var__tzname=no])])
    if test $ac_cv_var__tzname = yes; then
      U_TZNAME=_tzname
      U_HAVE_TZNAME=1
    else
     CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_TZNAME=0"
    fi
fi
AC_SUBST(U_HAVE_TZNAME)
AC_SUBST(U_TZNAME)

AC_CACHE_CHECK(for timezone,ac_cv_var_timezone,
[AC_LINK_IFELSE([AC_LANG_PROGRAM([#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif
#include <time.h>
], [[timezone = 1;]])],[ac_cv_var_timezone=yes],[ac_cv_var_timezone=no])])
U_HAVE_TIMEZONE=0
if test $ac_cv_var_timezone = yes; then
  U_TIMEZONE=timezone
  U_HAVE_TIMEZONE=1
else
  AC_CACHE_CHECK(for __timezone,ac_cv_var___timezone,
  [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <time.h>]], [[__timezone = 1;]])],[ac_cv_var___timezone=yes],[ac_cv_var___timezone=no])])
  if test $ac_cv_var___timezone = yes; then
    U_TIMEZONE=__timezone
    U_HAVE_TIMEZONE=1
  else
    AC_CACHE_CHECK(for _timezone,ac_cv_var__timezone,
    [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <time.h>]], [[_timezone = 1;]])],[ac_cv_var__timezone=yes],[ac_cv_var__timezone=no])])
    if test $ac_cv_var__timezone = yes; then
      U_TIMEZONE=_timezone
      U_HAVE_TIMEZONE=1
    else
     CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_TIMEZONE=0"
    fi
  fi
fi
AC_SUBST(U_HAVE_TIMEZONE)
AC_SUBST(U_TIMEZONE)

AC_CHECK_FUNC(strtod_l)
if test x$ac_cv_func_strtod_l = xyes
then
    U_HAVE_STRTOD_L=1
    AC_CHECK_HEADER(xlocale.h)
    if test "$ac_cv_header_xlocale_h" = yes; then
      U_HAVE_XLOCALE_H=1
      CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_STRTOD_L=1 -DU_HAVE_XLOCALE_H=1"
    else
      U_HAVE_XLOCALE_H=0
      CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_STRTOD_L=1 -DU_HAVE_XLOCALE_H=0"
    fi
else
    CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_STRTOD_L=0"
    U_HAVE_STRTOD_L=0
fi
AC_SUBST(U_HAVE_STRTOD_L)

# Check if C++17 std::string_view is available.
AC_LANG_PUSH([C++])
AC_CHECK_TYPE(std::string_view, [], [], [[#include <string_view>]])
if test "x$ac_cv_type_std__string_view" = xyes; then :
  CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_STRING_VIEW=1"
fi
AC_LANG_POP([C++])

# Checks for typedefs
AC_CHECK_TYPE(int8_t,signed char)
AC_CHECK_TYPE(uint8_t,unsigned char)
AC_CHECK_TYPE(int16_t,signed short)
AC_CHECK_TYPE(uint16_t,unsigned short)
AC_CHECK_TYPE(int32_t,signed long)
AC_CHECK_TYPE(uint32_t,unsigned long)
AC_CHECK_TYPE(int64_t,signed long long)
AC_CHECK_TYPE(uint64_t,unsigned long long)

if test $ac_cv_type_int8_t = no; then
CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_INT8_T=0"
fi

if test $ac_cv_type_uint8_t = no; then
CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_UINT8_T=0"
fi

if test $ac_cv_type_int16_t = no; then
CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_INT16_T=0"
fi

if test $ac_cv_type_uint16_t = no; then
CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_UINT16_T=0"
fi

if test $ac_cv_type_int32_t = no; then
CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_INT32_T=0"
fi

if test $ac_cv_type_uint32_t = no; then
CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_UINT32_T=0"
fi

if test $ac_cv_type_int64_t = no; then
CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_INT64_T=0"
fi

if test $ac_cv_type_uint64_t = no; then
CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_UINT64_T=0"
fi

# Do various wchar_t related checks
AC_CHECK_HEADER(wchar.h)
if test "$ac_cv_header_wchar_h" = no
then
    U_HAVE_WCHAR_H=0
    U_HAVE_WCSCPY=0
    CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_WCHAR_H=0 -DU_HAVE_WCSCPY=0"
else
    AC_DEFINE([HAVE_WCHAR_H], [1], [wchar.h was found.])
    U_HAVE_WCHAR_H=1
    # Some broken systems have wchar.h but not some of its functions...
    AC_SEARCH_LIBS(wcscpy, wcs w)
    if test "$ac_cv_search_wcscpy" != no; then
      U_HAVE_WCSCPY=1
    else
      U_HAVE_WCSCPY=0
      CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_HAVE_WCSCPY=0"
    fi
fi
AC_SUBST(U_HAVE_WCHAR_H)
AC_SUBST(U_HAVE_WCSCPY)

AC_CHECK_SIZEOF([wchar_t], 0, [
#if STDC_HEADERS
#include <stddef.h>
#endif
#include <stdlib.h>
#if HAVE_WCHAR_H
#include <string.h>
#include <wchar.h>
#endif])
U_SIZEOF_WCHAR_T=$ac_cv_sizeof_wchar_t
# We do this check to verify that everything is okay.
if test $U_SIZEOF_WCHAR_T = 0; then
    if test $U_HAVE_WCHAR_H=1; then
       AC_MSG_ERROR(There is wchar.h but the size of wchar_t is 0)
    fi
fi

AC_MSG_CHECKING([for UTF-16 string literal support])
U_CHECK_UTF16_STRING=1
CHECK_UTF16_STRING_RESULT="unknown"

case "${host}" in
*-*-aix*|powerpc64-*-linux*)
    if test "$GCC" = no; then
        OLD_CFLAGS="${CFLAGS}"
        OLD_CXXFLAGS="${CXXFLAGS}"
        CFLAGS="${CFLAGS} -qutf"
        CXXFLAGS="${CXXFLAGS} -qutf"
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[const unsigned short hello[] = u"hello";]], [[]])],[U_CHECK_UTF16_STRING=1],[U_CHECK_UTF16_STRING=0])
        if test "$U_CHECK_UTF16_STRING" = 0; then
            CFLAGS="${OLD_CFLAGS}"
            CXXFLAGS="${OLD_CXXFLAGS}"
        else
            UCONFIG_CXXFLAGS="${UCONFIG_CXXFLAGS} -qutf"
            CHECK_UTF16_STRING_RESULT="-qutf"
        fi
    fi
    ;;
*-*-solaris*)
    if test "$GCC" = no; then
        OLD_CFLAGS="${CFLAGS}"
        OLD_CXXFLAGS="${CXXFLAGS}"
        CFLAGS="${CFLAGS} -xustr=ascii_utf16_ushort"
        CXXFLAGS="${CXXFLAGS} -xustr=ascii_utf16_ushort"
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[const unsigned short hello[] = U"hello";]], [[]])],[U_CHECK_UTF16_STRING=1],[U_CHECK_UTF16_STRING=0])
        if test "$U_CHECK_UTF16_STRING" = 0; then
            CFLAGS="${OLD_CFLAGS}"
            CXXFLAGS="${OLD_CXXFLAGS}"
        else
            CHECK_UTF16_STRING_RESULT="-xustr=ascii_utf16_ushort"
            UCONFIG_CXXFLAGS="${UCONFIG_CXXFLAGS} -xustr=ascii_utf16_ushort"
            UCONFIG_CFLAGS="${UCONFIG_CFLAGS} -xustr=ascii_utf16_ushort"
            # Since we can't detect the availability of this UTF-16 syntax at compile time,
            # we depend on configure telling us that we can use it.
            # Since we can't ensure ICU users use -xustr=ascii_utf16_ushort,
            # we only use this macro within ICU.
            # If an ICU user uses icu-config, this feature will be enabled.
            CONFIG_CPPFLAGS="${CONFIG_CPPFLAGS} -DU_CHECK_UTF16_STRING=1"
            U_CHECK_UTF16_STRING=0
        fi
    fi
    ;;
*-*-hpux*)
    if test "$GCC" = no; then
        # The option will be detected at compile time without additional compiler options.
        CHECK_UTF16_STRING_RESULT="available"
    fi
    ;;
*-*-cygwin)
    # wchar_t can be used
    CHECK_UTF16_STRING_RESULT="available"
    ;;
*)
    ;;
esac

# GCC >= 4.4 supports UTF16 string literals. As of ICU 62, both C and C++ files require them.
if test "$CHECK_UTF16_STRING_RESULT" = "unknown"; then
    if test "$GCC" = yes; then
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
static const unsigned short test[] = u"This is a UTF16 literal string.";
        ]], [[]])],[CC_UTF16_STRING=1],[CC_UTF16_STRING=0])
        if test "$CC_UTF16_STRING" = 1; then
            CHECK_UTF16_STRING_RESULT="C only";
        fi
    fi
    if test "$GXX" = yes; then
        # -Wno-return-type-c-linkage is desired so that stable ICU API is not warned about.
        AC_LANG_PUSH([C++])
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
static const char16_t test[] = u"This is a UTF16 literal string.";
        ]], [[]])],[CXX_UTF16_STRING=1],[CXX_UTF16_STRING=0])
        AC_LANG_POP([C++])
        if test "$CXX_UTF16_STRING" = 1; then
            if test "$CC_UTF16_STRING" = 1; then
                CHECK_UTF16_STRING_RESULT="available";
            else
                CHECK_UTF16_STRING_RESULT="C++ only";
            fi
        fi
    fi
fi
AC_MSG_RESULT($CHECK_UTF16_STRING_RESULT)

# Enable/disable extras
AC_ARG_ENABLE(extras,
	[  --enable-extras         build ICU extras [default=yes]],
	[case "${enableval}" in
		yes) extras=true ;;
		no)  extras=false ;;
		*) AC_MSG_ERROR(bad value ${enableval} for --enable-extras) ;;
		esac],
	extras=true)
ICU_CONDITIONAL(EXTRAS, test "$extras" = true)
AC_ARG_ENABLE(icuio,
	[  --enable-icuio          build ICU's icuio library [default=yes]],
	[case "${enableval}" in
		yes) icuio=true ;;
		no)  icuio=false ;;
		*) AC_MSG_ERROR(bad value ${enableval} for --enable-icuio) ;;
		esac],
	icuio=true)
ICU_CONDITIONAL(ICUIO, test "$icuio" = true)

# Enable/disable layoutex
AC_ARG_ENABLE(layoutex,
	[  --enable-layoutex         build ICU's Paragraph Layout library [default=no].
            icu-le-hb must be installed via pkg-config. See http://harfbuzz.org],
	[case "${enableval}" in
		yes) layoutex=$have_icu_le_hb ;;
		no)  layoutex=false ;;
		*) AC_MSG_ERROR(bad value ${enableval} for --enable-layoutex) ;;
		esac],
	layoutex=false)
ICU_CONDITIONAL(LAYOUTEX, test "$layoutex" = true)

# Enable/disable layout
AC_ARG_ENABLE(layout,
	[],
	[case "${enableval}" in
		yes) AC_MSG_ERROR(The ICU Layout Engine has been removed.) ;;
		no)   ;;
		*) ;;
		esac],
	)

# Enable/disable tools
AC_ARG_ENABLE(tools,
	[  --enable-tools         build ICU's tools [default=yes]],
	[case "${enableval}" in
		yes) tools=true ;;
		no)  tools=false ;;
		*) AC_MSG_ERROR(bad value ${enableval} for --enable-tools) ;;
		esac],
	tools=true)
ICU_CONDITIONAL(TOOLS, test "$tools" = true)

# Enable/disable fuzzer target tests.
AC_ARG_ENABLE(fuzzer,
	[  --enable-fuzzer        build ICU's fuzzer test targets [default=no]],
	[case "${enableval}" in
		yes) fuzzer=true ;;
		no)  fuzzer=false ;;
		*) AC_MSG_ERROR(bad value ${enableval} for --enable-fuzzer) ;;
		esac],
	fuzzer=false)
ICU_CONDITIONAL(FUZZER, test "$fuzzer" = true)

ICU_CONDITIONAL(DATA, test "$tools" = true || test "$cross_compiling" = "yes")

AC_ARG_WITH(data-packaging,
	[  --with-data-packaging     specify how to package ICU data. Possible values:
        files    raw files (.res, etc)
        archive  build a single icudtXX.dat file
        library  shared library (.dll/.so/etc.)
        static   static library (.a/.lib/etc.)
        auto     build shared if possible (default)
           See https://unicode-org.github.io/icu/userguide/icudata for more info.],
	[case "${withval}" in
                files|archive|library) datapackaging=$withval ;;
		auto) datapackaging=$withval ;;
		common) datapackaging=archive ;;
		dll) datapackaging=library ;;
		static) datapackaging=static ;;
		*) AC_MSG_ERROR(bad value ${withval} for --with-data-packaging) ;;
		esac],
        [datapackaging=])

# Note:  'thesysconfdir' is an evaluated version, for Man pages, so also for thedatadir, thelibdir, etc..
# thesysconfdir=`eval echo $sysconfdir`
dnl# AC_SUBST(thesysconfdir)
dnl# thelibdir=`test "x$exec_prefix" = xNONE && exec_prefix="$prefix"; eval echo $libdir`
dnl# AC_SUBST(thelibdir)
thedatadir=`eval echo $datadir`
dnl# AC_SUBST(thedatadir)
# Always put raw data files in share/icu/{version}, etc.   Never use lib/icu/{version} for data files.. Actual shared libraries will go in {libdir}.
pkgicudatadir=$datadir
thepkgicudatadir=$thedatadir
AC_SUBST(pkgicudatadir)
AC_SUBST(thepkgicudatadir)

dnl# Shouldn't need the AC_SUBST

if test x"$datapackaging" = x -o x"$datapackaging" = xauto; then
	# default to library
	datapackaging=library
	if test "$ENABLE_STATIC" = "YES"; then
		if test "$ENABLE_SHARED" != "YES"; then
			datapackaging=static
		fi
	fi
fi

datapackaging_dir=`eval echo $thedatadir`"/icu/${VERSION}"

datapackaging_msg="(No explaination for mode $datapackaging.)"

datapackaging_msg_path="ICU will look in $datapackaging_dir which is the installation location. Call u_setDataDirectory() or use the ICU_DATA environment variable to override."
datapackaging_msg_set="ICU will use the linked data library. If linked with the stub library located in stubdata/, the application can use udata_setCommonData() or set a data path to override."
datapackaging_howfound="(unknown)"

case "$datapackaging" in
    files)
    DATA_PACKAGING_MODE=files
    datapackaging_msg="ICU data will be stored in individual files."
    datapackaging_howfound="$datapackaging_msg_path"
    ;;
    archive)
    DATA_PACKAGING_MODE=common
    datapackaging_msg="ICU data will be stored in a single .dat file."
    datapackaging_howfound="$datapackaging_msg_path"
    ;;
    library)
    DATA_PACKAGING_MODE=dll
    datapackaging_msg="ICU data will be linked with ICU."
    if test "$ENABLE_STATIC" = "YES"; then
        datapackaging_msg="$datapackaging_msg A static data library will be built. "
    fi
    if test "$ENABLE_SHARED" = "YES"; then
        datapackaging_msg="$datapackaging_msg A shared data library will be built. "
    fi
    datapackaging_howfound="$datapackaging_msg_set"
    ;;
    static)
    DATA_PACKAGING_MODE=static
    datapackaging_msg="ICU data will be stored in a static library."
    datapackaging_howfound="$datapackaging_msg_set"
    ;;
esac
AC_SUBST(DATA_PACKAGING_MODE)

# Sets a library suffix
AC_MSG_CHECKING([for a library suffix to use])
AC_ARG_WITH(library-suffix,
	[  --with-library-suffix=suffix    tag a suffix to the library names [default=]],
	[ICULIBSUFFIX="${withval}"],
	[ICULIBSUFFIX=])
msg=$ICULIBSUFFIX
if test "$msg" = ""; then
    msg=none
fi
AC_MSG_RESULT($msg)
AC_SUBST(ICULIBSUFFIX)
if test "$ICULIBSUFFIX" != ""
then
    U_HAVE_LIB_SUFFIX=1
    ICULIBSUFFIXCNAME=`echo _$ICULIBSUFFIX | sed 's/[^A-Za-z0-9_]/_/g'`
    UCONFIG_CPPFLAGS="${UCONFIG_CPPFLAGS} -DU_HAVE_LIB_SUFFIX=1 -DU_LIB_SUFFIX_C_NAME=${ICULIBSUFFIXCNAME} "
else
    U_HAVE_LIB_SUFFIX=0
fi
AC_SUBST(U_HAVE_LIB_SUFFIX)
AC_SUBST(ICULIBSUFFIXCNAME)

# Enable/disable tests
AC_ARG_ENABLE(tests,
	[  --enable-tests          build ICU tests [default=yes]],
	[case "${enableval}" in
		yes) tests=true ;;
		no)  tests=false ;;
		*) AC_MSG_ERROR(bad value ${enableval} for --enable-tests) ;;
		esac],
	tests=true)
ICU_CONDITIONAL(TESTS, test "$tests" = true)

# Enable/disable samples
AC_ARG_ENABLE(samples,
	[  --enable-samples        build ICU samples [default=yes]

Additionally, the variable FORCE_LIBS may be set before calling configure.
If set, it will REPLACE any automatic list of libraries.],
	[case "${enableval}" in
		yes) samples=true ;;
		no)  samples=false ;;
		*) AC_MSG_ERROR(bad value ${enableval} for --enable-samples) ;;
		esac],
	samples=true)
ICU_CONDITIONAL(SAMPLES, test "$samples" = true)

ICUDATA_CHAR=$U_ENDIAN_CHAR

# Platform-specific Makefile setup
# set ICUDATA_CHAR to 'e' for any EBCDIC (which should be big endian) platform.
case "${host}" in
	*-*-solaris*) 	platform=U_SOLARIS ;;
	*-*-linux*|*-*-gnu|*-*-k*bsd*-gnu|*-*-kopensolaris*-gnu)        platform=U_LINUX ;;
	*-*-*bsd*|*-*-dragonfly*) 	platform=U_BSD ;;
	*-*-aix*) 	platform=U_AIX ;;
	*-*-hpux*) 	platform=U_HPUX ;;
	*-apple-darwin*|*-apple-rhapsody*)	platform=U_DARWIN ;;
	*-*-cygwin*)	platform=U_CYGWIN ;;
	*-*-mingw*)	platform=U_MINGW ;;
	*-*ibm-openedition*|*-*-os390*)	platform=OS390
			if test "${ICU_ENABLE_ASCII_STRINGS}" != "1"; then
				ICUDATA_CHAR="e"
			fi ;;
	*-*-os400*)	platform=OS400
			if test "${ICU_ENABLE_ASCII_STRINGS}" != "1"; then
				ICUDATA_CHAR="e"
			fi ;;
	*-*-nto*)	platform=U_QNX ;;
	*-dec-osf*) 	platform=U_OSF ;;
	*-*-beos)		platform=U_BEOS ;;
	*-*-irix*)		platform=U_IRIX ;;
	*-ncr-*)		platform=U_MPRAS ;;
	*) 		platform=U_UNKNOWN_PLATFORM ;;
esac
AC_SUBST(ICUDATA_CHAR)
AC_SUBST(platform)
platform_make_fragment_name="$icu_cv_host_frag"
platform_make_fragment='$(top_srcdir)/config/'"$platform_make_fragment_name"
AC_SUBST(platform_make_fragment_name)
AC_SUBST(platform_make_fragment)

if test "${FORCE_LIBS}" != ""; then
   echo " *** Overriding automatically chosen [LIBS=$LIBS], using instead [FORCE_LIBS=${FORCE_LIBS}]"  1>&6
   LIBS=${FORCE_LIBS}
fi

# Now that we're done using CPPFLAGS etc. for tests, we can change it
# for build.

if test "${CC}" = "clang"; then
   CLANGCFLAGS="-Qunused-arguments -Wno-parentheses-equality"
else
   CLANGCFLAGS=""
fi

if test "${CXX}" = "clang++"; then
   CLANGCXXFLAGS="-Qunused-arguments -Wno-parentheses-equality"
   LIBCXXFLAGS="$LIBCXXFLAGS -Wglobal-constructors"
else
   CLANGCXXFLAGS=""
fi

CPPFLAGS="$CPPFLAGS \$(THREADSCPPFLAGS)"
CFLAGS="$CFLAGS \$(THREADSCFLAGS) $CLANGCFLAGS"
CXXFLAGS="$CXXFLAGS \$(THREADSCXXFLAGS) $CLANGCXXFLAGS"

AC_SUBST(LIBCFLAGS)
AC_SUBST(LIBCXXFLAGS)

# append all config cppflags
CPPFLAGS="$CPPFLAGS $CONFIG_CPPFLAGS $UCONFIG_CPPFLAGS"

echo "CPPFLAGS=$CPPFLAGS"
echo "CFLAGS=$CFLAGS"
echo "CXXFLAGS=$CXXFLAGS"


# output the Makefiles
AC_CONFIG_FILES([icudefs.mk \
		Makefile \
		data/pkgdataMakefile \
		config/Makefile.inc \
		config/icu.pc \
		config/pkgdataMakefile \
		data/Makefile \
		stubdata/Makefile \
		common/Makefile \
		i18n/Makefile \
		layoutex/Makefile \
		io/Makefile \
		extra/Makefile \
		extra/uconv/Makefile \
		extra/uconv/pkgdataMakefile \
		extra/scrptrun/Makefile \
		tools/Makefile \
		tools/ctestfw/Makefile \
		tools/toolutil/Makefile \
		tools/makeconv/Makefile \
		tools/genrb/Makefile \
		tools/genccode/Makefile \
		tools/gencmn/Makefile \
		tools/gencnval/Makefile \
		tools/gendict/Makefile \
		tools/gentest/Makefile \
		tools/gennorm2/Makefile \
		tools/genbrk/Makefile \
		tools/gensprep/Makefile \
		tools/icuinfo/Makefile \
		tools/icupkg/Makefile \
		tools/icuswap/Makefile \
		tools/pkgdata/Makefile \
		tools/tzcode/Makefile \
		tools/gencfu/Makefile \
		tools/escapesrc/Makefile \
		test/Makefile \
		test/compat/Makefile \
		test/testdata/Makefile \
		test/testdata/pkgdataMakefile \
		test/hdrtst/Makefile \
		test/intltest/Makefile \
		test/cintltst/Makefile \
		test/iotest/Makefile \
		test/letest/Makefile \
		test/perf/Makefile \
		test/perf/collationperf/Makefile \
		test/perf/collperf/Makefile \
		test/perf/collperf2/Makefile \
		test/perf/dicttrieperf/Makefile \
		test/perf/ubrkperf/Makefile \
		test/perf/charperf/Makefile \
		test/perf/convperf/Makefile \
		test/perf/localecanperf/Makefile \
		test/perf/normperf/Makefile \
		test/perf/DateFmtPerf/Makefile \
		test/perf/howExpensiveIs/Makefile \
		test/perf/strsrchperf/Makefile \
		test/perf/unisetperf/Makefile \
		test/perf/usetperf/Makefile \
		test/perf/ustrperf/Makefile \
		test/perf/utfperf/Makefile \
		test/perf/utrie2perf/Makefile \
		test/perf/leperf/Makefile \
		test/fuzzer/Makefile \
		samples/Makefile samples/date/Makefile \
		samples/cal/Makefile samples/layout/Makefile])
AC_OUTPUT

if test -z "$PYTHON";
then
    echo "" > data/rules.mk
    echo "" > test/testdata/rules.mk
else
    if test -f "$srcdir/data/locales/root.txt";
    then
        echo "Spawning Python to generate data/rules.mk..."
        PYTHONPATH="$srcdir/python" \
            $PYTHON -m icutools.databuilder \
            --mode gnumake \
            --seqmode parallel \
            --src_dir "$srcdir/data" \
            --filter_file "$ICU_DATA_FILTER_FILE" \
            $ICU_DATA_BUILDTOOL_OPTS \
            > data/rules.mk
        if test "$?" != "0"; then
            AC_MSG_ERROR(Python failed to run; see above error.)
        fi
    else
      echo "Not rebuilding data/rules.mk, assuming prebuilt data in data/in"
      touch data/rules.mk
    fi
    echo "Spawning Python to generate test/testdata/rules.mk..."
    PYTHONPATH="$srcdir/python" \
        $PYTHON -m icutools.databuilder \
        --mode gnumake \
        --seqmode parallel \
        --src_dir "$srcdir/test/testdata" \
        > test/testdata/rules.mk
    if test "$?" != "0"; then
        AC_MSG_ERROR(Python failed to run; see above error.)
    fi
fi

echo
echo "ICU for C/C++ $VERSION is ready to be built."
echo "=== Important Notes: ==="

echo "Data Packaging: $datapackaging"
echo " This means: $datapackaging_msg"
echo " To locate data: $datapackaging_howfound"

if test -n "`$U_MAKE -v 2>&1 | grep '^GNU Make'`"; then
echo "Building ICU: Use a GNU make such as $U_MAKE to build ICU."
else
echo "** WARNING: $U_MAKE may not be GNU make."
echo "This may cause ICU to fail to build. Please make sure that GNU make"
echo "is in your PATH so that the configure script can detect its location."
fi
if test "x$AR" = "xfalse"; then
  echo "*** WARNING: Archiver ar not found. Set AR= or fix PATH. Some builds (such as static) may fail."
fi

AC_MSG_CHECKING([the version of "$U_MAKE"])
if "$U_MAKE" -f "$srcdir/config/gmakever.mk" PLATFORM="$platform"; then
AC_MSG_RESULT([ok])
else
AC_MSG_RESULT([too old or test failed - try upgrading GNU Make])
fi

AC_SUBST(UCONFIG_CPPFLAGS)
if test -n "$UCONFIG_CPPFLAGS"; then
   HDRFILE="uconfig.h.prepend"
   echo "*** WARNING: You must set the following flags before code compiled against this ICU will function properly:"
   echo
   echo "   ${UCONFIG_CPPFLAGS}"
   echo
   echo 'The recommended way to do this is to prepend the following lines to source/common/unicode/uconfig.h or #include them near the top of that file.'
   echo "Creating the file ${HDRFILE}"
   echo
   echo '---------------   ' "${HDRFILE}"
   echo > "${HDRFILE}"
   echo '/* ICU customizations: put these lines at the top of uconfig.h */' >> "${HDRFILE}"
   echo >> "${HDRFILE}"
   for flag in ${UCONFIG_CPPFLAGS};
   do
        echo " /* $flag */" >> "${HDRFILE}"
        case "${flag}" in
        -D*=*)
          [ \echo "${flag}" | sed -n 's%-D\([^=]*\)=%#define \1 %p' >> "${HDRFILE}" ]
          \echo >> "${HDRFILE}"
          ;;
        -D*)
          [ \echo "${flag}" | sed -n 's%-D\([^=]*\)%#define \1 %p' >> "${HDRFILE}" ]
          \echo >> "${HDRFILE}"
          ;;
        *)
          \echo "/*  Not sure how to handle this argument: ${flag} */" >> "${HDRFILE}"
          \echo >> "${HDRFILE}"
          ;;
        esac
   done
   cat "${HDRFILE}"
   \echo  "/* End of ${HDRFILE} ------------ */" >> "${HDRFILE}"
   echo >> "${HDRFILE}"
   echo '--------------- end ' "${HDRFILE}"
fi

AC_SUBST(UCONFIG_CFLAGS)
if test -n "$UCONFIG_CFLAGS"; then
   echo "C   apps may want to build with CFLAGS   = ${UCONFIG_CFLAGS}"
fi
AC_SUBST(UCONFIG_CXXFLAGS)
if test -n "$UCONFIG_CXXFLAGS"; then
   echo "C++ apps may want to build with CXXFLAGS = ${UCONFIG_CXXFLAGS}"
fi

if test "$tools" = false;
then
        echo "## Note: you have disabled ICU's tools. This ICU cannot build its own data or tests."
        echo "## Expect build failures in the 'data', 'test', and other directories."
fi

if test -z "$PYTHON";
then
    echo "** Note: Python not found. You will not be able to build data from source or run tests."
fi

$as_unset _CXX_CXXSUFFIX

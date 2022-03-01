# Copyright (C) 2016 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html
# Copyright (c) 1999-2016, International Business Machines Corporation and
# others. All Rights Reserved.
# acinclude.m4 for ICU
# Don't edit aclocal.m4, do edit acinclude.m4
# Stephen F. Booth

# @TOP@

# ICU_CHECK_MH_FRAG
AC_DEFUN([ICU_CHECK_MH_FRAG], [
	AC_CACHE_CHECK(
		[which Makefile fragment to use for ${host}],
		[icu_cv_host_frag],
		[
case "${host}" in
*-*-solaris*)
	if test "$GCC" = yes; then	
		icu_cv_host_frag=mh-solaris-gcc
	else
		icu_cv_host_frag=mh-solaris
	fi ;;
alpha*-*-linux-gnu)
	if test "$GCC" = yes; then
		icu_cv_host_frag=mh-alpha-linux-gcc
	else
		icu_cv_host_frag=mh-alpha-linux-cc
	fi ;;
powerpc*-*-linux*)
	if test "$GCC" = yes; then
		icu_cv_host_frag=mh-linux
	else
		icu_cv_host_frag=mh-linux-va
	fi ;;
*-*-linux*|*-*-gnu|*-*-k*bsd*-gnu|*-*-kopensolaris*-gnu) icu_cv_host_frag=mh-linux ;;
i[[34567]]86-*-cygwin) 
	if test "$GCC" = yes; then
		icu_cv_host_frag=mh-cygwin
	else
		icu_cv_host_frag=mh-cygwin-msvc
	fi ;;
x86_64-*-cygwin) 
	if test "$GCC" = yes; then
		icu_cv_host_frag=mh-cygwin64
	else
		icu_cv_host_frag=mh-cygwin-msvc
	fi ;;
*-*-mingw*)
	if test "$GCC" = yes; then
                AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#ifndef __MINGW64__
#error This is not MinGW64
#endif]])],                        [icu_cv_host_frag=mh-mingw64],
                                   [icu_cv_host_frag=mh-mingw])
	else
	        case "${host}" in
		*-*-mingw*) icu_cv_host_frag=mh-msys-msvc ;;
		*-*-cygwin) icu_cv_host_frag=mh-cygwin-msvc ;;
		esac
	fi ;;
*-*-*bsd*|*-*-dragonfly*) 	icu_cv_host_frag=mh-bsd-gcc ;;
*-*-aix*)
	if test "$GCC" = yes; then
		icu_cv_host_frag=mh-aix-gcc
	else
		icu_cv_host_frag=mh-aix-va
	fi ;;
*-*-hpux*)
	if test "$GCC" = yes; then
		icu_cv_host_frag=mh-hpux-gcc
	else
		case "$CXX" in
		*aCC)    icu_cv_host_frag=mh-hpux-acc ;;
		esac
	fi ;;
*-*ibm-openedition*|*-*-os390*)	icu_cv_host_frag=mh-os390 ;;
*-*-os400*)	icu_cv_host_frag=mh-os400 ;;
*-apple-rhapsody*)	icu_cv_host_frag=mh-darwin ;;
*-apple-darwin*)	icu_cv_host_frag=mh-darwin ;;
*-*-beos)       icu_cv_host_frag=mh-beos ;; 
*-*-haiku)      icu_cv_host_frag=mh-haiku ;; 
*-*-irix*)	icu_cv_host_frag=mh-irix ;;
*-dec-osf*) icu_cv_host_frag=mh-alpha-osf ;;
*-*-nto*)	icu_cv_host_frag=mh-qnx ;;
*-ncr-*)	icu_cv_host_frag=mh-mpras ;;
*) 		icu_cv_host_frag=mh-unknown ;;
esac
		]
	)
])

# ICU_PROG_LINK - Make sure that the linker is usable
AC_DEFUN([ICU_PROG_LINK],
[
case "${host}" in
    *-*-cygwin*|*-*-mingw*)
        if test "$GCC" != yes && test -n "`link --version 2>&1 | grep 'GNU coreutils'`"; then
            AC_MSG_ERROR([link.exe is not a valid linker. Your PATH is incorrect.
                  Please follow the directions in ICU's readme.])
        fi;;
    *);;
esac])

# AC_SEARCH_LIBS_FIRST(FUNCTION, SEARCH-LIBS [, ACTION-IF-FOUND
#            [, ACTION-IF-NOT-FOUND [, OTHER-LIBRARIES]]])
# Search for a library defining FUNC, then see if it's not already available.

AC_DEFUN([AC_SEARCH_LIBS_FIRST],
[AC_PREREQ([2.13])
AC_CACHE_CHECK([for library containing $1], [ac_cv_search_$1],
[ac_func_search_save_LIBS="$LIBS"
ac_cv_search_$1="no"
for i in $2; do
LIBS="-l$i $5 $ac_func_search_save_LIBS"
AC_TRY_LINK_FUNC([$1],
[ac_cv_search_$1="-l$i"
break])
done
if test "$ac_cv_search_$1" = "no"; then
AC_TRY_LINK_FUNC([$1], [ac_cv_search_$1="none required"])
fi
LIBS="$ac_func_search_save_LIBS"])
if test "$ac_cv_search_$1" != "no"; then
  test "$ac_cv_search_$1" = "none required" || LIBS="$ac_cv_search_$1 $LIBS"
  $3
else :
  $4
fi])



# Check if we can build and use 64-bit libraries
AC_DEFUN([AC_CHECK_64BIT_LIBS],
[
    BITS_REQ=nochange
    ENABLE_64BIT_LIBS=unknown
    ## revisit this for cross-compile.
    
    AC_ARG_ENABLE(64bit-libs,
        [  --enable-64bit-libs     (deprecated, use --with-library-bits) build 64-bit libraries [default= platform default]],
        [echo "note, use --with-library-bits instead of --*-64bit-libs"
         case "${enableval}" in
            no|false|32) with_library_bits=32;  ;;
            yes|true|64) with_library_bits=64else32 ;;
            nochange) with_library_bits=nochange; ;;
            *) AC_MSG_ERROR(bad value ${enableval} for '--*-64bit-libs') ;;
            esac]    )
    

    AC_ARG_WITH(library-bits,
        [  --with-library-bits=bits specify how many bits to use for the library (32, 64, 64else32, nochange) [default=nochange]],
        [case "${withval}" in
            ""|nochange) BITS_REQ=$withval ;;
            32|64|64else32) BITS_REQ=$withval ;;
            *) AC_MSG_ERROR(bad value ${withval} for --with-library-bits) ;;
            esac])
        
    # don't use these for cross compiling
    if test "$cross_compiling" = "yes" -a "${BITS_REQ}" != "nochange"; then
        AC_MSG_ERROR([Don't specify bitness when cross compiling. See readme.html for help with cross compilation., and set compiler options manually.])
    fi
    AC_CHECK_SIZEOF([void *])
    AC_MSG_CHECKING([whether runnable 64 bit binaries are built by default])
    case $ac_cv_sizeof_void_p in
        8) DEFAULT_64BIT=yes ;;
        4) DEFAULT_64BIT=no ;;
        *) DEFAULT_64BIT=unknown
    esac
    BITS_GOT=unknown
    
    # 'OK' here means, we can exit any further checking, everything's copa
    BITS_OK=yes

    # do we need to check for buildable/runnable 32 or 64 bit?
    BITS_CHECK_32=no
    BITS_CHECK_64=no
    
    # later, can we run the 32/64 bit binaries so made?
    BITS_RUN_32=no
    BITS_RUN_64=no
    
    if test "$DEFAULT_64BIT" = "yes"; then
        # we get 64 bits by default.
        BITS_GOT=64
        case "$BITS_REQ" in
            32) 
                # need to look for 32 bit support. 
                BITS_CHECK_32=yes
                # not copa.
                BITS_OK=no;;
            # everyone else is happy.
            nochange) ;;
            *) ;;
        esac
    elif test "$DEFAULT_64BIT" = "no"; then
        # not 64 bit by default.
        BITS_GOT=32
        case "$BITS_REQ" in
            64|64else32)
                BITS_CHECK_64=yes
                #BITS_CHECK_32=yes
                BITS_OK=no;;
            nochange) ;;
            *) ;;
        esac
    elif test "$DEFAULT_64BIT" = "unknown"; then
        # cross compiling.
        BITS_GOT=unknown
        case "$BITS_REQ" in
            64|64else32) BITS_OK=no
            BITS_CHECK_32=yes
            BITS_CHECK_64=yes ;;
            32) BITS_OK=no;;
            nochange) ;;
            *) ;;
        esac
    fi
            
    AC_MSG_RESULT($DEFAULT_64BIT);

    if test "$BITS_OK" != "yes"; then
        # not copa. back these up.
        CFLAGS_OLD="${CFLAGS}"
        CXXFLAGS_OLD="${CXXFLAGS}"
        LDFLAGS_OLD="${LDFLAGS}"
        ARFLAGS_OLD="${ARFLAGS}"        
        
        CFLAGS_32="${CFLAGS}"
        CXXFLAGS_32="${CXXFLAGS}"
        LDFLAGS_32="${LDFLAGS}"
        ARFLAGS_32="${ARFLAGS}"        
        
        CFLAGS_64="${CFLAGS}"
        CXXFLAGS_64="${CXXFLAGS}"
        LDFLAGS_64="${LDFLAGS}"
        ARFLAGS_64="${ARFLAGS}"        
        
        CAN_BUILD_64=unknown
        CAN_BUILD_32=unknown
        # These results can't be cached because is sets compiler flags.
        if test "$BITS_CHECK_64" = "yes"; then
            AC_MSG_CHECKING([how to build 64-bit executables])
            CAN_BUILD_64=no
            ####
            # Find out if we think we can *build* for 64 bit. Doesn't check whether we can run it.
            #  Note, we don't have to actually check if the options work- we'll try them before using them.
            #  So, only try actually testing the options, if you are trying to decide between multiple options.
            # On exit from the following clauses:
            # if CAN_BUILD_64=yes:
            #    *FLAGS are assumed to contain the right settings for 64bit
            # else if CAN_BUILD_64=no: (default)
            #    *FLAGS are assumed to be trashed, and will be reset from *FLAGS_OLD
            
            if test "$GCC" = yes; then
                CFLAGS="${CFLAGS} -m64"
                CXXFLAGS="${CXXFLAGS} -m64"
                AC_COMPILE_IFELSE([AC_LANG_SOURCE([int main(void) {return (sizeof(void*)*8==64)?0:1;}])],
                   CAN_BUILD_64=yes, CAN_BUILD_64=no)
            else
                case "${host}" in
                sparc*-*-solaris*)
                    # 1. try -m64
                    CFLAGS="${CFLAGS} -m64"
                    CXXFLAGS="${CXXFLAGS} -m64"
                    AC_RUN_IFELSE([AC_LANG_SOURCE([int main(void) {return (sizeof(void*)*8==64)?0:1;}])],
                       CAN_BUILD_64=yes, CAN_BUILD_64=no, CAN_BUILD_64=unknown)
                    if test "$CAN_BUILD_64" != yes; then
                        # Nope. back out changes.
                        CFLAGS="${CFLAGS_OLD}"
                        CXXFLAGS="${CXXFLAGS_OLD}"
                        # 2. try xarch=v9 [deprecated]
                        ## TODO: cross compile: the following won't work.
                        SPARCV9=`isainfo -n 2>&1 | grep sparcv9`
                        SOL64=`$CXX -xarch=v9 2>&1 && $CC -xarch=v9 2>&1 | grep -v usage:`
                        # "Warning: -xarch=v9 is deprecated, use -m64 to create 64-bit programs"
                        if test -z "$SOL64" && test -n "$SPARCV9"; then
                            CFLAGS="${CFLAGS} -xtarget=ultra -xarch=v9"
                            CXXFLAGS="${CXXFLAGS} -xtarget=ultra -xarch=v9"
                            LDFLAGS="${LDFLAGS} -xtarget=ultra -xarch=v9"
                            CAN_BUILD_64=yes
                        fi
                    fi
                    ;;
                i386-*-solaris*)
                    # 1. try -m64
                    CFLAGS="${CFLAGS} -m64"
                    CXXFLAGS="${CXXFLAGS} -m64"
                    AC_RUN_IFELSE([AC_LANG_SOURCE([int main(void) {return (sizeof(void*)*8==64)?0:1;}])],
                       CAN_BUILD_64=yes, CAN_BUILD_64=no, CAN_BUILD_64=unknown)
                    if test "$CAN_BUILD_64" != yes; then
                        # Nope. back out changes.
                        CFLAGS="${CFLAGS_OLD}"
                        CXXFLAGS="${CXXFLAGS_OLD}"
                        # 2. try the older compiler option
                        ## TODO: cross compile problem
                        AMD64=`isainfo -n 2>&1 | grep amd64`
                        SOL64=`$CXX -xtarget=generic64 2>&1 && $CC -xtarget=generic64 2>&1 | grep -v usage:`
                        if test -z "$SOL64" && test -n "$AMD64"; then
                            CFLAGS="${CFLAGS} -xtarget=generic64"
                            CXXFLAGS="${CXXFLAGS} -xtarget=generic64"
                            CAN_BUILD_64=yes
                        fi
                    fi
                    ;;
                ia64-*-linux*)
                    # check for ecc/ecpc compiler support
                    ## TODO: cross compiler problem
                    if test -n "`$CXX --help 2>&1 && $CC --help 2>&1 | grep -v Intel`"; then
                        if test -n "`$CXX --help 2>&1 && $CC --help 2>&1 | grep -v Itanium`"; then
                            CAN_BUILD_64=yes
                        fi
                    fi
                    ;;
                *-*-cygwin)
                    # vcvarsamd64.bat should have been used to enable 64-bit builds.
                    # We only do this check to display the correct answer.
                    ## TODO: cross compiler problem
                    if test -n "`$CXX -help 2>&1 | grep 'for x64'`"; then
                        CAN_BUILD_64=yes
                    fi
                    ;;
                *-*-aix*|powerpc64-*-linux*)
                    CFLAGS="${CFLAGS} -q64"
                    CXXFLAGS="${CXXFLAGS} -q64"
                    LDFLAGS="${LDFLAGS} -q64"
                    AC_COMPILE_IFELSE([AC_LANG_SOURCE([int main(void) {return (sizeof(void*)*8==64)?0:1;}])],
                       CAN_BUILD_64=yes, CAN_BUILD_64=no)
                    if test "$CAN_BUILD_64" = yes; then
                        # worked- set other options.
                        case "${host}" in
                        *-*-aix*)
                            # tell AIX what executable mode to use.
                            ARFLAGS="${ARFLAGS} -X64"
                        esac
                    fi
                    ;;
                *-*-hpux*)
                    # First we try the newer +DD64, if that doesn't work,
                    # try other options.

                    CFLAGS="${CFLAGS} +DD64"
                    CXXFLAGS="${CXXFLAGS} +DD64"
                    AC_COMPILE_IFELSE([AC_LANG_SOURCE([int main(void) {return (sizeof(void*)*8==64)?0:1;}])],
                        CAN_BUILD_64=yes, CAN_BUILD_64=no)
                    if test "$CAN_BUILD_64" != yes; then
                        # reset
                        CFLAGS="${CFLAGS_OLD}"
                        CXXFLAGS="${CXXFLAGS_OLD}"
                        # append
                        CFLAGS="${CFLAGS} +DA2.0W"
                        CXXFLAGS="${CXXFLAGS} +DA2.0W"
                        AC_COMPILE_IFELSE([AC_LANG_SOURCE([int main(void) {return (sizeof(void*)*8==64)?0:1;}])],
                            CAN_BUILD_64=yes, CAN_BUILD_64=no)
                    fi
                    ;;
                *-*ibm-openedition*|*-*-os390*)
                    CFLAGS="${CFLAGS} -Wc,lp64"
                    CXXFLAGS="${CXXFLAGS} -Wc,lp64"
                    LDFLAGS="${LDFLAGS} -Wl,lp64"
                    AC_COMPILE_IFELSE([AC_LANG_SOURCE([int main(void) {return (sizeof(void*)*8==64)?0:1;}])],
                       CAN_BUILD_64=yes, CAN_BUILD_64=no)
                    ;;
                *)
                    # unknown platform.
                    ;;
                esac
            fi
            AC_MSG_RESULT($CAN_BUILD_64)
            if test "$CAN_BUILD_64" = yes; then
                AC_MSG_CHECKING([whether runnable 64-bit binaries are being built ])
                AC_RUN_IFELSE([AC_LANG_SOURCE([int main(void) {return (sizeof(void*)*8==64)?0:1;}])],
                   BITS_RUN_64=yes, BITS_RUN_64=no, BITS_RUN_64=unknown)
                AC_MSG_RESULT($BITS_RUN_64);

                CFLAGS_64="${CFLAGS}"
                CXXFLAGS_64="${CXXFLAGS}"
                LDFLAGS_64="${LDFLAGS}"
                ARFLAGS_64="${ARFLAGS}"        
            fi
            # put it back.
            CFLAGS="${CFLAGS_OLD}"
            CXXFLAGS="${CXXFLAGS_OLD}"
            LDFLAGS="${LDFLAGS_OLD}"
            ARFLAGS="${ARFLAGS_OLD}"     
        fi
        if test "$BITS_CHECK_32" = "yes"; then
            # see comment under 'if BITS_CHECK_64', above.
            AC_MSG_CHECKING([how to build 32-bit executables])
            if test "$GCC" = yes; then
                CFLAGS="${CFLAGS} -m32"
                CXXFLAGS="${CXXFLAGS} -m32"
                AC_COMPILE_IFELSE([AC_LANG_SOURCE([int main(void) {return (sizeof(void*)*8==32)?0:1;}])],
                   CAN_BUILD_32=yes, CAN_BUILD_32=no)
            fi
            AC_MSG_RESULT($CAN_BUILD_32)
            if test "$CAN_BUILD_32" = yes; then
                AC_MSG_CHECKING([whether runnable 32-bit binaries are being built ])
                AC_RUN_IFELSE([AC_LANG_SOURCE([int main(void) {return (sizeof(void*)*8==32)?0:1;}])],
                   BITS_RUN_32=yes, BITS_RUN_32=no, BITS_RUN_32=unknown)
                AC_MSG_RESULT($BITS_RUN_32);
                CFLAGS_32="${CFLAGS}"
                CXXFLAGS_32="${CXXFLAGS}"
                LDFLAGS_32="${LDFLAGS}"
                ARFLAGS_32="${ARFLAGS}"        
            fi
            # put it back.
            CFLAGS="${CFLAGS_OLD}"
            CXXFLAGS="${CXXFLAGS_OLD}"
            LDFLAGS="${LDFLAGS_OLD}"
            ARFLAGS="${ARFLAGS_OLD}"     
        fi
        
        ##
        # OK. Now, we've tested for 32 and 64 bitness. Let's see what we'll do.
        #
        
        # First, implement 64else32
        if test "$BITS_REQ" = "64else32"; then
            if test "$BITS_RUN_64" = "yes"; then
                BITS_REQ=64
            else
                # no changes.
                BITS_OK=yes 
            fi
        fi
        
        # implement.
        if test "$BITS_REQ" = "32" -a "$BITS_RUN_32" = "yes"; then
            CFLAGS="${CFLAGS_32}"
            CXXFLAGS="${CXXFLAGS_32}"
            LDFLAGS="${LDFLAGS_32}"
            ARFLAGS="${ARFLAGS_32}"     
            BITS_OK=yes
        elif test "$BITS_REQ" = "64" -a "$BITS_RUN_64" = "yes"; then
            CFLAGS="${CFLAGS_64}"
            CXXFLAGS="${CXXFLAGS_64}"
            LDFLAGS="${LDFLAGS_64}"
            ARFLAGS="${ARFLAGS_64}"     
            BITS_OK=yes
        elif test "$BITS_OK" != "yes"; then
            AC_MSG_ERROR([Requested $BITS_REQ bit binaries but could not compile and execute them. See readme.html for help with cross compilation., and set compiler options manually.])
        fi
     fi
])

# Strict compilation options.
AC_DEFUN([AC_CHECK_STRICT_COMPILE],
[
    AC_MSG_CHECKING([whether strict compiling is on])
    AC_ARG_ENABLE(strict,[  --enable-strict         compile with strict compiler options [default=yes]], [
        if test "$enableval" = no
        then
            ac_use_strict_options=no
        else
            ac_use_strict_options=yes
        fi
      ], [ac_use_strict_options=yes])
    AC_MSG_RESULT($ac_use_strict_options)

    if test "$ac_use_strict_options" = yes
    then
        if test "$GCC" = yes
        then
            case "${host}" in
            *-*-solaris*)
                # Don't use -std=c11 on Solaris because of timezone check fails
                ;;
            *)
                # Do not use -ansi. It limits us to C90, and it breaks some platforms.
                # We use -std=c11 to disable the gnu99 defaults and its associated warnings
                CFLAGS="$CFLAGS -std=c11"
                ;;
            esac
            
            CFLAGS="$CFLAGS -Wall -pedantic -Wshadow -Wpointer-arith -Wmissing-prototypes -Wwrite-strings"

            # Suppress clang C warnings:
            CFLAGS="$CFLAGS -Wno-sign-compare -Wno-unused"
        else
            case "${host}" in
            *-*-cygwin)
                if test "`$CC /help 2>&1 | head -c9`" = "Microsoft"
                then
                    CFLAGS="$CFLAGS /W4"
                fi ;;
            *-*-mingw*)
                CFLAGS="$CFLAGS -W4" ;;
            esac
        fi
        if test "$GXX" = yes
        then
            CXXFLAGS="$CXXFLAGS -W -Wall -pedantic -Wpointer-arith -Wwrite-strings -Wno-long-long"

            # Suppress clang C++ warnings:
            CXXFLAGS="$CXXFLAGS -Wno-unused -Wno-unused-parameter"
        else
            case "${host}" in
            *-*-cygwin)
                if test "`$CXX /help 2>&1 | head -c9`" = "Microsoft"
                then
                    CXXFLAGS="$CXXFLAGS /W4"
                fi ;;
            *-*-mingw*)
                CFLAGS="$CFLAGS -W4" ;;
            esac
        fi
    fi
])



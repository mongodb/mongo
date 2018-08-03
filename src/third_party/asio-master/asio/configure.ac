AC_INIT(asio, [1.12.0])
AC_CONFIG_SRCDIR(include/asio.hpp)
AM_MAINTAINER_MODE
AM_INIT_AUTOMAKE([tar-ustar])

AC_CANONICAL_HOST
AM_PROG_CC_C_O
AC_PROG_CXX
AC_LANG(C++)
AC_PROG_RANLIB

AC_DEFINE(_REENTRANT, [1], [Define this])

AC_ARG_WITH(boost,
  AC_HELP_STRING([--with-boost=DIR],[location of boost distribution]),
[
  if test "${withval}" = no; then
    STANDALONE="yes"
  else
    CPPFLAGS="$CPPFLAGS -I${withval} -DBOOST_CHRONO_HEADER_ONLY -DBOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING"
    LIBS="$LIBS -L${withval}/stage/lib"
  fi
],
[
  BOOSTDIR=`ls -1d ../boost_*_*_*/ 2>/dev/null | sort -t "_" -k 2nr -k 3nr -k 4nr | head -n 1 | sed -e 's/\/$//'`
  if test "${BOOSTDIR}" != ""; then
    BOOSTDIR="`pwd`/${BOOSTDIR}"
    if test -d "${BOOSTDIR}"; then
      echo "using automatically detected boost from ${BOOSTDIR}"
      CPPFLAGS="$CPPFLAGS -I${BOOSTDIR} -DBOOST_CHRONO_HEADER_ONLY -DBOOST_CHRONO_DONT_PROVIDE_HYBRID_ERROR_HANDLING"
      LIBS="$LIBS -L${BOOSTDIR}/stage/lib"
    fi
  fi
])

AC_ARG_ENABLE(separate-compilation,
[  --enable-separate-compilation  separate compilation of asio source],
[
  SEPARATE_COMPILATION=yes
])

AC_ARG_ENABLE(boost-coroutine,
[  --enable-boost-coroutine  use Boost.Coroutine to implement stackful coroutines],
[
  HAVE_BOOST_COROUTINE=yes
])

if test "$STANDALONE" != yes; then
  AC_CHECK_HEADER([boost/noncopyable.hpp],,
  [
    echo "Can't find boost headers. Please check the location of the boost"
    echo "distribution and rerun configure using the --with-boost=DIR option."
    echo "Alternatively, run with --without-boost to enable standalone build."
    exit 1
  ],[])
fi

AC_ARG_WITH(openssl,
  AC_HELP_STRING([--with-openssl=DIR],[location of openssl]),
[
  CPPFLAGS="$CPPFLAGS -I${withval}/include"
  LIBS="$LIBS -L${withval}/lib"
],[])

AC_CHECK_HEADER([openssl/ssl.h],,
[
  OPENSSL_FOUND=no
],[])

if test x$OPENSSL_FOUND != xno; then
  LIBS="$LIBS -lssl -lcrypto"
fi

AM_CONDITIONAL(HAVE_OPENSSL,test x$OPENSSL_FOUND != xno)

WINDOWS=no
case $host in
  *-*-linux*)
    CXXFLAGS="$CXXFLAGS -pthread"
    LDFLAGS="$LDFLAGS -pthread"
    LIBS="$LIBS -lrt"
    ;;
  *-*-solaris*)
    if test "$GXX" = yes; then
      CXXFLAGS="$CXXFLAGS -D_PTHREADS"
    else
      # We'll assume Sun's CC.
      CXXFLAGS="$CXXFLAGS -mt"
    fi
    LIBS="$LIBS -lsocket -lnsl -lpthread"
    ;;
  *-*-mingw32*)
    CXXFLAGS="$CXXFLAGS -mthreads"
    LDFLAGS="$LDFLAGS -mthreads"
    LIBS="$LIBS -lws2_32 -lmswsock"
    WINDOWS=yes
    ;;
  *-*-mingw64*)
    CXXFLAGS="$CXXFLAGS -mthreads"
    LDFLAGS="$LDFLAGS -mthreads"
    LIBS="$LIBS -lws2_32 -lmswsock"
    WINDOWS=yes
    ;;
  *-pc-cygwin*)
    CXXFLAGS="$CXXFLAGS -D__USE_W32_SOCKETS -D_WIN32_WINNT=0x0501"
    LIBS="$LIBS -lws2_32 -lmswsock"
    WINDOWS=yes
    ;;
  *-apple-darwin*)
    CXXFLAGS="$CXXFLAGS"
    LDFLAGS="$LDFLAGS"
    ;;
  *-*-freebsd*)
    CXXFLAGS="$CXXFLAGS -pthread"
    LDFLAGS="$LDFLAGS -pthread"
    ;;
  *-*-netbsd*)
    CXXFLAGS="$CXXFLAGS -pthread"
    LDFLAGS="$LDFLAGS -pthread"
    ;;
esac

if test "$GXX" = yes; then
  CXXFLAGS="$CXXFLAGS -ftemplate-depth-256"
fi

if test "$STANDALONE" = yes; then
  CPPFLAGS="$CPPFLAGS -DASIO_STANDALONE"
fi

if test "$SEPARATE_COMPILATION" = yes; then
  CPPFLAGS="$CPPFLAGS -DASIO_SEPARATE_COMPILATION"
fi

AC_MSG_CHECKING([whether C++11 is enabled])
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM(
    [[#if __cplusplus < 201103L]]
    [[#error C++11 not available]]
    [[#endif]])],
  [AC_MSG_RESULT([yes])
    HAVE_CXX11=yes;],
  [AC_MSG_RESULT([no])
    HAVE_CXX11=no;])

AC_MSG_CHECKING([whether C++14 is enabled])
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM(
    [[#if __cplusplus < 201402L]]
    [[#error C++14 not available]]
    [[#endif]])],
  [AC_MSG_RESULT([yes])
    HAVE_CXX14=yes;],
  [AC_MSG_RESULT([no])
    HAVE_CXX14=no;])

if test "$GXX" = yes; then
  if test "$STANDALONE" = yes; then
    if test "$HAVE_CXX11" = no; then
      HAVE_CXX11=yes
      CPPFLAGS="-std=c++0x $CPPFLAGS"
    fi
  fi
fi

AM_CONDITIONAL(STANDALONE,test x$STANDALONE = xyes)

AM_CONDITIONAL(SEPARATE_COMPILATION,test x$SEPARATE_COMPILATION = xyes)

AM_CONDITIONAL(HAVE_BOOST_COROUTINE,test x$HAVE_BOOST_COROUTINE = xyes)

AM_CONDITIONAL(WINDOWS_TARGET,test x$WINDOWS != xno)

AM_CONDITIONAL(HAVE_CXX11,test x$HAVE_CXX11 = xyes)

AM_CONDITIONAL(HAVE_CXX14,test x$HAVE_CXX14 = xyes)

AC_OUTPUT([
  Makefile
  include/Makefile
  src/Makefile
  src/tests/Makefile
  src/examples/cpp03/Makefile
  src/examples/cpp11/Makefile
  src/examples/cpp14/Makefile])

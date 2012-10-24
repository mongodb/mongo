/* src/config.h.  Generated from config.h.in by configure.  */
/* src/config.h.in.  Generated from configure.ac by autoheader.  */

/* Always the empty-string on non-windows systems. On windows, should be
   "__declspec(dllexport)". This way, when we compile the dll, we export our
   functions/classes. It's safe to define this here because config.h is only
   used internally, to compile the DLL, and every DLL source file #includes
   "config.h" before anything else. */
#define GFLAGS_DLL_DECL /**/

/* Namespace for Google classes */
#define GOOGLE_NAMESPACE ::google

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Define to 1 if you have the <fnmatch.h> header file. */
#ifndef _WIN32
#define HAVE_FNMATCH_H 1
#endif

/* Define to 1 if you have the <inttypes.h> header file. */
#ifndef _WIN32
#define HAVE_INTTYPES_H 1
#endif

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* define if the compiler implements namespaces */
#define HAVE_NAMESPACES 1

/* Define if you have POSIX threads libraries and header files. */
#define HAVE_PTHREAD 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the `strtoll' function. */
#define HAVE_STRTOLL 1

/* Define to 1 if you have the `strtoq' function. */
#define HAVE_STRTOQ 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* define if your compiler has __attribute__ */
#ifndef _WIN32
#define HAVE___ATTRIBUTE__ 1
#else
#define HAVE___ATTRIBUTE__ 0
#endif

/* Define to the sub-directory in which libtool stores uninstalled libraries.
   */
#define LT_OBJDIR ".libs/"

/* Name of package */
#define PACKAGE "gflags"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "google-gflags@googlegroups.com"

/* Define to the full name of this package. */
#define PACKAGE_NAME "gflags"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "gflags 2.0"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "gflags"

/* Define to the version of this package. */
#define PACKAGE_VERSION "2.0"

/* Define to necessary symbol if this constant uses a non-standard name on
   your system. */
/* #undef PTHREAD_CREATE_JOINABLE */

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* the namespace where STL code like vector<> is defined */
#define STL_NAMESPACE std

/* Version number of package */
#define VERSION "2.0"

/* Stops putting the code inside the Google namespace */
#define _END_GOOGLE_NAMESPACE_ }

/* Puts following code inside the Google namespace */
#define _START_GOOGLE_NAMESPACE_ namespace google {


#if defined( __MINGW32__) || defined(__MINGW64__) || defined (_MSC_VER)
#include "windows/port.h"
#endif


/* src/config.h.in.  Generated from configure.ac by autoheader.  */

/* Sometimes we accidentally #include this config.h instead of the one
   in .. -- this is particularly true for msys/mingw, which uses the
   unix config.h but also runs code in the windows directory.
   */
#ifdef __MINGW32__
#include "../config.h"
#define GOOGLE_GFLAGS_WINDOWS_CONFIG_H_
#endif

#ifndef GOOGLE_GFLAGS_WINDOWS_CONFIG_H_
#define GOOGLE_GFLAGS_WINDOWS_CONFIG_H_

/* Always the empty-string on non-windows systems. On windows, should be
   "__declspec(dllexport)". This way, when we compile the dll, we export our
   functions/classes. It's safe to define this here because config.h is only
   used internally, to compile the DLL, and every DLL source file #includes
   "config.h" before anything else. */
#ifndef GFLAGS_DLL_DECL
# define GFLAGS_IS_A_DLL  1   /* not set if you're statically linking */
# define GFLAGS_DLL_DECL  __declspec(dllexport)
# define GFLAGS_DLL_DECL_FOR_UNITTESTS  __declspec(dllimport)
#endif

/* Namespace for Google classes */
#define GOOGLE_NAMESPACE  ::google

/* Define to 1 if you have the <dlfcn.h> header file. */
#undef HAVE_DLFCN_H

/* Define to 1 if you have the <fnmatch.h> header file. */
#undef HAVE_FNMATCH_H

/* Define to 1 if you have the <inttypes.h> header file. */
#undef HAVE_INTTYPES_H

/* Define to 1 if you have the <memory.h> header file. */
#undef HAVE_MEMORY_H

/* define if the compiler implements namespaces */
#define HAVE_NAMESPACES  1

/* Define if you have POSIX threads libraries and header files. */
#undef HAVE_PTHREAD

/* Define to 1 if you have the `putenv' function. */
#define HAVE_PUTENV  1

/* Define to 1 if you have the `setenv' function. */
#undef HAVE_SETENV

/* Define to 1 if you have the <stdint.h> header file. */
#undef HAVE_STDINT_H

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#undef HAVE_STRINGS_H

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the `strtoll' function. */
#define HAVE_STRTOLL  1

/* Define to 1 if you have the `strtoq' function. */
#define HAVE_STRTOQ  1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#undef HAVE_UNISTD_H

/* define if your compiler has __attribute__ */
#undef HAVE___ATTRIBUTE__

/* Define to the sub-directory in which libtool stores uninstalled libraries.
   */
#undef LT_OBJDIR

/* Name of package */
#undef PACKAGE

/* Define to the address where bug reports for this package should be sent. */
#undef PACKAGE_BUGREPORT

/* Define to the full name of this package. */
#undef PACKAGE_NAME

/* Define to the full name and version of this package. */
#undef PACKAGE_STRING

/* Define to the one symbol short name of this package. */
#undef PACKAGE_TARNAME

/* Define to the home page for this package. */
#undef PACKAGE_URL

/* Define to the version of this package. */
#undef PACKAGE_VERSION

/* Define to necessary symbol if this constant uses a non-standard name on
   your system. */
#undef PTHREAD_CREATE_JOINABLE

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS  1

/* the namespace where STL code like vector<> is defined */
#define STL_NAMESPACE  std

/* Version number of package */
#undef VERSION

/* Stops putting the code inside the Google namespace */
#define _END_GOOGLE_NAMESPACE_  }

/* Puts following code inside the Google namespace */
#define _START_GOOGLE_NAMESPACE_  namespace google {

// ---------------------------------------------------------------------
// Extra stuff not found in config.h.in

// This must be defined before the windows.h is included.  It's needed
// for mutex.h, to give access to the TryLock method.
#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0400
#endif

// TODO(csilvers): include windows/port.h in every relevant source file instead?
#include "windows/port.h"

#endif  /* GOOGLE_GFLAGS_WINDOWS_CONFIG_H_ */

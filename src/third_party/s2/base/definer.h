#ifndef BASE_DEFINER_H
#define BASE_DEFINER_H

#if defined __APPLE__ && defined __MACH__
#  define OS_MACOSX
#  define HASH_NAMESPACE __gnu_cxx
#elif defined __linux__
#  define _GLIBCXX_PERMIT_BACKWARD_HASH
#  define OS_LINUX
#  define HASH_NAMESPACE __gnu_cxx
#elif defined _WIN32
#  define OS_WINDOWS
#  define HASH_NAMESPACE std
#  define _USE_MATH_DEFINES
#endif

#endif  // BASE_DEFINER_H

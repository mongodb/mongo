#ifndef BASE_DEFINER_H
#define BASE_DEFINER_H

#if defined __APPLE__ && defined __MACH__
#  define OS_MACOSX
#elif defined __linux__
#  define OS_LINUX
#elif defined _WIN32
#  define OS_WINDOWS
#elif defined __FreeBSD__ || defined __OpenBSD__
#  define OS_FREEBSD
#endif

#endif  // BASE_DEFINER_H

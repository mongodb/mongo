// windows_basic.h

#pragma once

#if defined(_WIN32)
// for rand_s() usage:
# define _CRT_RAND_S
# ifndef NOMINMAX
#  define NOMINMAX
# endif
// tell windows.h not to include a bunch of headers
// we don't need:
# define WIN32_LEAN_AND_MEAN
# include "targetver.h"
# include <winsock2.h> //this must be included before the first windows.h include
# include <ws2tcpip.h>
# include <wspiapi.h>
# include <windows.h>
#endif

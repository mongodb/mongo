// windows_basic.h

/*
 *    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#if defined(_WIN32)
// for rand_s() usage:
# define _CRT_RAND_S
# ifndef NOMINMAX
#  define NOMINMAX
# endif
// tell windows.h not to include a bunch of headers we don't need:
# define WIN32_LEAN_AND_MEAN
# include "mongo/targetver.h"
# include <winsock2.h> //this must be included before the first windows.h include
# include <ws2tcpip.h>
# include <wspiapi.h>
# include <windows.h>
#endif

// basic.h

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

#ifdef _WIN32
#include "windows_basic.h"
#endif

#if defined(__linux__)

#include <cstring>

// glibc's optimized versions are better than g++ builtins
# define __builtin_strcmp strcmp
# define __builtin_strlen strlen
# define __builtin_memchr memchr
# define __builtin_memcmp memcmp
# define __builtin_memcpy memcpy
# define __builtin_memset memset
# define __builtin_memmove memmove
#endif


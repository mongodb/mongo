   /** @file pch.h : include file for standard system include files,
 *  or project specific include files that are used frequently, but
 *  are changed infrequently
 */

/*    Copyright 2009 10gen Inc.
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

#ifndef MONGO_PCH_H
#define MONGO_PCH_H

// our #define macros must not be active when we include
// system headers and boost headers
#include "mongo/client/undef_macros.h"

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

#if defined(__linux__)
// glibc's optimized versions are better than g++ builtins
# define __builtin_strcmp strcmp
# define __builtin_strlen strlen
# define __builtin_memchr memchr
# define __builtin_memcmp memcmp
# define __builtin_memcpy memcpy
# define __builtin_memset memset
# define __builtin_memmove memmove
#endif

#include <ctime>
#include <cstring>
#include <string>
#include <memory>
#include <string>
#include <iostream>
#include <map>
#include <vector>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include "time.h"
#include "string.h"
#include "limits.h"

#define BOOST_FILESYSTEM_VERSION 2
#include <boost/shared_ptr.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/thread/tss.hpp>
#include <boost/detail/endian.hpp>
#include <boost/version.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/condition.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/xtime.hpp>


#include "mongo/client/redef_macros.h"

#include "mongo/util/exit_code.h"

namespace mongo {

    using namespace std;
    using boost::shared_ptr;

    void dbexit( ExitCode returnCode, const char *whyMsg = "", bool tryToGetLock = false);

    /**
       this is here so you can't just type exit() to quit the program
       you should either use dbexit to shutdown cleanly, or ::exit to tell the system to quit
       if you use this, you'll get a link error since mongo::exit isn't defined
     */
    void exit( ExitCode returnCode );
    bool inShutdown();

}


#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/goodies.h"
#include "mongo/util/allocator.h"
#include "mongo/util/log.h"

#endif // MONGO_PCH_H

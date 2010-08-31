// pch.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

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

#if defined(MONGO_EXPOSE_MACROS)
# define JS_C_STRINGS_ARE_UTF8
# undef  SUPPORT_UCP
# define SUPPORT_UCP
# undef  SUPPORT_UTF8
# define SUPPORT_UTF8
# undef  _CRT_SECURE_NO_WARNINGS
# define _CRT_SECURE_NO_WARNINGS
#endif

#if defined(WIN32)

#ifndef _WIN32
#define _WIN32
#endif

#endif

#if defined(_WIN32)
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <winsock2.h> //this must be included before the first windows.h include
# include <ws2tcpip.h>
# include <wspiapi.h>
# include <windows.h>
#endif

#include <ctime>
#include <sstream>
#include <string>
#include <memory>
#include <string>
#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <sstream>
#include <signal.h>
#include "targetver.h"
#include "time.h"
#include "string.h"
#include "limits.h"

#include <boost/any.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/function.hpp>
#include "boost/bind.hpp"
#include "boost/function.hpp"
#include <boost/thread/tss.hpp>
#include "boost/detail/endian.hpp"
#define BOOST_SPIRIT_THREADSAFE
#include <boost/version.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/condition.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/xtime.hpp>
#undef assert
#define assert MONGO_assert

namespace mongo {

    using namespace std;
    using boost::shared_ptr;

#if defined(_DEBUG)
    const bool debug=true;
#else
    const bool debug=false;
#endif

    // pdfile versions
    const int VERSION = 4;
    const int VERSION_MINOR = 5;

    enum ExitCode {
        EXIT_CLEAN = 0 , 
        EXIT_BADOPTIONS = 2 , 
        EXIT_REPLICATION_ERROR = 3 ,
        EXIT_NEED_UPGRADE = 4 ,
        EXIT_KILL = 12 ,
        EXIT_ABRUPT = 14 ,
        EXIT_NTSERVICE_ERROR = 20 ,
        EXIT_JAVA = 21 ,
        EXIT_OOM_MALLOC = 42 , 
        EXIT_OOM_REALLOC = 43 , 
        EXIT_FS = 45 ,
        EXIT_CLOCK_SKEW = 47 ,
        EXIT_NET_ERROR = 48 ,
        EXIT_POSSIBLE_CORRUPTION = 60 , // this means we detected a possible corruption situation, like a buf overflow
        EXIT_UNCAUGHT = 100 , // top level exception that wasn't caught
        EXIT_TEST = 101 ,

    };

    void dbexit( ExitCode returnCode, const char *whyMsg = "", bool tryToGetLock = false);

    /**
       this is here so you can't just type exit() to quit the program
       you should either use dbexit to shutdown cleanly, or ::exit to tell the system to quit
       if you use this, you'll get a link error since mongo::exit isn't defined
     */
    void exit( ExitCode returnCode );
    bool inShutdown();
    
} // namespace mongo

namespace mongo {
    using namespace boost::filesystem;
    void asserted(const char *msg, const char *file, unsigned line);
}

#define MONGO_assert(_Expression) (void)( (!!(_Expression)) || (mongo::asserted(#_Expression, __FILE__, __LINE__), 0) )

#include "util/debug_util.h"
#include "util/goodies.h"
#include "util/log.h"
#include "util/allocator.h"
#include "util/assert_util.h"

namespace mongo {

    void sayDbContext(const char *msg = 0);
    void rawOut( const string &s );

} // namespace mongo

namespace mongo {

    typedef char _TCHAR;

    using boost::uint32_t;
    using boost::uint64_t;

} // namespace mongo

#endif // MONGO_PCH_H

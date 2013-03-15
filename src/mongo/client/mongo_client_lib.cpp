/* @file client_lib.cpp */

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

/*

   MongoDB C++ Driver

   Normally one includes dbclient.h, and links against libmongoclient.a, when connecting to MongoDB
   from C++.  However, if you have a situation where the pre-built library does not work, you can use
   this file instead to build all the necessary symbols.  To do so, include mongo_client_lib.cpp in your
   project.

   GCC
   ---
   For example, to build and run simple_client_demo.cpp with GCC and run it:

    g++ -I .. simple_client_demo.cpp mongo_client_lib.cpp -lboost_thread-mt -lboost_filesystem
    ./a.out

   Visual Studio (2010 tested)
   ---------------------------
   First, see client/examples/simple_client_demo.vcxproj.
   - Be sure to include your boost include directory in your project as an Additional Include Directory.
   - Define  _CRT_SECURE_NO_WARNINGS to avoid warnings on use of strncpy and such by the MongoDB client code.
   - Include the boost libraries directory.
   - Linker.Input.Additional Dependencies - add ws2_32.lib for the Winsock library.
*/

#if defined(_WIN32)
// C4800 forcing value to bool 'true' or 'false' (performance warning)
#pragma warning( disable : 4800 )
#endif

#include "../util/md5main.cpp"

#define MONGO_EXPOSE_MACROS

#include "../pch.h"

#include "../util/assert_util.cpp"
#include "../util/net/message.cpp"
#include "../util/util.cpp"
#include "../util/background.cpp"
#include "../util/base64.cpp"
#include "../util/net/sock.cpp"
#include "../util/log.cpp"
#include "../util/password.cpp"
#include "../util/net/message_port.cpp"
#include "../util/concurrency/thread_pool.cpp"
#include "../util/concurrency/task.cpp"
#include "../util/concurrency/spin_lock.cpp"

// in client/ directory:
#include "authentication_table.cpp"
#include "connpool.cpp"
#include "syncclusterconnection.cpp"
#include "dbclient.cpp"
#include "clientOnly.cpp"
#include "gridfs.cpp"
#include "dbclientcursor.cpp"
#include "dbclient_rs.cpp"

#include "../util/text.cpp"
#include "../bson/oid.cpp"
#include "../db/lasterror.cpp"
#include "../db/json.cpp"
#include "../db/jsobj.cpp"
#include "../db/dbmessage.cpp"

#include "../pch.cpp"

extern "C" {
#include "../util/md5.cpp"
}

// in client/ directory:
#include "clientAndShell.cpp"

#include "../util/time_support.cpp"
#include "../util/timer.cpp"
#include "../util/concurrency/mutexdebugger.cpp"
#include "../util/stringutils.cpp"

/* these should probably be in clientOnly.cpp. however as a first step putting here so that there 
   is no risk we break the LIB build of cpp client.  so this is interim.
   */
namespace mongo {
    void printStackTrace(class std::basic_ostream<char,struct std::char_traits<char> > &) { }
    void mongo_breakpoint() { }
    void printStackAndExit( int signalNum ) {
        ::_exit( EXIT_ABRUPT );
    }
}

/* @file client_lib.cpp

   MongoDB C++ Driver

   Normally one includes dbclient.h, and links against libmongoclient.a, when connecting to MongoDB
   from C++.  However, if you have a situation where the pre-built library does not work, you can use
   this file instead to build all the necessary symbols.  To do so, include client_lib.cpp in your
   project.

   For example, to build and run simple_client_demo.cpp with GCC and run it:

    g++ -I .. simple_client_demo.cpp mongo_client_lib.cpp -lboost_thread-mt -lboost_filesystem
    ./a.out
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

#include "../util/md5main.cpp"

#define MONGO_EXPOSE_MACROS
#include "../pch.h"

#include "../util/assert_util.cpp"
#include "../util/message.cpp"
#include "../util/util.cpp"
#include "../util/background.cpp"
#include "../util/base64.cpp"
#include "../util/sock.cpp"
#include "../util/log.cpp"
#include "../util/password.cpp"

#include "../util/concurrency/thread_pool.cpp"
#include "../util/concurrency/vars.cpp"
#include "../util/concurrency/task.cpp"

#include "connpool.cpp"
#include "syncclusterconnection.cpp"
#include "dbclient.cpp"
#include "clientOnly.cpp"
#include "gridfs.cpp"
#include "dbclientcursor.cpp"

#include "../util/text.cpp"
#include "dbclient_rs.cpp"
#include "../bson/oid.cpp"

#include "../db/lasterror.cpp"
#include "../db/json.cpp"
#include "../db/jsobj.cpp"
#include "../db/common.cpp"
#include "../db/nonce.cpp"
#include "../db/commands.cpp"

extern "C" {
#include "../util/md5.c"
}


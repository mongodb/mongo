// pch.cpp : helper for using precompiled headers

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

#include "pch.h"

#ifndef JSTIME_VIRTUAL_SKEW
#define JSTIME_VIRTUAL_SKEW

namespace mongo {
    // jsTime_virtual_skew is just for testing. a test command manipulates it.
    long long jsTime_virtual_skew = 0;
    boost::thread_specific_ptr<long long> jsTime_virtual_thread_skew;
}

#endif

#if defined( __MSVC__ )
// should probably check VS version here
#elif defined( __GNUC__ )

#if __GNUC__ < 4
#error gcc < 4 not supported
#endif

#else
// unknown compiler
#endif

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

#include "mongo/platform/basic.h"

#include "mongo/platform/compiler.h"
#include "mongo/util/concurrency/thread_name.h"

#include <boost/thread/tss.hpp>

namespace mongo {

namespace {
    boost::thread_specific_ptr<std::string> _threadName;

#if defined(_WIN32)

#define MS_VC_EXCEPTION 0x406D1388
#pragma pack(push,8)
    typedef struct tagTHREADNAME_INFO {
        DWORD dwType; // Must be 0x1000.
        LPCSTR szName; // Pointer to name (in user addr space).
        DWORD dwThreadID; // Thread ID (-1=caller thread).
        DWORD dwFlags; // Reserved for future use, must be zero.
    } THREADNAME_INFO;
#pragma pack(pop)

    void setWinThreadName(const char *name) {
        /* is the sleep here necessary???
           Sleep(10);
           */
        THREADNAME_INFO info;
        info.dwType = 0x1000;
        info.szName = name;
        info.dwThreadID = -1;
        info.dwFlags = 0;
        __try {
            RaiseException( MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(ULONG_PTR), (ULONG_PTR*)&info );
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
        }
    }
#endif

}  // namespace

    void setThreadName(StringData name) {
        _threadName.reset(new string(name.rawData(), name.size()));

#if defined( DEBUG ) && defined( _WIN32 )
        // naming might be expensive so don't do "conn*" over and over
        setWinThreadName(_threadName.get()->c_str());
#endif

    }

    const std::string& getThreadName() {
        std::string* s;
        while (!(s = _threadName.get())) {
            setThreadName("");
        }
        return *s;
    }

}  // namespace mongo

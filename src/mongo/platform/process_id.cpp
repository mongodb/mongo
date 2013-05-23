/*    Copyright 2013 10gen Inc.
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

#include "mongo/platform/process_id.h"

#include <boost/static_assert.hpp>
#include <iostream>
#include <sstream>
#include <limits>

namespace mongo {

    BOOST_STATIC_ASSERT(sizeof(NativeProcessId) == sizeof(uint32_t));

    namespace {
#ifdef _WIN32
        inline NativeProcessId getCurrentNativeProcessId() { return GetCurrentProcessId(); }
#else
        inline NativeProcessId getCurrentNativeProcessId() { return getpid(); }
#endif
    }  // namespace

    ProcessId ProcessId::getCurrent() {
        return fromNative(getCurrentNativeProcessId());
    }

    int64_t ProcessId::asInt64() const {
        typedef std::numeric_limits<NativeProcessId> limits;
        if (limits::is_signed)
            return _npid;
        else
            return static_cast<int64_t>(static_cast<uint64_t>(_npid));
    }

    long long ProcessId::asLongLong() const {
        return static_cast<long long>(asInt64());
    }

    std::string ProcessId::toString() const {
        std::ostringstream os;
        os << *this;
        return os.str();
    }

    std::ostream& operator<<(std::ostream& os, ProcessId pid) {
        return os << pid.toNative();
    }

}  // namespace mongo

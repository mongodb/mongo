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

#pragma once

#include <iosfwd>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "mongo/platform/cstdint.h"
#include "mongo/platform/hash_namespace.h"

namespace mongo {

#ifdef _WIN32
    typedef DWORD NativeProcessId;
#else
    typedef pid_t NativeProcessId;
#endif

    /**
     * Platform-independent representation of a process identifier.
     */
    class ProcessId {
    public:
        /**
         * Gets the process id for the currently executing process.
         */
        static ProcessId getCurrent();

        /**
         * Constructs a ProcessId from a NativeProcessId.
         */
        static inline ProcessId fromNative(NativeProcessId npid) { return ProcessId(npid); }

        /**
         * Constructs a ProcessId with an unspecified value.
         */
        ProcessId() {}

        /**
         * Gets the native process id corresponding to this ProcessId.
         */
        NativeProcessId toNative() const { return _npid; }

        /**
         * Represents this process id as a signed 64-bit integer.
         *
         * This representation will faithfully serialize and format to text files, but is at least
         * twice the number of bits needed to uniquely represent valid process numbers on supported
         * OSes..
         */
        int64_t asInt64() const;

        /**
         * Similar to asInt64(), for compatibility with code that uses "long long" to mean 64-bit
         * signed integer.
         */
        long long asLongLong() const;

        /**
         * Represents this process id as an unsigned 32-bit integer.
         *
         * This representation will contain all of the bits of the native process id, but may not
         * serialize or format to text files meaningfully.
         */
        uint32_t asUInt32() const { return static_cast<uint32_t>(_npid); }

        /**
         * Provides a string representation of the pid.
         */
        std::string toString() const;

        bool operator==(const ProcessId other) const { return _npid == other._npid; }
        bool operator!=(const ProcessId other) const { return _npid != other._npid; }
        bool operator<(const ProcessId other) const { return _npid < other._npid; }
        bool operator<=(const ProcessId other) const { return _npid <= other._npid; }
        bool operator>(const ProcessId other) const { return _npid > other._npid; }
        bool operator>=(const ProcessId other) const { return _npid >= other._npid; }

    private:
        explicit ProcessId(NativeProcessId npid): _npid(npid) {}

        NativeProcessId _npid;
    };

    std::ostream& operator<<(std::ostream& os, ProcessId pid);

}  // namespace mongo

MONGO_HASH_NAMESPACE_START
template<> struct hash< ::mongo::ProcessId > {
    size_t operator()(const ::mongo::ProcessId pid) const {
        return hash< ::mongo::uint32_t >()(pid.asUInt32());
    }
};
MONGO_HASH_NAMESPACE_END

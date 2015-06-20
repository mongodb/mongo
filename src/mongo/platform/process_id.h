/*    Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

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
    static inline ProcessId fromNative(NativeProcessId npid) {
        return ProcessId(npid);
    }

    /**
     * Constructs a ProcessId with an unspecified value.
     */
    ProcessId() {}

    /**
     * Gets the native process id corresponding to this ProcessId.
     */
    NativeProcessId toNative() const {
        return _npid;
    }

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
    uint32_t asUInt32() const {
        return static_cast<uint32_t>(_npid);
    }

    /**
     * Provides a std::string representation of the pid.
     */
    std::string toString() const;

    bool operator==(const ProcessId other) const {
        return _npid == other._npid;
    }
    bool operator!=(const ProcessId other) const {
        return _npid != other._npid;
    }
    bool operator<(const ProcessId other) const {
        return _npid < other._npid;
    }
    bool operator<=(const ProcessId other) const {
        return _npid <= other._npid;
    }
    bool operator>(const ProcessId other) const {
        return _npid > other._npid;
    }
    bool operator>=(const ProcessId other) const {
        return _npid >= other._npid;
    }

private:
    explicit ProcessId(NativeProcessId npid) : _npid(npid) {}

    NativeProcessId _npid;
};

std::ostream& operator<<(std::ostream& os, ProcessId pid);

}  // namespace mongo

MONGO_HASH_NAMESPACE_START
template <>
struct hash<::mongo::ProcessId> {
    size_t operator()(const ::mongo::ProcessId pid) const {
        return hash<::std::uint32_t>()(pid.asUInt32());
    }
};
MONGO_HASH_NAMESPACE_END

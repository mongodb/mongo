// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
     * Gets the thread id for the currently executing process.
     */
    static ProcessId getCurrentThreadId();

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

    template <typename H>
    friend H AbslHashValue(
        H h, const ProcessId pid) {  // NOLINT(readability-avoid-const-params-in-decls)
        return H::combine(std::move(h), pid.asUInt32());
    }

private:
    explicit ProcessId(NativeProcessId npid) : _npid(npid) {}

    NativeProcessId _npid;
};

std::ostream& operator<<(std::ostream& os, ProcessId pid);

}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] logv2 {

class LogTag {
public:
    enum Value {
        kNone = 0,

        // startupWarnings ramlog
        kStartupWarnings = 1 << 0,

        // representing the logv1 plainShellOutput domain
        kPlainShell = 1 << 1,

        // allow logging while the shell is waiting for user input
        kAllowDuringPromptingShell = 1 << 2,

        // log to the backtrace log file
        kBacktraceLog = 1 << 3,
    };

    friend Value operator|(Value a, Value b) {
        return static_cast<Value>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
    }
    friend Value operator&(Value a, Value b) {
        return static_cast<Value>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
    }

    LogTag() : _value(kNone) {}
    /* implicit */ LogTag(Value value) {
        _value = static_cast<uint64_t>(value);
    }

    operator Value() const {
        return static_cast<Value>(_value);
    }

    bool has(LogTag other) const {
        return _value & other._value;
    }

    BSONArray toBSONArray();

private:
    uint64_t _value;
};

}  // namespace logv2
}  // namespace mongo

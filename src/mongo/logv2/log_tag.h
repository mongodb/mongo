/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"

#include <cstdint>
#include <string>

namespace mongo::logv2 {

class LogTag {
public:
    enum Value {
        kNone = 0,

        // replica set ramlog
        kRS = 1 << 0,

        // startupWarnings ramlog
        kStartupWarnings = 1 << 1,

        // representing the logv1 plainShellOutput domain
        kPlainShell = 1 << 2,

        // allow logging while the shell is waiting for user input
        kAllowDuringPromptingShell = 1 << 3,
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

}  // namespace mongo::logv2

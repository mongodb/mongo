/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace mongo {

class BSONObj;
class BSONObjBuilder;

/**
 *  The LogicalTime class holds the cluster time of the cluster. It provides conversions to
 *  a Timestamp to allow integration with opLog.
 */
class LogicalTime {
public:
    static constexpr StringData kOperationTimeFieldName = "operationTime"_sd;

    LogicalTime() = default;
    explicit LogicalTime(Timestamp ts);

    /**
     * Parses the 'operationTime' field of the specified object and extracts a LogicalTime from it.
     * If 'operationTime' is missing or of the wrong type, throws.
     */
    static LogicalTime fromOperationTime(const BSONObj& obj);

    /**
     * Appends "operationTime" field to the specified builder as a Timestamp type.
     */
    void appendAsOperationTime(BSONObjBuilder* builder) const;

    Timestamp asTimestamp() const {
        return Timestamp(_time);
    }

    /**
     * Increases the _time by ticks.
     */
    void addTicks(uint64_t ticks);

    /**
     * Const version, returns the LogicalTime with increased _time by ticks.
     */
    LogicalTime addTicks(uint64_t ticks) const;

    std::string toString() const;

    /**
     * Returns the LogicalTime as an array of unsigned chars in little endian order for use with the
     * crypto::hmacSHA1 function.
     */
    std::array<unsigned char, sizeof(uint64_t)> toUnsignedArray() const;

    /**
     *  serialize into BSON object.
     */
    BSONObj toBSON() const;

    /*
     * These methods support IDL parsing of logical times.
     */
    static LogicalTime parseFromBSON(const BSONElement& elem);
    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const;

    /**
     * An uninitialized value of LogicalTime. Default constructed.
     */
    static const LogicalTime kUninitialized;

private:
    uint64_t _time{0};
};

inline bool operator==(const LogicalTime& l, const LogicalTime& r) {
    return l.asTimestamp() == r.asTimestamp();
}

inline bool operator!=(const LogicalTime& l, const LogicalTime& r) {
    return !(l == r);
}

inline bool operator<(const LogicalTime& l, const LogicalTime& r) {
    return l.asTimestamp() < r.asTimestamp();
}

inline bool operator<=(const LogicalTime& l, const LogicalTime& r) {
    return (l < r || l == r);
}

inline bool operator>(const LogicalTime& l, const LogicalTime& r) {
    return (r < l);
}

inline bool operator>=(const LogicalTime& l, const LogicalTime& r) {
    return (l > r || l == r);
}

inline std::ostream& operator<<(std::ostream& s, const LogicalTime& v) {
    return (s << v.toString());
}

}  // namespace mongo

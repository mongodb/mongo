/**
 *    Copyright (C) 2017 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/timestamp.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;

/**
 *  The LogicalTime class holds the cluster time of the cluster. It provides conversions to
 *  a Timestamp to allow integration with opLog.
 */
class LogicalTime {
public:
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

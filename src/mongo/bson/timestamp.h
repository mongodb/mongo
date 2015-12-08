/*    Copyright 2009 10gen Inc.
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

#include "mongo/base/data_view.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Timestamp: A combination of a count of seconds since the POSIX epoch plus an ordinal value.
 */
class Timestamp {
public:
    // Maximum Timestamp value.
    static Timestamp max();

    /**
     * DEPRECATED Constructor that builds a Timestamp from a Date_t by using the
     * high-order 4 bytes of "date" for the "secs" field and the low-order 4 bytes
     * for the "i" field.
     */
    explicit Timestamp(Date_t date) : Timestamp(date.toULL()) {}

    /**
     * DEPRECATED Constructor that builds a Timestamp from a 64-bit unsigned integer by using
     * the high-order 4 bytes of "v" for the "secs" field and the low-order 4 bytes for the "i"
     * field.
     */
    explicit Timestamp(unsigned long long v) : Timestamp(v >> 32, v) {}

    Timestamp(Seconds s, unsigned increment) : Timestamp(s.count(), increment) {}

    Timestamp(unsigned a, unsigned b) : i(b), secs(a) {}

    Timestamp() = default;

    unsigned getSecs() const {
        return secs;
    }

    unsigned getInc() const {
        return i;
    }

    unsigned long long asULL() const {
        unsigned long long result = secs;
        result <<= 32;
        result |= i;
        return result;
    }
    long long asLL() const {
        return static_cast<long long>(asULL());
    }

    bool isNull() const {
        return secs == 0;
    }

    std::string toStringLong() const;

    std::string toStringPretty() const;

    std::string toString() const;

    bool operator==(const Timestamp& r) const {
        return tie() == r.tie();
    }
    bool operator!=(const Timestamp& r) const {
        return tie() != r.tie();
    }
    bool operator<(const Timestamp& r) const {
        return tie() < r.tie();
    }
    bool operator<=(const Timestamp& r) const {
        return tie() <= r.tie();
    }
    bool operator>(const Timestamp& r) const {
        return tie() > r.tie();
    }
    bool operator>=(const Timestamp& r) const {
        return tie() >= r.tie();
    }

    // Append the BSON representation of this Timestamp to the given BufBuilder with the given
    // name. This lives here because Timestamp manages its own serialization format.
    void append(BufBuilder& builder, const StringData& fieldName) const;

private:
    std::tuple<unsigned, unsigned> tie() const {
        return std::tie(secs, i);
    }

    unsigned i = 0;
    unsigned secs = 0;
};

}  // namespace mongo

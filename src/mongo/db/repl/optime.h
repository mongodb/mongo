/**
*    Copyright (C) 2015 MongoDB Inc.
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

#include <tuple>
#include <utility>

#include "mongo/bson/timestamp.h"

namespace mongo {
namespace repl {

    class OpTime {
    public:
        OpTime() = default;
        OpTime(Timestamp ts, long long term) : _timestamp(std::move(ts)), _term(term) {}

        Timestamp getTimestamp() const {
            return _timestamp;
        }

        long long getTerm() const {
            return _term;
        }

    private:
        Timestamp _timestamp;
        long long _term = -1;
    };

    inline bool operator==(const OpTime& lhs, const OpTime& rhs) {
        return std::tie(lhs.opTime, lhs.term) == std::tie(rhs.opTime, rhs.term);
    }

    inline bool operator<(const OpTime& lhs, const OpTime& rhs) {
        // Compare term first, then the opTimes.
        return std::tie(lhs.term, lhs.opTime) < std::tie(rhs.term, rhs.opTime);
    }

    inline bool operator!=(const OpTime& lhs, const OpTime& rhs) {
        return !(lhs == rhs);
    }

    inline bool operator<=(const OpTime& lhs, const OpTime& rhs) {
        return lhs < rhs || lhs == rhs;
    }

    inline bool operator>(const OpTime& lhs, const OpTime& rhs) {
        return !(lhs <= rhs);
    }

    inline bool operator>=(const OpTime& lhs, const OpTime& rhs) {
        return !(lhs < rhs);
    }

} // namespace repl
} // namespace mongo

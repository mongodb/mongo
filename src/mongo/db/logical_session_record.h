/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <utility>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/logical_session_record_gen.h"
#include "mongo/db/signed_logical_session_id.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * This class is the in-memory representation of a logical session record.
 *
 * The BSON representation of a session record follows this form:
 *
 * {
 *    lsid    : SignedLogicalSessionId,
 *    lastUse : Date_t
 * }
 */
class LogicalSessionRecord : public Logical_session_record {
public:
    /**
     * Constructs and returns a LogicalSessionRecord from a BSON representation,
     * or throws an error. For IDL.
     */
    static StatusWith<LogicalSessionRecord> parse(const BSONObj& bson);

    /**
     * Construct a new record, for a new session that does not yet have an
     * authoritative record in the sessions collection. This method should
     * only be used when the caller is intending to make a new authoritative
     * record and subsequently insert that record into the sessions collection.
     */
    static LogicalSessionRecord makeAuthoritativeRecord(SignedLogicalSessionId id, Date_t now);

    /**
     * Return a BSON representation of this session record.
     */
    BSONObj toBSON() const;

    /**
     * Return a string represenation of this session record.
     */
    std::string toString() const;

    inline bool operator==(const LogicalSessionRecord& rhs) const {
        return getSignedLsid() == rhs.getSignedLsid();
    }

    inline bool operator!=(const LogicalSessionRecord& rhs) const {
        return !(*this == rhs);
    }

private:
    LogicalSessionRecord() = default;

    LogicalSessionRecord(SignedLogicalSessionId id, Date_t now);
};

inline std::ostream& operator<<(std::ostream& s, const LogicalSessionRecord& record) {
    return (s << record.toString());
}

inline StringBuilder& operator<<(StringBuilder& s, const LogicalSessionRecord& record) {
    return (s << record.toString());
}

}  // namespace mongo

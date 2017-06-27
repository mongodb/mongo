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

#include <string>

#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/signed_logical_session_id_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/uuid.h"

namespace mongo {

class BSONObjBuilder;
class OperationContext;

/**
 * An identifier for a logical session. A LogicalSessionId has the following components:
 *
 * - A 128-bit unique identifier (UUID)
 * - An optional user id (ObjectId)
 * - A key id (long long)
 * - An HMAC signature (SHA1Block)
 */
class SignedLogicalSessionId : public Signed_logical_session_id {
public:
    using Owner = boost::optional<OID>;

    friend class Logical_session_id;
    friend class Logical_session_record;

    using keyIdType = long long;

    /**
     * Create and return a new LogicalSessionId with a random UUID. This method
     * should be used for testing only. The generated SignedLogicalSessionId will
     * not be signed, and will have no owner.
     */
    static SignedLogicalSessionId gen();

    /**
     * Creates a new SignedLogicalSessionId.
     */
    SignedLogicalSessionId(LogicalSessionId lsid,
                           boost::optional<OID> userId,
                           long long keyId,
                           SHA1Block signature);

    /**
     * Constructs a new LogicalSessionId out of a BSONObj. For IDL.
     */
    static SignedLogicalSessionId parse(const BSONObj& doc);

    /**
     * Returns a string representation of this session id.
     */
    std::string toString() const;

    /**
     * Serialize this object to BSON.
     */
    BSONObj toBSON() const;

    inline bool operator==(const SignedLogicalSessionId& rhs) const {
        return getLsid() == rhs.getLsid() && getUserId() == rhs.getUserId() &&
            getKeyId() == rhs.getKeyId() && getSignature() == rhs.getSignature();
    }

    inline bool operator!=(const SignedLogicalSessionId& rhs) const {
        return !(*this == rhs);
    }

    /**
     * This constructor exists for IDL only.
     */
    SignedLogicalSessionId();
};

inline std::ostream& operator<<(std::ostream& s, const SignedLogicalSessionId& lsid) {
    return (s << lsid.toString());
}

inline StringBuilder& operator<<(StringBuilder& s, const SignedLogicalSessionId& lsid) {
    return (s << lsid.toString());
}

}  // namespace mongo

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
#include "mongo/bson/oid.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_record_gen.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * This class is the in-memory representation of a logical session record.
 *
 * The BSON representation of a session record follows this form:
 *
 * {
 *    lsid    : LogicalSessionId,
 *    lastUse : Date_t,
 *    owner   : {
 *        user   : UserName,
 *        userId : OID
 *    }
 */
class LogicalSessionRecord : public Logical_session_record {
public:
    using Owner = std::pair<UserName, boost::optional<OID>>;

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
    static LogicalSessionRecord makeAuthoritativeRecord(LogicalSessionId id,
                                                        UserName user,
                                                        boost::optional<OID> userId);

    /**
     * Return a BSON representation of this session record.
     */
    BSONObj toBSON() const;

    /**
     * Return the username and id of the User who owns this session. Only a User
     * that matches both the name and id returned by this method should be
     * permitted to use this session.
     *
     * Note: if the returned optional OID is set to boost::none, this implies that
     * the owning user is a pre-3.6 user that has no id. In this case, only a User
     * with a matching UserName who also has an unset optional id should be
     * permitted to use this session.
     */
    Owner getSessionOwner() const;

private:
    LogicalSessionRecord() = default;

    LogicalSessionRecord(LogicalSessionId id, UserName user, boost::optional<OID> userId);

    Owner _owner;
};

}  // namespace mongo

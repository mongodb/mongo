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

#include "mongo/platform/basic.h"

#include <utility>

#include "mongo/db/logical_session_record.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

StatusWith<LogicalSessionRecord> LogicalSessionRecord::parse(const BSONObj& bson) {
    try {
        IDLParserErrorContext ctxt("logical session record");

        LogicalSessionRecord record;
        record.parseProtected(ctxt, bson);

        auto owner = record.getOwner();
        UserName user{owner.getUserName(), owner.getDbName()};
        record._owner = std::make_pair(user, owner.getUserId());

        return record;
    } catch (std::exception e) {
        return exceptionToStatus();
    }
}

LogicalSessionRecord LogicalSessionRecord::makeAuthoritativeRecord(LogicalSessionId id,
                                                                   UserName user,
                                                                   boost::optional<OID> userId) {
    return LogicalSessionRecord(std::move(id), std::move(user), std::move(userId));
}

BSONObj LogicalSessionRecord::toBSON() const {
    BSONObjBuilder builder;
    serialize(&builder);
    return builder.obj();
}

LogicalSessionRecord::LogicalSessionRecord(LogicalSessionId id,
                                           UserName user,
                                           boost::optional<OID> userId)
    : _owner(std::make_pair(std::move(user), std::move(userId))) {
    setLsid(std::move(id));
    setLastUse(Date_t::now());

    Session_owner owner;
    owner.setUserName(_owner.first.getUser());
    owner.setDbName(_owner.first.getDB());
    owner.setUserId(_owner.second);
    setOwner(std::move(owner));
}

LogicalSessionRecord::Owner LogicalSessionRecord::getSessionOwner() const {
    return _owner;
}

}  // namespace mongo

/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_session_id.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

namespace {

// Field name, which the extractFromBSON method expects. Use this value if adding a migration
// session to BSON.
const char kFieldName[] = "sessionId";

}  // namespace

MigrationSessionId MigrationSessionId::generate(StringData donor, StringData recipient) {
    invariant(!donor.empty());
    invariant(!recipient.empty());

    return MigrationSessionId(str::stream() << donor << "_" << recipient << "_"
                                            << OID::gen().toString());
}

StatusWith<MigrationSessionId> MigrationSessionId::extractFromBSON(const BSONObj& obj) {
    string sessionId;
    Status status = bsonExtractStringField(obj, kFieldName, &sessionId);
    if (status.isOK()) {
        return MigrationSessionId(sessionId);
    } else if (status == ErrorCodes::NoSuchKey) {
        return MigrationSessionId();
    }

    return status;
}

MigrationSessionId::MigrationSessionId() = default;

MigrationSessionId::MigrationSessionId(std::string sessionId) {
    invariant(!sessionId.empty());
    _sessionId = std::move(sessionId);
}

bool MigrationSessionId::matches(const MigrationSessionId& other) const {
    if (_sessionId && other._sessionId)
        return *_sessionId == *other._sessionId;

    return !_sessionId && !other._sessionId;
}

void MigrationSessionId::append(BSONObjBuilder* builder) const {
    if (_sessionId) {
        builder->append(kFieldName, *_sessionId);
    }
}

std::string MigrationSessionId::toString() const {
    return (_sessionId ? *_sessionId : "");
}

bool MigrationSessionId::isEmpty() const {
    return !_sessionId;
}

}  // namespace mongo

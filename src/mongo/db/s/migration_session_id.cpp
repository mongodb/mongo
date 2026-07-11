// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/migration_session_id.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {

using std::string;

namespace {

// Field name, which the extractFromBSON method expects. Use this value if adding a migration
// session to BSON.
const char kFieldName[] = "sessionId";

}  // namespace

MigrationSessionId MigrationSessionId::generate(std::string_view donor,
                                                std::string_view recipient) {
    invariant(!donor.empty());
    invariant(!recipient.empty());

    return MigrationSessionId(str::stream()
                              << donor << "_" << recipient << "_" << OID::gen().toString());
}

StatusWith<MigrationSessionId> MigrationSessionId::extractFromBSON(const BSONObj& obj) {
    string sessionId;
    Status status = bsonExtractStringField(obj, kFieldName, &sessionId);
    if (status.isOK()) {
        return MigrationSessionId(sessionId);
    }

    return status;
}

MigrationSessionId MigrationSessionId::parseFromBSON(const BSONObj& obj) {
    return uassertStatusOK(extractFromBSON(obj));
}

MigrationSessionId::MigrationSessionId(std::string sessionId) {
    invariant(!sessionId.empty());
    _sessionId = std::move(sessionId);
}

bool MigrationSessionId::matches(const MigrationSessionId& other) const {
    return _sessionId == other._sessionId;
}

void MigrationSessionId::append(BSONObjBuilder* builder) const {
    builder->append(kFieldName, _sessionId);
}

std::string MigrationSessionId::toString() const {
    return _sessionId;
}

}  // namespace mongo

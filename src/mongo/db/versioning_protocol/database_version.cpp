// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/versioning_protocol/database_version.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

DatabaseVersion DatabaseVersion::makeFixed() {
    DatabaseVersion dbVersion(UUID::gen(), Timestamp());
    dbVersion.setLastMod(0);
    return dbVersion;
}
DatabaseVersion DatabaseVersion::makeUpdated() const {
    DatabaseVersion newVersion = *this;
    newVersion.setLastMod(newVersion.getLastMod() + 1);
    return newVersion;
}

bool DatabaseVersion::operator<(const DatabaseVersion& other) const {
    if (getTimestamp() == other.getTimestamp()) {
        return getLastMod() < other.getLastMod();
    } else {
        return getTimestamp() < other.getTimestamp();
    }
}

std::string DatabaseVersion::toString() const {
    return BSON("dbVersion" << toBSON()).toString();
}

}  // namespace mongo

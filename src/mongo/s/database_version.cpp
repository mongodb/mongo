/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/database_version.h"

namespace mongo {

AtomicWord<uint64_t> ComparableDatabaseVersion::_uuidDisambiguatingSequenceNumSource{1ULL};

DatabaseVersion DatabaseVersion::makeFixed() {
    DatabaseVersion dbVersion;
    dbVersion.setLastMod(0);
    dbVersion.setUuid(UUID::gen());
    return dbVersion;
}
DatabaseVersion DatabaseVersion::makeUpdated() const {
    DatabaseVersion newVersion = *this;
    newVersion.setLastMod(newVersion.getLastMod() + 1);
    return newVersion;
}

ComparableDatabaseVersion ComparableDatabaseVersion::makeComparableDatabaseVersion(
    const DatabaseVersion& version) {
    return ComparableDatabaseVersion(version, _uuidDisambiguatingSequenceNumSource.fetchAndAdd(1));
}

BSONObj ComparableDatabaseVersion::toBSONForLogging() const {
    BSONObjBuilder builder;
    if (_dbVersion)
        builder.append("dbVersion"_sd, _dbVersion->toBSON());
    else
        builder.append("dbVersion"_sd, "None");

    builder.append("uuidDisambiguatingSequenceNum"_sd,
                   static_cast<int64_t>(_uuidDisambiguatingSequenceNum));

    return builder.obj();
}

bool ComparableDatabaseVersion::operator==(const ComparableDatabaseVersion& other) const {
    if (!_dbVersion && !other._dbVersion)
        return true;  // Default constructed value
    if (_dbVersion.is_initialized() != other._dbVersion.is_initialized())
        return false;  // One side is default constructed value

    return *_dbVersion == *other._dbVersion;
}

bool ComparableDatabaseVersion::operator<(const ComparableDatabaseVersion& other) const {
    if (!_dbVersion && !other._dbVersion)
        return false;  // Default constructed value

    if (_dbVersion && other._dbVersion && _dbVersion->getUuid() == other._dbVersion->getUuid()) {
        return _dbVersion->getLastMod() < other._dbVersion->getLastMod();
    } else {
        return _uuidDisambiguatingSequenceNum < other._uuidDisambiguatingSequenceNum;
    }
}

}  // namespace mongo

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
AtomicWord<uint64_t> ComparableDatabaseVersion::_forcedRefreshSequenceNumSource{1ULL};

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
    const boost::optional<DatabaseVersion>& version) {
    return ComparableDatabaseVersion(version,
                                     _uuidDisambiguatingSequenceNumSource.fetchAndAdd(1),
                                     _forcedRefreshSequenceNumSource.load());
}

ComparableDatabaseVersion
ComparableDatabaseVersion::makeComparableDatabaseVersionForForcedRefresh() {
    return ComparableDatabaseVersion(boost::none /* version */,
                                     _uuidDisambiguatingSequenceNumSource.fetchAndAdd(1),
                                     _forcedRefreshSequenceNumSource.addAndFetch(2) - 1);
}

void ComparableDatabaseVersion::setDatabaseVersion(const DatabaseVersion& version) {
    _dbVersion = version;
}

BSONObj ComparableDatabaseVersion::toBSONForLogging() const {
    BSONObjBuilder builder;
    if (_dbVersion)
        builder.append("dbVersion"_sd, _dbVersion->toBSON());
    else
        builder.append("dbVersion"_sd, "None");

    builder.append("uuidDisambiguatingSequenceNum"_sd,
                   static_cast<int64_t>(_uuidDisambiguatingSequenceNum));

    builder.append("forcedRefreshSequenceNum"_sd, static_cast<int64_t>(_forcedRefreshSequenceNum));

    return builder.obj();
}

bool ComparableDatabaseVersion::operator==(const ComparableDatabaseVersion& other) const {
    if (_forcedRefreshSequenceNum != other._forcedRefreshSequenceNum)
        return false;  // Values created on two sides of a forced refresh sequence number are always
                       // considered different
    if (_forcedRefreshSequenceNum == 0)
        return true;  // Only default constructed values have _forcedRefreshSequenceNum == 0 and
                      // they are always equal

    // Relying on the boost::optional<DatabaseVersion>::operator== comparison
    return _dbVersion == other._dbVersion;
}

bool ComparableDatabaseVersion::operator<(const ComparableDatabaseVersion& other) const {
    if (_forcedRefreshSequenceNum < other._forcedRefreshSequenceNum)
        return true;  // Values created on two sides of a forced refresh sequence number are always
                      // considered different
    if (_forcedRefreshSequenceNum > other._forcedRefreshSequenceNum)
        return false;  // Values created on two sides of a forced refresh sequence number are always
                       // considered different
    if (_forcedRefreshSequenceNum == 0)
        return false;  // Only default constructed values have _forcedRefreshSequenceNum == 0 and
                       // they are always equal

    // 1. If both versions are valid and have timestamps
    //    1.1. if their timestamps are the same -> rely on lastMod to define the order
    //    1.2. Otherwise  -> rely on the timestamps values to define order
    // 2. If both versions are valid and have the same uuid -> rely on lastMod to define the order
    // 3. Any other scenario -> rely on disambiguating sequence number
    if (_dbVersion && other._dbVersion) {
        const auto timestamp = _dbVersion->getTimestamp();
        const auto otherTimestamp = other._dbVersion->getTimestamp();
        if (timestamp && otherTimestamp) {
            if (*timestamp == *otherTimestamp)
                return _dbVersion->getLastMod() < other._dbVersion->getLastMod();
            else
                return *timestamp < *otherTimestamp;

        } else if (_dbVersion->getUuid() == other._dbVersion->getUuid()) {
            return _dbVersion->getLastMod() < other._dbVersion->getLastMod();
        }
    }

    return _uuidDisambiguatingSequenceNum < other._uuidDisambiguatingSequenceNum;
}

}  // namespace mongo

/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_metadata_accessor.h"

#include "mongo/logv2/log.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

DatabaseShardingMetadataAccessor::DatabaseShardingMetadataAccessor(DatabaseName dbName)
    : _dbName(dbName), _accessType(AccessType::kReadAccess) {}

boost::optional<DatabaseVersion> DatabaseShardingMetadataAccessor::getDbVersion() const {
    tassert(10371101,
            fmt::format("Expected read access to retrieve database version for database: {}",
                        _dbName.toStringForErrorMsg()),
            _accessType == AccessType::kReadAccess);

    return _dbVersion;
}

boost::optional<ShardId> DatabaseShardingMetadataAccessor::getDbPrimaryShard() const {
    tassert(10371102,
            fmt::format("Expected read access to retrieve primary shard for database: {}",
                        _dbName.toStringForErrorMsg()),
            _accessType == AccessType::kReadAccess);

    return _dbPrimaryShard;
}

bool DatabaseShardingMetadataAccessor::isMovePrimaryInProgress() const {
    return _movePrimaryInProgress;
}

void DatabaseShardingMetadataAccessor::setDbMetadata(const ShardId& dbPrimaryShard,
                                                     const DatabaseVersion& dbVersion) {
    LOGV2(10371103,
          "Setting this node's cached database metadata",
          logAttrs(_dbName),
          "dbVersion"_attr = dbVersion);

    tassert(10371104,
            fmt::format("Expected write access to set metadata for database: {}",
                        _dbName.toStringForErrorMsg()),
            _accessType == AccessType::kWriteAccess);

    _dbPrimaryShard.emplace(dbPrimaryShard);
    _dbVersion.emplace(dbVersion);
}

void DatabaseShardingMetadataAccessor::clearDbMetadata() {
    LOGV2(10371105, "Clearing this node's cached database metadata", logAttrs(_dbName));

    tassert(10371106,
            fmt::format("Expected write access to clear metadata for database: {}",
                        _dbName.toStringForErrorMsg()),
            _accessType == AccessType::kWriteAccess);

    _dbPrimaryShard = boost::none;
    _dbVersion = boost::none;
}

void DatabaseShardingMetadataAccessor::setMovePrimaryInProgress() {
    _movePrimaryInProgress = true;
}

void DatabaseShardingMetadataAccessor::unsetMovePrimaryInProgress() {
    _movePrimaryInProgress = false;
}

void DatabaseShardingMetadataAccessor::setAccessType(AccessType accessType) {
    _accessType = accessType;
}

void DatabaseShardingMetadataAccessor::setDbMetadata_UNSAFE(const ShardId& dbPrimaryShard,
                                                            const DatabaseVersion& dbVersion) {
    LOGV2(10371107,
          "Setting this node's cached database metadata",
          logAttrs(_dbName),
          "dbVersion"_attr = dbVersion);

    _dbPrimaryShard.emplace(dbPrimaryShard);
    _dbVersion.emplace(dbVersion);
}

}  // namespace mongo

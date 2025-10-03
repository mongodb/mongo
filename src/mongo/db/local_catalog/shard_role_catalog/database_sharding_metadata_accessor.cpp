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

#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/testing_proctor.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {
template <typename MsgType>
void tassertInTestingOrLog(int code,
                           MsgType&& msg,
                           const DatabaseName& dbName,
                           bool conditionToSatisfy) {
    if (conditionToSatisfy)
        return;

    if (TestingProctor::instance().isEnabled()) {
        auto printmsg = fmt::format("{} dbName: {}", msg, dbName.toStringForErrorMsg());
        tasserted(code, printmsg);
    } else {
        LOGV2_WARNING(
            code, std::forward<MsgType>(msg), "dbName"_attr = redact(dbName.toStringForErrorMsg()));
    }
}
}  // namespace

namespace {

const auto getOpCtxDecoration = OperationContext::declareDecoration<OperationDatabaseMetadata>();

}

DatabaseShardingMetadataAccessor::DatabaseShardingMetadataAccessor(DatabaseName dbName)
    : _dbName(dbName), _accessType(AccessType::kReadAccess) {}

boost::optional<DatabaseVersion> DatabaseShardingMetadataAccessor::getDbVersion(
    OperationContext* opCtx) const {
    if (!OperationDatabaseMetadata::get(opCtx).getBypassReadDbMetadataAccess()) {
        tassertInTestingOrLog(10371101,
                              "Expected read access to retrieve database version for database.",
                              _dbName,
                              _accessType == AccessType::kReadAccess);
    }

    return _dbVersion;
}

boost::optional<ShardId> DatabaseShardingMetadataAccessor::getDbPrimaryShard(
    OperationContext* opCtx) const {
    if (!OperationDatabaseMetadata::get(opCtx).getBypassReadDbMetadataAccess()) {
        tassertInTestingOrLog(10371102,
                              "Expected read access to retrieve primary shard for database.",
                              _dbName,
                              _accessType == AccessType::kReadAccess);
    }
    return _dbPrimaryShard;
}

bool DatabaseShardingMetadataAccessor::isMovePrimaryInProgress() const {
    return _movePrimaryInProgress;
}

void DatabaseShardingMetadataAccessor::setDbMetadata(OperationContext* opCtx,
                                                     const ShardId& dbPrimaryShard,
                                                     const DatabaseVersion& dbVersion) {
    LOGV2(10371103,
          "Setting this node's cached database metadata",
          logAttrs(_dbName),
          "dbVersion"_attr = dbVersion);
    if (!OperationDatabaseMetadata::get(opCtx).getBypassWriteDbMetadataAccess()) {
        tassertInTestingOrLog(10371104,
                              "Expected write access to set metadata for database.",
                              _dbName,
                              _accessType == AccessType::kWriteAccess);
    }
    _dbPrimaryShard.emplace(dbPrimaryShard);
    _dbVersion.emplace(dbVersion);
}

void DatabaseShardingMetadataAccessor::clearDbMetadata(OperationContext* opCtx) {
    LOGV2(10371105, "Clearing this node's cached database metadata", logAttrs(_dbName));


    /*
       Temporarily disable this check: movePrimary may have to clear non-authoritative filtering
       metadata when aborted before reaching the commit phase of the critical section.
       TODO SERVER-98118 Re-enable this check once once 9.0 becomes last LTS.
        if (!OperationDatabaseMetadata::get(opCtx).getBypassWriteDbMetadataAccess()) {
            tassertInTestingOrLog(10371106,
                                "Expected write access to clear metadata for database.",
                                _dbName,
                                _accessType == AccessType::kWriteAccess);
        }
    */
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

OperationDatabaseMetadata& OperationDatabaseMetadata::get(OperationContext* opCtx) {
    return getOpCtxDecoration(opCtx);
}

}  // namespace mongo

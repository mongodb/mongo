// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/database_sharding_metadata_accessor.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
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
    ++_numMetadataMutations;
    ShardingStatistics::get(opCtx)
        .databaseShardingMetadataStatistics.registerDatabaseMetadataCacheSet();
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
    ++_numMetadataMutations;
    ShardingStatistics::get(opCtx)
        .databaseShardingMetadataStatistics.registerDatabaseMetadataCacheClear();
}

void DatabaseShardingMetadataAccessor::setMovePrimaryInProgress(OperationContext* opCtx) {
    auto oldState = std::exchange(_movePrimaryInProgress, true);
    if (oldState == false)
        ShardingStatistics::get(opCtx)
            .databaseShardingMetadataStatistics.registerMovePrimaryInProgress(true);
}

void DatabaseShardingMetadataAccessor::unsetMovePrimaryInProgress(OperationContext* opCtx) {
    auto oldState = std::exchange(_movePrimaryInProgress, false);
    if (oldState == true)
        ShardingStatistics::get(opCtx)
            .databaseShardingMetadataStatistics.registerMovePrimaryInProgress(false);
}

void DatabaseShardingMetadataAccessor::setAccessType(OperationContext* opCtx,
                                                     AccessType accessType) {
    _accessType = accessType;
    ShardingStatistics::get(opCtx)
        .databaseShardingMetadataStatistics.registerDatabaseMetadataAccessorAccessTypeChange();
}

void DatabaseShardingMetadataAccessor::setDbMetadata_UNSAFE(OperationContext* opCtx,
                                                            const ShardId& dbPrimaryShard,
                                                            const DatabaseVersion& dbVersion) {
    LOGV2(10371107,
          "Setting this node's cached database metadata",
          logAttrs(_dbName),
          "dbVersion"_attr = dbVersion);

    _dbPrimaryShard.emplace(dbPrimaryShard);
    _dbVersion.emplace(dbVersion);
    ++_numMetadataMutations;
    ShardingStatistics::get(opCtx)
        .databaseShardingMetadataStatistics.registerDatabaseMetadataCacheSet();
}

OperationDatabaseMetadata& OperationDatabaseMetadata::get(OperationContext* opCtx) {
    return getOpCtxDecoration(opCtx);
}

}  // namespace mongo

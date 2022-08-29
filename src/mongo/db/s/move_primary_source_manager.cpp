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

#include "mongo/db/s/move_primary_source_manager.h"

#include "mongo/client/connpool.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/s/type_shard_database.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_primary_gen.h"
#include "mongo/util/exit.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangInCloneStage);
MONGO_FAIL_POINT_DEFINE(hangInCleanStaleDataStage);

MovePrimarySourceManager::MovePrimarySourceManager(OperationContext* opCtx,
                                                   ShardMovePrimary requestArgs,
                                                   StringData dbname,
                                                   ShardId& fromShard,
                                                   ShardId& toShard)
    : _requestArgs(std::move(requestArgs)),
      _dbname(dbname),
      _fromShard(std::move(fromShard)),
      _toShard(std::move(toShard)),
      _critSecReason(BSON("command"
                          << "movePrimary"
                          << "dbName" << _dbname << "fromShard" << fromShard << "toShard"
                          << toShard)) {}

MovePrimarySourceManager::~MovePrimarySourceManager() {}

NamespaceString MovePrimarySourceManager::getNss() const {
    return _requestArgs.get_shardsvrMovePrimary();
}

Status MovePrimarySourceManager::clone(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCreated);
    ScopeGuard scopedGuard([&] { cleanupOnError(opCtx); });

    LOGV2(22042,
          "Moving {db} primary from: {fromShard} to: {toShard}",
          "Moving primary for database",
          "db"_attr = _dbname,
          "fromShard"_attr = _fromShard,
          "toShard"_attr = _toShard);

    // Record start in changelog
    auto logChangeCheckedStatus = ShardingLogging::get(opCtx)->logChangeChecked(
        opCtx,
        "movePrimary.start",
        _dbname.toString(),
        _buildMoveLogEntry(_dbname.toString(), _fromShard.toString(), _toShard.toString()),
        ShardingCatalogClient::kMajorityWriteConcern);

    if (!logChangeCheckedStatus.isOK()) {
        return logChangeCheckedStatus;
    }

    {
        // We use AutoGetDb::ensureDbExists() the first time just in case movePrimary was called
        // before any data was inserted into the database.
        AutoGetDb autoDb(opCtx, getNss().dbName(), MODE_X);
        invariant(autoDb.ensureDbExists(opCtx), getNss().toString());

        auto dss = DatabaseShardingState::get(opCtx, getNss().toString());
        auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);

        dss->setMovePrimarySourceManager(opCtx, this, dssLock);
    }

    _state = kCloning;

    if (MONGO_unlikely(hangInCloneStage.shouldFail())) {
        LOGV2(4908700, "Hit hangInCloneStage");
        hangInCloneStage.pauseWhileSet(opCtx);
    }

    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto fromShardObj = uassertStatusOK(shardRegistry->getShard(opCtx, _fromShard));
    auto toShardObj = uassertStatusOK(shardRegistry->getShard(opCtx, _toShard));

    BSONObjBuilder cloneCatalogDataCommandBuilder;
    cloneCatalogDataCommandBuilder << "_shardsvrCloneCatalogData" << _dbname << "from"
                                   << fromShardObj->getConnString().toString();


    auto cloneCommandResponse = toShardObj->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        "admin",
        CommandHelpers::appendMajorityWriteConcern(cloneCatalogDataCommandBuilder.obj()),
        Shard::RetryPolicy::kNotIdempotent);

    auto cloneCommandStatus = Shard::CommandResponse::getEffectiveStatus(cloneCommandResponse);
    if (!cloneCommandStatus.isOK()) {
        return cloneCommandStatus;
    }

    auto clonedCollsArray = cloneCommandResponse.getValue().response["clonedColls"];
    for (const auto& elem : clonedCollsArray.Obj()) {
        if (elem.type() == String) {
            _clonedColls.push_back(NamespaceString(elem.String()));
        }
    }

    _state = kCloneCaughtUp;
    scopedGuard.dismiss();
    return Status::OK();
}

Status MovePrimarySourceManager::enterCriticalSection(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCloneCaughtUp);
    ScopeGuard scopedGuard([&] { cleanupOnError(opCtx); });

    // Mark the shard as running a critical operation that requires recovery on crash.
    auto startMetadataOpStatus = ShardingStateRecovery::startMetadataOp(opCtx);
    if (!startMetadataOpStatus.isOK()) {
        return startMetadataOpStatus;
    }

    {
        // The critical section must be entered with the database X lock in order to ensure there
        // are no writes which could have entered and passed the database version check just before
        // we entered the critical section, but will potentially complete after we left it.
        AutoGetDb autoDb(opCtx, getNss().dbName(), MODE_X);

        if (!autoDb.getDb()) {
            uasserted(ErrorCodes::ConflictingOperationInProgress,
                      str::stream() << "The database " << getNss().toString()
                                    << " was dropped during the movePrimary operation.");
        }

        auto dss = DatabaseShardingState::get(opCtx, getNss().toString());
        auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);

        // IMPORTANT: After this line, the critical section is in place and needs to be signaled
        dss->enterCriticalSectionCatchUpPhase(opCtx, dssLock, _critSecReason);
    }

    _state = kCriticalSection;

    // Persist a signal to secondaries that we've entered the critical section. This will cause
    // secondaries to refresh their routing table when next accessed, which will block behind the
    // critical section. This ensures causal consistency by preventing a stale mongos with a cluster
    // time inclusive of the move primary config commit update from accessing secondary data.
    // Note: this write must occur after the critSec flag is set, to ensure the secondary refresh
    // will stall behind the flag.
    Status signalStatus = shardmetadatautil::updateShardDatabasesEntry(
        opCtx,
        BSON(ShardDatabaseType::kNameFieldName << getNss().toString()),
        BSONObj(),
        BSON(ShardDatabaseType::kEnterCriticalSectionCounterFieldName << 1),
        false /*upsert*/);
    if (!signalStatus.isOK()) {
        return {
            ErrorCodes::OperationFailed,
            str::stream() << "Failed to persist critical section signal for secondaries due to: "
                          << signalStatus.toString()};
    }

    LOGV2(22043, "movePrimary successfully entered critical section");

    scopedGuard.dismiss();

    return Status::OK();
}

Status MovePrimarySourceManager::commitOnConfig(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCriticalSection);
    ScopeGuard scopedGuard([&] { cleanupOnError(opCtx); });

    boost::optional<DatabaseVersion> expectedDbVersion;

    {
        AutoGetDb autoDb(opCtx, getNss().dbName(), MODE_X);

        if (!autoDb.getDb()) {
            uasserted(ErrorCodes::ConflictingOperationInProgress,
                      str::stream() << "The database " << getNss().toString()
                                    << " was dropped during the movePrimary operation.");
        }

        auto dss = DatabaseShardingState::get(opCtx, getNss().toString());
        auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);

        // Read operations must begin to wait on the critical section just before we send the
        // commit operation to the config server
        dss->enterCriticalSectionCommitPhase(opCtx, dssLock, _critSecReason);

        expectedDbVersion = DatabaseHolder::get(opCtx)->getDbVersion(opCtx, _dbname);
    }

    auto commitStatus = [&]() {
        try {
            return _commitOnConfig(opCtx, *expectedDbVersion);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }();

    if (!commitStatus.isOK()) {
        // Need to get the latest optime in case the refresh request goes to a secondary --
        // otherwise the read won't wait for the write that commit on config server may have done.
        LOGV2(22044,
              "Error occurred while committing the movePrimary. Performing a majority write "
              "against the config server to obtain its latest optime: {error}",
              "Error occurred while committing the movePrimary. Performing a majority write "
              "against the config server to obtain its latest optime",
              "error"_attr = redact(commitStatus));

        Status validateStatus = ShardingLogging::get(opCtx)->logChangeChecked(
            opCtx,
            "movePrimary.validating",
            getNss().ns(),
            _buildMoveLogEntry(_dbname.toString(), _fromShard.toString(), _toShard.toString()),
            ShardingCatalogClient::kMajorityWriteConcern);

        if ((ErrorCodes::isInterruption(validateStatus.code()) ||
             ErrorCodes::isShutdownError(validateStatus.code()) ||
             validateStatus == ErrorCodes::CallbackCanceled) &&
            globalInShutdownDeprecated()) {
            // Since the server is already doing a clean shutdown, this call will just join the
            // previous shutdown call
            shutdown(waitForShutdown());
        }

        // If we failed to get the latest config optime because we stepped down as primary, then it
        // is safe to fail without crashing because the new primary will fetch the latest optime
        // when it recovers the sharding state recovery document, as long as we also clear the
        // metadata for this database, forcing subsequent callers to do a full refresh. Check if
        // this node can accept writes for this collection as a proxy for it being primary.
        if (!validateStatus.isOK()) {
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());
            AutoGetDb autoDb(opCtx, getNss().dbName(), MODE_IX);

            if (!autoDb.getDb()) {
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream() << "The database " << getNss().toString()
                                        << " was dropped during the movePrimary operation.");
            }

            if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, getNss())) {
                DatabaseHolder::get(opCtx)->clearDbInfo(
                    opCtx, DatabaseName(boost::none, getNss().toString()));
                uassertStatusOK(validateStatus.withContext(
                    str::stream() << "Unable to verify movePrimary commit for database: "
                                  << getNss().ns()
                                  << " because the node's replication role changed. Version "
                                     "was cleared for: "
                                  << getNss().ns()
                                  << ", so it will get a full refresh when accessed again."));
            }
        }

        // We would not be able to guarantee our next database refresh would pick up the write for
        // the movePrimary commit (if it happened), because we were unable to get the latest config
        // OpTime.
        fassert(50762,
                validateStatus.withContext(
                    str::stream() << "Failed to commit movePrimary for database " << getNss().ns()
                                  << " due to " << redact(commitStatus)
                                  << ". Updating the optime with a write before clearing the "
                                  << "version also failed"));

        // If we can validate but the commit still failed, return the status.
        return commitStatus;
    }

    _state = kCloneCompleted;

    _cleanup(opCtx);

    uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
        opCtx,
        "movePrimary.commit",
        _dbname.toString(),
        _buildMoveLogEntry(_dbname.toString(), _fromShard.toString(), _toShard.toString()),
        ShardingCatalogClient::kMajorityWriteConcern));

    scopedGuard.dismiss();

    _state = kNeedCleanStaleData;

    return Status::OK();
}

Status MovePrimarySourceManager::_commitOnConfig(OperationContext* opCtx,
                                                 const DatabaseVersion& expectedDbVersion) {
    LOGV2_DEBUG(6697200,
                3,
                "Committing movePrimary",
                "db"_attr = _dbname,
                "fromShard"_attr = _fromShard,
                "toShard"_attr = _toShard,
                "expectedDbVersion"_attr = expectedDbVersion);

    const auto commitStatus = [&] {
        ConfigsvrCommitMovePrimary commitRequest(_dbname, expectedDbVersion, _toShard);
        commitRequest.setDbName(NamespaceString::kAdminDb);

        const auto commitResponse =
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                NamespaceString::kAdminDb.toString(),
                CommandHelpers::appendMajorityWriteConcern(commitRequest.toBSON({})),
                Shard::RetryPolicy::kIdempotent);

        const auto status = Shard::CommandResponse::getEffectiveStatus(commitResponse);
        if (status != ErrorCodes::CommandNotFound) {
            return status;
        }

        LOGV2(6697201,
              "_configsvrCommitMovePrimary command not found on config server, so try to update "
              "the metadata document directly",
              "db"_attr = _dbname);

        // The fallback logic is not synchronized with the removeShard command and simultaneous
        // invocations of movePrimary and removeShard can lead to data loss.
        return _fallbackCommitOnConfig(opCtx, expectedDbVersion);
    }();

    if (!commitStatus.isOK()) {
        LOGV2(6697202,
              "Error committing movePrimary",
              "db"_attr = _dbname,
              "error"_attr = redact(commitStatus));
        return commitStatus;
    }

    const auto updatedDbType = [&]() {
        auto findResponse = uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kMajorityReadConcern,
                NamespaceString::kConfigDatabasesNamespace,
                BSON(DatabaseType::kNameFieldName << _dbname),
                BSON(DatabaseType::kNameFieldName << -1),
                1));

        const auto databases = std::move(findResponse.docs);
        uassert(ErrorCodes::IncompatibleShardingMetadata,
                "Tried to find version for database {}, but found no databases"_format(_dbname),
                !databases.empty());

        return DatabaseType::parse(IDLParserContext("DatabaseType"), databases.front());
    }();
    tassert(6851100,
            "Error committing movePrimary: database version went backwards",
            updatedDbType.getVersion() > expectedDbVersion);
    uassert(6851101,
            "Error committing movePrimary: update of config.databases failed",
            updatedDbType.getPrimary() != _fromShard);

    LOGV2_DEBUG(6697203,
                3,
                "Commited movePrimary",
                "db"_attr = _dbname,
                "fromShard"_attr = _fromShard,
                "toShard"_attr = _toShard,
                "updatedDbVersion"_attr = updatedDbType.getVersion());

    return Status::OK();
}

Status MovePrimarySourceManager::_fallbackCommitOnConfig(OperationContext* opCtx,
                                                         const DatabaseVersion& expectedDbVersion) {
    const auto query = [&] {
        BSONObjBuilder queryBuilder;
        queryBuilder.append(DatabaseType::kNameFieldName, _dbname);
        // Include the version in the update filter to be resilient to potential network retries and
        // delayed messages.
        for (const auto [fieldName, fieldValue] : expectedDbVersion.toBSON()) {
            const auto dottedFieldName = DatabaseType::kVersionFieldName + "." + fieldName;
            queryBuilder.appendAs(fieldValue, dottedFieldName);
        }
        return queryBuilder.obj();
    }();

    const auto update = [&] {
        const auto newDbVersion = expectedDbVersion.makeUpdated();

        BSONObjBuilder updateBuilder;
        updateBuilder.append(DatabaseType::kPrimaryFieldName, _toShard);
        updateBuilder.append(DatabaseType::kVersionFieldName, newDbVersion.toBSON());
        return updateBuilder.obj();
    }();

    return Grid::get(opCtx)
        ->catalogClient()
        ->updateConfigDocument(opCtx,
                               NamespaceString::kConfigDatabasesNamespace,
                               query,
                               update,
                               false,
                               ShardingCatalogClient::kMajorityWriteConcern)
        .getStatus();
}

Status MovePrimarySourceManager::cleanStaleData(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kNeedCleanStaleData);

    if (MONGO_unlikely(hangInCleanStaleDataStage.shouldFail())) {
        LOGV2(4908701, "Hit hangInCleanStaleDataStage");
        hangInCleanStaleDataStage.pauseWhileSet(opCtx);
    }

    // Only drop the cloned (unsharded) collections.
    DBDirectClient client(opCtx);
    for (auto& coll : _clonedColls) {
        BSONObj dropCollResult;
        client.runCommand(_dbname.toString(), BSON("drop" << coll.coll()), dropCollResult);
        Status dropStatus = getStatusFromCommandResult(dropCollResult);
        if (!dropStatus.isOK()) {
            LOGV2(22045,
                  "Failed to drop cloned collection {namespace} in movePrimary: {error}",
                  "Failed to drop cloned collection in movePrimary",
                  "namespace"_attr = coll,
                  "error"_attr = redact(dropStatus));
        }
    }

    _state = kDone;
    return Status::OK();
}

void MovePrimarySourceManager::cleanupOnError(OperationContext* opCtx) {
    if (_state == kDone) {
        return;
    }

    ShardingLogging::get(opCtx)->logChange(
        opCtx,
        "movePrimary.error",
        _dbname.toString(),
        _buildMoveLogEntry(_dbname.toString(), _fromShard.toString(), _toShard.toString()),
        ShardingCatalogClient::kMajorityWriteConcern);

    try {
        _cleanup(opCtx);
    } catch (const ExceptionForCat<ErrorCategory::NotPrimaryError>& ex) {
        BSONObjBuilder requestArgsBSON;
        _requestArgs.serialize(&requestArgsBSON);
        LOGV2_WARNING(22046,
                      "Failed to clean up movePrimary with request parameters {request} due to: "
                      "{error}",
                      "Failed to clean up movePrimary",
                      "request"_attr = redact(requestArgsBSON.obj()),
                      "error"_attr = redact(ex));
    }
}

void MovePrimarySourceManager::_cleanup(OperationContext* opCtx) {
    invariant(_state != kDone);

    {
        // Unregister from the database's sharding state if we're still registered.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetDb autoDb(opCtx, getNss().dbName(), MODE_IX);

        auto dss = DatabaseShardingState::get(opCtx, getNss().db());
        dss->clearMovePrimarySourceManager(opCtx);
        DatabaseHolder::get(opCtx)->clearDbInfo(opCtx,
                                                DatabaseName(boost::none, getNss().toString()));
        // Leave the critical section if we're still registered.
        dss->exitCriticalSection(opCtx, _critSecReason);
    }

    if (_state == kCriticalSection || _state == kCloneCompleted) {
        // Clear the 'minOpTime recovery' document so that the next time a node from this shard
        // becomes a primary, it won't have to recover the config server optime.
        ShardingStateRecovery::endMetadataOp(opCtx);
    }

    // If we're in the kCloneCompleted state, then we need to do the last step of cleaning up
    // now-stale data on the old primary. Otherwise, indicate that we're done.
    if (_state != kCloneCompleted) {
        _state = kDone;
    }

    return;
}

}  // namespace mongo

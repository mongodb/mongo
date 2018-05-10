/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/move_primary_source_manager.h"

#include "mongo/client/connpool.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_shard_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using namespace shardmetadatautil;

MovePrimarySourceManager::MovePrimarySourceManager(OperationContext* opCtx,
                                                   ShardMovePrimary requestArgs,
                                                   StringData dbname,
                                                   ShardId& fromShard,
                                                   ShardId& toShard)
    : _requestArgs(std::move(requestArgs)),
      _dbname(dbname),
      _fromShard(std::move(fromShard)),
      _toShard(std::move(toShard)) {}

MovePrimarySourceManager::~MovePrimarySourceManager() {}

NamespaceString MovePrimarySourceManager::getNss() const {
    return _requestArgs.get_movePrimary();
}

Status MovePrimarySourceManager::clone(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCreated);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });

    log() << "Moving " << _dbname << " primary from: " << _fromShard << " to: " << _toShard;

    // Record start in changelog
    uassertStatusOK(Grid::get(opCtx)->catalogClient()->logChange(
        opCtx,
        "movePrimary.start",
        _dbname.toString(),
        _buildMoveLogEntry(_dbname.toString(), _fromShard.toString(), _toShard.toString()),
        ShardingCatalogClient::kMajorityWriteConcern));

    {
        // Register for notifications from the replication subsystem
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());

        // We use AutoGetOrCreateDb the first time just in case movePrimary was called before any
        // data was inserted into the database.
        AutoGetOrCreateDb autoDb(opCtx, getNss().toString(), MODE_X);
        DatabaseShardingState::get(autoDb.getDb()).setMovePrimarySourceManager(opCtx, this);
    }

    _state = kCloning;

    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto fromShardObj = uassertStatusOK(shardRegistry->getShard(opCtx, _fromShard));
    auto toShardObj = uassertStatusOK(shardRegistry->getShard(opCtx, _toShard));

    BSONObjBuilder cloneCatalogDataCommandBuilder;
    cloneCatalogDataCommandBuilder << "_cloneCatalogData" << _dbname << "from"
                                   << fromShardObj->getConnString().toString();


    auto cloneCommandResponse = toShardObj->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        "admin",
        CommandHelpers::appendMajorityWriteConcern(cloneCatalogDataCommandBuilder.obj()),
        Shard::RetryPolicy::kIdempotent);

    auto cloneCommandStatus = Shard::CommandResponse::getEffectiveStatus(cloneCommandResponse);

    uassertStatusOK(cloneCommandStatus);

    auto clonedCollsArray = cloneCommandResponse.getValue().response["clonedColls"];
    for (const auto& elem : clonedCollsArray.Obj()) {
        if (elem.type() == String) {
            _clonedColls.push_back(NamespaceString(elem.String()));
        }
    }

    _state = kCloneCompleted;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MovePrimarySourceManager::enterCriticalSection(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCloneCompleted);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });

    // Mark the shard as running a critical operation that requires recovery on crash.
    uassertStatusOK(ShardingStateRecovery::startMetadataOp(opCtx));

    {
        // The critical section must be entered with the database X lock in order to ensure there
        // are no writes which could have entered and passed the database version check just before
        // we entered the critical section, but will potentially complete after we left it.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetDb autoDb(opCtx, getNss().toString(), MODE_X);

        if (!autoDb.getDb()) {
            uasserted(ErrorCodes::ConflictingOperationInProgress,
                      str::stream() << "The database " << getNss().toString()
                                    << " was dropped during the movePrimary operation.");
        }

        // IMPORTANT: After this line, the critical section is in place and needs to be signaled
        DatabaseShardingState::get(autoDb.getDb()).enterCriticalSectionCatchUpPhase(opCtx);
    }

    _state = kCriticalSection;

    // Persist a signal to secondaries that we've entered the critical section. This will cause
    // secondaries to refresh their routing table when next accessed, which will block behind the
    // critical section. This ensures causal consistency by preventing a stale mongos with a cluster
    // time inclusive of the move primary config commit update from accessing secondary data.
    // Note: this write must occur after the critSec flag is set, to ensure the secondary refresh
    // will stall behind the flag.
    Status signalStatus =
        updateShardDatabasesEntry(opCtx,
                                  BSON(ShardDatabaseType::name() << getNss().toString()),
                                  BSONObj(),
                                  BSON(ShardDatabaseType::enterCriticalSectionCounter() << 1),
                                  false /*upsert*/);
    if (!signalStatus.isOK()) {
        return {
            ErrorCodes::OperationFailed,
            str::stream() << "Failed to persist critical section signal for secondaries due to: "
                          << signalStatus.toString()};
    }

    log() << "movePrimary successfully entered critical section";

    scopedGuard.Dismiss();
    return Status::OK();
}

Status MovePrimarySourceManager::commitOnConfig(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCriticalSection);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });

    ConfigsvrCommitMovePrimary commitMovePrimaryRequest;
    commitMovePrimaryRequest.set_configsvrCommitMovePrimary(getNss().ns());
    commitMovePrimaryRequest.setTo(_toShard.toString());

    {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetDb autoDb(opCtx, getNss().toString(), MODE_X);

        if (!autoDb.getDb()) {
            uasserted(ErrorCodes::ConflictingOperationInProgress,
                      str::stream() << "The database " << getNss().toString()
                                    << " was dropped during the movePrimary operation.");
        }

        // Read operations must begin to wait on the critical section just before we send the
        // commit operation to the config server
        DatabaseShardingState::get(autoDb.getDb()).enterCriticalSectionCommitPhase(opCtx);
    }

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    BSONObj finalCommandObj;
    auto commitMovePrimaryResponse = configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        CommandHelpers::appendMajorityWriteConcern(CommandHelpers::appendPassthroughFields(
            finalCommandObj, commitMovePrimaryRequest.toBSON())),
        Shard::RetryPolicy::kIdempotent);

    auto commitStatus = Shard::CommandResponse::getEffectiveStatus(commitMovePrimaryResponse);

    if (!commitStatus.isOK()) {
        // Need to get the latest optime in case the refresh request goes to a secondary --
        // otherwise the read won't wait for the write that _configsvrCommitMovePrimary may have
        // done
        log() << "Error occurred while committing the movePrimary. Performing a majority write "
                 "against the config server to obtain its latest optime"
              << causedBy(redact(commitStatus));

        Status validateStatus = Grid::get(opCtx)->catalogClient()->logChange(
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
            AutoGetDb autoDb(opCtx, getNss().toString(), MODE_X);

            if (!autoDb.getDb()) {
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream() << "The database " << getNss().toString()
                                        << " was dropped during the movePrimary operation.");
            }

            if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, getNss())) {
                DatabaseShardingState::get(autoDb.getDb()).setDbVersion(opCtx, boost::none);
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
                                  << " due to "
                                  << redact(commitStatus)
                                  << ". Updating the optime with a write before clearing the "
                                  << "version also failed"));

        // If we can validate but the commit still failed, return the status.
        return commitStatus;
    }

    _state = kCommitted;

    _cleanup(opCtx);

    uassertStatusOK(Grid::get(opCtx)->catalogClient()->logChange(
        opCtx,
        "movePrimary.commit",
        _dbname.toString(),
        _buildMoveLogEntry(_dbname.toString(), _fromShard.toString(), _toShard.toString()),
        ShardingCatalogClient::kMajorityWriteConcern));

    scopedGuard.Dismiss();

    _state = kNeedCleanStaleData;

    return Status::OK();
}

Status MovePrimarySourceManager::cleanStaleData(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kNeedCleanStaleData);

    // Only drop the cloned (unsharded) collections.
    DBDirectClient client(opCtx);
    for (auto& coll : _clonedColls) {
        BSONObj dropCollResult;
        client.runCommand(_dbname.toString(), BSON("drop" << coll.coll()), dropCollResult);
        Status dropStatus = getStatusFromCommandResult(dropCollResult);
        if (!dropStatus.isOK()) {
            log() << "failed to drop cloned collection " << coll << causedBy(redact(dropStatus));
        }
    }

    _state = kDone;
    return Status::OK();
}


void MovePrimarySourceManager::cleanupOnError(OperationContext* opCtx) {
    if (_state == kDone) {
        return;
    }

    try {
        uassertStatusOK(Grid::get(opCtx)->catalogClient()->logChange(
            opCtx,
            "movePrimary.error",
            _dbname.toString(),
            _buildMoveLogEntry(_dbname.toString(), _fromShard.toString(), _toShard.toString()),
            ShardingCatalogClient::kMajorityWriteConcern));

        _cleanup(opCtx);
    } catch (const ExceptionForCat<ErrorCategory::NotMasterError>& ex) {
        BSONObjBuilder requestArgsBSON;
        _requestArgs.serialize(&requestArgsBSON);
        warning() << "Failed to clean up movePrimary: " << redact(requestArgsBSON.obj())
                  << "due to: " << redact(ex);
    }
}

void MovePrimarySourceManager::_cleanup(OperationContext* opCtx) {
    invariant(_state != kDone);

    {
        // Unregister from the database's sharding state if we're still registered.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetDb autoDb(opCtx, getNss().toString(), MODE_X);

        if (autoDb.getDb()) {
            DatabaseShardingState::get(autoDb.getDb()).clearMovePrimarySourceManager(opCtx);

            // Leave the critical section if we're still registered.
            DatabaseShardingState::get(autoDb.getDb()).exitCriticalSection(opCtx, boost::none);
        }
    }

    if (_state == kCriticalSection || _state == kCloneCompleted) {

        // TODO SERVER-32608
        // Wait for writes to 'config.cache.databases' to flush before removing the 'minOpTime
        // 'recovery' document.

        // Clear the 'minOpTime recovery' document so that the next time a node from this shard
        // becomes a primary, it won't have to recover the config server optime.
        ShardingStateRecovery::endMetadataOp(opCtx);
    }

    // If we're in the kCommitted state, then we need to do the last step of cleaning up
    // now-stale data on the old primary. Otherwise, indicate that we're done.
    if (_state != kCommitted) {
        _state = kDone;
    }

    return;
}

}  // namespace mongo

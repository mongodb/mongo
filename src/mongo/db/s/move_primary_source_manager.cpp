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
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MovePrimarySourceManager::MovePrimarySourceManager(OperationContext* opCtx,
                                                   ShardMovePrimary requestArgs,
                                                   StringData dbname,
                                                   std::unique_ptr<Shard> fromShard,
                                                   std::unique_ptr<Shard> toShard)
    : _requestArgs(std::move(requestArgs)),
      _dbname(dbname),
      _fromShard(std::move(fromShard)),
      _toShard(std::move(toShard)) {
    // TODO: Add exceptions for missing database version or stale database version.
}

MovePrimarySourceManager::~MovePrimarySourceManager() {}

NamespaceString MovePrimarySourceManager::getNss() const {
    return _requestArgs.get_movePrimary();
}

Status MovePrimarySourceManager::clone(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCreated);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });

    log() << "Moving " << _dbname << " primary from: " << _fromShard->toString()
          << " to: " << _toShard->toString();

    // Record start in changelog
    uassertStatusOK(Grid::get(opCtx)->catalogClient()->logChange(
        opCtx,
        "movePrimary.start",
        _dbname.toString(),
        _buildMoveLogEntry(_dbname.toString(), _fromShard->toString(), _toShard->toString()),
        ShardingCatalogClient::kMajorityWriteConcern));

    {
        // Register for notifications from the replication subsystem
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetDb autoDb(opCtx, getNss().toString(), MODE_X);
        DatabaseShardingState::get(autoDb.getDb()).setMovePrimarySourceManager(opCtx, this);
    }

    _state = kCloning;

    BSONObjBuilder cloneCatalogDataCommandBuilder;
    cloneCatalogDataCommandBuilder << "_cloneCatalogData" << _fromShard->getConnString().toString();

    auto cloneCommandStatus =
        Shard::CommandResponse::getEffectiveStatus(_toShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            "admin",
            CommandHelpers::appendMajorityWriteConcern(cloneCatalogDataCommandBuilder.obj()),
            Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(cloneCommandStatus);

    _state = kCloneCompleted;
    scopedGuard.Dismiss();
    return Status::OK();
}

void MovePrimarySourceManager::cleanupOnError(OperationContext* opCtx) {
    if (_state == kDone) {
        return;
    }

    uassertStatusOK(Grid::get(opCtx)->catalogClient()->logChange(
        opCtx,
        "movePrimary.error",
        _dbname.toString(),
        _buildMoveLogEntry(_dbname.toString(), _fromShard->toString(), _toShard->toString()),
        ShardingCatalogClient::kMajorityWriteConcern));

    try {
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
        // Unregister from the database's sharding state
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetDb autoDb(opCtx, getNss().toString(), MODE_X);
        DatabaseShardingState::get(autoDb.getDb()).clearMovePrimarySourceManager(opCtx);
    }

    _state = kDone;

    return;
}

}  // namespace mongo

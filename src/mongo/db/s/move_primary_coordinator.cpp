/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/move_primary_coordinator.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/s/move_primary_source_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_id.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

void MovePrimaryCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    stdx::lock_guard lk{_docMutex};
    cmdInfoBuilder->append("request", BSON(_doc.kToShardIdFieldName << _doc.getToShardId()));
};

void MovePrimaryCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    // If we have two shard collections on the same namespace, then the arguments must be the same.
    const auto otherDoc = MovePrimaryCoordinatorDocument::parse(
        IDLParserContext("MovePrimaryCoordinatorDocument"), doc);

    uassert(
        ErrorCodes::ConflictingOperationInProgress,
        "Another move primary with different arguments is already running for the same namespace",
        _doc.getToShardId() == otherDoc.getToShardId());
}


ExecutorFuture<void> MovePrimaryCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            // Any error should terminate the coordinator, even if it is a retryable error, this way
            // we have a movePrimary with a similar behavior of the previous one.
            _completeOnError = true;

            auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
            // Make sure we're as up-to-date as possible with shard information. This catches the
            // case where we might have changed a shard's host by removing/adding a shard with the
            // same name.
            shardRegistry->reload(opCtx);

            const auto& dbName = nss().db();
            const auto& toShard =
                uassertStatusOK(shardRegistry->getShard(opCtx, _doc.getToShardId()));

            const auto& selfShardId = ShardingState::get(opCtx)->shardId();
            if (selfShardId == toShard->getId()) {
                LOGV2(5275803,
                      "Database already on the requested primary shard",
                      "db"_attr = dbName,
                      "shardId"_attr = _doc.getToShardId());
                // The database primary is already the `to` shard
                return;
            }

            // Enable write blocking bypass to allow cloning and droping the stale collections even
            // if user writes are currently disallowed.
            WriteBlockBypass::get(opCtx).set(true);

            ShardMovePrimary movePrimaryRequest(nss(), _doc.getToShardId().toString());

            auto primaryId = selfShardId;
            auto toId = toShard->getId();
            MovePrimarySourceManager movePrimarySourceManager(
                opCtx, movePrimaryRequest, dbName, primaryId, toId);
            uassertStatusOK(movePrimarySourceManager.clone(opCtx));
            uassertStatusOK(movePrimarySourceManager.enterCriticalSection(opCtx));
            uassertStatusOK(movePrimarySourceManager.commitOnConfig(opCtx));
            uassertStatusOK(movePrimarySourceManager.cleanStaleData(opCtx));
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(5275804,
                        "Error running move primary",
                        "database"_attr = nss().db(),
                        "to"_attr = _doc.getToShardId(),
                        "error"_attr = redact(status));

            return status;
        });
}

}  // namespace mongo

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

#include "mongo/db/s/move_primary_coordinator.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

MovePrimaryCoordinator::MovePrimaryCoordinator(ShardingDDLCoordinatorService* service,
                                               const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(service, "MovePrimaryCoordinator", initialState),
      _dbName(nss().dbName()) {}

bool MovePrimaryCoordinator::canAlwaysStartWhenUserWritesAreDisabled() const {
    return true;
}

StringData MovePrimaryCoordinator::serializePhase(const Phase& phase) const {
    return MovePrimaryCoordinatorPhase_serializer(phase);
}

void MovePrimaryCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    stdx::lock_guard lk(_docMutex);
    cmdInfoBuilder->append(
        "request",
        BSON(MovePrimaryCoordinatorDocument::kToShardIdFieldName << _doc.getToShardId()));
};

void MovePrimaryCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = MovePrimaryCoordinatorDocument::parse(
        IDLParserContext("MovePrimaryCoordinatorDocument"), doc);
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another movePrimary operation with different arguments is already running ont the "
            "same database",
            _doc.getToShardId() == otherDoc.getToShardId());
}

ExecutorFuture<void> MovePrimaryCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(Phase::kCheckPreconditions,
                                 [this, anchor = shared_from_this()] {
                                     auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);
                                 }))
        .then(_buildPhaseHandler(Phase::kCloneCatalogData,
                                 [this, anchor = shared_from_this()] {
                                     auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);
                                 }))
        .then(_buildPhaseHandler(Phase::kEnterCriticalSection,
                                 [this, anchor = shared_from_this()] {
                                     auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);
                                 }))
        .then(_buildPhaseHandler(Phase::kCommitMetadataChanges,
                                 [this, anchor = shared_from_this()] {
                                     auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);
                                 }))
        .then(_buildPhaseHandler(Phase::kCleanStaleData,
                                 [this, anchor = shared_from_this()] {
                                     auto opCtxHolder = cc().makeOperationContext();
                                     auto* opCtx = opCtxHolder.get();
                                     getForwardableOpMetadata().setOn(opCtx);
                                 }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(7120000,
                        "Error running movePrimary",
                        "database"_attr = nss(),
                        "to"_attr = _doc.getToShardId(),
                        "error"_attr = redact(status));

            return status;
        });
}

}  // namespace mongo

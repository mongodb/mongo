/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

#include "mongo/db/s/resharding/resharding_recipient_service.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/logv2/log.h"

namespace mongo {

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingRecipientService::constructInstance(
    BSONObj initialState) const {
    return std::make_shared<RecipientStateMachine>(std::move(initialState));
}

RecipientStateMachine::RecipientStateMachine(const BSONObj& recipientDoc)
    : repl::PrimaryOnlyService::TypedInstance<RecipientStateMachine>(),
      _recipientDoc(ReshardingRecipientDocument::parse(
          IDLParserErrorContext("ReshardingRecipientDocument"), recipientDoc)),
      _id(_recipientDoc.getCommonReshardingMetadata().get_id()) {}

SemiFuture<void> RecipientStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this] { _createTemporaryReshardingCollectionThenTransitionToInitialized(); })
        .then([this, executor] {
            return _awaitAllDonorsPreparedToDonateThenTransitionToCloning(executor);
        })
        .then([this] { return _cloneThenTransitionToApplying(); })
        .then([this] { return _applyThenTransitionToSteadyState(); })
        .then([this, executor] {
            return _awaitAllDonorsMirroringThenTransitionToStrictConsistency(executor);
        })
        .then([this, executor] {
            return _awaitCoordinatorHasCommittedThenTransitionToRenaming(executor);
        })
        .then([this] { _renameTemporaryReshardingCollectionThenDeleteLocalState(); })
        .onError([this](Status status) {
            LOGV2(4956500,
                  "Resharding operation recipient state machine failed",
                  "namespace"_attr = _recipientDoc.getNss().ns(),
                  "reshardingId"_attr = _id,
                  "error"_attr = status);
            // TODO SERVER-50584 Report errors to the coordinator so that the resharding operation
            // can be aborted.
            this->_transitionStateToError(status);
            return status;
        })
        .semi();
}

void onReshardingFieldsChanges(boost::optional<TypeCollectionReshardingFields> reshardingFields) {}

void onDonorReportsMirroring(const ShardId& donor) {}

void RecipientStateMachine::_createTemporaryReshardingCollectionThenTransitionToInitialized() {
    if (_recipientDoc.getState() > RecipientStateEnum::kInitializing) {
        return;
    }

    _transitionState(RecipientStateEnum::kInitialized);
}

ExecutorFuture<void> RecipientStateMachine::_awaitAllDonorsPreparedToDonateThenTransitionToCloning(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientDoc.getState() > RecipientStateEnum::kInitialized) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _allDonorsPreparedToDonate.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(RecipientStateEnum::kCloning);
    });
}

void RecipientStateMachine::_cloneThenTransitionToApplying() {
    if (_recipientDoc.getState() > RecipientStateEnum::kCloning) {
        return;
    }

    _transitionState(RecipientStateEnum::kApplying);
}

void RecipientStateMachine::_applyThenTransitionToSteadyState() {
    if (_recipientDoc.getState() > RecipientStateEnum::kApplying) {
        return;
    }

    _transitionState(RecipientStateEnum::kSteadyState);
}

ExecutorFuture<void>
RecipientStateMachine::_awaitAllDonorsMirroringThenTransitionToStrictConsistency(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientDoc.getState() > RecipientStateEnum::kSteadyState) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _allDonorsMirroring.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(RecipientStateEnum::kStrictConsistency);
    });
}

ExecutorFuture<void> RecipientStateMachine::_awaitCoordinatorHasCommittedThenTransitionToRenaming(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientDoc.getState() > RecipientStateEnum::kStrictConsistency) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _coordinatorHasCommitted.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(RecipientStateEnum::kRenaming);
    });
}

void RecipientStateMachine::_renameTemporaryReshardingCollectionThenDeleteLocalState() {
    if (_recipientDoc.getState() > RecipientStateEnum::kRenaming) {
        return;
    }

    _transitionState(RecipientStateEnum::kDone);
}

void RecipientStateMachine::_transitionState(RecipientStateEnum endState) {
    ReshardingRecipientDocument replacementDoc(_recipientDoc);
    replacementDoc.setState(endState);
    _updateRecipientDocument(std::move(replacementDoc));
}

void RecipientStateMachine::_transitionStateToError(const Status& status) {
    ReshardingRecipientDocument replacementDoc(_recipientDoc);
    replacementDoc.setState(RecipientStateEnum::kError);
    _updateRecipientDocument(std::move(replacementDoc));
}

void RecipientStateMachine::_updateRecipientDocument(ReshardingRecipientDocument&& replacementDoc) {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);
    store.update(opCtx.get(),
                 BSON(ReshardingRecipientDocument::k_idFieldName << _id),
                 replacementDoc.toBSON(),
                 WriteConcerns::kMajorityWriteConcern);

    _recipientDoc = replacementDoc;
}

}  // namespace mongo

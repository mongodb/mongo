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

#include "mongo/db/s/resharding/resharding_donor_service.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/logv2/log.h"

namespace mongo {

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingDonorService::constructInstance(
    BSONObj initialState) const {
    return std::make_shared<DonorStateMachine>(std::move(initialState));
}

DonorStateMachine::DonorStateMachine(const BSONObj& donorDoc)
    : repl::PrimaryOnlyService::TypedInstance<DonorStateMachine>(),
      _donorDoc(ReshardingDonorDocument::parse(IDLParserErrorContext("ReshardingDonorDocument"),
                                               donorDoc)),
      _id(_donorDoc.getCommonReshardingMetadata().get_id()) {}

SemiFuture<void> DonorStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this] { _onInitializingCalculateMinFetchTimestampThenBeginDonating(); })
        .then([this, executor] {
            return _awaitAllRecipientsDoneApplyingThenStartMirroring(executor);
        })
        .then([this, executor] {
            return _awaitCoordinatorHasCommittedThenTransitionToDropping(executor);
        })
        .then([this] { return _dropOriginalCollectionThenDeleteLocalState(); })
        .onError([this](Status status) {
            LOGV2(4956400,
                  "Resharding operation donor state machine failed",
                  "namespace"_attr = _donorDoc.getNss().ns(),
                  "reshardingId"_attr = _id,
                  "error"_attr = status);
            // TODO SERVER-50584 Report errors to the coordinator so that the resharding operation
            // can be aborted.
            this->_transitionStateToError(status);
            return status;
        })
        .semi();
}

void DonorStateMachine::onReshardingFieldsChanges(
    boost::optional<TypeCollectionReshardingFields> reshardingFields) {}

void DonorStateMachine::_onInitializingCalculateMinFetchTimestampThenBeginDonating() {
    if (_donorDoc.getState() > DonorStateEnum::kInitializing) {
        return;
    }

    // TODO SERVER-50021 Calculate minFetchTimestamp and send to coordinator.

    _transitionState(DonorStateEnum::kDonating);
}

ExecutorFuture<void> DonorStateMachine::_awaitAllRecipientsDoneApplyingThenStartMirroring(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_donorDoc.getState() > DonorStateEnum::kDonating) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _allRecipientsDoneApplying.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(DonorStateEnum::kMirroring);
    });
}

ExecutorFuture<void> DonorStateMachine::_awaitCoordinatorHasCommittedThenTransitionToDropping(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_donorDoc.getState() > DonorStateEnum::kMirroring) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _coordinatorHasCommitted.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(DonorStateEnum::kDropping);
    });
}

void DonorStateMachine::_dropOriginalCollectionThenDeleteLocalState() {
    if (_donorDoc.getState() > DonorStateEnum::kDropping) {
        return;
    }

    _transitionState(DonorStateEnum::kDone);
}

void DonorStateMachine::_transitionState(DonorStateEnum endState) {
    ReshardingDonorDocument replacementDoc(_donorDoc);
    replacementDoc.setState(endState);
    _updateDonorDocument(std::move(replacementDoc));
}

void DonorStateMachine::_transitionStateToError(const Status& status) {
    ReshardingDonorDocument replacementDoc(_donorDoc);
    replacementDoc.setState(DonorStateEnum::kError);
    _updateDonorDocument(std::move(replacementDoc));
}

void DonorStateMachine::_updateDonorDocument(ReshardingDonorDocument&& replacementDoc) {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<ReshardingDonorDocument> store(
        NamespaceString::kDonorReshardingOperationsNamespace);
    store.update(opCtx.get(),
                 BSON(ReshardingDonorDocument::k_idFieldName << _id),
                 replacementDoc.toBSON(),
                 WriteConcerns::kMajorityWriteConcern);

    _donorDoc = replacementDoc;
}

}  // namespace mongo

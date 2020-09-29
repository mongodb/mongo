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

#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
Timestamp generateMinFetchTimestamp(const ReshardingDonorDocument& donorDoc) {
    auto opCtx = cc().makeOperationContext();

    // Do a no-op write and use the OpTime as the minFetchTimestamp
    {
        AutoGetOplog oplogWrite(opCtx.get(), OplogAccessMode::kWrite);
        writeConflictRetry(
            opCtx.get(),
            "resharding donor minFetchTimestamp",
            NamespaceString::kRsOplogNamespace.ns(),
            [&] {
                const std::string msg = str::stream()
                    << "All future oplog entries on the namespace " << donorDoc.getNss().ns()
                    << " must include a 'destinedRecipient' field";
                WriteUnitOfWork wuow(opCtx.get());
                opCtx->getClient()->getServiceContext()->getOpObserver()->onInternalOpMessage(
                    opCtx.get(),
                    donorDoc.getNss(),
                    donorDoc.getExistingUUID(),
                    {},
                    BSON("msg" << msg),
                    boost::none,
                    boost::none,
                    boost::none,
                    boost::none);
                wuow.commit();
            });
    }

    auto generatedOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    WriteConcernResult result;
    uassertStatusOK(waitForWriteConcern(
        opCtx.get(), generatedOpTime, WriteConcerns::kMajorityWriteConcern, &result));

    // TODO notify storage engine to pin the minFetchTimestamp

    return generatedOpTime.getTimestamp();
}
}  // namespace

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingDonorService::constructInstance(
    BSONObj initialState) const {
    return std::make_shared<DonorStateMachine>(std::move(initialState));
}

ReshardingDonorService::DonorStateMachine::DonorStateMachine(const BSONObj& donorDoc)
    : repl::PrimaryOnlyService::TypedInstance<DonorStateMachine>(),
      _donorDoc(ReshardingDonorDocument::parse(IDLParserErrorContext("ReshardingDonorDocument"),
                                               donorDoc)),
      _id(_donorDoc.getCommonReshardingMetadata().get_id()) {}

ReshardingDonorService::DonorStateMachine::~DonorStateMachine() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_allRecipientsDoneApplying.getFuture().isReady());
    invariant(_coordinatorHasCommitted.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

void ReshardingDonorService::DonorStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {
    ExecutorFuture<void>(**executor)
        .then([this] { _transitionState(DonorStateEnum::kPreparingToDonate); })
        .then([this] { _onPreparingToDonateCalculateMinFetchTimestampThenBeginDonating(); })
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
        .getAsync([this](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got here.
                return;
            }

            if (status.isOK()) {
                _completionPromise.emplaceValue();
            } else {
                _completionPromise.setError(status);
            }
        });
}

void ReshardingDonorService::DonorStateMachine::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_allRecipientsDoneApplying.getFuture().isReady()) {
        _allRecipientsDoneApplying.setError(status);
    }

    if (!_coordinatorHasCommitted.getFuture().isReady()) {
        _coordinatorHasCommitted.setError(status);
    }

    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

void ReshardingDonorService::DonorStateMachine::onReshardingFieldsChanges(
    boost::optional<TypeCollectionReshardingFields> reshardingFields) {}

void ReshardingDonorService::DonorStateMachine::
    _onPreparingToDonateCalculateMinFetchTimestampThenBeginDonating() {
    if (_donorDoc.getState() > DonorStateEnum::kPreparingToDonate) {
        invariant(_donorDoc.getMinFetchTimestamp());
        return;
    }

    auto minFetchTimestamp = generateMinFetchTimestamp(_donorDoc);
    _transitionState(DonorStateEnum::kDonating, minFetchTimestamp);
}

ExecutorFuture<void>
ReshardingDonorService::DonorStateMachine::_awaitAllRecipientsDoneApplyingThenStartMirroring(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_donorDoc.getState() > DonorStateEnum::kDonating) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _allRecipientsDoneApplying.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(DonorStateEnum::kMirroring);
    });
}

ExecutorFuture<void>
ReshardingDonorService::DonorStateMachine::_awaitCoordinatorHasCommittedThenTransitionToDropping(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_donorDoc.getState() > DonorStateEnum::kMirroring) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _coordinatorHasCommitted.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(DonorStateEnum::kDropping);
    });
}

void ReshardingDonorService::DonorStateMachine::_dropOriginalCollectionThenDeleteLocalState() {
    if (_donorDoc.getState() > DonorStateEnum::kDropping) {
        return;
    }

    _transitionState(DonorStateEnum::kDone);
}

void ReshardingDonorService::DonorStateMachine::_transitionState(
    DonorStateEnum endState, boost::optional<Timestamp> minFetchTimestamp) {
    ReshardingDonorDocument replacementDoc(_donorDoc);
    replacementDoc.setState(endState);
    if (minFetchTimestamp) {
        auto& minFetchTimestampStruct = replacementDoc.getMinFetchTimestampStruct();
        if (minFetchTimestampStruct.getMinFetchTimestamp())
            invariant(minFetchTimestampStruct.getMinFetchTimestamp().get() ==
                      minFetchTimestamp.get());

        minFetchTimestampStruct.setMinFetchTimestamp(std::move(minFetchTimestamp));
    }

    _updateDonorDocument(std::move(replacementDoc));
}

void ReshardingDonorService::DonorStateMachine::_transitionStateToError(const Status& status) {
    ReshardingDonorDocument replacementDoc(_donorDoc);
    replacementDoc.setState(DonorStateEnum::kError);
    _updateDonorDocument(std::move(replacementDoc));
}

void ReshardingDonorService::DonorStateMachine::_updateDonorDocument(
    ReshardingDonorDocument&& replacementDoc) {
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

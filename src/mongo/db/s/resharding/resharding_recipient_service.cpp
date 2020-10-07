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

#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {

namespace resharding {

void createTemporaryReshardingCollectionLocally(OperationContext* opCtx,
                                                const NamespaceString& reshardingNss,
                                                Timestamp fetchTimestamp) {
    LOGV2_DEBUG(
        5002300, 1, "Creating temporary resharding collection", "namespace"_attr = reshardingNss);

    auto catalogCache = Grid::get(opCtx)->catalogCache();
    auto reshardingCm =
        uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, reshardingNss));
    uassert(
        5002301,
        "Expected cached metadata for resharding temporary collection to have resharding fields",
        reshardingCm.getReshardingFields() &&
            reshardingCm.getReshardingFields()->getRecipientFields());
    auto originalNss =
        reshardingCm.getReshardingFields()->getRecipientFields()->getOriginalNamespace();

    // Load the original collection's options from the database's primary shard.
    auto [collOptions, uuid] = sharded_agg_helpers::shardVersionRetry(
        opCtx,
        catalogCache,
        reshardingNss,
        "loading collection options to create temporary resharding collection"_sd,
        [&]() -> MigrationDestinationManager::CollectionOptionsAndUUID {
            auto originalCm =
                uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, originalNss));
            uassert(ErrorCodes::InvalidUUID,
                    "Expected cached metadata for resharding temporary collection to have a UUID",
                    originalCm.getUUID());
            return MigrationDestinationManager::getCollectionOptions(
                opCtx,
                NamespaceStringOrUUID(originalNss.db().toString(), *originalCm.getUUID()),
                originalCm.dbPrimary(),
                originalCm,
                fetchTimestamp);
        });

    // Load the original collection's indexes from the shard that owns the global minimum chunk.
    auto [indexes, idIndex] = sharded_agg_helpers::shardVersionRetry(
        opCtx,
        catalogCache,
        reshardingNss,
        "loading indexes to create temporary resharding collection"_sd,
        [&]() -> MigrationDestinationManager::IndexesAndIdIndex {
            auto originalCm =
                uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, originalNss));
            uassert(ErrorCodes::NamespaceNotSharded,
                    str::stream() << "Expected collection " << originalNss << " to be sharded",
                    originalCm.isSharded());
            uassert(ErrorCodes::InvalidUUID,
                    "Expected cached metadata for resharding temporary collection to have a UUID",
                    originalCm.getUUID());
            auto indexShardId = originalCm.getMinKeyShardIdWithSimpleCollation();
            return MigrationDestinationManager::getCollectionIndexes(
                opCtx,
                NamespaceStringOrUUID(originalNss.db().toString(), *originalCm.getUUID()),
                indexShardId,
                originalCm,
                fetchTimestamp);
        });

    // Set the temporary resharding collection's UUID to the resharding UUID. Note that
    // BSONObj::addFields() replaces any fields that already exist.
    auto reshardingUUID = reshardingCm.getReshardingFields()->getUuid();
    collOptions = collOptions.addFields(BSON("uuid" << reshardingUUID));

    CollectionOptionsAndIndexes optionsAndIndexes = {reshardingUUID, indexes, idIndex, collOptions};
    MigrationDestinationManager::cloneCollectionIndexesAndOptions(
        opCtx, reshardingNss, optionsAndIndexes);
}

}  // namespace resharding

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingRecipientService::constructInstance(
    BSONObj initialState) const {
    return std::make_shared<RecipientStateMachine>(std::move(initialState));
}

ReshardingRecipientService::RecipientStateMachine::RecipientStateMachine(
    const BSONObj& recipientDoc)
    : repl::PrimaryOnlyService::TypedInstance<RecipientStateMachine>(),
      _recipientDoc(ReshardingRecipientDocument::parse(
          IDLParserErrorContext("ReshardingRecipientDocument"), recipientDoc)),
      _id(_recipientDoc.getCommonReshardingMetadata().get_id()) {}

ReshardingRecipientService::RecipientStateMachine::~RecipientStateMachine() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_allDonorsMirroring.getFuture().isReady());
    invariant(_coordinatorHasCommitted.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

void ReshardingRecipientService::RecipientStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {
    ExecutorFuture<void>(**executor)
        .then([this] { _transitionToCreatingTemporaryReshardingCollection(); })
        .then([this] { _createTemporaryReshardingCollectionThenTransitionToCloning(); })
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
        .getAsync([this](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got her.e
                return;
            }

            if (status.isOK()) {
                _completionPromise.emplaceValue();
            } else {
                // Set error on all promises
                _completionPromise.setError(status);
            }
        });
}

void ReshardingRecipientService::RecipientStateMachine::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lg(_mutex);

    if (!_allDonorsMirroring.getFuture().isReady()) {
        _allDonorsMirroring.setError(status);
    }

    if (!_coordinatorHasCommitted.getFuture().isReady()) {
        _coordinatorHasCommitted.setError(status);
    }

    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

void ReshardingRecipientService::RecipientStateMachine::onReshardingFieldsChanges(
    boost::optional<TypeCollectionReshardingFields> reshardingFields) {}

void ReshardingRecipientService::RecipientStateMachine::
    _transitionToCreatingTemporaryReshardingCollection() {
    if (_recipientDoc.getState() > RecipientStateEnum::kCreatingCollection) {
        return;
    }

    _transitionState(RecipientStateEnum::kCreatingCollection);
}

void ReshardingRecipientService::RecipientStateMachine::
    _createTemporaryReshardingCollectionThenTransitionToCloning() {
    if (_recipientDoc.getState() > RecipientStateEnum::kCreatingCollection) {
        return;
    }

    // TODO SERVER-51217: Call
    // resharding_recipient_service_util::createTemporaryReshardingCollectionLocally()

    _transitionState(RecipientStateEnum::kCloning);
}

void ReshardingRecipientService::RecipientStateMachine::_cloneThenTransitionToApplying() {
    if (_recipientDoc.getState() > RecipientStateEnum::kCloning) {
        return;
    }

    _transitionState(RecipientStateEnum::kApplying);
}

void ReshardingRecipientService::RecipientStateMachine::_applyThenTransitionToSteadyState() {
    if (_recipientDoc.getState() > RecipientStateEnum::kApplying) {
        return;
    }

    _transitionState(RecipientStateEnum::kSteadyState);
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitAllDonorsMirroringThenTransitionToStrictConsistency(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientDoc.getState() > RecipientStateEnum::kSteadyState) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _allDonorsMirroring.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(RecipientStateEnum::kStrictConsistency);
    });
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitCoordinatorHasCommittedThenTransitionToRenaming(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientDoc.getState() > RecipientStateEnum::kStrictConsistency) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _coordinatorHasCommitted.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(RecipientStateEnum::kRenaming);
    });
}

void ReshardingRecipientService::RecipientStateMachine::
    _renameTemporaryReshardingCollectionThenDeleteLocalState() {
    if (_recipientDoc.getState() > RecipientStateEnum::kRenaming) {
        return;
    }

    _transitionState(RecipientStateEnum::kDone);
}

void ReshardingRecipientService::RecipientStateMachine::_transitionState(
    RecipientStateEnum endState, boost::optional<Timestamp> fetchTimestamp) {
    ReshardingRecipientDocument replacementDoc(_recipientDoc);
    replacementDoc.setState(endState);
    if (endState == RecipientStateEnum::kCreatingCollection) {
        _insertRecipientDocument(replacementDoc);
        return;
    }

    emplaceFetchTimestampIfExists(replacementDoc, std::move(fetchTimestamp));

    _updateRecipientDocument(std::move(replacementDoc));
}

void ReshardingRecipientService::RecipientStateMachine::_transitionStateToError(
    const Status& status) {
    ReshardingRecipientDocument replacementDoc(_recipientDoc);
    replacementDoc.setState(RecipientStateEnum::kError);
    _updateRecipientDocument(std::move(replacementDoc));
}

void ReshardingRecipientService::RecipientStateMachine::_insertRecipientDocument(
    const ReshardingRecipientDocument& doc) {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);
    store.add(opCtx.get(), doc, WriteConcerns::kMajorityWriteConcern);

    _recipientDoc = doc;
}

void ReshardingRecipientService::RecipientStateMachine::_updateRecipientDocument(
    ReshardingRecipientDocument&& replacementDoc) {
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

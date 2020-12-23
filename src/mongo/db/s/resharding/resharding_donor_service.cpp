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

#include <fmt/format.h>

#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"

namespace mongo {

using namespace fmt::literals;

namespace {
ChunkManager getShardedCollectionRoutingInfoWithRefreshAndFlush(const NamespaceString& nss) {
    auto opCtx = cc().makeOperationContext();

    auto swRoutingInfo = Grid::get(opCtx.get())
                             ->catalogCache()
                             ->getShardedCollectionRoutingInfoWithRefresh(opCtx.get(), nss);
    auto routingInfo = uassertStatusOK(swRoutingInfo);

    CatalogCacheLoader::get(opCtx.get()).waitForCollectionFlush(opCtx.get(), nss);

    return routingInfo;
}

void refreshTemporaryReshardingCollection(const ReshardingDonorDocument& donorDoc) {
    auto tempNss =
        constructTemporaryReshardingNss(donorDoc.getNss().db(), donorDoc.getExistingUUID());
    std::ignore = getShardedCollectionRoutingInfoWithRefreshAndFlush(tempNss);
}

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

/**
 * Fulfills the promise if it is not already. Otherwise, does nothing.
 */
void ensureFulfilledPromise(WithLock lk, SharedPromise<void>& sp) {
    if (!sp.getFuture().isReady()) {
        sp.emplaceValue();
    }
}

void ensureFulfilledPromise(WithLock lk, SharedPromise<void>& sp, Status error) {
    if (!sp.getFuture().isReady()) {
        sp.setError(error);
    }
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
    invariant(_allRecipientsDoneCloning.getFuture().isReady());
    invariant(_allRecipientsDoneApplying.getFuture().isReady());
    invariant(_coordinatorHasCommitted.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

SemiFuture<void> ReshardingDonorService::DonorStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancelationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(
            [this] { _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData(); })
        .then([this, executor] {
            return _awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries(executor);
        })
        .then([this, executor] {
            return _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToMirror(executor);
        })
        .then([this] { _writeTransactionOplogEntryThenTransitionToMirroring(); })
        .then([this, executor] {
            return _awaitCoordinatorHasCommittedThenTransitionToDropping(executor);
        })
        .then([this, self = shared_from_this()] {
            // After this line, the shared_ptr stored in the PrimaryOnlyService's map for
            // the ReshardingDonorService Instance is removed. It is necessary to use
            // shared_from_this() to extend the lifetime for the remaining callbacks.
            return _dropOriginalCollectionThenDeleteLocalState();
        })
        .onError([this, self = shared_from_this()](Status status) {
            LOGV2(4956400,
                  "Resharding operation donor state machine failed",
                  "namespace"_attr = _donorDoc.getNss().ns(),
                  "reshardingId"_attr = _id,
                  "error"_attr = status);
            // TODO SERVER-50584 Report errors to the coordinator so that the resharding operation
            // can be aborted.
            _transitionStateToError(status);
            return status;
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got here.
                return;
            }

            if (status.isOK()) {
                _removeDonorDocument();
                _completionPromise.emplaceValue();
            } else {
                _completionPromise.setError(status);
            }
        })
        .semi();
}

void ReshardingDonorService::DonorStateMachine::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_allRecipientsDoneCloning.getFuture().isReady()) {
        _allRecipientsDoneCloning.setError(status);
    }

    if (!_allRecipientsDoneApplying.getFuture().isReady()) {
        _allRecipientsDoneApplying.setError(status);
    }

    if (!_finalOplogEntriesWritten.getFuture().isReady()) {
        _finalOplogEntriesWritten.setError(status);
    }

    if (!_coordinatorHasCommitted.getFuture().isReady()) {
        _coordinatorHasCommitted.setError(status);
    }

    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

void ReshardingDonorService::DonorStateMachine::onReshardingFieldsChanges(
    const TypeCollectionReshardingFields& reshardingFields) {
    auto coordinatorState = reshardingFields.getState();
    if (coordinatorState == CoordinatorStateEnum::kError) {
        // TODO SERVER-52838: Investigate if we want to have a special error code so the donor knows
        // when it has recieved the error from the coordinator rather than needing to report an
        // error to the coordinator.
        interrupt({ErrorCodes::InternalError,
                   "ReshardingDonorService observed CoordinatorStateEnum::kError"});
        return;
    }

    stdx::lock_guard<Latch> lk(_mutex);
    if (coordinatorState >= CoordinatorStateEnum::kApplying) {
        ensureFulfilledPromise(lk, _allRecipientsDoneCloning);
    }

    if (coordinatorState >= CoordinatorStateEnum::kMirroring) {
        ensureFulfilledPromise(lk, _allRecipientsDoneApplying);
    }

    if (coordinatorState >= CoordinatorStateEnum::kCommitted) {
        ensureFulfilledPromise(lk, _coordinatorHasCommitted);
    }
}

void ReshardingDonorService::DonorStateMachine::
    _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData() {
    if (_donorDoc.getState() > DonorStateEnum::kPreparingToDonate) {
        invariant(_donorDoc.getMinFetchTimestamp());
        return;
    }

    _insertDonorDocument(_donorDoc);

    // Recipient shards expect to read from the donor shard's existing sharded collection
    // and the config.cache.chunks collection of the temporary resharding collection using
    // {atClusterTime: <fetchTimestamp>}. Refreshing the temporary resharding collection on
    // the donor shards causes them to create the config.cache.chunks collection. Without
    // this refresh, the {atClusterTime: <fetchTimestamp>} read on the config.cache.chunks
    // namespace would fail with a SnapshotUnavailable error response.
    refreshTemporaryReshardingCollection(_donorDoc);

    auto minFetchTimestamp = generateMinFetchTimestamp(_donorDoc);
    _transitionStateAndUpdateCoordinator(DonorStateEnum::kDonatingInitialData, minFetchTimestamp);
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::
    _awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_donorDoc.getState() > DonorStateEnum::kDonatingInitialData) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _allRecipientsDoneCloning.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(DonorStateEnum::kDonatingOplogEntries);
    });
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::
    _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToMirror(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_donorDoc.getState() > DonorStateEnum::kDonatingOplogEntries) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _allRecipientsDoneApplying.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(DonorStateEnum::kPreparingToMirror);
    });
}

void ReshardingDonorService::DonorStateMachine::
    _writeTransactionOplogEntryThenTransitionToMirroring() {
    if (_donorDoc.getState() > DonorStateEnum::kPreparingToMirror) {
        return;
    }

    {
        const auto& nss = _donorDoc.getNss();
        const auto& nssUUID = _donorDoc.getExistingUUID();
        const auto& reshardingUUID = _donorDoc.get_id();
        auto opCtx = cc().makeOperationContext();
        auto rawOpCtx = opCtx.get();

        auto generateOplogEntry = [&](ShardId destinedRecipient) {
            repl::MutableOplogEntry oplog;
            oplog.setNss(nss);
            oplog.setOpType(repl::OpTypeEnum::kNoop);
            oplog.setUuid(nssUUID);
            oplog.setDestinedRecipient(destinedRecipient);
            oplog.setObject(
                BSON("msg" << fmt::format("Writes to {} are temporarily blocked for resharding.",
                                          nss.toString())));
            oplog.setObject2(
                BSON("type" << kReshardFinalOpLogType << "reshardingUUID" << reshardingUUID));
            oplog.setOpTime(OplogSlot());
            oplog.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());
            return oplog;
        };

        try {
            Timer latency;
            const auto& tempNss = constructTemporaryReshardingNss(_donorDoc.getNss().db(),
                                                                  _donorDoc.getExistingUUID());
            auto* catalogCache = Grid::get(rawOpCtx)->catalogCache();
            auto cm = uassertStatusOK(catalogCache->getCollectionRoutingInfo(rawOpCtx, tempNss));

            uassert(ErrorCodes::NamespaceNotSharded,
                    str::stream() << "Expected collection " << tempNss << " to be sharded",
                    cm.isSharded());

            std::set<ShardId> recipients;
            cm.getAllShardIds(&recipients);

            for (const auto& recipient : recipients) {
                auto oplog = generateOplogEntry(recipient);
                writeConflictRetry(
                    rawOpCtx,
                    "ReshardingBlockWritesOplog",
                    NamespaceString::kRsOplogNamespace.ns(),
                    [&] {
                        AutoGetOplog oplogWrite(rawOpCtx, OplogAccessMode::kWrite);
                        WriteUnitOfWork wunit(rawOpCtx);
                        const auto& oplogOpTime = repl::logOp(rawOpCtx, &oplog);
                        uassert(5279507,
                                str::stream()
                                    << "Failed to create new oplog entry for oplog with opTime: "
                                    << oplog.getOpTime().toString() << ": "
                                    << redact(oplog.toBSON()),
                                !oplogOpTime.isNull());
                        wunit.commit();
                    });
            }

            {
                stdx::lock_guard<Latch> lg(_mutex);
                LOGV2_DEBUG(5279504,
                            0,
                            "Committed oplog entries to temporarily block writes for resharding",
                            "namespace"_attr = nss,
                            "reshardingUUID"_attr = reshardingUUID,
                            "numRecipients"_attr = recipients.size(),
                            "duration"_attr = duration_cast<Milliseconds>(latency.elapsed()));
                ensureFulfilledPromise(lg, _finalOplogEntriesWritten);
            }
        } catch (const DBException& e) {
            const auto& status = e.toStatus();
            stdx::lock_guard<Latch> lg(_mutex);
            LOGV2_ERROR(5279508,
                        "Exception while writing resharding final oplog entries",
                        "reshardingUUID"_attr = reshardingUUID,
                        "error"_attr = status);
            ensureFulfilledPromise(lg, _finalOplogEntriesWritten, status);
            uassertStatusOK(status);
        }
    }

    _transitionState(DonorStateEnum::kMirroring);
}

SharedSemiFuture<void> ReshardingDonorService::DonorStateMachine::awaitFinalOplogEntriesWritten() {
    return _finalOplogEntriesWritten.getFuture();
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

    auto origNssRoutingInfo =
        getShardedCollectionRoutingInfoWithRefreshAndFlush(_donorDoc.getNss());
    auto currentCollectionUUID =
        getCollectionUUIDFromChunkManger(_donorDoc.getNss(), origNssRoutingInfo);

    if (currentCollectionUUID == _donorDoc.getExistingUUID()) {
        _dropOriginalCollection();
    } else {
        uassert(ErrorCodes::InvalidUUID,
                "Expected collection {} to have either the original UUID {} or the resharding UUID"
                " {}, but the collection instead has UUID {}"_format(
                    _donorDoc.getNss().toString(),
                    _donorDoc.getExistingUUID().toString(),
                    _donorDoc.get_id().toString(),
                    currentCollectionUUID.toString()),
                currentCollectionUUID == _donorDoc.get_id());
    }

    _transitionStateAndUpdateCoordinator(DonorStateEnum::kDone);
}

void ReshardingDonorService::DonorStateMachine::_dropOriginalCollection() {
    DBDirectClient client(cc().makeOperationContext().get());
    BSONObj dropResult;
    if (!client.dropCollection(
            _donorDoc.getNss().toString(), WriteConcerns::kMajorityWriteConcern, &dropResult)) {
        auto dropStatus = getStatusFromCommandResult(dropResult);
        if (dropStatus != ErrorCodes::NamespaceNotFound) {
            uassertStatusOK(dropStatus);
        }
    }
}

void ReshardingDonorService::DonorStateMachine::_transitionState(
    DonorStateEnum endState, boost::optional<Timestamp> minFetchTimestamp) {
    ReshardingDonorDocument replacementDoc(_donorDoc);
    replacementDoc.setState(endState);
    emplaceMinFetchTimestampIfExists(replacementDoc, minFetchTimestamp);

    LOGV2_INFO(5279505,
               "Transition resharding donor state",
               "newState"_attr = DonorState_serializer(replacementDoc.getState()),
               "oldState"_attr = DonorState_serializer(_donorDoc.getState()),
               "reshardingUUID"_attr = _donorDoc.get_id());

    _updateDonorDocument(std::move(replacementDoc));
}

void ReshardingDonorService::DonorStateMachine::_transitionStateAndUpdateCoordinator(
    DonorStateEnum endState, boost::optional<Timestamp> minFetchTimestamp) {
    _transitionState(endState, minFetchTimestamp);

    auto opCtx = cc().makeOperationContext();
    auto shardId = ShardingState::get(opCtx.get())->shardId();

    BSONObjBuilder updateBuilder;
    updateBuilder.append("donorShards.$.state", DonorState_serializer(endState));

    if (minFetchTimestamp) {
        updateBuilder.append("donorShards.$.minFetchTimestamp", minFetchTimestamp.get());
    }

    uassertStatusOK(
        Grid::get(opCtx.get())
            ->catalogClient()
            ->updateConfigDocument(opCtx.get(),
                                   NamespaceString::kConfigReshardingOperationsNamespace,
                                   BSON("_id" << _donorDoc.get_id() << "donorShards.id" << shardId),
                                   BSON("$set" << updateBuilder.done()),
                                   false /* upsert */,
                                   ShardingCatalogClient::kMajorityWriteConcern));
}

void ReshardingDonorService::DonorStateMachine::_transitionStateToError(const Status& status) {
    ReshardingDonorDocument replacementDoc(_donorDoc);
    replacementDoc.setState(DonorStateEnum::kError);
    _updateDonorDocument(std::move(replacementDoc));
}

void ReshardingDonorService::DonorStateMachine::_insertDonorDocument(
    const ReshardingDonorDocument& doc) {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<ReshardingDonorDocument> store(
        NamespaceString::kDonorReshardingOperationsNamespace);
    store.add(opCtx.get(), doc, WriteConcerns::kMajorityWriteConcern);

    _donorDoc = doc;
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

void ReshardingDonorService::DonorStateMachine::_removeDonorDocument() {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<ReshardingDonorDocument> store(
        NamespaceString::kDonorReshardingOperationsNamespace);
    store.remove(opCtx.get(),
                 BSON(ReshardingDonorDocument::k_idFieldName << _id),
                 WriteConcerns::kMajorityWriteConcern);
    _donorDoc = {};
}

}  // namespace mongo

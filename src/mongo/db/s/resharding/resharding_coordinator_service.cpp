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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/db/s/resharding/resharding_coordinator_service.h"

#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

template <typename Callable>
void withAlternateSession(OperationContext* opCtx, Callable&& callable) {
    AlternativeSessionRegion asr(opCtx);
    AuthorizationSession::get(asr.opCtx()->getClient())
        ->grantInternalAuthorization(asr.opCtx()->getClient());
    TxnNumber txnNumber = 0;

    auto guard = makeGuard([opCtx = asr.opCtx(), txnNumber] {
        ShardingCatalogManager::get(opCtx)->abortTxnForConfigDocument(opCtx, txnNumber);
    });

    callable(asr.opCtx(), txnNumber);

    guard.dismiss();
}

void writeConfigDocs(OperationContext* opCtx,
                     const NamespaceString& nss,
                     BatchedCommandRequest request,
                     bool startTransaction,
                     TxnNumber txnNumber,
                     boost::optional<int> expectedNumModified) {
    auto response = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, nss, request, startTransaction, txnNumber);
    uassertStatusOK(getStatusFromCommandResult(response));

    if (!expectedNumModified)
        return;

    uassert(5030400,
            str::stream() << "Expected to match " << expectedNumModified
                          << " docs, but only matched " << response.getIntField("n")
                          << " for write request " << request.toString(),
            response.getIntField("n") == expectedNumModified);
}

void writeToCoordinatorStateNss(OperationContext* opCtx,
                                const ReshardingCoordinatorDocument& coordinatorDoc,
                                TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        auto nextState = coordinatorDoc.getState();
        switch (nextState) {
            case CoordinatorStateEnum::kInitialized:
                // Insert the new coordinator state document
                return buildInsertOp(NamespaceString::kConfigReshardingOperationsNamespace,
                                     std::vector<BSONObj>{coordinatorDoc.toBSON()});
            case CoordinatorStateEnum::kDone:
                // Remove the coordinator state document
                return buildDeleteOp(NamespaceString::kConfigReshardingOperationsNamespace,
                                     BSON("_id" << coordinatorDoc.get_id()),  // query
                                     false                                    // multi
                );
            default:
                // Replacement update for the coordinator state document
                return buildUpdateOp(NamespaceString::kConfigReshardingOperationsNamespace,
                                     BSON("_id" << coordinatorDoc.get_id()),
                                     coordinatorDoc.toBSON(),
                                     false,  // upsert
                                     false   // multi
                );
        }
    }());

    writeConfigDocs(opCtx,
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    std::move(request),
                    true,  // startTransaction
                    txnNumber,
                    (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
                        ? boost::none
                        : boost::make_optional(1)  // expectedNumModified
    );
}

BSONObj createReshardingFieldsUpdateForOriginalNss(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<OID> newCollectionEpoch) {
    auto nextState = coordinatorDoc.getState();
    switch (nextState) {
        case CoordinatorStateEnum::kInitialized: {
            // Append 'reshardingFields' to the config.collections entry for the original nss
            TypeCollectionReshardingFields originalEntryReshardingFields(coordinatorDoc.get_id());
            originalEntryReshardingFields.setState(coordinatorDoc.getState());
            TypeCollectionDonorFields donorField(coordinatorDoc.getReshardingKey());
            originalEntryReshardingFields.setDonorFields(donorField);

            return BSON(
                "$set" << BSON("reshardingFields"
                               << originalEntryReshardingFields.toBSON() << "lastmod"
                               << opCtx->getServiceContext()->getPreciseClockSource()->now()));
        }
        case CoordinatorStateEnum::kCommitted:
            // Update the config.collections entry for the original nss to reflect
            // the new sharded collection. Set 'uuid' to the reshardingUUID, 'key' to the new shard
            // key, and 'lastmodEpoch' to newCollectionEpoch. Also update the 'state' field in the
            // 'reshardingFields' section
            return BSON("$set" << BSON(
                            "uuid"
                            << coordinatorDoc.get_id() << "key" << coordinatorDoc.getReshardingKey()
                            << "lastmodEpoch" << newCollectionEpoch.get() << "lastmod"
                            << opCtx->getServiceContext()->getPreciseClockSource()->now()
                            << "reshardingFields.state"
                            << CoordinatorState_serializer(coordinatorDoc.getState()).toString()));
        case mongo::CoordinatorStateEnum::kDone:
            // Remove 'reshardingFields' from the config.collections entry
            return BSON(
                "$unset" << BSON("reshardingFields"
                                 << "")
                         << "$set"
                         << BSON("lastmod"
                                 << opCtx->getServiceContext()->getPreciseClockSource()->now()));
        default:
            // Update the 'state' field in the 'reshardingFields' section
            return BSON(
                "$set" << BSON("reshardingFields.state"
                               << CoordinatorState_serializer(nextState).toString() << "lastmod"
                               << opCtx->getServiceContext()->getPreciseClockSource()->now()));
    }
}

void updateConfigCollectionsForOriginalNss(OperationContext* opCtx,
                                           const ReshardingCoordinatorDocument& coordinatorDoc,
                                           boost::optional<OID> newCollectionEpoch,
                                           TxnNumber txnNumber) {
    auto writeOp =
        createReshardingFieldsUpdateForOriginalNss(opCtx, coordinatorDoc, newCollectionEpoch);

    writeConfigDocs(
        opCtx,
        CollectionType::ConfigNS,
        buildUpdateOp(CollectionType::ConfigNS,
                      BSON(CollectionType::fullNs(coordinatorDoc.getNss().ns())),  // query
                      writeOp,
                      false,  // upsert
                      false   // multi
                      ),
        false,  // startTransaction
        txnNumber,
        1);  // expectedNumModified
}

void writeToConfigCollectionsForTempNss(OperationContext* opCtx,
                                        const ReshardingCoordinatorDocument& coordinatorDoc,
                                        boost::optional<ChunkVersion> chunkVersion,
                                        boost::optional<const BSONObj&> collation,
                                        TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        auto nextState = coordinatorDoc.getState();
        switch (nextState) {
            case CoordinatorStateEnum::kInitialized: {
                // Insert new entry for the temporary nss into config.collections
                auto collType = resharding::createTempReshardingCollectionType(
                    opCtx, coordinatorDoc, chunkVersion.get(), collation.get());
                return buildInsertOp(CollectionType::ConfigNS,
                                     std::vector<BSONObj>{collType.toBSON()});
            }
            case CoordinatorStateEnum::kCloning:
                // Update the 'state' and 'fetchTimestamp' fields in the
                // 'reshardingFields.recipient' section
                return buildUpdateOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::fullNs(coordinatorDoc.getTempReshardingNss().ns())),
                    BSON("$set" << BSON(
                             "reshardingFields.state"
                             << CoordinatorState_serializer(nextState).toString()
                             << "reshardingFields.recipientFields.fetchTimestamp"
                             << coordinatorDoc.getFetchTimestamp().get() << "lastmod"
                             << opCtx->getServiceContext()->getPreciseClockSource()->now())),
                    false,  // upsert
                    false   // multi
                );
            case CoordinatorStateEnum::kCommitted:
                // Remove the entry for the temporary nss
                return buildDeleteOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::fullNs(coordinatorDoc.getTempReshardingNss().ns())),
                    false  // multi
                );
            default:
                // Update the 'state' field in the 'reshardingFields' section
                return buildUpdateOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::fullNs(coordinatorDoc.getTempReshardingNss().ns())),
                    BSON("$set" << BSON(
                             "reshardingFields.state"
                             << CoordinatorState_serializer(nextState).toString() << "lastmod"
                             << opCtx->getServiceContext()->getPreciseClockSource()->now())),
                    false,  // upsert
                    false   // multi
                );
        }
    }());

    writeConfigDocs(opCtx,
                    CollectionType::ConfigNS,
                    std::move(request),
                    false,  // startTransaction
                    txnNumber,
                    (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
                        ? boost::none
                        : boost::make_optional(1)  // expectedNumModified
    );
}

void insertChunkAndTagDocsForTempNss(OperationContext* opCtx,
                                     std::vector<ChunkType> initialChunks,
                                     std::vector<TagsType> newZones,
                                     TxnNumber txnNumber) {
    // Insert new initial chunk documents for temp nss
    std::vector<BSONObj> initialChunksBSON(initialChunks.size());
    std::transform(initialChunks.begin(),
                   initialChunks.end(),
                   initialChunksBSON.begin(),
                   [](ChunkType chunk) { return chunk.toConfigBSON(); });

    ShardingCatalogManager::get(opCtx)->insertConfigDocumentsInTxn(opCtx,
                                                                   ChunkType::ConfigNS,
                                                                   std::move(initialChunksBSON),
                                                                   false,  // startTransaction
                                                                   txnNumber);

    // Insert tag documents for temp nss
    std::vector<BSONObj> zonesBSON(newZones.size());
    std::transform(newZones.begin(), newZones.end(), zonesBSON.begin(), [](TagsType chunk) {
        return chunk.toBSON();
    });

    ShardingCatalogManager::get(opCtx)->insertConfigDocumentsInTxn(opCtx,
                                                                   TagsType::ConfigNS,
                                                                   std::move(zonesBSON),
                                                                   false,  // startTransaction
                                                                   txnNumber);
}

void removeChunkAndTagsDocsForOriginalNss(OperationContext* opCtx,
                                          const ReshardingCoordinatorDocument& coordinatorDoc,
                                          TxnNumber txnNumber) {
    // Remove all chunk documents for the original nss. We do not know how many chunk docs currently
    // exist, so cannot pass a value for expectedNumModified
    writeConfigDocs(opCtx,
                    ChunkType::ConfigNS,
                    buildDeleteOp(ChunkType::ConfigNS,
                                  BSON(ChunkType::ns(coordinatorDoc.getNss().ns())),  // query
                                  true                                                // multi
                                  ),
                    false,  // startTransaction
                    txnNumber,
                    boost::none  // expectedNumModified
    );

    // Remove all tag documents for the original nss. We do not know how many tag docs currently
    // exist, so cannot pass a value for expectedNumModified
    writeConfigDocs(opCtx,
                    TagsType::ConfigNS,
                    buildDeleteOp(TagsType::ConfigNS,
                                  BSON(ChunkType::ns(coordinatorDoc.getNss().ns())),  // query
                                  true                                                // multi
                                  ),
                    false,  // startTransaction
                    txnNumber,
                    boost::none  // expectedNumModified
    );
}

void updateChunkAndTagsDocsForTempNss(OperationContext* opCtx,
                                      const ReshardingCoordinatorDocument& coordinatorDoc,
                                      OID newCollectionEpoch,
                                      boost::optional<int> expectedNumChunksModified,
                                      boost::optional<int> expectedNumZonesModified,
                                      TxnNumber txnNumber) {
    // Update all chunk documents that currently have 'ns' as the temporary collection namespace
    // such that 'ns' is now the original collection namespace and 'lastmodEpoch' is
    // newCollectionEpoch.
    writeConfigDocs(
        opCtx,
        ChunkType::ConfigNS,
        buildUpdateOp(ChunkType::ConfigNS,
                      BSON(ChunkType::ns(coordinatorDoc.getTempReshardingNss().ns())),  // query
                      BSON("$set" << BSON("ns" << coordinatorDoc.getNss().ns() << "lastmodEpoch"
                                               << newCollectionEpoch)),  // update
                      false,                                             // upsert
                      true                                               // multi
                      ),
        false,  // startTransaction
        txnNumber,
        expectedNumChunksModified);

    // Update the 'ns' field to be the original collection namespace for all tags documents that
    // currently have 'ns' as the temporary collection namespace
    writeConfigDocs(
        opCtx,
        TagsType::ConfigNS,
        buildUpdateOp(TagsType::ConfigNS,
                      BSON(TagsType::ns(coordinatorDoc.getTempReshardingNss().ns())),  // query
                      BSON("$set" << BSON("ns" << coordinatorDoc.getNss().ns())),      // update
                      false,                                                           // upsert
                      true                                                             // multi
                      ),
        false,  // startTransaction
        txnNumber,
        expectedNumZonesModified);
}
}  // namespace

namespace resharding {
CollectionType createTempReshardingCollectionType(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    const ChunkVersion& chunkVersion,
    const BSONObj& collation) {
    CollectionType collType;
    collType.setNs(coordinatorDoc.getTempReshardingNss());
    collType.setUUID(coordinatorDoc.get_id());
    collType.setEpoch(chunkVersion.epoch());
    collType.setUpdatedAt(opCtx->getServiceContext()->getPreciseClockSource()->now());
    collType.setKeyPattern(coordinatorDoc.getReshardingKey());
    collType.setDefaultCollation(collation);
    collType.setUnique(false);
    collType.setDistributionMode(CollectionType::DistributionMode::kSharded);

    TypeCollectionReshardingFields tempEntryReshardingFields(coordinatorDoc.get_id());
    tempEntryReshardingFields.setState(coordinatorDoc.getState());
    TypeCollectionRecipientFields recipient(coordinatorDoc.getNss());
    if (coordinatorDoc.getFetchTimestampStruct().getFetchTimestamp()) {
        recipient.setFetchTimestampStruct(coordinatorDoc.getFetchTimestampStruct());
    }
    tempEntryReshardingFields.setRecipientFields(recipient);
    collType.setReshardingFields(std::move(tempEntryReshardingFields));

    invariant(collType.validate().isOK());
    return collType;
}

void persistInitialStateAndCatalogUpdates(OperationContext* opCtx,
                                          const ReshardingCoordinatorDocument& coordinatorDoc,
                                          std::vector<ChunkType> initialChunks,
                                          std::vector<TagsType> newZones) {
    auto originalCollType = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getCollection(
        opCtx, coordinatorDoc.getNss(), repl::ReadConcernLevel::kMajorityReadConcern));
    const auto collation = originalCollType.value.getDefaultCollation();

    withAlternateSession(opCtx, [=](OperationContext* opCtx, TxnNumber txnNumber) {
        // Insert state doc to config.reshardingOperations
        writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

        // Update the config.collections entry for the original collection to include
        // 'reshardingFields'
        updateConfigCollectionsForOriginalNss(opCtx, coordinatorDoc, boost::none, txnNumber);

        // Insert the config.collections entry for the temporary resharding collection. The chunks
        // all have the same epoch, so picking the last chunk here is arbitrary.
        auto chunkVersion = initialChunks.back().getVersion();
        writeToConfigCollectionsForTempNss(
            opCtx, coordinatorDoc, chunkVersion, collation, txnNumber);

        // Insert new initial chunk and tag documents
        insertChunkAndTagDocsForTempNss(opCtx, initialChunks, newZones, txnNumber);

        // Commit the transaction
        ShardingCatalogManager::get(opCtx)->commitTxnForConfigDocument(opCtx, txnNumber);
    });
}

void persistCommittedState(OperationContext* opCtx,
                           const ReshardingCoordinatorDocument& coordinatorDoc,
                           OID newCollectionEpoch,
                           boost::optional<int> expectedNumChunksModified,
                           boost::optional<int> expectedNumZonesModified) {
    withAlternateSession(opCtx, [=](OperationContext* opCtx, TxnNumber txnNumber) {
        // Update the config.reshardingOperations entry
        writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

        // Remove the config.collections entry for the temporary collection
        writeToConfigCollectionsForTempNss(
            opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);

        // Update the config.collections entry for the original namespace to reflect the new shard
        // key, new epoch, and new UUID
        updateConfigCollectionsForOriginalNss(opCtx, coordinatorDoc, newCollectionEpoch, txnNumber);

        // Remove all chunk and tag documents associated with the original collection, then update
        // the chunk and tag docs currently associated with the temp nss to be associated with the
        // original nss
        removeChunkAndTagsDocsForOriginalNss(opCtx, coordinatorDoc, txnNumber);
        updateChunkAndTagsDocsForTempNss(opCtx,
                                         coordinatorDoc,
                                         newCollectionEpoch,
                                         expectedNumChunksModified,
                                         expectedNumZonesModified,
                                         txnNumber);

        // Commit the transaction
        ShardingCatalogManager::get(opCtx)->commitTxnForConfigDocument(opCtx, txnNumber);
    });
}

void persistStateTransition(OperationContext* opCtx,
                            const ReshardingCoordinatorDocument& coordinatorDoc) {
    // Run updates to config.reshardingOperations and config.collections in a transaction
    auto nextState = coordinatorDoc.getState();
    withAlternateSession(opCtx, [=](OperationContext* opCtx, TxnNumber txnNumber) {
        // Update the config.reshardingOperations entry
        writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

        // Update the config.collections entry for the original collection
        updateConfigCollectionsForOriginalNss(opCtx, coordinatorDoc, boost::none, txnNumber);

        // Update the config.collections entry for the temporary resharding collection. If we've
        // already committed this operation, we've removed the entry for the temporary
        // collection and updated the entry with original namespace to have the new shard key, UUID,
        // and epoch
        if (nextState < CoordinatorStateEnum::kCommitted ||
            nextState == CoordinatorStateEnum::kError) {
            writeToConfigCollectionsForTempNss(
                opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);
        }

        // Commit the transaction
        ShardingCatalogManager::get(opCtx)->commitTxnForConfigDocument(opCtx, txnNumber);
    });
}

void removeCoordinatorDocAndReshardingFields(OperationContext* opCtx,
                                             const ReshardingCoordinatorDocument& coordinatorDoc) {
    withAlternateSession(opCtx, [=](OperationContext* opCtx, TxnNumber txnNumber) {
        // Remove entry for this resharding operation from config.reshardingOperations
        writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

        // Remove the resharding fields from the config.collections entry
        updateConfigCollectionsForOriginalNss(opCtx, coordinatorDoc, boost::none, txnNumber);

        // Commit the transaction
        ShardingCatalogManager::get(opCtx)->commitTxnForConfigDocument(opCtx, txnNumber);
    });
}
}  // namespace resharding

ReshardingCoordinatorService::ReshardingCoordinator::ReshardingCoordinator(const BSONObj& state)
    : PrimaryOnlyService::TypedInstance<ReshardingCoordinator>(),
      _id(state["_id"].wrap().getOwned()),
      _coordinatorDoc(ReshardingCoordinatorDocument::parse(
          IDLParserErrorContext("ReshardingCoordinatorStateDoc"), state)) {
    _reshardingCoordinatorObserver = std::make_shared<ReshardingCoordinatorObserver>();
}

ReshardingCoordinatorService::ReshardingCoordinator::~ReshardingCoordinator() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_initialChunksAndZonesPromise.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

void ReshardingCoordinatorService::ReshardingCoordinator::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {
    ExecutorFuture<void>(**executor)
        .then([this, executor] { return _init(executor); })
        .then([this] { _tellAllRecipientsToRefresh(); })
        .then([this, executor] { return _awaitAllRecipientsCreatedCollection(executor); })
        .then([this] { _tellAllDonorsToRefresh(); })
        .then([this, executor] { return _awaitAllDonorsReadyToDonate(executor); })
        .then([this] { _tellAllRecipientsToRefresh(); })
        .then([this, executor] { return _awaitAllRecipientsFinishedCloning(executor); })
        .then([this] { _tellAllDonorsToRefresh(); })
        .then([this, executor] { return _awaitAllRecipientsInStrictConsistency(executor); })
        .then([this](const ReshardingCoordinatorDocument& updatedStateDoc) {
            return _commit(updatedStateDoc);
        })
        .then([this] {
            if (_coordinatorDoc.getState() > CoordinatorStateEnum::kRenaming) {
                return;
            }

            this->_runUpdates(CoordinatorStateEnum::kRenaming, _coordinatorDoc);
            return;
        })
        .then([this, executor] { return _awaitAllRecipientsRenamedCollection(executor); })
        .then([this] { _tellAllDonorsToRefresh(); })
        .then([this, executor] { return _awaitAllDonorsDroppedOriginalCollection(executor); })
        .then([this] { _tellAllRecipientsToRefresh(); })
        .then([this] { _tellAllDonorsToRefresh(); })
        .onError([this](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got here.
                return status;
            }

            _runUpdates(CoordinatorStateEnum::kError, _coordinatorDoc);

            LOGV2(4956902,
                  "Resharding failed",
                  "namespace"_attr = _coordinatorDoc.getNss().ns(),
                  "newShardKeyPattern"_attr = _coordinatorDoc.getReshardingKey(),
                  "error"_attr = status);

            // TODO wait for donors and recipients to abort the operation and clean up state
            _tellAllRecipientsToRefresh();
            _tellAllDonorsToRefresh();

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

void ReshardingCoordinatorService::ReshardingCoordinator::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_initialChunksAndZonesPromise.getFuture().isReady()) {
        _initialChunksAndZonesPromise.setError(status);
    }

    _reshardingCoordinatorObserver->interrupt(status);

    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

void ReshardingCoordinatorService::ReshardingCoordinator::setInitialChunksAndZones(
    std::vector<ChunkType> initialChunks, std::vector<TagsType> newZones) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing ||
        _initialChunksAndZonesPromise.getFuture().isReady()) {
        return;
    }

    _initialChunksAndZonesPromise.emplaceValue(
        ChunksAndZones{std::move(initialChunks), std::move(newZones)});
}

std::shared_ptr<ReshardingCoordinatorObserver>
ReshardingCoordinatorService::ReshardingCoordinator::getObserver() {
    return _reshardingCoordinatorObserver;
}

ExecutorFuture<void> ReshardingCoordinatorService::ReshardingCoordinator::_init(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _initialChunksAndZonesPromise.getFuture()
        .thenRunOn(**executor)
        .then([this](const ChunksAndZones& initialChunksAndZones) {
            auto initialChunks = initialChunksAndZones.initialChunks;
            auto newZones = initialChunksAndZones.newZones;

            // Create state document that will be written to disk and afterward set to the in-memory
            // _coordinatorDoc
            ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;
            updatedCoordinatorDoc.setState(CoordinatorStateEnum::kInitialized);

            auto opCtx = cc().makeOperationContext();
            resharding::persistInitialStateAndCatalogUpdates(
                opCtx.get(), updatedCoordinatorDoc, initialChunks, newZones);

            invariant(_coordinatorDoc.getState() == CoordinatorStateEnum::kInitializing);
            _coordinatorDoc = updatedCoordinatorDoc;
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsCreatedCollection(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kInitialized) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsCreatedCollection()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument updatedStateDoc) {
            this->_runUpdates(CoordinatorStateEnum::kPreparingToDonate, updatedStateDoc);
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllDonorsReadyToDonate(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kPreparingToDonate) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllDonorsReadyToDonate()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument updatedStateDoc) {
            // TODO SERVER-49573 Calculate the fetchTimestamp from the updatedStateDoc then pass it
            // into _runUpdates.
            this->_runUpdates(CoordinatorStateEnum::kCloning, updatedStateDoc);
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsFinishedCloning(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kCloning) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsFinishedCloning()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument updatedStateDoc) {
            this->_runUpdates(CoordinatorStateEnum::kMirroring, updatedStateDoc);
        });
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsInStrictConsistency(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kMirroring) {
        // If in recovery, just return the existing _stateDoc.
        return _coordinatorDoc;
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsInStrictConsistency();
}

Future<void> ReshardingCoordinatorService::ReshardingCoordinator::_commit(
    const ReshardingCoordinatorDocument& updatedDoc) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kMirroring) {
        return Status::OK();
    }

    ReshardingCoordinatorDocument updatedCoordinatorDoc = updatedDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kCommitted);

    // Get the number of initial chunks and new zones that we inserted during initialization in
    // order to assert we match the same number when updating below.
    boost::optional<int> expectedNumChunksModified;
    boost::optional<int> expectedNumZonesModified;
    if (_initialChunksAndZonesPromise.getFuture().isReady()) {
        auto chunksAndZones =
            uassertStatusOK(_initialChunksAndZonesPromise.getFuture().getNoThrow());
        // TODO asserting that the updates to config.chunks and config.tags (run in
        // persistCommittedState) match 'expectedNumChunksModified' and 'expectedNumZonesModified'
        // is dependent on both the balancer and auto-splitter being off for the temporary
        // collection. If we decide to leave the auto-splitter enabled, we will need to revisit this
        // assumption.
        expectedNumChunksModified = chunksAndZones.initialChunks.size();
        expectedNumZonesModified = chunksAndZones.newZones.size();
    }
    auto opCtx = cc().makeOperationContext();

    // The new epoch to use for the resharded collection to indicate that the collection is a
    // new incarnation of the namespace
    auto newCollectionEpoch = OID::gen();
    resharding::persistCommittedState(opCtx.get(),
                                      updatedCoordinatorDoc,
                                      newCollectionEpoch,
                                      expectedNumChunksModified,
                                      expectedNumZonesModified);

    // Update the in memory state
    _coordinatorDoc = updatedCoordinatorDoc;

    return Status::OK();
};

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsRenamedCollection(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kRenaming) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsRenamedCollection()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument updatedStateDoc) {
            this->_runUpdates(CoordinatorStateEnum::kDropping, updatedStateDoc);
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllDonorsDroppedOriginalCollection(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kDropping) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllDonorsDroppedOriginalCollection()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument updatedStateDoc) {
            this->_runUpdates(CoordinatorStateEnum::kDone, updatedStateDoc);
        });
}

void ReshardingCoordinatorService::ReshardingCoordinator::_runUpdates(
    CoordinatorStateEnum nextState,
    ReshardingCoordinatorDocument updatedStateDoc,
    boost::optional<Timestamp> fetchTimestamp) {
    // Build new state doc for coordinator state update
    ReshardingCoordinatorDocument updatedCoordinatorDoc = updatedStateDoc;
    updatedCoordinatorDoc.setState(nextState);
    if (fetchTimestamp) {
        auto& fetchTimestampStruct = updatedCoordinatorDoc.getFetchTimestampStruct();
        if (fetchTimestampStruct.getFetchTimestamp())
            invariant(fetchTimestampStruct.getFetchTimestamp().get() == fetchTimestamp.get());

        fetchTimestampStruct.setFetchTimestamp(std::move(fetchTimestamp));
    }

    auto opCtx = cc().makeOperationContext();
    resharding::persistStateTransition(opCtx.get(), updatedCoordinatorDoc);

    // Update in-memory coordinator doc
    _coordinatorDoc = updatedCoordinatorDoc;
}

// TODO
void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllRecipientsToRefresh() {}

// TODO
void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllDonorsToRefresh() {}

}  // namespace mongo

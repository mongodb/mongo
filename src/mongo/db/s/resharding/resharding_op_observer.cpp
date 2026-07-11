// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/resharding/resharding_op_observer.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/db/s/resharding/resharding_coordinator_observer.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {

namespace {

bool shouldUseRegistry() {
    return resharding::gFeatureFlagReshardingRegistry.isEnabled();
}

std::shared_ptr<ReshardingCoordinatorObserver> getReshardingCoordinatorObserver(
    OperationContext* opCtx, const BSONObj& reshardingId) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(ReshardingCoordinatorService::kServiceName);
    auto [instance, _] = ReshardingCoordinator::lookup(opCtx, service, reshardingId);

    iassert(5400001, "ReshardingCoordinatorService instance does not exist", instance.has_value());

    return (*instance)->getObserver();
}

boost::optional<Timestamp> parseNewMinFetchTimestampValue(const BSONObj& obj) {
    auto doc = ReshardingDonorDocument::parse(obj, IDLParserContext("Resharding"));
    if (doc.getMutableState().getState() == DonorStateEnum::kDonatingInitialData) {
        return doc.getMutableState().getMinFetchTimestamp().value();
    } else {
        return boost::none;
    }
}

void assertCanExtractShardKeyFromDocs(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      std::vector<InsertStatement>::const_iterator begin,
                                      std::vector<InsertStatement>::const_iterator end) {
    auto collDesc = CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)
                        ->getCollectionDescription(opCtx);

    // A user can manually create a 'db.system.resharding.' collection that isn't guaranteed to be
    // tracked outside of running reshardCollection.
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Temporary resharding collection metadata for "
                          << nss.toStringForErrorMsg() << " not found",
            collDesc.hasRoutingTable());

    const ShardKeyPattern shardKeyPattern(collDesc.getKeyPattern());
    for (auto it = begin; it != end; ++it) {
        shardKeyPattern.extractShardKeyFromDocThrows(it->doc);
    }
}

boost::optional<Timestamp> _calculatePin(OperationContext* opCtx) {
    // We recalculate the pin by looking at all documents inside the resharding donor
    // collection. The caller may or may not be in a transaction. If the caller is in a transaction,
    // we intentionally read any uncommitted writes it has made.
    //
    // If there are concurrent transactions updating different keys in the donor collection, there
    // can be write skew resulting in the wrong pin, including leaking a resource. We enforce the
    // collection is held in exclusive mode to prevent this. However an exception to this is oplog
    // application, which already serializes these writes.

    invariant(!opCtx->isEnforcingConstraints() ||
              shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
                  NamespaceString::kDonorReshardingOperationsNamespace, LockMode::MODE_X));

    // If the RecoveryUnit already had an open snapshot, keep the snapshot open. Otherwise abandon
    // the snapshot when exitting the function.
    ScopeGuard scopeGuard([&] { shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot(); });
    if (shard_role_details::getRecoveryUnit(opCtx)->isActive()) {
        scopeGuard.dismiss();
    }

    auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
        opCtx, NamespaceString::kDonorReshardingOperationsNamespace);
    if (!collection) {
        return boost::none;
    }

    Timestamp ret = Timestamp::max();
    auto cursor = collection->getCursor(opCtx);
    for (auto doc = cursor->next(); doc; doc = cursor->next()) {
        if (auto fetchTs = parseNewMinFetchTimestampValue(doc.value().data.toBson()); fetchTs) {
            ret = std::min(ret, fetchTs.value());
        }
    }

    if (ret == Timestamp::max()) {
        return boost::none;
    }

    return ret;
}

void _doPin(OperationContext* opCtx) {
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    boost::optional<Timestamp> pin = _calculatePin(opCtx);
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!pin) {
        storageEngine->unpinOldestTimestamp(std::string{ReshardingHistoryHook::kName});
        return;
    }

    StatusWith<Timestamp> res =
        storageEngine->pinOldestTimestamp(*shard_role_details::getRecoveryUnit(opCtx),
                                          std::string{ReshardingHistoryHook::kName},
                                          pin.value(),
                                          false);
    if (!res.isOK()) {
        if (!replCoord->getSettings().isReplSet()) {
            // The pin has failed, but we're in standalone mode. Ignore the error.
            return;
        }

        const auto state = replCoord->getMemberState();
        if (state.primary()) {
            // If we're a primary, the pin can fail and the error should bubble up and fail
            // resharding.
            uassertStatusOK(res);
        } else if (state.secondary()) {
            // The pin timestamp can be earlier than the oplog entry being processed. Thus
            // the oldest timestamp can race ahead of the pin request. It's not ideal this
            // node cannot participate in donating documents for the cloning phase, but this
            // is the most robust path forward. Ignore this case.
            LOGV2_WARNING(5384104,
                          "This node is unable to pin history for resharding",
                          "requestedTs"_attr = pin.value());
        } else {
            // For recovery cases we also ignore the error. The expected scenario is the pin
            // request is no longer needed, but the write to delete the pin was rolled
            // back. The write to delete the pin won't be issued until the collection
            // cloning phase of resharding is majority committed. Thus there should be no
            // consequence to observing this error. Ignore this case.
            LOGV2(5384103,
                  "The requested pin was unavailable, but should also be unnecessary",
                  "requestedTs"_attr = pin.value());
        }
    }
}

}  // namespace

boost::optional<Timestamp> ReshardingHistoryHook::calculatePin(OperationContext* opCtx) {
    return _calculatePin(opCtx);
}

ReshardingOpObserver::ReshardingOpObserver() = default;

ReshardingOpObserver::~ReshardingOpObserver() = default;

void ReshardingOpObserver::onInserts(OperationContext* opCtx,
                                     const CollectionPtr& coll,
                                     std::vector<InsertStatement>::const_iterator begin,
                                     std::vector<InsertStatement>::const_iterator end,
                                     const std::vector<RecordId>& recordIds,
                                     std::vector<bool> fromMigrate,
                                     bool defaultFromMigrate,
                                     OpStateAccumulator* opAccumulator) {
    const auto& nss = coll->ns();

    if (shouldUseRegistry() && _nssToRoleMap.contains(nss)) {
        // We should only get a single document here as each resharding participant writes a single
        // state document per operation but we loop to be defensive.
        for (auto it = begin; it != end; ++it) {
            auto commonMetadata = CommonReshardingMetadata::parse(
                it->doc, IDLParserContext("ReshardingOpObserver::onInserts"));
            LocalReshardingOperationsRegistry::get().registerOperation(_nssToRoleMap.at(nss),
                                                                       commonMetadata);
        }
    }

    if (nss == NamespaceString::kDonorReshardingOperationsNamespace) {
        // If a document is inserted into the resharding donor collection with a
        // `minFetchTimestamp`, we assume the document was inserted as part of initial sync and do
        // nothing to pin history.
        return;
    }

    // This is a no-op if either replication is not enabled or this node is a secondary
    if (!repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet() ||
        !opCtx->writesAreReplicated()) {
        return;
    }

    if (nss.isTemporaryReshardingCollection()) {
        assertCanExtractShardKeyFromDocs(opCtx, nss, begin, end);
    }
}

void ReshardingOpObserver::onUpdate(OperationContext* opCtx,
                                    const OplogUpdateEntryArgs& args,
                                    OpStateAccumulator* opAccumulator) {
    const auto& nss = args.coll->ns();
    if (nss == NamespaceString::kDonorReshardingOperationsNamespace) {
        // Primaries and secondaries should execute pinning logic when observing changes to the
        // donor resharding document.
        _doPin(opCtx);
    }

    const auto& useRegistry = shouldUseRegistry();

    if (useRegistry && nss == NamespaceString::kConfigReshardingOperationsNamespace) {
        auto coordinatorDoc = ReshardingCoordinatorDocument::parse(
            args.updateArgs->updatedDoc, IDLParserContext("ReshardingOpObserver::onUpdate"));
        if (resharding::excludeFromRegistry(coordinatorDoc)) {
            const auto& commonMetadata = coordinatorDoc.getCommonReshardingMetadata();
            LocalReshardingOperationsRegistry::get().unregisterOperation(Role::kCoordinator,
                                                                         commonMetadata);
        }
    }

    if (useRegistry && nss == NamespaceString::kDonorReshardingOperationsNamespace) {
        auto donorDoc = ReshardingDonorDocument::parse(
            args.updateArgs->updatedDoc, IDLParserContext("ReshardingOpObserver::onUpdate"));
        if (resharding::excludeFromRegistry(donorDoc)) {
            const auto& commonMetadata = donorDoc.getCommonReshardingMetadata();
            LocalReshardingOperationsRegistry::get().unregisterOperation(Role::kDonor,
                                                                         commonMetadata);
        }
    }

    if (useRegistry && nss == NamespaceString::kRecipientReshardingOperationsNamespace) {
        auto recipientDoc = ReshardingRecipientDocument::parse(
            args.updateArgs->updatedDoc, IDLParserContext("ReshardingOpObserver::onUpdate"));
        if (resharding::excludeFromRegistry(recipientDoc)) {
            const auto& commonMetadata = recipientDoc.getCommonReshardingMetadata();
            LocalReshardingOperationsRegistry::get().unregisterOperation(Role::kRecipient,
                                                                         commonMetadata);
        }
    }

    // This is a no-op if either replication is not enabled or this node is a secondary
    if (!repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet() ||
        !opCtx->writesAreReplicated()) {
        return;
    }

    if (nss == NamespaceString::kConfigReshardingOperationsNamespace) {
        auto newCoordinatorDoc = ReshardingCoordinatorDocument::parse(
            args.updateArgs->updatedDoc, IDLParserContext("reshardingCoordinatorDoc"));
        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [newCoordinatorDoc = std::move(newCoordinatorDoc)](OperationContext* opCtx,
                                                               boost::optional<Timestamp>) mutable {
                try {
                    // It is possible that the ReshardingCoordinatorService is still being rebuilt.
                    // We must defer calling ReshardingCoordinator::lookup() until after our storage
                    // transaction has committed to ensure we aren't holding open an oplog hole and
                    // preventing replication from making progress while we wait.
                    auto reshardingId = BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName
                                             << newCoordinatorDoc.getReshardingUUID());
                    auto observer = getReshardingCoordinatorObserver(opCtx, reshardingId);
                    observer->onReshardingParticipantTransition(newCoordinatorDoc);
                } catch (const DBException& ex) {
                    LOGV2_INFO(6148200,
                               "Interrupted while waiting for resharding coordinator to be rebuilt;"
                               " will retry on new primary",
                               logAttrs(newCoordinatorDoc.getSourceNss()),
                               "reshardingUUID"_attr = newCoordinatorDoc.getReshardingUUID(),
                               "error"_attr = redact(ex.toStatus()));
                }
            });
    } else if (nss.isTemporaryReshardingCollection()) {
        const std::vector<InsertStatement> updateDoc{InsertStatement{args.updateArgs->updatedDoc}};
        assertCanExtractShardKeyFromDocs(
            opCtx, args.coll->ns(), updateDoc.begin(), updateDoc.end());
    }
}

void ReshardingOpObserver::onDelete(OperationContext* opCtx,
                                    const CollectionPtr& coll,
                                    StmtId stmtId,
                                    const BSONObj& doc,
                                    const DocumentKey& documentKey,
                                    const OplogDeleteEntryArgs& args,
                                    OpStateAccumulator* opAccumulator) {
    const auto& nss = coll->ns();
    if (shouldUseRegistry() && _nssToRoleMap.contains(nss)) {
        auto commonMetadata = CommonReshardingMetadata::parse(
            doc, IDLParserContext("ReshardingOpObserver::onDelete"));
        LocalReshardingOperationsRegistry::get().unregisterOperation(_nssToRoleMap.at(nss),
                                                                     commonMetadata);
    }
    if (nss == NamespaceString::kDonorReshardingOperationsNamespace) {
        _doPin(opCtx);
    }
}

repl::OpTime ReshardingOpObserver::onDropCollection(OperationContext* opCtx,
                                                    const NamespaceString& collectionName,
                                                    const UUID& uuid,
                                                    std::uint64_t numRecords,
                                                    bool markFromMigrate,
                                                    bool isTimeseries) {
    if (shouldUseRegistry() && _nssToRoleMap.contains(collectionName)) {
        LocalReshardingOperationsRegistry::get().clearOperationsForRole(
            _nssToRoleMap.at(collectionName));
    }
    return {};
}

}  // namespace mongo

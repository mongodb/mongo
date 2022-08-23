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


#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_op_observer.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {

namespace {

std::shared_ptr<ReshardingCoordinatorObserver> getReshardingCoordinatorObserver(
    OperationContext* opCtx, const BSONObj& reshardingId) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(ReshardingCoordinatorService::kServiceName);
    auto instance =
        ReshardingCoordinatorService::ReshardingCoordinator::lookup(opCtx, service, reshardingId);

    iassert(5400001, "ReshardingCoordinatorService instance does not exist", instance.has_value());

    return (*instance)->getObserver();
}

boost::optional<Timestamp> parseNewMinFetchTimestampValue(const BSONObj& obj) {
    auto doc = ReshardingDonorDocument::parse(IDLParserContext("Resharding"), obj);
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
    const auto collDesc = CollectionShardingState::get(opCtx, nss)->getCollectionDescription(opCtx);
    // A user can manually create a 'db.system.resharding.' collection that isn't guaranteed to be
    // sharded outside of running reshardCollection.
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Temporary resharding collection " << nss.toString()
                          << " is not sharded",
            collDesc.isSharded());

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
    // collection is held in exclusive mode to prevent this.

    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString::kDonorReshardingOperationsNamespace, LockMode::MODE_X));

    // If the RecoveryUnit already had an open snapshot, keep the snapshot open. Otherwise abandon
    // the snapshot when exitting the function.
    ScopeGuard scopeGuard([&] { opCtx->recoveryUnit()->abandonSnapshot(); });
    if (opCtx->recoveryUnit()->isActive()) {
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
        storageEngine->unpinOldestTimestamp(ReshardingHistoryHook::kName.toString());
        return;
    }

    StatusWith<Timestamp> res = storageEngine->pinOldestTimestamp(
        opCtx, ReshardingHistoryHook::kName.toString(), pin.value(), false);
    if (!res.isOK()) {
        if (replCoord->getReplicationMode() != repl::ReplicationCoordinator::Mode::modeReplSet) {
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
                                     bool fromMigrate) {
    const auto& nss = coll->ns();

    if (nss == NamespaceString::kDonorReshardingOperationsNamespace) {
        // If a document is inserted into the resharding donor collection with a
        // `minFetchTimestamp`, we assume the document was inserted as part of initial sync and do
        // nothing to pin history.
        return;
    }

    // This is a no-op if either replication is not enabled or this node is a secondary
    if (!repl::ReplicationCoordinator::get(opCtx)->isReplEnabled() ||
        !opCtx->writesAreReplicated()) {
        return;
    }

    if (nss.isTemporaryReshardingCollection()) {
        assertCanExtractShardKeyFromDocs(opCtx, nss, begin, end);
    }
}

void ReshardingOpObserver::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    if (args.nss == NamespaceString::kDonorReshardingOperationsNamespace) {
        // Primaries and secondaries should execute pinning logic when observing changes to the
        // donor resharding document.
        _doPin(opCtx);
    }

    // This is a no-op if either replication is not enabled or this node is a secondary
    if (!repl::ReplicationCoordinator::get(opCtx)->isReplEnabled() ||
        !opCtx->writesAreReplicated()) {
        return;
    }

    if (args.nss == NamespaceString::kConfigReshardingOperationsNamespace) {
        auto newCoordinatorDoc = ReshardingCoordinatorDocument::parse(
            IDLParserContext("reshardingCoordinatorDoc"), args.updateArgs->updatedDoc);
        opCtx->recoveryUnit()->onCommit([opCtx, newCoordinatorDoc = std::move(newCoordinatorDoc)](
                                            boost::optional<Timestamp> unusedCommitTime) mutable {
            try {
                // It is possible that the ReshardingCoordinatorService is still being rebuilt. We
                // must defer calling ReshardingCoordinator::lookup() until after our storage
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
                           "namespace"_attr = newCoordinatorDoc.getSourceNss(),
                           "reshardingUUID"_attr = newCoordinatorDoc.getReshardingUUID(),
                           "error"_attr = redact(ex.toStatus()));
            }
        });
    } else if (args.nss.isTemporaryReshardingCollection()) {
        const std::vector<InsertStatement> updateDoc{InsertStatement{args.updateArgs->updatedDoc}};
        assertCanExtractShardKeyFromDocs(opCtx, args.nss, updateDoc.begin(), updateDoc.end());
    }
}

void ReshardingOpObserver::onDelete(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const UUID& uuid,
                                    StmtId stmtId,
                                    const OplogDeleteEntryArgs& args) {
    if (nss == NamespaceString::kDonorReshardingOperationsNamespace) {
        _doPin(opCtx);
    }
}


}  // namespace mongo

/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/cleanup_structured_encryption_data_coordinator.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/fle_options_gen.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/commands/rename_collection_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/drop_gen.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/logv2/log.h"
#include "mongo/s/router_role.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(fleCleanupHangBeforeECOCCreate);
MONGO_FAIL_POINT_DEFINE(fleCleanupHangBeforeCleanupESCNonAnchors);
MONGO_FAIL_POINT_DEFINE(fleCleanupHangAfterCleanupESCAnchors);
MONGO_FAIL_POINT_DEFINE(fleCleanupHangAfterDropTempCollection);

const auto kMajorityWriteConcern = BSON("writeConcern" << BSON("w"
                                                               << "majority"));
/**
 * Issue a simple success/fail command such as renameCollection or drop
 * using majority write concern.
 */
template <typename Request>
Status doRunCommand(OperationContext* opCtx, const DatabaseName& dbname, const Request& request) {
    DBDirectClient client(opCtx);
    BSONObj cmd = request.toBSON(kMajorityWriteConcern);
    auto reply =
        client.runCommand(OpMsgRequestBuilder::create(dbname, std::move(cmd)))->getCommandReply();
    return getStatusFromCommandResult(reply);
}

void createQEClusteredStateCollection(OperationContext* opCtx, const NamespaceString& nss) {
    CreateCommand createCmd(nss);
    static const mongo::ClusteredIndexSpec clusterIdxSpec(BSON("_id" << 1), true);
    createCmd.setClusteredIndex(
        stdx::variant<bool, mongo::ClusteredIndexSpec>(std::move(clusterIdxSpec)));
    auto status = doRunCommand(opCtx, nss.dbName(), createCmd);
    if (!status.isOK()) {
        if (status != ErrorCodes::NamespaceExists) {
            uassertStatusOK(status);
        }
        LOGV2_DEBUG(7647901,
                    1,
                    "Create collection failed because namespace already exists",
                    "namespace"_attr = nss);
    }
}

void dropQEStateCollection(OperationContext* opCtx,
                           const NamespaceString& nss,
                           boost::optional<UUID> collId) {
    Drop cmd(nss);
    cmd.setCollectionUUID(collId);
    uassertStatusOK(doRunCommand(opCtx, nss.dbName(), cmd));
}

void checkRequiredOperation(const CleanupStructuredEncryptionDataState& state,
                            OperationContext* opCtx,
                            bool* needLeftoverAnchorCleanup,
                            bool* needRename,
                            bool* needEcocCreate,
                            bool* needCleanup) {
    const auto& ecocNss = state.getEcocNss();
    const auto& ecocRenameNss = state.getEcocRenameNss();
    const auto& escDeletesNss = state.getEscDeletesNss();

    auto catalog = CollectionCatalog::get(opCtx);

    auto ecocUuid = catalog->lookupUUIDByNSS(opCtx, ecocNss);
    auto ecocRenameUuid = catalog->lookupUUIDByNSS(opCtx, ecocRenameNss);
    auto escDeletesUuid = catalog->lookupUUIDByNSS(opCtx, escDeletesNss);

    auto hasEcocBefore = state.getEcocUuid().has_value();
    auto hasEcocNow = !!ecocUuid;
    auto hasEcocRenameBefore = state.getEcocRenameUuid().has_value();
    auto hasEcocRenameNow = !!ecocRenameUuid;
    auto hasEscDeletesBefore = state.getEscDeletesUuid().has_value();
    auto hasEscDeletesNow = !!escDeletesUuid;

    *needLeftoverAnchorCleanup = false;
    *needRename = false;
    *needEcocCreate = false;
    *needCleanup = true;

    // Check the current state of the 'esc.deletes' collection is consistent with the
    // state document. If not, then don't do cleanup.
    if (hasEscDeletesBefore != hasEscDeletesNow) {
        LOGV2_DEBUG(7647902,
                    1,
                    "Skipping cleanup due to change in collection state",
                    "escDeletesNss"_attr = escDeletesNss,
                    "hasEscDeletesBefore"_attr = hasEscDeletesBefore,
                    "hasEscDeletesNow"_attr = hasEscDeletesNow);
        *needCleanup = false;
        return;
    } else if (hasEscDeletesNow && escDeletesUuid.value() != state.getEscDeletesUuid().value()) {
        LOGV2_DEBUG(7647903,
                    1,
                    "Skipping cleanup due to mismatched collection uuid",
                    "escDeletesNss"_attr = escDeletesNss,
                    "uuid"_attr = escDeletesUuid.value(),
                    "expectedUUID"_attr = state.getEscDeletesUuid().value());
        *needCleanup = false;
        return;
    } else {
        *needLeftoverAnchorCleanup = hasEscDeletesNow;
    }

    if (hasEcocRenameBefore != hasEcocRenameNow) {
        LOGV2_DEBUG(7647904,
                    1,
                    "Skipping cleanup due to change in collection state",
                    "ecocRenameNss"_attr = ecocRenameNss,
                    "hasEcocRenameBefore"_attr = hasEcocRenameBefore,
                    "hasEcocRenameNow"_attr = hasEcocRenameNow);
        *needCleanup = false;
        return;
    }

    if (hasEcocRenameNow) {
        if (ecocRenameUuid.value() != state.getEcocRenameUuid().value()) {
            LOGV2_DEBUG(7647905,
                        1,
                        "Skipping cleanup due to mismatched collection uuid",
                        "ecocRenameNss"_attr = ecocRenameNss,
                        "uuid"_attr = ecocRenameUuid.value(),
                        "expectedUUID"_attr = state.getEcocRenameUuid().value());
            *needCleanup = false;
        }
        // If the ECOC does not exist, create it
        *needEcocCreate = !hasEcocNow;
        // The temp ECOC from a previous cleanup/compact still exists, so no need to rename.
        // This cleanup will use the existing temp ECOC.
        return;
    }

    if (!hasEcocNow) {
        // Nothing to rename & there's no existing temp ECOC, so skip cleanup.
        LOGV2_DEBUG(7647906,
                    1,
                    "Skipping rename stage as there is no source collection",
                    "ecocNss"_attr = ecocNss);
        *needCleanup = false;
    } else if (!hasEcocBefore) {
        // Mismatch of before/after state, so skip rename & cleanup.
        LOGV2_DEBUG(7647907,
                    1,
                    "Skipping cleanup due to change in collection state",
                    "ecocNss"_attr = ecocNss);
        *needCleanup = false;
    } else if (ecocUuid.value() != state.getEcocUuid().value()) {
        // The generation of the collection to be cleaned up is different than the one which was
        // requested. Skip rename & cleanup.
        LOGV2_DEBUG(7647908,
                    1,
                    "Skipping rename of mismatched collection uuid",
                    "ecocNss"_attr = ecocNss,
                    "uuid"_attr = ecocUuid.value(),
                    "expectedUUID"_attr = state.getEcocUuid().value());
        *needCleanup = false;
    } else {
        // ECOC is safe to rename & create; cleanup can be performed
        *needRename = true;
        *needEcocCreate = true;
    }
}

void doAnchorCleanupWithUpdatedCollectionState(OperationContext* opCtx,
                                               const NamespaceString& escNss,
                                               const NamespaceString& escDeletesNss,
                                               StringData description,
                                               ECStats* escStats) {
    auto tagsPerDelete =
        ServerParameterSet::getClusterParameterSet()
            ->get<ClusterParameterWithStorage<FLECompactionOptions>>("fleCompactionOptions")
            ->getValue(boost::none)
            .getMaxESCEntriesPerCompactionDelete();

    // Run the anchor cleanups in CollectionRouters to force refresh of catalog cache entries
    // for the ESC and ESC.deletes collections, and retry if write errors occur due to StaleConfig.
    sharding::router::CollectionRouter escDeletesRouter(opCtx->getServiceContext(), escDeletesNss);
    sharding::router::CollectionRouter escRouter(opCtx->getServiceContext(), escNss);

    // TODO: SERVER-77402 refactor once sharding API has better support for this use case
    escDeletesRouter.route(
        opCtx,
        description,
        [&](OperationContext* outerOpCtx, const CollectionRoutingInfo& outerCri) {
            tassert(7647923,
                    str::stream() << "Namespace " << escDeletesNss
                                  << " is expected to be unsharded, but is sharded",
                    !outerCri.cm.isSharded());

            onCollectionPlacementVersionMismatch(
                outerOpCtx, escDeletesNss, ChunkVersion::UNSHARDED());
            ScopedSetShardRole escDeletesShardRole(
                outerOpCtx, escDeletesNss, ShardVersion::UNSHARDED(), outerCri.cm.dbVersion());

            escRouter.route(
                outerOpCtx,
                description,
                [&](OperationContext* innerOpCtx, const CollectionRoutingInfo& innerCri) {
                    tassert(7647924,
                            str::stream() << "Namespace " << escNss
                                          << " is expected to be unsharded, but is sharded",
                            !innerCri.cm.isSharded());

                    onCollectionPlacementVersionMismatch(
                        innerOpCtx, escNss, ChunkVersion::UNSHARDED());
                    ScopedSetShardRole escShardRole(
                        innerOpCtx, escNss, ShardVersion::UNSHARDED(), innerCri.cm.dbVersion());

                    cleanupESCAnchors(innerOpCtx, escNss, escDeletesNss, tagsPerDelete, escStats);
                });
        });
}

bool doRenameOperation(const CleanupStructuredEncryptionDataState& state,
                       boost::optional<UUID>* newEcocRenameUuid,
                       boost::optional<UUID>* newEscDeletesUuid,
                       FLECompactESCDeleteSet* escDeleteSet,
                       ECStats* escStats) {
    LOGV2_DEBUG(
        7647909, 1, "Queryable Encryption cleanup entered rename phase", "state"_attr = state);

    const auto& ecocNss = state.getEcocNss();
    const auto& ecocRenameNss = state.getEcocRenameNss();
    const auto& escNss = state.getEscNss();
    const auto& escDeletesNss = state.getEscDeletesNss();
    auto opCtx = cc().makeOperationContext();

    bool needLeftoverAnchorCleanup, needRename, needEcocCreate, needCleanup;

    checkRequiredOperation(
        state, opCtx.get(), &needLeftoverAnchorCleanup, &needRename, &needEcocCreate, &needCleanup);

    *newEcocRenameUuid = state.getEcocRenameUuid();
    *newEscDeletesUuid = state.getEscDeletesUuid();

    if (needLeftoverAnchorCleanup) {
        LOGV2(7647910,
              "Cleaning up ESC deletes collection from a prior cleanup operation",
              logAttrs(escDeletesNss));
        doAnchorCleanupWithUpdatedCollectionState(
            opCtx.get(),
            escNss,
            escDeletesNss,
            "rename phase of queryable encryption cleanup coordinator"_sd,
            escStats);
        dropQEStateCollection(opCtx.get(), escDeletesNss, state.getEscDeletesUuid());
    }

    if (needRename) {
        invariant(needEcocCreate);

        if (escDeleteSet) {
            auto memoryLimit =
                ServerParameterSet::getClusterParameterSet()
                    ->get<ClusterParameterWithStorage<FLECompactionOptions>>("fleCompactionOptions")
                    ->getValue(boost::none)
                    .getMaxCompactionSize();

            *escDeleteSet =
                readRandomESCNonAnchorIds(opCtx.get(), state.getEscNss(), memoryLimit, escStats);
        }

        // Perform the rename so long as the target namespace does not exist.
        RenameCollectionCommand cmd(ecocNss, ecocRenameNss);
        cmd.setDropTarget(false);
        cmd.setCollectionUUID(state.getEcocUuid().value());

        uassertStatusOK(doRunCommand(opCtx.get(), ecocNss.dbName(), cmd));
        *newEcocRenameUuid = state.getEcocUuid();
    }

    if (needEcocCreate) {
        if (MONGO_unlikely(fleCleanupHangBeforeECOCCreate.shouldFail())) {
            LOGV2(7647911, "Hanging due to fleCleanupHangBeforeECOCCreate fail point");
            fleCleanupHangBeforeECOCCreate.pauseWhileSet();
        }

        // Create the new ECOC collection
        createQEClusteredStateCollection(opCtx.get(), ecocNss);
    }

    if (needCleanup) {
        // Create the temporary 'esc.deletes' clustered collection
        createQEClusteredStateCollection(opCtx.get(), escDeletesNss);

        auto catalog = CollectionCatalog::get(opCtx.get());
        *newEscDeletesUuid = catalog->lookupUUIDByNSS(opCtx.get(), escDeletesNss);
        invariant(newEscDeletesUuid->has_value());
    }

    // returns whether we can skip the remaining phases of cleanup
    return !needCleanup;
}

void doCleanupOperation(const CleanupStructuredEncryptionDataState& state,
                        const FLECompactESCDeleteSet& escDeleteSet,
                        ECStats* escStats,
                        ECOCStats* ecocStats) {
    LOGV2_DEBUG(
        7647912, 1, "Queryable Encryption cleanup entered cleanup phase", "state"_attr = state);

    if (state.getSkipCleanup()) {
        LOGV2_DEBUG(7647913,
                    1,
                    "Skipping cleanup structured encryption data phase",
                    logAttrs(state.getId().getNss()));
        return;
    }

    EncryptedStateCollectionsNamespaces namespaces;
    namespaces.edcNss = state.getId().getNss();
    namespaces.escNss = state.getEscNss();
    namespaces.ecocNss = state.getEcocNss();
    namespaces.ecocRenameNss = state.getEcocRenameNss();
    namespaces.escDeletesNss = state.getEscDeletesNss();
    auto opCtx = cc().makeOperationContext();
    CleanupStructuredEncryptionData request(namespaces.edcNss, state.getCleanupTokens());

    processFLECleanup(
        opCtx.get(), request, &getTransactionWithRetriesForMongoS, namespaces, escStats, ecocStats);

    if (MONGO_unlikely(fleCleanupHangBeforeCleanupESCNonAnchors.shouldFail())) {
        LOGV2(7647914, "Hanging due to fleCleanupHangBeforeCleanupESCNonAnchors fail point");
        fleCleanupHangBeforeCleanupESCNonAnchors.pauseWhileSet();
    }

    auto tagsPerDelete =
        ServerParameterSet::getClusterParameterSet()
            ->get<ClusterParameterWithStorage<FLECompactionOptions>>("fleCompactionOptions")
            ->getValue(boost::none)
            .getMaxESCEntriesPerCompactionDelete();

    cleanupESCNonAnchors(opCtx.get(), namespaces.escNss, escDeleteSet, tagsPerDelete, escStats);
}

void doAnchorRemovalOperation(const CleanupStructuredEncryptionDataState& state,
                              ECStats* escStats) {
    LOGV2_DEBUG(7647915,
                1,
                "Queryable Encryption cleanup entered anchor deletes phase",
                "state"_attr = state);

    if (state.getSkipCleanup()) {
        LOGV2_DEBUG(7647916, 1, "Skipping anchor cleanup phase", logAttrs(state.getId().getNss()));
        return;
    }

    auto opCtx = cc().makeOperationContext();
    auto escNss = state.getEscNss();
    auto escDeletesNss = state.getEscDeletesNss();

    doAnchorCleanupWithUpdatedCollectionState(
        opCtx.get(),
        escNss,
        escDeletesNss,
        "anchor deletes phase of queryable encryption cleanup coordinator"_sd,
        escStats);

    if (MONGO_unlikely(fleCleanupHangAfterCleanupESCAnchors.shouldFail())) {
        LOGV2(7647917, "Hanging due to fleCleanupHangAfterCleanupESCAnchors fail point");
        fleCleanupHangAfterCleanupESCAnchors.pauseWhileSet();
    }
}

void doDropOperation(const CleanupStructuredEncryptionDataState& state) {
    LOGV2_DEBUG(
        7647918, 1, "Queryable Encryption cleanup entered drop phase", "state"_attr = state);

    if (state.getSkipCleanup()) {
        LOGV2_DEBUG(7647919, 1, "Skipping cleanup drop phase", logAttrs(state.getId().getNss()));
        return;
    }

    auto opCtx = cc().makeOperationContext();
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto escDeletesNss = state.getEscDeletesNss();
    auto escDeletesUuid = catalog->lookupUUIDByNSS(opCtx.get(), escDeletesNss);
    auto ecocCompactNss = state.getEcocRenameNss();
    auto ecocCompactUuid = catalog->lookupUUIDByNSS(opCtx.get(), ecocCompactNss);

    if (escDeletesUuid) {
        dropQEStateCollection(opCtx.get(), escDeletesNss, state.getEscDeletesUuid());
    } else {
        LOGV2_DEBUG(7647920,
                    1,
                    "Skipping drop operation as 'esc.deletes' does not exist",
                    logAttrs(escDeletesNss));
    }

    if (ecocCompactUuid) {
        dropQEStateCollection(opCtx.get(), ecocCompactNss, state.getEcocRenameUuid());
    } else {
        LOGV2_DEBUG(7647921,
                    1,
                    "Skipping drop operation as 'ecoc.compact' does not exist",
                    logAttrs(ecocCompactNss));
    }

    if (MONGO_unlikely(fleCleanupHangAfterDropTempCollection.shouldFail())) {
        LOGV2(7647922, "Hanging due to fleCleanupHangAfterDropTempCollection fail point");
        fleCleanupHangAfterDropTempCollection.pauseWhileSet();
    }
}
}  // namespace


boost::optional<BSONObj> CleanupStructuredEncryptionDataCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    auto bob = basicReportBuilder();

    stdx::lock_guard lg{_docMutex};
    bob.append("escNss", _doc.getEscNss().ns());
    bob.append("ecocNss", _doc.getEcocNss().ns());
    bob.append("ecocUuid", _doc.getEcocUuid() ? _doc.getEcocUuid().value().toString() : "none");
    bob.append("ecocRenameNss", _doc.getEcocRenameNss().ns());
    bob.append("ecocRenameUuid",
               _doc.getEcocRenameUuid() ? _doc.getEcocRenameUuid().value().toString() : "none");
    bob.append("escDeletesNss", _doc.getEscDeletesNss().ns());
    bob.append("escDeletesUuid",
               _doc.getEscDeletesUuid() ? _doc.getEscDeletesUuid().value().toString() : "none");
    return bob.obj();
}

void CleanupStructuredEncryptionDataCoordinator::updateCleanupStats(const ECOCStats& phaseEcocStats,
                                                                    const ECStats& phaseEscStats) {
    // update stats in server status
    FLEStatusSection::get().updateCleanupStats(CleanupStats(phaseEcocStats, phaseEscStats));

    // update stats in state document
    stdx::lock_guard lg(_docMutex);
    auto docEscStats = _doc.getEscStats().value_or(ECStats{});
    auto docEcocStats = _doc.getEcocStats().value_or(ECOCStats{});
    FLEStatsUtil::accumulateStats(docEscStats, phaseEscStats);
    FLEStatsUtil::accumulateStats(docEcocStats, phaseEcocStats);
    _doc.setEscStats(docEscStats);
    _doc.setEcocStats(docEcocStats);
}

ExecutorFuture<void> CleanupStructuredEncryptionDataCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kRenameEcocForCleanup,
            [this, anchor = shared_from_this()]() {
                // if this was resumed from an interrupt, the _escDeleteSet
                // might not be empty, so clear it.
                _escDeleteSet.clear();

                ECStats phaseEscStats;
                boost::optional<UUID> ecocRenameUuid;
                boost::optional<UUID> escDeletesUuid;

                bool skipCleanup = doRenameOperation(
                    _doc, &ecocRenameUuid, &escDeletesUuid, &_escDeleteSet, &phaseEscStats);

                updateCleanupStats(ECOCStats{}, phaseEscStats);

                stdx::lock_guard lg(_docMutex);
                _doc.setSkipCleanup(skipCleanup);
                _doc.setEcocRenameUuid(ecocRenameUuid);
                _doc.setEscDeletesUuid(escDeletesUuid);
            }))
        .then(_buildPhaseHandler(Phase::kCleanupStructuredEncryptionData,
                                 [this, anchor = shared_from_this()]() {
                                     ECStats phaseEscStats;
                                     ECOCStats phaseEcocStats;

                                     doCleanupOperation(
                                         _doc, _escDeleteSet, &phaseEscStats, &phaseEcocStats);
                                     updateCleanupStats(phaseEcocStats, phaseEscStats);
                                 }))
        .then(_buildPhaseHandler(Phase::kDeleteAnchors,
                                 [this, anchor = shared_from_this()]() {
                                     ECStats phaseEscStats;

                                     doAnchorRemovalOperation(_doc, &phaseEscStats);
                                     updateCleanupStats(ECOCStats{}, phaseEscStats);
                                 }))
        .then(_buildPhaseHandler(Phase::kDropTempCollection, [this, anchor = shared_from_this()] {
            ECStats phaseEscStats = _doc.getEscStats().value_or(ECStats{});
            ECOCStats phaseEcocStats = _doc.getEcocStats().value_or(ECOCStats{});

            _response = CleanupStructuredEncryptionDataCommandReply(
                CleanupStats(phaseEcocStats, phaseEscStats));

            doDropOperation(_doc);
        }));
}

}  // namespace mongo

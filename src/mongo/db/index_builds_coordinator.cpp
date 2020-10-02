/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/index_builds_coordinator.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_build_entry_gen.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index_build_entry_helpers.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/storage_util.h"
#include "mongo/db/storage/two_phase_index_build_knobs_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/filesystem/operations.hpp>
#include <boost/iterator/transform_iterator.hpp>

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildFirstDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildSecondDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildDumpsInsertsFromBulk);
MONGO_FAIL_POINT_DEFINE(hangAfterInitializingIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangBeforeCompletingAbort);
MONGO_FAIL_POINT_DEFINE(failIndexBuildOnCommit);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildBeforeAbortCleanUp);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildOnStepUp);
MONGO_FAIL_POINT_DEFINE(hangAfterSettingUpResumableIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildBeforeCommit);
MONGO_FAIL_POINT_DEFINE(hangBeforeBuildingIndex);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildBeforeWaitingUntilMajorityOpTime);
MONGO_FAIL_POINT_DEFINE(failSetUpResumeIndexBuild);

namespace {

constexpr StringData kCreateIndexesFieldName = "createIndexes"_sd;
constexpr StringData kCommitIndexBuildFieldName = "commitIndexBuild"_sd;
constexpr StringData kAbortIndexBuildFieldName = "abortIndexBuild"_sd;
constexpr StringData kIndexesFieldName = "indexes"_sd;
constexpr StringData kKeyFieldName = "key"_sd;
constexpr StringData kUniqueFieldName = "unique"_sd;

/**
 * Checks if unique index specification is compatible with sharding configuration.
 */
void checkShardKeyRestrictions(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& newIdxKey) {
    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx, nss);

    const auto collDesc = CollectionShardingState::get(opCtx, nss)->getCollectionDescription(opCtx);
    if (!collDesc.isSharded())
        return;

    const ShardKeyPattern shardKeyPattern(collDesc.getKeyPattern());
    uassert(ErrorCodes::CannotCreateIndex,
            str::stream() << "cannot create unique index over " << newIdxKey
                          << " with shard key pattern " << shardKeyPattern.toBSON(),
            shardKeyPattern.isUniqueIndexCompatible(newIdxKey));
}

/**
 * Returns true if we should build the indexes an empty collection using the IndexCatalog and
 * bypass the index build registration.
 */
bool shouldBuildIndexesOnEmptyCollectionSinglePhased(OperationContext* opCtx,
                                                     const CollectionPtr& collection,
                                                     IndexBuildProtocol protocol) {
    const auto& nss = collection->ns();
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X), str::stream() << nss);

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // Check whether the replica set member's config has {buildIndexes:false} set, which means
    // we are not allowed to build non-_id indexes on this server.
    if (!replCoord->buildsIndexes()) {
        return false;
    }

    // Secondaries should not bypass index build registration (and _runIndexBuild()) for two phase
    // index builds because they need to report index build progress to the primary per commit
    // quorum.
    if (IndexBuildProtocol::kTwoPhase == protocol && replCoord->getSettings().usingReplSets() &&
        !replCoord->canAcceptWritesFor(opCtx, nss)) {
        return false;
    }

    // We use the fast count information, through Collection::numRecords(), to determine if the
    // collection is empty. However, this information is either unavailable or inaccurate when the
    // node is in certain replication states, such as recovery or rollback. In these cases, we
    // have to build the index by scanning the collection.
    auto memberState = replCoord->getMemberState();
    if (memberState.rollback()) {
        return false;
    }
    if (inReplicationRecovery(opCtx->getServiceContext())) {
        return false;
    }

    // Now, it's fine to trust Collection::isEmpty().
    // Fast counts are prone to both false positives and false negatives on unclean shutdowns. False
    // negatives can cause to skip index building. And, false positives can cause mismatch in number
    // of index entries among the nodes in the replica set. So, verify the collection is really
    // empty by opening the WT cursor and reading the first document.
    return collection->isEmpty(opCtx);
}

/*
 * Determines whether to skip the index build state transition check.
 * Index builder not using ReplIndexBuildState::waitForNextAction to signal primary and secondaries
 * to commit or abort signal will violate index build state transition. So, we should skip state
 * transition verification. Otherwise, we would invariant.
 */
bool shouldSkipIndexBuildStateTransitionCheck(OperationContext* opCtx,
                                              IndexBuildProtocol protocol) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().usingReplSets() && protocol == IndexBuildProtocol::kTwoPhase) {
        return false;
    }
    return true;
}

/**
 * Removes the index build from the config.system.indexBuilds collection after the primary has
 * written the commitIndexBuild or abortIndexBuild oplog entry.
 */
void removeIndexBuildEntryAfterCommitOrAbort(OperationContext* opCtx,
                                             const NamespaceStringOrUUID& dbAndUUID,
                                             const ReplIndexBuildState& replState) {
    if (IndexBuildProtocol::kSinglePhase == replState.protocol) {
        return;
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->canAcceptWritesFor(opCtx, dbAndUUID)) {
        return;
    }

    auto status = indexbuildentryhelpers::removeIndexBuildEntry(opCtx, replState.buildUUID);

    // If we fail to remove the document from config.system.indexBuilds, it is because the document
    // or collection is missing. In any case, we do not need to fail the commit or abort operation.
    // TODO(SERVER-47323): Do not ignore removeIndexBuildEntry() errors. Convert to fatal assertion.
    if (!status.isOK()) {
        LOGV2(4763501,
              "Unable to remove index build from system collection. Ignoring error",
              "buildUUID"_attr = replState.buildUUID,
              "collectionUUID"_attr = replState.collectionUUID,
              "error"_attr = status);
    }
}


/**
 * Replicates a commitIndexBuild oplog entry for two-phase builds, which signals downstream
 * secondary nodes to commit the index build.
 */
void onCommitIndexBuild(OperationContext* opCtx,
                        const NamespaceString& nss,
                        std::shared_ptr<ReplIndexBuildState> replState) {
    const auto& buildUUID = replState->buildUUID;

    auto skipCheck = shouldSkipIndexBuildStateTransitionCheck(opCtx, replState->protocol);
    opCtx->recoveryUnit()->onCommit([replState, skipCheck](boost::optional<Timestamp> commitTime) {
        stdx::unique_lock<Latch> lk(replState->mutex);
        replState->indexBuildState.setState(IndexBuildState::kCommitted, skipCheck);
    });
    if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
        return;
    }

    invariant(IndexBuildProtocol::kTwoPhase == replState->protocol,
              str::stream() << "onCommitIndexBuild: " << buildUUID);
    invariant(opCtx->lockState()->isWriteLocked(),
              str::stream() << "onCommitIndexBuild: " << buildUUID);

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    const auto& collUUID = replState->collectionUUID;
    const auto& indexSpecs = replState->indexSpecs;
    auto fromMigrate = false;

    // Since two phase index builds are allowed to survive replication state transitions, we should
    // check if the node is currently a primary before attempting to write to the oplog.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->canAcceptWritesFor(opCtx, nss)) {
        invariant(!opCtx->recoveryUnit()->getCommitTimestamp().isNull(),
                  str::stream() << "commitIndexBuild: " << buildUUID);
        return;
    }

    opObserver->onCommitIndexBuild(opCtx, nss, collUUID, buildUUID, indexSpecs, fromMigrate);
}

/**
 * Replicates an abortIndexBuild oplog entry for two-phase builds, which signals downstream
 * secondary nodes to abort the index build.
 */
void onAbortIndexBuild(OperationContext* opCtx,
                       const NamespaceString& nss,
                       ReplIndexBuildState& replState,
                       const Status& cause) {
    if (IndexBuildProtocol::kTwoPhase != replState.protocol) {
        return;
    }

    invariant(opCtx->lockState()->isWriteLocked(), replState.buildUUID.toString());

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    auto collUUID = replState.collectionUUID;
    auto fromMigrate = false;
    opObserver->onAbortIndexBuild(
        opCtx, nss, collUUID, replState.buildUUID, replState.indexSpecs, cause, fromMigrate);
}

/**
 * We do not need synchronization with step up and step down. Dropping the RSTL is important because
 * otherwise if we held the RSTL it would create deadlocks with prepared transactions on step up and
 * step down.  A deadlock could result if the index build was attempting to acquire a Collection S
 * or X lock while a prepared transaction held a Collection IX lock, and a step down was waiting to
 * acquire the RSTL in mode X.
 */
void unlockRSTL(OperationContext* opCtx) {
    invariant(opCtx->lockState()->unlockRSTLforPrepare());
    invariant(!opCtx->lockState()->isRSTLLocked());
}

/**
 * Logs the index build failure error in a standard format.
 */
void logFailure(Status status,
                const NamespaceString& nss,
                std::shared_ptr<ReplIndexBuildState> replState) {
    LOGV2(20649,
          "Index build failed",
          "buildUUID"_attr = replState->buildUUID,
          "namespace"_attr = nss,
          "uuid"_attr = replState->collectionUUID,
          "error"_attr = status);
}

/**
 * Iterates over index builds with the provided function.
 */
void forEachIndexBuild(
    const std::vector<std::shared_ptr<ReplIndexBuildState>>& indexBuilds,
    StringData context,
    std::function<void(std::shared_ptr<ReplIndexBuildState> replState)> onIndexBuild) {
    if (indexBuilds.empty()) {
        return;
    }

    auto indexBuildLogger = [](const auto& indexBuild) {
        BSONObjBuilder builder;
        builder.append("buildUUID"_sd, indexBuild->buildUUID.toBSON());
        builder.append("collectionUUID"_sd, indexBuild->collectionUUID.toBSON());

        BSONArrayBuilder names;
        for (const auto& indexName : indexBuild->indexNames) {
            names.append(indexName);
        }
        builder.append("indexNames"_sd, names.arr());
        builder.append("protocol"_sd,
                       indexBuild->protocol == IndexBuildProtocol::kTwoPhase ? "two phase"_sd
                                                                             : "single phase"_sd);

        return builder.obj();
    };
    auto begin = boost::make_transform_iterator(indexBuilds.begin(), indexBuildLogger);
    auto end = boost::make_transform_iterator(indexBuilds.end(), indexBuildLogger);

    LOGV2(20650,
          "Active index builds",
          "context"_attr = context,
          "builds"_attr = logv2::seqLog(begin, end));

    if (onIndexBuild) {
        for (const auto& indexBuild : indexBuilds) {
            onIndexBuild(indexBuild);
        }
    }
}

/**
 * Updates currentOp for commitIndexBuild or abortIndexBuild.
 */
void updateCurOpForCommitOrAbort(OperationContext* opCtx, StringData fieldName, UUID buildUUID) {
    BSONObjBuilder builder;
    buildUUID.appendToBuilder(&builder, fieldName);
    stdx::unique_lock<Client> lk(*opCtx->getClient());
    auto curOp = CurOp::get(opCtx);
    builder.appendElementsUnique(curOp->opDescription());
    auto opDescObj = builder.obj();
    curOp->setLogicalOp_inlock(LogicalOp::opCommand);
    curOp->setOpDescription_inlock(opDescObj);
    curOp->ensureStarted();
}

/**
 * Fetches the latest oplog entry's optime. Bypasses the oplog visibility rules.
 */
repl::OpTime getLatestOplogOpTime(OperationContext* opCtx) {
    // Reset the snapshot so that it is ensured to see the latest oplog entries.
    opCtx->recoveryUnit()->abandonSnapshot();

    // Helpers::getLast will bypass the oplog visibility rules by doing a backwards collection
    // scan.
    BSONObj oplogEntryBSON;
    invariant(
        Helpers::getLast(opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), oplogEntryBSON));

    auto optime = repl::OpTime::parseFromOplogEntry(oplogEntryBSON);
    invariant(optime.isOK(),
              str::stream() << "Found an invalid oplog entry: " << oplogEntryBSON
                            << ", error: " << optime.getStatus());
    return optime.getValue();
}

/**
 * Returns true if the index build is resumable.
 */
bool isIndexBuildResumable(OperationContext* opCtx,
                           const ReplIndexBuildState& replState,
                           const IndexBuildsCoordinator::IndexBuildOptions& indexBuildOptions) {

    if (replState.protocol != IndexBuildProtocol::kTwoPhase) {
        return false;
    }

    if (indexBuildOptions.applicationMode != IndexBuildsCoordinator::ApplicationMode::kNormal) {
        return false;
    }

    // This check may be unnecessary due to current criteria for resumable index build support in
    // storage engine.
    if (!serverGlobalParams.enableMajorityReadConcern) {
        return false;
    }

    // The last optime could be null if the node is in initial sync while building the index.
    // This check may be redundant with the 'applicationMode' check and the replication requirement
    // for two phase index builds.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeNone) {
        return false;
    }

    // When we are applying a startIndexBuild oplog entry during the oplog application phase of
    // startup recovery, the last optime here derived from the local oplog may not be a valid
    // optime to wait on for the majority commit point since the rest of the replica set may
    // be on a different branch of history.
    if (inReplicationRecovery(opCtx->getServiceContext())) {
        LOGV2(5039100,
              "Index build: in replication recovery. Not waiting for last optime before "
              "interceptors to be majority committed",
              "buildUUID"_attr = replState.buildUUID);
        return false;
    }

    // TODO(SERVER-50479): Remove this check when resumable index builds work with ESE in GCM mode.
    if (EncryptionHooks::get(opCtx->getServiceContext())->enabled()) {
        return false;
    }

    if (!opCtx->getServiceContext()->getStorageEngine()->supportsResumableIndexBuilds()) {
        return false;
    }

    // Only index builds with the default "all-voters" commit quorum running on voting nodes should
    // be resumable. A node that cannot contribute to the commit quorum should not be waiting for
    // the majority commit point when trying to commit the index build.
    // IndexBuildsOptions::commitQuorum will be set if we are primary. Otherwise, we have to check
    // the config.system.indexBuilds collection.
    if (indexBuildOptions.commitQuorum) {
        if (CommitQuorumOptions::kVotingMembers != indexBuildOptions.commitQuorum->mode) {
            return false;
        }
    } else {
        // The commit quorum may be updated using the setIndexBuildCommitQuorum command, so we will
        // rely on the deadline to unblock ourselves from the majority wait if the commit quorum is
        // no longer "all-voters".
        auto swCommitQuorum = indexbuildentryhelpers::getCommitQuorum(opCtx, replState.buildUUID);
        if (!swCommitQuorum.isOK()) {
            LOGV2(5044600,
                  "Index build: cannot read commit quorum from config db. "
                  "Index build will not be resumable.",
                  "buildUUID"_attr = replState.buildUUID,
                  "error"_attr = swCommitQuorum.getStatus());
            return false;
        }
        auto commitQuorum = swCommitQuorum.getValue();
        if (CommitQuorumOptions::kVotingMembers != commitQuorum.mode) {
            return false;
        }
    }

    // Ensure that this node is a voting member in the replica set config.
    auto rsConfig = replCoord->getConfig();
    auto hap = replCoord->getMyHostAndPort();
    if (auto memberConfig = rsConfig.findMemberByHostAndPort(hap)) {
        if (!memberConfig->isVoter()) {
            return false;
        }
    } else {
        // We cannot determine our member config, so skip the majority wait and leave this index
        // build as non-resumable.
        return false;
    }

    return true;
}

/**
 * Returns the ReadSource to be used for a drain occurring before the commit quorum has been
 * satisfied.
 */
RecoveryUnit::ReadSource getReadSourceForDrainBeforeCommitQuorum(
    const ReplIndexBuildState& replState) {
    return replState.lastOpTimeBeforeInterceptors.isNull()
        ? RecoveryUnit::ReadSource::kNoTimestamp
        : RecoveryUnit::ReadSource::kMajorityCommitted;
}

}  // namespace

const auto getIndexBuildsCoord =
    ServiceContext::declareDecoration<std::unique_ptr<IndexBuildsCoordinator>>();

void IndexBuildsCoordinator::set(ServiceContext* serviceContext,
                                 std::unique_ptr<IndexBuildsCoordinator> ibc) {
    auto& indexBuildsCoordinator = getIndexBuildsCoord(serviceContext);
    invariant(!indexBuildsCoordinator);

    indexBuildsCoordinator = std::move(ibc);
}

IndexBuildsCoordinator* IndexBuildsCoordinator::get(ServiceContext* serviceContext) {
    auto& indexBuildsCoordinator = getIndexBuildsCoord(serviceContext);
    invariant(indexBuildsCoordinator);

    return indexBuildsCoordinator.get();
}

IndexBuildsCoordinator* IndexBuildsCoordinator::get(OperationContext* OperationContext) {
    return get(OperationContext->getServiceContext());
}

IndexBuildsCoordinator::~IndexBuildsCoordinator() {
    invariant(_allIndexBuilds.empty());
}

std::vector<std::string> IndexBuildsCoordinator::extractIndexNames(
    const std::vector<BSONObj>& specs) {
    std::vector<std::string> indexNames;
    for (const auto& spec : specs) {
        std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
        invariant(!name.empty(),
                  str::stream() << "Bad spec passed into ReplIndexBuildState constructor, missing '"
                                << IndexDescriptor::kIndexNameFieldName << "' field: " << spec);
        indexNames.push_back(name);
    }
    return indexNames;
}

StatusWith<std::pair<long long, long long>> IndexBuildsCoordinator::rebuildIndexesForRecovery(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<BSONObj>& specs,
    const UUID& buildUUID,
    RepairData repair) {

    const auto protocol = IndexBuildProtocol::kSinglePhase;
    auto status = _startIndexBuildForRecovery(opCtx, nss, specs, buildUUID, protocol);
    if (!status.isOK()) {
        return status;
    }

    CollectionWriter collection(opCtx, nss, CollectionCatalog::LifetimeMode::kInplace);

    // Complete the index build.
    return _runIndexRebuildForRecovery(opCtx, collection, buildUUID, repair);
}

Status IndexBuildsCoordinator::_startIndexBuildForRecovery(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           const std::vector<BSONObj>& specs,
                                                           const UUID& buildUUID,
                                                           IndexBuildProtocol protocol) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));

    std::vector<std::string> indexNames;
    for (auto& spec : specs) {
        std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
        if (name.empty()) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream()
                              << "Cannot create an index for a spec '" << spec
                              << "' without a non-empty string value for the 'name' field");
        }
        indexNames.push_back(name);
    }

    CollectionWriter collection(opCtx, nss, CollectionCatalog::LifetimeMode::kInplace);
    {
        // These steps are combined into a single WUOW to ensure there are no commits without
        // the indexes.
        // 1) Drop all unfinished indexes.
        // 2) Start, but do not complete the index build process.
        WriteUnitOfWork wuow(opCtx);
        auto indexCatalog = collection.getWritableCollection()->getIndexCatalog();


        for (size_t i = 0; i < indexNames.size(); i++) {
            bool includeUnfinished = false;
            auto descriptor =
                indexCatalog->findIndexByName(opCtx, indexNames[i], includeUnfinished);
            if (descriptor) {
                Status s = indexCatalog->dropIndex(opCtx, descriptor);
                if (!s.isOK()) {
                    return s;
                }
                continue;
            }

            // If the index is not present in the catalog, then we are trying to drop an already
            // aborted index. This may happen when rollback-via-refetch restarts an index build
            // after an abort has been rolled back.
            if (!DurableCatalog::get(opCtx)->isIndexPresent(
                    opCtx, collection->getCatalogId(), indexNames[i])) {
                LOGV2(20652,
                      "The index for build {buildUUID} was not found while trying to drop the "
                      "index during recovery: {index}",
                      "An index was not found in the catalog while trying to drop the index during "
                      "recovery",
                      "buildUUID"_attr = buildUUID,
                      "index"_attr = indexNames[i]);
                continue;
            }

            const auto durableBuildUUID = DurableCatalog::get(opCtx)->getIndexBuildUUID(
                opCtx, collection->getCatalogId(), indexNames[i]);

            // A build UUID is present if and only if we are rebuilding a two-phase build.
            invariant((protocol == IndexBuildProtocol::kTwoPhase) ==
                      durableBuildUUID.is_initialized());
            // When a buildUUID is present, it must match the build UUID parameter to this
            // function.
            invariant(!durableBuildUUID || *durableBuildUUID == buildUUID,
                      str::stream() << "durable build UUID: " << durableBuildUUID
                                    << "buildUUID: " << buildUUID);

            // If the unfinished index is in the IndexCatalog, drop it through there, otherwise drop
            // it from the DurableCatalog. Rollback-via-refetch does not clear any in-memory state,
            // so we should do it manually here.
            includeUnfinished = true;
            descriptor = indexCatalog->findIndexByName(opCtx, indexNames[i], includeUnfinished);
            if (descriptor) {
                Status s = indexCatalog->dropUnfinishedIndex(opCtx, descriptor);
                if (!s.isOK()) {
                    return s;
                }
            } else {
                // There are no concurrent users of the index during startup recovery, so it is OK
                // to pass in a nullptr for the index 'ident', promising that the index is not in
                // use.
                catalog::removeIndex(opCtx,
                                     indexNames[i],
                                     collection->getCatalogId(),
                                     collection->uuid(),
                                     collection->ns(),
                                     nullptr /* ident */);
            }
        }

        // We need to initialize the collection to rebuild the indexes. The collection may already
        // be initialized when rebuilding indexes with rollback-via-refetch.
        if (!collection->isInitialized()) {
            collection.getWritableCollection()->init(opCtx);
        }

        auto dbName = nss.db().toString();
        auto replIndexBuildState = std::make_shared<ReplIndexBuildState>(
            buildUUID, collection->uuid(), dbName, specs, protocol);

        Status status = [&]() {
            stdx::unique_lock<Latch> lk(_mutex);
            return _registerIndexBuild(lk, replIndexBuildState);
        }();
        if (!status.isOK()) {
            return status;
        }

        IndexBuildsManager::SetupOptions options;
        options.protocol = protocol;
        status = _indexBuildsManager.setUpIndexBuild(
            opCtx, collection, specs, buildUUID, MultiIndexBlock::kNoopOnInitFn, options);
        if (!status.isOK()) {
            // An index build failure during recovery is fatal.
            logFailure(status, nss, replIndexBuildState);
            fassertNoTrace(51086, status);
        }

        wuow.commit();
    }

    return Status::OK();
}

Status IndexBuildsCoordinator::_setUpResumeIndexBuild(OperationContext* opCtx,
                                                      std::string dbName,
                                                      CollectionUUID collectionUUID,
                                                      const std::vector<BSONObj>& specs,
                                                      const UUID& buildUUID,
                                                      const ResumeIndexInfo& resumeInfo) {
    NamespaceStringOrUUID nssOrUuid{dbName, collectionUUID};

    if (MONGO_unlikely(failSetUpResumeIndexBuild.shouldFail())) {
        return {ErrorCodes::FailPointEnabled, "failSetUpResumeIndexBuild fail point is enabled"};
    }

    Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
    Lock::CollectionLock collLock(opCtx, nssOrUuid, MODE_X);

    CollectionWriter collection(
        opCtx, resumeInfo.getCollectionUUID(), CollectionCatalog::LifetimeMode::kInplace);
    invariant(collection);
    auto durableCatalog = DurableCatalog::get(opCtx);

    for (auto spec : specs) {
        std::string indexName = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
        if (indexName.empty()) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream()
                              << "Cannot create an index for a spec '" << spec
                              << "' without a non-empty string value for the 'name' field");
        }

        // Check that the information in the durable catalog matches the resume info.
        uassert(4841702,
                "Index not found in durable catalog while attempting to resume index build",
                durableCatalog->isIndexPresent(opCtx, collection->getCatalogId(), indexName));

        const auto durableBuildUUID =
            durableCatalog->getIndexBuildUUID(opCtx, collection->getCatalogId(), indexName);
        uassert(ErrorCodes::IndexNotFound,
                str::stream() << "Cannot resume index build with a buildUUID: " << buildUUID
                              << " that did not match the buildUUID in the durable catalog: "
                              << durableBuildUUID,
                durableBuildUUID == buildUUID);

        auto indexIdent =
            durableCatalog->getIndexIdent(opCtx, collection->getCatalogId(), indexName);
        uassert(
            4841703,
            str::stream() << "No index ident found on disk that matches the index build to resume: "
                          << indexName,
            indexIdent.size() > 0);

        uassertStatusOK(durableCatalog->checkMetaDataForIndex(
            opCtx, collection->getCatalogId(), indexName, spec));
    }

    if (!collection->isInitialized()) {
        collection.getWritableCollection()->init(opCtx);
    }

    auto protocol = IndexBuildProtocol::kTwoPhase;
    auto replIndexBuildState = std::make_shared<ReplIndexBuildState>(
        buildUUID, collection->uuid(), dbName, specs, protocol);

    Status status = [&]() {
        stdx::unique_lock<Latch> lk(_mutex);
        return _registerIndexBuild(lk, replIndexBuildState);
    }();
    if (!status.isOK()) {
        return status;
    }

    IndexBuildsManager::SetupOptions options;
    options.protocol = protocol;
    status = _indexBuildsManager.setUpIndexBuild(
        opCtx, collection, specs, buildUUID, MultiIndexBlock::kNoopOnInitFn, options, resumeInfo);
    if (!status.isOK()) {
        LOGV2(4841705,
              "Failed to resume index build",
              "buildUUID"_attr = buildUUID,
              logAttrs(collection->ns()),
              "collectionUUID"_attr = collectionUUID,
              "error"_attr = status);

        stdx::lock_guard<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replIndexBuildState);
    }

    return status;
}

std::string IndexBuildsCoordinator::_indexBuildActionToString(IndexBuildAction action) {
    if (action == IndexBuildAction::kNoAction) {
        return "No action";
    } else if (action == IndexBuildAction::kOplogCommit) {
        return "Oplog commit";
    } else if (action == IndexBuildAction::kOplogAbort) {
        return "Oplog abort";
    } else if (action == IndexBuildAction::kInitialSyncAbort) {
        return "Initial sync abort";
    } else if (action == IndexBuildAction::kRollbackAbort) {
        return "Rollback abort";
    } else if (action == IndexBuildAction::kPrimaryAbort) {
        return "Primary abort";
    } else if (action == IndexBuildAction::kSinglePhaseCommit) {
        return "Single-phase commit";
    } else if (action == IndexBuildAction::kCommitQuorumSatisfied) {
        return "Commit quorum Satisfied";
    }
    MONGO_UNREACHABLE;
}

void IndexBuildsCoordinator::waitForAllIndexBuildsToStopForShutdown(OperationContext* opCtx) {
    stdx::unique_lock<Latch> lk(_mutex);

    // All index builds should have been signaled to stop via the ServiceContext.

    if (_allIndexBuilds.empty()) {
        return;
    }

    auto indexBuildToUUID = [](const auto& indexBuild) { return indexBuild.first; };
    auto begin = boost::make_transform_iterator(_allIndexBuilds.begin(), indexBuildToUUID);
    auto end = boost::make_transform_iterator(_allIndexBuilds.end(), indexBuildToUUID);
    LOGV2(4725201,
          "Waiting until the following index builds are finished",
          "indexBuilds"_attr = logv2::seqLog(begin, end));

    // Wait for all the index builds to stop.
    auto pred = [this]() { return _allIndexBuilds.empty(); };
    _indexBuildsCondVar.wait(lk, pred);
}

std::vector<UUID> IndexBuildsCoordinator::abortCollectionIndexBuilds(
    OperationContext* opCtx,
    const NamespaceString collectionNss,
    const UUID collectionUUID,
    const std::string& reason) {
    auto collIndexBuilds = [&]() -> std::vector<std::shared_ptr<ReplIndexBuildState>> {
        stdx::unique_lock<Latch> lk(_mutex);
        auto indexBuildFilter = [=](const auto& replState) {
            return collectionUUID == replState.collectionUUID;
        };
        return _filterIndexBuilds_inlock(lk, indexBuildFilter);
    }();

    if (collIndexBuilds.empty()) {
        return {};
    }

    LOGV2(23879,
          "About to abort all index builders",
          "namespace"_attr = collectionNss,
          "uuid"_attr = collectionUUID,
          "reason"_attr = reason);

    std::vector<UUID> buildUUIDs;
    for (auto replState : collIndexBuilds) {
        if (abortIndexBuildByBuildUUID(
                opCtx, replState->buildUUID, IndexBuildAction::kPrimaryAbort, reason)) {
            buildUUIDs.push_back(replState->buildUUID);
        }
    }
    return buildUUIDs;
}

void IndexBuildsCoordinator::_awaitNoBgOpInProgForDb(stdx::unique_lock<Latch>& lk,
                                                     OperationContext* opCtx,
                                                     StringData db) {
    auto indexBuildFilter = [db](const auto& replState) { return db == replState.dbName; };
    auto pred = [&, this]() {
        auto dbIndexBuilds = _filterIndexBuilds_inlock(lk, indexBuildFilter);
        return dbIndexBuilds.empty();
    };
    _indexBuildsCondVar.wait(lk, pred);
}

void IndexBuildsCoordinator::abortDatabaseIndexBuilds(OperationContext* opCtx,
                                                      StringData db,
                                                      const std::string& reason) {
    LOGV2(4612302,
          "About to abort all index builders running for collections in the given database",
          "database"_attr = db,
          "reason"_attr = reason);

    auto builds = [&]() -> std::vector<std::shared_ptr<ReplIndexBuildState>> {
        stdx::unique_lock<Latch> lk(_mutex);
        auto indexBuildFilter = [=](const auto& replState) { return db == replState.dbName; };
        return _filterIndexBuilds_inlock(lk, indexBuildFilter);
    }();
    for (auto replState : builds) {
        if (!abortIndexBuildByBuildUUID(
                opCtx, replState->buildUUID, IndexBuildAction::kPrimaryAbort, reason)) {
            // The index build may already be in the midst of tearing down.
            LOGV2(5010502,
                  "Index build: failed to abort index build for database drop",
                  "buildUUID"_attr = replState->buildUUID,
                  "database"_attr = db,
                  "collectionUUID"_attr = replState->collectionUUID);
        }
    }
}

void IndexBuildsCoordinator::abortAllIndexBuildsForInitialSync(OperationContext* opCtx,
                                                               const std::string& reason) {
    LOGV2(4833200, "About to abort all index builders running", "reason"_attr = reason);

    auto builds = [&]() -> std::vector<std::shared_ptr<ReplIndexBuildState>> {
        stdx::unique_lock<Latch> lk(_mutex);
        auto indexBuildFilter = [](const auto& replState) { return true; };
        return _filterIndexBuilds_inlock(lk, indexBuildFilter);
    }();
    for (auto replState : builds) {
        if (!abortIndexBuildByBuildUUID(
                opCtx, replState->buildUUID, IndexBuildAction::kInitialSyncAbort, reason)) {
            // The index build may already be in the midst of tearing down.
            LOGV2(5010503,
                  "Index build: failed to abort index build for initial sync",
                  "buildUUID"_attr = replState->buildUUID,
                  "database"_attr = replState->dbName,
                  "collectionUUID"_attr = replState->collectionUUID);
        }
    }
}

namespace {
NamespaceString getNsFromUUID(OperationContext* opCtx, const UUID& uuid) {
    auto& catalog = CollectionCatalog::get(opCtx);
    auto nss = catalog.lookupNSSByUUID(opCtx, uuid);
    uassert(ErrorCodes::NamespaceNotFound, "No namespace with UUID " + uuid.toString(), nss);
    return *nss;
}
}  // namespace

void IndexBuildsCoordinator::applyStartIndexBuild(OperationContext* opCtx,
                                                  ApplicationMode applicationMode,
                                                  const IndexBuildOplogEntry& oplogEntry) {
    const auto collUUID = oplogEntry.collUUID;
    const auto nss = getNsFromUUID(opCtx, collUUID);

    IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions;
    indexBuildOptions.applicationMode = applicationMode;

    // If this is an initial syncing node, drop any conflicting ready index specs prior to
    // proceeding with building them.
    if (indexBuildOptions.applicationMode == ApplicationMode::kInitialSync) {
        auto dbAndUUID = NamespaceStringOrUUID(nss.db().toString(), collUUID);
        writeConflictRetry(opCtx, "IndexBuildsCoordinator::applyStartIndexBuild", nss.ns(), [&] {
            WriteUnitOfWork wuow(opCtx);

            AutoGetCollection coll(opCtx, dbAndUUID, MODE_X);
            invariant(coll,
                      str::stream() << "Collection with UUID " << collUUID << " was dropped.");

            IndexCatalog* indexCatalog = coll.getWritableCollection()->getIndexCatalog();

            const bool includeUnfinished = false;
            for (const auto& spec : oplogEntry.indexSpecs) {
                std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
                uassert(ErrorCodes::BadValue,
                        str::stream() << "Index spec is missing the 'name' field " << spec,
                        !name.empty());

                if (auto desc = indexCatalog->findIndexByName(opCtx, name, includeUnfinished)) {
                    uassertStatusOK(indexCatalog->dropIndex(opCtx, desc));
                }
            }

            wuow.commit();
        });
    }

    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    uassertStatusOK(
        indexBuildsCoord
            ->startIndexBuild(opCtx,
                              nss.db().toString(),
                              collUUID,
                              oplogEntry.indexSpecs,
                              oplogEntry.buildUUID,
                              /* This oplog entry is only replicated for two-phase index builds */
                              IndexBuildProtocol::kTwoPhase,
                              indexBuildOptions)
            .getStatus());
}

void IndexBuildsCoordinator::applyCommitIndexBuild(OperationContext* opCtx,
                                                   const IndexBuildOplogEntry& oplogEntry) {
    const auto collUUID = oplogEntry.collUUID;
    const auto nss = getNsFromUUID(opCtx, collUUID);
    const auto& buildUUID = oplogEntry.buildUUID;

    updateCurOpForCommitOrAbort(opCtx, kCommitIndexBuildFieldName, buildUUID);

    uassert(31417,
            str::stream()
                << "No commit timestamp set while applying commitIndexBuild operation. Build UUID: "
                << buildUUID,
            !opCtx->recoveryUnit()->getCommitTimestamp().isNull());

    // There is a possibility that we cannot find an active index build with the given build UUID.
    // This can be the case when the index already exists or was dropped on the sync source before
    // the collection was cloned during initial sync. The oplog code will ignore the NoSuchKey
    // error code.
    //
    // Case 1: Index already exists:
    // +-----------------------------------------+--------------------------------+
    // |               Sync Target               |          Sync Source           |
    // +-----------------------------------------+--------------------------------+
    // |                                         | startIndexBuild 'x' at TS: 1.  |
    // | Start oplog fetcher at TS: 2.           |                                |
    // |                                         | commitIndexBuild 'x' at TS: 2. |
    // | Begin cloning the collection.           |                                |
    // | Index 'x' is listed as ready, build it. |                                |
    // | Finish cloning the collection.          |                                |
    // | Start the oplog replay phase.           |                                |
    // | Apply commitIndexBuild 'x'.             |                                |
    // | --- Index build not found ---           |                                |
    // +-----------------------------------------+--------------------------------+
    //
    // Case 2: Sync source dropped the index:
    // +--------------------------------+--------------------------------+
    // |          Sync Target           |          Sync Source           |
    // +--------------------------------+--------------------------------+
    // |                                | startIndexBuild 'x' at TS: 1.  |
    // | Start oplog fetcher at TS: 2.  |                                |
    // |                                | commitIndexBuild 'x' at TS: 2. |
    // |                                | dropIndex 'x' at TS: 3.        |
    // | Begin cloning the collection.  |                                |
    // | No user indexes to build.      |                                |
    // | Finish cloning the collection. |                                |
    // | Start the oplog replay phase.  |                                |
    // | Apply commitIndexBuild 'x'.    |                                |
    // | --- Index build not found ---  |                                |
    // +--------------------------------+--------------------------------+
    auto replState = uassertStatusOK(_getIndexBuild(buildUUID));

    // Retry until we are able to put the index build in the kPrepareCommit state. None of the
    // conditions for retrying are common or expected to be long-lived, so we believe this to be
    // safe to poll at this frequency.
    while (!_tryCommit(opCtx, replState)) {
        opCtx->sleepFor(Milliseconds(100));
    }

    auto fut = replState->sharedPromise.getFuture();
    auto waitStatus = fut.waitNoThrow();              // Result from waiting on future.
    auto buildStatus = fut.getNoThrow().getStatus();  // Result from _runIndexBuildInner().
    LOGV2(20654,
          "Index build: joined after commit",
          "buildUUID"_attr = buildUUID,
          "waitResult"_attr = waitStatus,
          "status"_attr = buildStatus);

    // Throws if there was an error building the index.
    fut.get();
}

bool IndexBuildsCoordinator::_tryCommit(OperationContext* opCtx,
                                        std::shared_ptr<ReplIndexBuildState> replState) {
    stdx::unique_lock<Latch> lk(replState->mutex);
    if (replState->indexBuildState.isSettingUp()) {
        // It's possible that the index build thread has not reached the point where it can be
        // committed yet.
        return false;
    }
    if (replState->waitForNextAction->getFuture().isReady()) {
        // If the future wait were uninterruptible, then shutdown could hang.  If the
        // IndexBuildsCoordinator thread gets interrupted on shutdown, the oplog applier will hang
        // waiting for the promise applying the commitIndexBuild oplog entry.
        const auto nextAction = replState->waitForNextAction->getFuture().get(opCtx);
        invariant(nextAction == IndexBuildAction::kCommitQuorumSatisfied);
        // Retry until the current promise result is consumed by the index builder thread and
        // a new empty promise got created by the indexBuildscoordinator thread.
        return false;
    }
    auto skipCheck = shouldSkipIndexBuildStateTransitionCheck(opCtx, replState->protocol);
    replState->indexBuildState.setState(
        IndexBuildState::kPrepareCommit, skipCheck, opCtx->recoveryUnit()->getCommitTimestamp());
    // Promise can be set only once.
    // We can't skip signaling here if a signal is already set because the previous commit or
    // abort signal might have been sent to handle for primary case.
    setSignalAndCancelVoteRequestCbkIfActive(lk, opCtx, replState, IndexBuildAction::kOplogCommit);
    return true;
}

void IndexBuildsCoordinator::applyAbortIndexBuild(OperationContext* opCtx,
                                                  const IndexBuildOplogEntry& oplogEntry) {
    const auto collUUID = oplogEntry.collUUID;
    const auto nss = getNsFromUUID(opCtx, collUUID);
    const auto& buildUUID = oplogEntry.buildUUID;

    updateCurOpForCommitOrAbort(opCtx, kCommitIndexBuildFieldName, buildUUID);

    invariant(oplogEntry.cause);
    uassert(31420,
            str::stream()
                << "No commit timestamp set while applying abortIndexBuild operation. Build UUID: "
                << buildUUID,
            !opCtx->recoveryUnit()->getCommitTimestamp().isNull());

    std::string abortReason(str::stream()
                            << "abortIndexBuild oplog entry encountered: " << *oplogEntry.cause);
    if (!abortIndexBuildByBuildUUID(opCtx, buildUUID, IndexBuildAction::kOplogAbort, abortReason)) {
        // The index build may already be in the midst of tearing down.
        LOGV2(5010504,
              "Index build: failed to abort index build while applying abortIndexBuild operation",
              "buildUUID"_attr = buildUUID,
              "namespace"_attr = nss,
              "collectionUUID"_attr = collUUID,
              "cause"_attr = *oplogEntry.cause);
    }
}

boost::optional<UUID> IndexBuildsCoordinator::abortIndexBuildByIndexNames(
    OperationContext* opCtx,
    const UUID& collectionUUID,
    const std::vector<std::string>& indexNames,
    std::string reason) {
    boost::optional<UUID> buildUUID;
    auto indexBuilds = _getIndexBuilds();
    auto onIndexBuild = [&](const std::shared_ptr<ReplIndexBuildState>& replState) {
        if (replState->collectionUUID != collectionUUID) {
            return;
        }

        bool matchedBuilder = std::is_permutation(indexNames.begin(),
                                                  indexNames.end(),
                                                  replState->indexNames.begin(),
                                                  replState->indexNames.end());
        if (!matchedBuilder) {
            return;
        }

        LOGV2(23880,
              "About to abort index builder: {buildUUID} on collection: "
              "{collectionUUID}. First index: {firstIndex}",
              "About to abort index builder",
              "buildUUID"_attr = replState->buildUUID,
              "collectionUUID"_attr = collectionUUID,
              "firstIndex"_attr = replState->indexNames.front());

        if (abortIndexBuildByBuildUUID(
                opCtx, replState->buildUUID, IndexBuildAction::kPrimaryAbort, reason)) {
            buildUUID = replState->buildUUID;
        }
    };
    forEachIndexBuild(
        indexBuilds, "IndexBuildsCoordinator::abortIndexBuildByIndexNames"_sd, onIndexBuild);
    return buildUUID;
}

bool IndexBuildsCoordinator::hasIndexBuilder(OperationContext* opCtx,
                                             const UUID& collectionUUID,
                                             const std::vector<std::string>& indexNames) const {
    bool foundIndexBuilder = false;
    boost::optional<UUID> buildUUID;
    auto indexBuilds = _getIndexBuilds();
    auto onIndexBuild = [&](const std::shared_ptr<ReplIndexBuildState>& replState) {
        if (replState->collectionUUID != collectionUUID) {
            return;
        }

        bool matchedBuilder = std::is_permutation(indexNames.begin(),
                                                  indexNames.end(),
                                                  replState->indexNames.begin(),
                                                  replState->indexNames.end());
        if (!matchedBuilder) {
            return;
        }

        foundIndexBuilder = true;
    };
    forEachIndexBuild(indexBuilds, "IndexBuildsCoordinator::hasIndexBuilder"_sd, onIndexBuild);
    return foundIndexBuilder;
}

IndexBuildsCoordinator::TryAbortResult IndexBuildsCoordinator::_tryAbort(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    IndexBuildAction signalAction,
    std::string reason) {

    {
        stdx::unique_lock<Latch> lk(replState->mutex);
        // Wait until the build is done setting up. This indicates that all required state is
        // initialized to attempt an abort.
        if (replState->indexBuildState.isSettingUp()) {
            LOGV2_DEBUG(465605,
                        2,
                        "waiting until index build is done setting up before attempting to abort",
                        "buildUUID"_attr = replState->buildUUID);
            return TryAbortResult::kRetry;
        }
        if (replState->waitForNextAction->getFuture().isReady()) {
            const auto nextAction = replState->waitForNextAction->getFuture().get(opCtx);
            invariant(nextAction == IndexBuildAction::kSinglePhaseCommit ||
                      nextAction == IndexBuildAction::kCommitQuorumSatisfied ||
                      nextAction == IndexBuildAction::kPrimaryAbort);

            // Index build coordinator already received a signal to commit or abort. So, it's ok
            // to return and wait for the index build to complete if we are trying to signal
            // 'kPrimaryAbort'. The index build coordinator will not perform the signaled action
            // (i.e, will not commit or abort the index build) only when the node steps down.
            // When the node steps down, the caller of this function, dropIndexes/createIndexes
            // command (user operation) will also get interrupted. So, we no longer need to
            // abort the index build on step down.
            if (signalAction == IndexBuildAction::kPrimaryAbort) {
                // Indicate if the index build is already being committed or aborted.
                if (nextAction == IndexBuildAction::kPrimaryAbort) {
                    return TryAbortResult::kAlreadyAborted;
                } else {
                    return TryAbortResult::kNotAborted;
                }
            }

            // Retry until the current promise result is consumed by the index builder thread
            // and a new empty promise got created by the indexBuildscoordinator thread. Or,
            // until the index build got torn down after index build commit.
            return TryAbortResult::kRetry;
        }

        LOGV2(4656003,
              "Aborting index build",
              "buildUUID"_attr = replState->buildUUID,
              "error"_attr = reason);

        // Set the state on replState. Once set, the calling thread must complete the abort process.
        auto abortTimestamp =
            boost::make_optional<Timestamp>(!opCtx->recoveryUnit()->getCommitTimestamp().isNull(),
                                            opCtx->recoveryUnit()->getCommitTimestamp());
        auto skipCheck = shouldSkipIndexBuildStateTransitionCheck(opCtx, replState->protocol);
        replState->indexBuildState.setState(
            IndexBuildState::kAborted, skipCheck, abortTimestamp, reason);

        // Interrupt the builder thread so that it can no longer acquire locks or make progress.
        // It is possible that the index build thread may have completed its operation and removed
        // itself from the ServiceContext. This may happen in the case of an explicit db.killOp()
        // operation or during shutdown.
        // During normal operation, the abort logic, initiated through external means such as
        // dropIndexes or internally through an indexing error, should have set the state in
        // ReplIndexBuildState so that this code would not be reachable as it is no longer necessary
        // to interrupt the builder thread here.
        auto serviceContext = opCtx->getServiceContext();
        if (auto target = serviceContext->getLockedClient(replState->opId)) {
            auto targetOpCtx = target->getOperationContext();
            serviceContext->killOperation(target, targetOpCtx, ErrorCodes::IndexBuildAborted);
        }

        // Set the signal. Because we have already interrupted the index build, it will not observe
        // this signal. We do this so that other observers do not also try to abort the index build.
        setSignalAndCancelVoteRequestCbkIfActive(lk, opCtx, replState, signalAction);
    }
    return TryAbortResult::kContinueAbort;
}

bool IndexBuildsCoordinator::abortIndexBuildByBuildUUID(OperationContext* opCtx,
                                                        const UUID& buildUUID,
                                                        IndexBuildAction signalAction,
                                                        std::string reason) {
    std::shared_ptr<ReplIndexBuildState> replState;
    bool retry = false;
    while (true) {
        // Retry until we are able to put the index build into the kAborted state. None of the
        // conditions for retrying are common or expected to be long-lived, so we believe this to be
        // safe to poll at this frequency.
        if (retry) {
            opCtx->sleepFor(Milliseconds(1000));
            retry = false;
        }

        // It is possible to receive an abort for a non-existent index build. Abort should always
        // succeed, so suppress the error.
        auto replStateResult = _getIndexBuild(buildUUID);
        if (!replStateResult.isOK()) {
            LOGV2(20656,
                  "ignoring error while aborting index build {buildUUID}: {error}",
                  "Ignoring error while aborting index build",
                  "buildUUID"_attr = buildUUID,
                  "error"_attr = replStateResult.getStatus());
            return false;
        }

        replState = replStateResult.getValue();
        LOGV2(4656010, "Attempting to abort index build", "buildUUID"_attr = replState->buildUUID);

        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
        Lock::DBLock dbLock(opCtx, replState->dbName, MODE_IX);

        if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
            // Unlock RSTL to avoid deadlocks with prepare conflicts and state transitions caused by
            // taking a strong collection lock. See SERVER-42621.
            unlockRSTL(opCtx);
        }
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_X);

        // If we are using two-phase index builds and are no longer primary after receiving an
        // abort, we cannot replicate an abortIndexBuild oplog entry. Continue holding the RSTL to
        // check the replication state and to prevent any state transitions from happening while
        // aborting the index build. Once an index build is put into kAborted, the index builder
        // thread will be torn down, and an oplog entry must be replicated. Single-phase builds do
        // not have this restriction and may be aborted after a stepDown. Initial syncing nodes need
        // to be able to abort two phase index builds during the oplog replay phase.
        if (IndexBuildProtocol::kTwoPhase == replState->protocol) {
            // The DBLock helper takes the RSTL implicitly.
            invariant(opCtx->lockState()->isRSTLLocked());

            // Override the 'signalAction' as this is an initial syncing node.
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            if (replCoord->getMemberState().startup2()) {
                LOGV2_DEBUG(4665902,
                            1,
                            "Overriding abort 'signalAction' for initial sync",
                            "from"_attr = signalAction,
                            "to"_attr = IndexBuildAction::kInitialSyncAbort);
                signalAction = IndexBuildAction::kInitialSyncAbort;
            }

            if (IndexBuildAction::kPrimaryAbort == signalAction &&
                !replCoord->canAcceptWritesFor(opCtx, dbAndUUID)) {
                uassertStatusOK({ErrorCodes::NotWritablePrimary,
                                 str::stream()
                                     << "Unable to abort index build because we are not primary: "
                                     << buildUUID});
            }
        }

        auto tryAbortResult = _tryAbort(opCtx, replState, signalAction, reason);
        switch (tryAbortResult) {
            case TryAbortResult::kNotAborted:
                return false;
            case TryAbortResult::kAlreadyAborted:
                return true;
            case TryAbortResult::kRetry:
            case TryAbortResult::kContinueAbort:
                break;
        }

        if (TryAbortResult::kRetry == tryAbortResult) {
            retry = true;
            continue;
        }

        invariant(TryAbortResult::kContinueAbort == tryAbortResult);

        if (MONGO_unlikely(hangBeforeCompletingAbort.shouldFail())) {
            LOGV2(4806200, "Hanging before completing index build abort");
            hangBeforeCompletingAbort.pauseWhileSet();
        }

        // At this point we must continue aborting the index build.
        try {
            // We are holding the RSTL and an exclusive collection lock, so we will block stepdown
            // and be targeted for being killed. In addition to writing to the catalog, we need to
            // acquire an IX lock to write to the config.system.indexBuilds collection. Since
            // we must perform these final writes, but we expect them not to block, we can safely,
            // temporarily disable interrupts.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());
            _completeAbort(opCtx, replState, signalAction, {ErrorCodes::IndexBuildAborted, reason});
        } catch (const DBException& e) {
            LOGV2_FATAL(
                4656011,
                "Failed to abort index build after partially tearing-down index build state",
                "buildUUID"_attr = replState->buildUUID,
                "error"_attr = e);
        }

        // Wait for the builder thread to receive the signal before unregistering. Don't release the
        // Collection lock until this happens, guaranteeing the thread has stopped making progress
        // and has exited.
        auto fut = replState->sharedPromise.getFuture();
        auto waitStatus = fut.waitNoThrow();              // Result from waiting on future.
        auto buildStatus = fut.getNoThrow().getStatus();  // Result from _runIndexBuildInner().
        LOGV2(20655,
              "Index build: joined after abort",
              "buildUUID"_attr = buildUUID,
              "waitResult"_attr = waitStatus,
              "status"_attr = buildStatus);

        if (IndexBuildAction::kRollbackAbort == signalAction) {
            // Index builds interrupted for rollback may be resumed during recovery. We wait for the
            // builder thread to complete before persisting the in-memory state that will be used
            // to resume the index build.
            // No locks are required when aborting due to rollback. This performs no storage engine
            // writes, only cleans up the remaining in-memory state.
            CollectionWriter coll(opCtx, replState->collectionUUID);
            auto isResumable = !replState->lastOpTimeBeforeInterceptors.isNull();
            _indexBuildsManager.abortIndexBuildWithoutCleanup(
                opCtx, coll.get(), replState->buildUUID, isResumable);
        }

        {
            // Unregister last once we guarantee all other state has been cleaned up.
            stdx::unique_lock<Latch> lk(_mutex);
            _unregisterIndexBuild(lk, replState);
        }
        break;
    }

    return true;
}

void IndexBuildsCoordinator::_completeAbort(OperationContext* opCtx,
                                            std::shared_ptr<ReplIndexBuildState> replState,
                                            IndexBuildAction signalAction,
                                            Status reason) {
    CollectionWriter coll(opCtx, replState->collectionUUID);
    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
    auto nss = coll->ns();
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    switch (signalAction) {
        // Replicates an abortIndexBuild oplog entry and deletes the index from the durable catalog.
        case IndexBuildAction::kPrimaryAbort: {
            // Single-phase builds are aborted on step-down, so it's possible to no longer be
            // primary after we process an abort. We must continue with the abort, but since
            // single-phase builds do not replicate abort oplog entries, this write will use a ghost
            // timestamp.
            bool isPrimaryOrSinglePhase = replState->protocol == IndexBuildProtocol::kSinglePhase ||
                replCoord->canAcceptWritesFor(opCtx, nss);
            invariant(isPrimaryOrSinglePhase,
                      str::stream() << "singlePhase: "
                                    << (IndexBuildProtocol::kSinglePhase == replState->protocol));
            auto onCleanUpFn = [&] { onAbortIndexBuild(opCtx, coll->ns(), *replState, reason); };
            _indexBuildsManager.abortIndexBuild(opCtx, coll, replState->buildUUID, onCleanUpFn);
            removeIndexBuildEntryAfterCommitOrAbort(opCtx, dbAndUUID, *replState);
            break;
        }
        // Deletes the index from the durable catalog.
        case IndexBuildAction::kInitialSyncAbort: {
            invariant(replState->protocol == IndexBuildProtocol::kTwoPhase);
            invariant(replCoord->getMemberState().startup2());

            bool isMaster = replCoord->canAcceptWritesFor(opCtx, nss);
            invariant(!isMaster, str::stream() << "Index build: " << replState->buildUUID);
            invariant(replState->indexBuildState.isAborted(),
                      str::stream()
                          << "Index build: " << replState->buildUUID
                          << ",  index build state: " << replState->indexBuildState.toString());
            invariant(replState->indexBuildState.getAbortReason(), replState->buildUUID.toString());
            LOGV2(4665903,
                  "Aborting index build during initial sync",
                  "buildUUID"_attr = replState->buildUUID,
                  "abortReason"_attr = replState->indexBuildState.getAbortReason().get(),
                  "collectionUUID"_attr = replState->collectionUUID);

            _indexBuildsManager.abortIndexBuild(
                opCtx, coll, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
            break;
        }
        // Deletes the index from the durable catalog.
        case IndexBuildAction::kOplogAbort: {
            invariant(IndexBuildProtocol::kTwoPhase == replState->protocol);
            // This signal can be received during primary (drain phase), secondary,
            // startup (startup recovery) and startup2 (initial sync).
            bool isMaster = replCoord->canAcceptWritesFor(opCtx, nss);
            invariant(!isMaster, str::stream() << "Index build: " << replState->buildUUID);
            invariant(replState->indexBuildState.isAborted(),
                      str::stream()
                          << "Index build: " << replState->buildUUID
                          << ",  index build state: " << replState->indexBuildState.toString());
            invariant(replState->indexBuildState.getTimestamp() &&
                          replState->indexBuildState.getAbortReason(),
                      replState->buildUUID.toString());
            LOGV2(3856206,
                  "Aborting index build from oplog entry",
                  "buildUUID"_attr = replState->buildUUID,
                  "abortTimestamp"_attr = replState->indexBuildState.getTimestamp().get(),
                  "abortReason"_attr = replState->indexBuildState.getAbortReason().get(),
                  "collectionUUID"_attr = replState->collectionUUID);

            _indexBuildsManager.abortIndexBuild(
                opCtx, coll, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
            break;
        }
        case IndexBuildAction::kRollbackAbort: {
            invariant(replState->protocol == IndexBuildProtocol::kTwoPhase);
            invariant(replCoord->getMemberState().rollback());
            // Defer cleanup until builder thread is joined.
            break;
        }
        case IndexBuildAction::kNoAction:
        case IndexBuildAction::kCommitQuorumSatisfied:
        case IndexBuildAction::kOplogCommit:
        case IndexBuildAction::kSinglePhaseCommit:
            MONGO_UNREACHABLE;
    }

    LOGV2(465611, "Cleaned up index build after abort. ", "buildUUID"_attr = replState->buildUUID);
}

void IndexBuildsCoordinator::_completeSelfAbort(OperationContext* opCtx,
                                                std::shared_ptr<ReplIndexBuildState> replState,
                                                Status reason) {
    _completeAbort(opCtx, replState, IndexBuildAction::kPrimaryAbort, reason);
    {
        auto skipCheck = shouldSkipIndexBuildStateTransitionCheck(opCtx, replState->protocol);
        stdx::unique_lock<Latch> lk(replState->mutex);
        replState->indexBuildState.setState(IndexBuildState::kAborted, skipCheck);
    }
    {
        stdx::unique_lock<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replState);
    }
}

void IndexBuildsCoordinator::_completeAbortForShutdown(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const CollectionPtr& collection) {
    // Leave it as-if kill -9 happened. Startup recovery will restart the index build.
    auto isResumable = !replState->lastOpTimeBeforeInterceptors.isNull();
    _indexBuildsManager.abortIndexBuildWithoutCleanup(
        opCtx, collection, replState->buildUUID, isResumable);

    {
        // Promise should be set at least once before it's getting destroyed.
        stdx::unique_lock<Latch> lk(replState->mutex);
        if (!replState->waitForNextAction->getFuture().isReady()) {
            replState->waitForNextAction->emplaceValue(IndexBuildAction::kNoAction);
        }
        auto skipCheck = shouldSkipIndexBuildStateTransitionCheck(opCtx, replState->protocol);
        replState->indexBuildState.setState(IndexBuildState::kAborted, skipCheck);
    }
    {
        // This allows the builder thread to exit.
        stdx::unique_lock<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replState);
    }
}

std::size_t IndexBuildsCoordinator::getActiveIndexBuildCount(OperationContext* opCtx) {
    auto indexBuilds = _getIndexBuilds();
    // We use forEachIndexBuild() to log basic details on the current index builds and don't intend
    // to modify any of the index builds, hence the no-op.
    forEachIndexBuild(indexBuilds, "IndexBuildsCoordinator::getActiveIndexBuildCount"_sd, nullptr);

    return indexBuilds.size();
}

void IndexBuildsCoordinator::onStepUp(OperationContext* opCtx) {
    if (MONGO_unlikely(hangIndexBuildOnStepUp.shouldFail())) {
        LOGV2(4753600, "Hanging due to hangIndexBuildOnStepUp fail point");
        hangIndexBuildOnStepUp.pauseWhileSet();
    }

    LOGV2(20657, "IndexBuildsCoordinator::onStepUp - this node is stepping up to primary");

    // This would create an empty table even for FCV 4.2 to handle case where a primary node started
    // with FCV 4.2, and then upgraded FCV 4.4.
    indexbuildentryhelpers::ensureIndexBuildEntriesNamespaceExists(opCtx);

    auto indexBuilds = _getIndexBuilds();
    auto onIndexBuild = [this, opCtx](const std::shared_ptr<ReplIndexBuildState>& replState) {
        if (IndexBuildProtocol::kTwoPhase != replState->protocol) {
            return;
        }

        if (!_signalIfCommitQuorumNotEnabled(opCtx, replState)) {
            // This reads from system.indexBuilds collection to see if commit quorum got satisfied.
            try {
                _signalIfCommitQuorumIsSatisfied(opCtx, replState);
            } catch (DBException& ex) {
                fassert(31440, ex.toStatus());
            }
        }
    };
    forEachIndexBuild(indexBuilds, "IndexBuildsCoordinator::onStepUp"_sd, onIndexBuild);
}

IndexBuilds IndexBuildsCoordinator::stopIndexBuildsForRollback(OperationContext* opCtx) {
    LOGV2(20658, "Stopping index builds before rollback");

    IndexBuilds buildsStopped;

    auto indexBuilds = _getIndexBuilds();
    auto onIndexBuild = [&](const std::shared_ptr<ReplIndexBuildState>& replState) {
        if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
            LOGV2(20659,
                  "Not stopping single phase index build",
                  "buildUUID"_attr = replState->buildUUID);
            return;
        }

        // This will unblock the index build and allow it to complete without cleaning up.
        // Subsequently, the rollback algorithm can decide how to undo the index build depending on
        // the state of the oplog. Signals the kRollbackAbort and then waits for the thread to join.
        const std::string reason = "rollback";
        if (!abortIndexBuildByBuildUUID(
                opCtx, replState->buildUUID, IndexBuildAction::kRollbackAbort, reason)) {
            // The index build may already be in the midst of tearing down.
            // Leave this index build out of 'buildsStopped'.
            LOGV2(5010505,
                  "Index build: failed to abort index build before rollback",
                  "buildUUID"_attr = replState->buildUUID,
                  "database"_attr = replState->dbName,
                  "collectionUUID"_attr = replState->collectionUUID);
            return;
        }

        IndexBuildDetails aborted{replState->collectionUUID};
        // Record the index builds aborted due to rollback. This allows any rollback algorithm
        // to efficiently restart all unfinished index builds without having to scan all indexes
        // in all collections.
        for (auto spec : replState->indexSpecs) {
            aborted.indexSpecs.emplace_back(spec.getOwned());
        }
        buildsStopped.insert({replState->buildUUID, aborted});
    };
    forEachIndexBuild(
        indexBuilds, "IndexBuildsCoordinator::stopIndexBuildsForRollback"_sd, onIndexBuild);

    return buildsStopped;
}

void IndexBuildsCoordinator::restartIndexBuildsForRecovery(
    OperationContext* opCtx,
    const IndexBuilds& buildsToRestart,
    const std::vector<ResumeIndexInfo>& buildsToResume) {

    stdx::unordered_set<UUID, UUID::Hash> successfullyResumed;

    for (const auto& resumeInfo : buildsToResume) {
        auto buildUUID = resumeInfo.getBuildUUID();
        auto collUUID = resumeInfo.getCollectionUUID();

        boost::optional<NamespaceString> nss =
            CollectionCatalog::get(opCtx).lookupNSSByUUID(opCtx, resumeInfo.getCollectionUUID());
        invariant(nss);

        std::vector<BSONObj> indexSpecs;
        indexSpecs.reserve(resumeInfo.getIndexes().size());

        for (const auto& index : resumeInfo.getIndexes()) {
            indexSpecs.push_back(index.getSpec());
        }

        LOGV2(4841700,
              "Index build: resuming",
              logAttrs(nss.get()),
              "collectionUUID"_attr = collUUID,
              "buildUUID"_attr = buildUUID,
              "specs"_attr = indexSpecs,
              "phase"_attr = IndexBuildPhase_serializer(resumeInfo.getPhase()),
              "details"_attr = resumeInfo.toBSON());

        try {
            // This spawns a new thread and returns immediately. These index builds will resume and
            // wait for a commit or abort to be replicated.
            MONGO_COMPILER_VARIABLE_UNUSED auto fut = uassertStatusOK(resumeIndexBuild(
                opCtx, nss->db().toString(), collUUID, indexSpecs, buildUUID, resumeInfo));
            successfullyResumed.insert(buildUUID);
        } catch (const DBException& e) {
            LOGV2(4841701,
                  "Failed to resume index build, restarting instead",
                  "buildUUID"_attr = buildUUID,
                  "error"_attr = e);

            // Clean up the persisted Sorter data since resuming failed.
            for (const auto& index : resumeInfo.getIndexes()) {
                if (!index.getFileName()) {
                    continue;
                }

                LOGV2(5043100,
                      "Removing resumable index build temp file",
                      "file"_attr = index.getFileName(),
                      "buildUUID"_attr = buildUUID);

                boost::system::error_code ec;
                boost::filesystem::remove(
                    storageGlobalParams.dbpath + "/_tmp/" + index.getFileName()->toString(), ec);

                if (ec) {
                    LOGV2(5043101,
                          "Failed to remove resumable index build temp file",
                          "file"_attr = index.getFileName(),
                          "buildUUID"_attr = buildUUID,
                          "error"_attr = ec.message());
                }
            }
        }
    }

    for (auto& [buildUUID, build] : buildsToRestart) {
        // Don't restart an index build that was already resumed.
        if (successfullyResumed.contains(buildUUID)) {
            continue;
        }

        boost::optional<NamespaceString> nss =
            CollectionCatalog::get(opCtx).lookupNSSByUUID(opCtx, build.collUUID);
        invariant(nss);

        LOGV2(20660,
              "Index build: restarting",
              logAttrs(nss.get()),
              "collectionUUID"_attr = build.collUUID,
              "buildUUID"_attr = buildUUID);
        IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions;
        // Indicate that the initialization should not generate oplog entries or timestamps for the
        // first catalog write, and that the original durable catalog entries should be dropped and
        // replaced.
        indexBuildOptions.applicationMode = ApplicationMode::kStartupRepair;
        // This spawns a new thread and returns immediately. These index builds will start and wait
        // for a commit or abort to be replicated.
        MONGO_COMPILER_VARIABLE_UNUSED auto fut =
            uassertStatusOK(startIndexBuild(opCtx,
                                            nss->db().toString(),
                                            build.collUUID,
                                            build.indexSpecs,
                                            buildUUID,
                                            IndexBuildProtocol::kTwoPhase,
                                            indexBuildOptions));
    }
}

int IndexBuildsCoordinator::numInProgForDb(StringData db) const {
    stdx::unique_lock<Latch> lk(_mutex);
    auto indexBuildFilter = [db](const auto& replState) { return db == replState.dbName; };
    auto dbIndexBuilds = _filterIndexBuilds_inlock(lk, indexBuildFilter);
    return int(dbIndexBuilds.size());
}

bool IndexBuildsCoordinator::inProgForCollection(const UUID& collectionUUID,
                                                 IndexBuildProtocol protocol) const {
    stdx::unique_lock<Latch> lk(_mutex);
    auto indexBuildFilter = [=](const auto& replState) {
        return collectionUUID == replState.collectionUUID && protocol == replState.protocol;
    };
    auto indexBuilds = _filterIndexBuilds_inlock(lk, indexBuildFilter);
    return !indexBuilds.empty();
}

bool IndexBuildsCoordinator::inProgForCollection(const UUID& collectionUUID) const {
    stdx::unique_lock<Latch> lk(_mutex);
    auto indexBuilds = _filterIndexBuilds_inlock(
        lk, [=](const auto& replState) { return collectionUUID == replState.collectionUUID; });
    return !indexBuilds.empty();
}

bool IndexBuildsCoordinator::inProgForDb(StringData db) const {
    return numInProgForDb(db) > 0;
}

void IndexBuildsCoordinator::assertNoIndexBuildInProgress() const {
    stdx::unique_lock<Latch> lk(_mutex);
    uassert(ErrorCodes::BackgroundOperationInProgressForDatabase,
            str::stream() << "cannot perform operation: there are currently "
                          << _allIndexBuilds.size() << " index builds running.",
            _allIndexBuilds.size() == 0);
}

void IndexBuildsCoordinator::assertNoIndexBuildInProgForCollection(
    const UUID& collectionUUID) const {
    uassert(ErrorCodes::BackgroundOperationInProgressForNamespace,
            str::stream() << "cannot perform operation: an index build is currently running for "
                             "collection with UUID: "
                          << collectionUUID,
            !inProgForCollection(collectionUUID));
}

void IndexBuildsCoordinator::assertNoBgOpInProgForDb(StringData db) const {
    uassert(ErrorCodes::BackgroundOperationInProgressForDatabase,
            str::stream() << "cannot perform operation: an index build is currently running for "
                             "database "
                          << db,
            !inProgForDb(db));
}

void IndexBuildsCoordinator::awaitIndexBuildFinished(OperationContext* opCtx,
                                                     const UUID& buildUUID) {
    stdx::unique_lock<Latch> lk(_mutex);
    auto pred = [&, this]() { return _allIndexBuilds.end() == _allIndexBuilds.find(buildUUID); };
    _indexBuildsCondVar.wait(lk, pred);
}

void IndexBuildsCoordinator::awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                                      const UUID& collectionUUID,
                                                                      IndexBuildProtocol protocol) {
    stdx::unique_lock<Latch> lk(_mutex);
    auto noIndexBuildsPred = [&, this]() {
        auto indexBuilds = _filterIndexBuilds_inlock(lk, [&](const auto& replState) {
            return collectionUUID == replState.collectionUUID && protocol == replState.protocol;
        });
        return indexBuilds.empty();
    };
    opCtx->waitForConditionOrInterrupt(_indexBuildsCondVar, lk, noIndexBuildsPred);
}

void IndexBuildsCoordinator::awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                                      const UUID& collectionUUID) {
    stdx::unique_lock<Latch> lk(_mutex);
    auto pred = [&, this]() {
        auto indexBuilds = _filterIndexBuilds_inlock(
            lk, [&](const auto& replState) { return collectionUUID == replState.collectionUUID; });
        return indexBuilds.empty();
    };
    _indexBuildsCondVar.wait(lk, pred);
}

void IndexBuildsCoordinator::awaitNoBgOpInProgForDb(OperationContext* opCtx, StringData db) {
    stdx::unique_lock<Latch> lk(_mutex);
    _awaitNoBgOpInProgForDb(lk, opCtx, db);
}

void IndexBuildsCoordinator::waitUntilAnIndexBuildFinishes(OperationContext* opCtx) {
    stdx::unique_lock<Latch> lk(_mutex);
    if (_allIndexBuilds.empty()) {
        return;
    }
    const auto generation = _indexBuildsCompletedGen;
    opCtx->waitForConditionOrInterrupt(
        _indexBuildsCondVar, lk, [&] { return _indexBuildsCompletedGen != generation; });
}

void IndexBuildsCoordinator::createIndex(OperationContext* opCtx,
                                         UUID collectionUUID,
                                         const BSONObj& spec,
                                         IndexBuildsManager::IndexConstraints indexConstraints,
                                         bool fromMigrate) {
    CollectionWriter collection(opCtx, collectionUUID);

    invariant(collection,
              str::stream() << "IndexBuildsCoordinator::createIndexes: " << collectionUUID);
    auto nss = collection->ns();
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X),
              str::stream() << "IndexBuildsCoordinator::createIndexes: " << collectionUUID);

    auto buildUUID = UUID::gen();

    // Rest of this function can throw, so ensure the build cleanup occurs.
    ON_BLOCK_EXIT([&] { _indexBuildsManager.unregisterIndexBuild(buildUUID); });

    try {
        auto onInitFn = MultiIndexBlock::makeTimestampedIndexOnInitFn(opCtx, collection.get());
        IndexBuildsManager::SetupOptions options;
        options.indexConstraints = indexConstraints;
        uassertStatusOK(_indexBuildsManager.setUpIndexBuild(
            opCtx, collection, {spec}, buildUUID, onInitFn, options));
    } catch (DBException& ex) {
        const auto& status = ex.toStatus();
        if (status == ErrorCodes::IndexAlreadyExists ||
            ((status == ErrorCodes::IndexOptionsConflict ||
              status == ErrorCodes::IndexKeySpecsConflict) &&
             IndexBuildsManager::IndexConstraints::kRelax == indexConstraints)) {
            LOGV2_DEBUG(4718200,
                        1,
                        "Ignoring indexing error",
                        "error"_attr = redact(status),
                        "namespace"_attr = nss,
                        "collectionUUID"_attr = collectionUUID,
                        "spec"_attr = spec);
            return;
        }
        throw;
    }

    auto abortOnExit = makeGuard([&] {
        _indexBuildsManager.abortIndexBuild(
            opCtx, collection, buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
    });
    uassertStatusOK(_indexBuildsManager.startBuildingIndex(opCtx, collection.get(), buildUUID));

    // Retry indexing records that failed key generation, but only if we are primary. Secondaries
    // rely on the primary's decision to commit as assurance that it has checked all key generation
    // errors on its behalf.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->canAcceptWritesFor(opCtx, nss)) {
        uassertStatusOK(
            _indexBuildsManager.retrySkippedRecords(opCtx, buildUUID, collection.get()));
    }
    uassertStatusOK(
        _indexBuildsManager.checkIndexConstraintViolations(opCtx, collection.get(), buildUUID));

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    auto onCreateEachFn = [&](const BSONObj& spec) {
        opObserver->onCreateIndex(opCtx, collection->ns(), collectionUUID, spec, fromMigrate);
    };
    auto onCommitFn = MultiIndexBlock::kNoopOnCommitFn;
    uassertStatusOK(_indexBuildsManager.commitIndexBuild(
        opCtx, collection, nss, buildUUID, onCreateEachFn, onCommitFn));
    abortOnExit.dismiss();
}

void IndexBuildsCoordinator::createIndexesOnEmptyCollection(OperationContext* opCtx,
                                                            CollectionWriter& collection,
                                                            const std::vector<BSONObj>& specs,
                                                            bool fromMigrate) {
    auto collectionUUID = collection->uuid();

    invariant(collection, str::stream() << collectionUUID);
    invariant(collection->isEmpty(opCtx), str::stream() << collectionUUID);
    invariant(!specs.empty(), str::stream() << collectionUUID);

    auto nss = collection->ns();
    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx,
                                                                               collection->ns());

    auto opObserver = opCtx->getServiceContext()->getOpObserver();

    auto indexCatalog = collection.getWritableCollection()->getIndexCatalog();
    // Always run single phase index build for empty collection. And, will be coordinated using
    // createIndexes oplog entry.
    for (const auto& spec : specs) {
        // Each index will be added to the mdb catalog using the preceding createIndexes
        // timestamp.
        opObserver->onCreateIndex(opCtx, nss, collectionUUID, spec, fromMigrate);
        uassertStatusOK(indexCatalog->createIndexOnEmptyCollection(opCtx, spec));
    }
}

void IndexBuildsCoordinator::sleepIndexBuilds_forTestOnly(bool sleep) {
    stdx::unique_lock<Latch> lk(_mutex);
    _sleepForTest = sleep;
}

void IndexBuildsCoordinator::verifyNoIndexBuilds_forTestOnly() {
    stdx::unique_lock<Latch> lk(_mutex);
    invariant(_allIndexBuilds.empty());
}

// static
void IndexBuildsCoordinator::updateCurOpOpDescription(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const std::vector<BSONObj>& indexSpecs) {
    BSONObjBuilder builder;

    // If the collection namespace is provided, add a 'createIndexes' field with the collection name
    // to allow tests to identify this op as an index build.
    if (!nss.isEmpty()) {
        builder.append(kCreateIndexesFieldName, nss.coll());
    }

    // If index specs are provided, add them under the 'indexes' field.
    if (!indexSpecs.empty()) {
        BSONArrayBuilder indexesBuilder;
        for (const auto& spec : indexSpecs) {
            indexesBuilder.append(spec);
        }
        builder.append(kIndexesFieldName, indexesBuilder.arr());
    }

    stdx::unique_lock<Client> lk(*opCtx->getClient());
    auto curOp = CurOp::get(opCtx);
    builder.appendElementsUnique(curOp->opDescription());
    auto opDescObj = builder.obj();
    curOp->setLogicalOp_inlock(LogicalOp::opCommand);
    curOp->setOpDescription_inlock(opDescObj);
    curOp->setNS_inlock(nss.ns());
    curOp->ensureStarted();
}

Status IndexBuildsCoordinator::_registerIndexBuild(
    WithLock lk, std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {
    // Check whether any indexes are already being built with the same index name(s). (Duplicate
    // specs will be discovered by the index builder.)
    auto pred = [&](const auto& replState) {
        return replIndexBuildState->collectionUUID == replState.collectionUUID;
    };
    auto collIndexBuilds = _filterIndexBuilds_inlock(lk, pred);
    for (auto existingIndexBuild : collIndexBuilds) {
        for (const auto& name : replIndexBuildState->indexNames) {
            if (existingIndexBuild->indexNames.end() !=
                std::find(existingIndexBuild->indexNames.begin(),
                          existingIndexBuild->indexNames.end(),
                          name)) {

                str::stream ss;
                ss << "Index build conflict: " << replIndexBuildState->buildUUID
                   << ": There's already an index with name '" << name
                   << "' being built on the collection "
                   << " ( " << replIndexBuildState->collectionUUID
                   << " ) under an existing index build: " << existingIndexBuild->buildUUID;
                auto aborted = false;
                IndexBuildState existingIndexBuildState;
                {
                    // We have to lock the mutex in order to read the committed/aborted state.
                    stdx::unique_lock<Latch> lkExisting(existingIndexBuild->mutex);
                    existingIndexBuildState = existingIndexBuild->indexBuildState;
                }
                ss << " index build state: " << existingIndexBuildState.toString();
                if (auto ts = existingIndexBuildState.getTimestamp()) {
                    ss << ", timestamp: " << ts->toString();
                }
                if (existingIndexBuildState.isAborted()) {
                    if (auto abortReason = existingIndexBuildState.getAbortReason()) {
                        ss << ", abort reason: " << abortReason.get();
                    }
                    aborted = true;
                }
                std::string msg = ss;
                LOGV2(20661,
                      "Index build conflict. There's already an index with the same name being "
                      "built under an existing index build",
                      "buildUUID"_attr = replIndexBuildState->buildUUID,
                      "existingBuildUUID"_attr = existingIndexBuild->buildUUID,
                      "index"_attr = name,
                      "collectionUUID"_attr = replIndexBuildState->collectionUUID);
                if (aborted) {
                    return {ErrorCodes::IndexBuildAborted, msg};
                }
                return Status(ErrorCodes::IndexBuildAlreadyInProgress, msg);
            }
        }
    }

    invariant(_allIndexBuilds.emplace(replIndexBuildState->buildUUID, replIndexBuildState).second);

    _indexBuildsCondVar.notify_all();

    return Status::OK();
}

void IndexBuildsCoordinator::_unregisterIndexBuild(
    WithLock lk, std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {

    invariant(_allIndexBuilds.erase(replIndexBuildState->buildUUID));

    LOGV2_DEBUG(4656004,
                1,
                "Index build: Unregistering",
                "buildUUID"_attr = replIndexBuildState->buildUUID);
    _indexBuildsManager.unregisterIndexBuild(replIndexBuildState->buildUUID);
    _indexBuildsCompletedGen++;
    _indexBuildsCondVar.notify_all();
}

Status IndexBuildsCoordinator::_setUpIndexBuildForTwoPhaseRecovery(
    OperationContext* opCtx,
    StringData dbName,
    CollectionUUID collectionUUID,
    const std::vector<BSONObj>& specs,
    const UUID& buildUUID) {
    NamespaceStringOrUUID nssOrUuid{dbName.toString(), collectionUUID};

    // Don't use the AutoGet helpers because they require an open database, which may not be the
    // case when an index builds is restarted during recovery.
    Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
    Lock::CollectionLock collLock(opCtx, nssOrUuid, MODE_X);
    auto collection = CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, collectionUUID);
    invariant(collection);
    const auto& nss = collection->ns();
    const auto protocol = IndexBuildProtocol::kTwoPhase;
    return _startIndexBuildForRecovery(opCtx, nss, specs, buildUUID, protocol);
}

StatusWith<boost::optional<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>>
IndexBuildsCoordinator::_filterSpecsAndRegisterBuild(OperationContext* opCtx,
                                                     StringData dbName,
                                                     CollectionUUID collectionUUID,
                                                     const std::vector<BSONObj>& specs,
                                                     const UUID& buildUUID,
                                                     IndexBuildProtocol protocol) {

    // AutoGetCollection throws an exception if it is unable to look up the collection by UUID.
    NamespaceStringOrUUID nssOrUuid{dbName.toString(), collectionUUID};
    AutoGetCollection autoColl(opCtx, nssOrUuid, MODE_X);
    CollectionWriter collection(autoColl);

    // Disallow index builds on drop-pending namespaces (system.drop.*) if we are primary.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().usingReplSets() &&
        replCoord->canAcceptWritesFor(opCtx, nssOrUuid)) {
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "drop-pending collection: " << collection.get()->ns(),
                !collection.get()->ns().isDropPendingNamespace());
    }

    // This check is for optimization purposes only as since this lock is released after this,
    // and is acquired again when we build the index in _setUpIndexBuild.
    CollectionShardingState::get(opCtx, collection.get()->ns())->checkShardVersionOrThrow(opCtx);

    // Lock from when we ascertain what indexes to build through to when the build is registered
    // on the Coordinator and persistedly set up in the catalog. This serializes setting up an
    // index build so that no attempts are made to register the same build twice.
    stdx::unique_lock<Latch> lk(_mutex);

    std::vector<BSONObj> filteredSpecs;
    try {
        filteredSpecs =
            prepareSpecListForCreate(opCtx, collection.get(), collection.get()->ns(), specs);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    if (filteredSpecs.size() == 0) {
        // The requested index (specs) are already built or are being built. Return success
        // early (this is v4.0 behavior compatible).
        ReplIndexBuildState::IndexCatalogStats indexCatalogStats;
        int numIndexes = getNumIndexesTotal(opCtx, collection.get());
        indexCatalogStats.numIndexesBefore = numIndexes;
        indexCatalogStats.numIndexesAfter = numIndexes;
        return SharedSemiFuture(indexCatalogStats);
    }

    // Bypass the thread pool if we are building indexes on an empty collection.
    if (shouldBuildIndexesOnEmptyCollectionSinglePhased(opCtx, collection.get(), protocol)) {
        ReplIndexBuildState::IndexCatalogStats indexCatalogStats;
        indexCatalogStats.numIndexesBefore = getNumIndexesTotal(opCtx, collection.get());
        try {
            // Replicate this index build using the old-style createIndexes oplog entry to avoid
            // timestamping issues that would result from this empty collection optimization on a
            // secondary. If we tried to generate two phase index build startIndexBuild and
            // commitIndexBuild oplog entries, this optimization will fail to accurately timestamp
            // the catalog update when it uses the timestamp from the startIndexBuild, rather than
            // the commitIndexBuild, oplog entry.
            writeConflictRetry(opCtx,
                               "IndexBuildsCoordinator::_filterSpecsAndRegisterBuild",
                               collection.get()->ns().ns(),
                               [&] {
                                   WriteUnitOfWork wuow(opCtx);
                                   createIndexesOnEmptyCollection(
                                       opCtx, collection, filteredSpecs, false);
                                   wuow.commit();
                               });
        } catch (DBException& ex) {
            ex.addContext(str::stream() << "index build on empty collection failed: " << buildUUID);
            return ex.toStatus();
        }
        indexCatalogStats.numIndexesAfter = getNumIndexesTotal(opCtx, collection.get());
        return SharedSemiFuture(indexCatalogStats);
    }

    auto replIndexBuildState = std::make_shared<ReplIndexBuildState>(
        buildUUID, collectionUUID, dbName.toString(), filteredSpecs, protocol);
    replIndexBuildState->stats.numIndexesBefore = getNumIndexesTotal(opCtx, collection.get());

    auto status = _registerIndexBuild(lk, replIndexBuildState);
    if (!status.isOK()) {
        return status;
    }

    // The index has been registered on the Coordinator in an unstarted state. Return an
    // uninitialized Future so that the caller can set up the index build by calling
    // _setUpIndexBuild(). The completion of the index build will be communicated via a Future
    // obtained from 'replIndexBuildState->sharedPromise'.
    return boost::none;
}

IndexBuildsCoordinator::PostSetupAction IndexBuildsCoordinator::_setUpIndexBuildInner(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    Timestamp startTimestamp,
    const IndexBuildOptions& indexBuildOptions) {
    const NamespaceStringOrUUID nssOrUuid{replState->dbName, replState->collectionUUID};

    AutoGetCollection coll(opCtx, nssOrUuid, MODE_X);
    CollectionWriter collection(coll);
    CollectionShardingState::get(opCtx, collection->ns())->checkShardVersionOrThrow(opCtx);

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool replSetAndNotPrimary = replCoord->getSettings().usingReplSets() &&
        !replCoord->canAcceptWritesFor(opCtx, collection->ns());

    // We will not have a start timestamp if we are newly a secondary (i.e. we started as
    // primary but there was a stepdown). We will be unable to timestamp the initial catalog write,
    // so we must fail the index build. During initial sync, there is no commit timestamp set.
    if (replSetAndNotPrimary &&
        indexBuildOptions.applicationMode != ApplicationMode::kInitialSync) {
        uassert(ErrorCodes::NotWritablePrimary,
                str::stream() << "Replication state changed while setting up the index build: "
                              << replState->buildUUID,
                !startTimestamp.isNull());
    }

    MultiIndexBlock::OnInitFn onInitFn;
    if (IndexBuildProtocol::kTwoPhase == replState->protocol) {
        // Change the startIndexBuild Oplog entry.
        // Two-phase index builds write a different oplog entry than the default behavior which
        // writes a no-op just to generate an optime.
        onInitFn = [&](std::vector<BSONObj>& specs) {
            if (!(replCoord->getSettings().usingReplSets() &&
                  replCoord->canAcceptWritesFor(opCtx, collection->ns()))) {
                // Not primary.
                return Status::OK();
            }

            // Two phase index builds should have commit quorum set.
            invariant(indexBuildOptions.commitQuorum,
                      str::stream()
                          << "Commit quorum required for two phase index build, buildUUID: "
                          << replState->buildUUID
                          << " collectionUUID: " << replState->collectionUUID);

            // Persist the commit quorum value in the config.system.indexBuilds collection.
            IndexBuildEntry indexBuildEntry(replState->buildUUID,
                                            replState->collectionUUID,
                                            indexBuildOptions.commitQuorum.get(),
                                            replState->indexNames);
            uassertStatusOK(indexbuildentryhelpers::addIndexBuildEntry(opCtx, indexBuildEntry));

            opCtx->getServiceContext()->getOpObserver()->onStartIndexBuild(
                opCtx,
                collection->ns(),
                replState->collectionUUID,
                replState->buildUUID,
                replState->indexSpecs,
                false /* fromMigrate */);

            return Status::OK();
        };
    } else {
        onInitFn = MultiIndexBlock::makeTimestampedIndexOnInitFn(opCtx, collection.get());
    }

    IndexBuildsManager::SetupOptions options;
    options.indexConstraints =
        repl::ReplicationCoordinator::get(opCtx)->shouldRelaxIndexConstraints(opCtx,
                                                                              collection->ns())
        ? IndexBuildsManager::IndexConstraints::kRelax
        : IndexBuildsManager::IndexConstraints::kEnforce;
    options.protocol = replState->protocol;

    try {
        if (!replSetAndNotPrimary) {
            // On standalones and primaries, call setUpIndexBuild(), which makes the initial catalog
            // write. On primaries, this replicates the startIndexBuild oplog entry.
            uassertStatusOK(_indexBuildsManager.setUpIndexBuild(
                opCtx, collection, replState->indexSpecs, replState->buildUUID, onInitFn, options));
        } else {
            // If we are starting the index build as a secondary, we must suppress calls to write
            // our initial oplog entry in setUpIndexBuild().
            repl::UnreplicatedWritesBlock uwb(opCtx);

            boost::optional<TimestampBlock> tsBlock;
            if (indexBuildOptions.applicationMode != ApplicationMode::kInitialSync) {
                // Use the provided timestamp to write the initial catalog entry. Initial sync does
                // not set a commit timestamp.
                invariant(!startTimestamp.isNull());
                tsBlock.emplace(opCtx, startTimestamp);
            }

            uassertStatusOK(_indexBuildsManager.setUpIndexBuild(
                opCtx, collection, replState->indexSpecs, replState->buildUUID, onInitFn, options));
        }
    } catch (DBException& ex) {
        _indexBuildsManager.abortIndexBuild(
            opCtx, collection, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);

        const auto& status = ex.toStatus();
        if (status == ErrorCodes::IndexAlreadyExists ||
            ((status == ErrorCodes::IndexOptionsConflict ||
              status == ErrorCodes::IndexKeySpecsConflict) &&
             options.indexConstraints == IndexBuildsManager::IndexConstraints::kRelax)) {
            LOGV2_DEBUG(20662,
                        1,
                        "Ignoring indexing error: {error}",
                        "Ignoring indexing error",
                        "error"_attr = redact(status));
            return PostSetupAction::kCompleteIndexBuildEarly;
        }

        throw;
    }

    if (isIndexBuildResumable(opCtx, *replState, indexBuildOptions)) {
        // We should only set this value if this is a hybrid index build.
        invariant(_indexBuildsManager.isBackgroundBuilding(replState->buildUUID));

        // After the interceptors are set, get the latest optime in the oplog that could have
        // contained a write to this collection. We need to be holding the collection lock in X mode
        // so that we ensure that there are not any uncommitted transactions on this collection.
        replState->lastOpTimeBeforeInterceptors = getLatestOplogOpTime(opCtx);
    }

    return PostSetupAction::kContinueIndexBuild;
}

Status IndexBuildsCoordinator::_setUpIndexBuild(OperationContext* opCtx,
                                                const UUID& buildUUID,
                                                Timestamp startTimestamp,
                                                const IndexBuildOptions& indexBuildOptions) {
    auto replState = invariant(_getIndexBuild(buildUUID));

    auto postSetupAction = PostSetupAction::kContinueIndexBuild;
    try {
        postSetupAction =
            _setUpIndexBuildInner(opCtx, replState, startTimestamp, indexBuildOptions);
    } catch (const DBException& ex) {
        stdx::unique_lock<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replState);

        return ex.toStatus();
    }

    // The indexes are in the durable catalog in an unfinished state. Return an OK status so
    // that the caller can continue building the indexes by calling _runIndexBuild().
    if (PostSetupAction::kContinueIndexBuild == postSetupAction) {
        return Status::OK();
    }

    // Unregister the index build before setting the promise, so callers do not see the build again.
    {
        stdx::unique_lock<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replState);
    }

    // The requested index (specs) are already built or are being built. Return success
    // early (this is v4.0 behavior compatible).
    invariant(PostSetupAction::kCompleteIndexBuildEarly == postSetupAction,
              str::stream() << "failed to set up index build " << buildUUID
                            << " with start timestamp " << startTimestamp.toString());
    ReplIndexBuildState::IndexCatalogStats indexCatalogStats;
    int numIndexes = replState->stats.numIndexesBefore;
    indexCatalogStats.numIndexesBefore = numIndexes;
    indexCatalogStats.numIndexesAfter = numIndexes;
    replState->sharedPromise.emplaceValue(indexCatalogStats);
    return Status::OK();
}

void IndexBuildsCoordinator::_runIndexBuild(
    OperationContext* opCtx,
    const UUID& buildUUID,
    const IndexBuildOptions& indexBuildOptions,
    const boost::optional<ResumeIndexInfo>& resumeInfo) noexcept {
    {
        stdx::unique_lock<Latch> lk(_mutex);
        while (_sleepForTest) {
            lk.unlock();
            sleepmillis(100);
            lk.lock();
        }
    }

    // If the index build does not exist, do not continue building the index. This may happen if an
    // ignorable indexing error occurred during setup. The promise will have been fulfilled, but the
    // build has already been unregistered.
    auto swReplState = _getIndexBuild(buildUUID);
    if (swReplState.getStatus() == ErrorCodes::NoSuchKey) {
        return;
    }
    auto replState = invariant(swReplState);
    {
        // The index build is now past the setup stage and in progress. This makes it eligible to be
        // aborted. Use the current OperationContext's opId as the means for interrupting the index
        // build.
        stdx::unique_lock<Latch> lk(replState->mutex);
        replState->opId = opCtx->getOpID();
        replState->indexBuildState.setState(IndexBuildState::kInProgress, false /* skipCheck */);
    }

    // Add build UUID to lock manager diagnostic output.
    auto locker = opCtx->lockState();
    auto oldLockerDebugInfo = locker->getDebugInfo();
    {
        str::stream ss;
        ss << "index build: " << replState->buildUUID;
        if (!oldLockerDebugInfo.empty()) {
            ss << "; " << oldLockerDebugInfo;
        }
        locker->setDebugInfo(ss);
    }

    auto status = [&]() {
        try {
            _runIndexBuildInner(opCtx, replState, indexBuildOptions, resumeInfo);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
        return Status::OK();
    }();

    locker->setDebugInfo(oldLockerDebugInfo);

    // Ensure the index build is unregistered from the Coordinator and the Promise is set with
    // the build's result so that callers are notified of the outcome.
    if (status.isOK()) {
        stdx::unique_lock<Latch> lk(_mutex);
        // Unregister first so that when we fulfill the future, the build is not observed as active.
        _unregisterIndexBuild(lk, replState);
        replState->sharedPromise.emplaceValue(replState->stats);
        return;
    }

    // During a failure, unregistering is handled by either the caller or the current thread,
    // depending on where the error originated. Signal to any waiters that an error occurred.
    replState->sharedPromise.setError(status);
}

namespace {

template <typename Func>
void runOnAlternateContext(OperationContext* opCtx, std::string name, Func func) {
    auto newClient = opCtx->getServiceContext()->makeClient(name);
    AlternativeClientRegion acr(newClient);
    const auto newCtx = cc().makeOperationContext();
    func(newCtx.get());
}
}  // namespace

void IndexBuildsCoordinator::_cleanUpSinglePhaseAfterFailure(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    const Status& status) {
    if (status.isA<ErrorCategory::ShutdownError>()) {
        _completeAbortForShutdown(opCtx, replState, collection);
        return;
    }

    // The index builder thread can abort on its own if it is interrupted by a user killop. This
    // would prevent us from taking locks. Use a new OperationContext to abort the index build.
    runOnAlternateContext(
        opCtx, "self-abort", [this, replState, status](OperationContext* abortCtx) {
            ShouldNotConflictWithSecondaryBatchApplicationBlock noConflict(abortCtx->lockState());
            Lock::DBLock dbLock(abortCtx, replState->dbName, MODE_IX);

            // Unlock RSTL to avoid deadlocks with prepare conflicts and state transitions caused by
            // taking a strong collection lock. See SERVER-42621.
            unlockRSTL(abortCtx);

            const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
            Lock::CollectionLock collLock(abortCtx, dbAndUUID, MODE_X);
            _completeSelfAbort(abortCtx, replState, status);
        });
}

void IndexBuildsCoordinator::_cleanUpTwoPhaseAfterFailure(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    const Status& status) {

    if (status.isA<ErrorCategory::ShutdownError>()) {
        _completeAbortForShutdown(opCtx, replState, collection);
        return;
    }

    // The index builder thread can abort on its own if it is interrupted by a user killop. This
    // would prevent us from taking locks. Use a new OperationContext to abort the index build.
    runOnAlternateContext(
        opCtx, "self-abort", [this, replState, status](OperationContext* abortCtx) {
            ShouldNotConflictWithSecondaryBatchApplicationBlock noConflict(abortCtx->lockState());

            // Take RSTL (implicitly by DBLock) to observe and prevent replication state from
            // changing.
            Lock::DBLock dbLock(abortCtx, replState->dbName, MODE_IX);

            // Index builds may not fail on secondaries. If a primary replicated an abortIndexBuild
            // oplog entry, then this index build would have received an IndexBuildAborted error
            // code.
            const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
            auto replCoord = repl::ReplicationCoordinator::get(abortCtx);
            if (replCoord->getSettings().usingReplSets() &&
                !replCoord->canAcceptWritesFor(abortCtx, dbAndUUID)) {
                fassert(51101,
                        status.withContext(str::stream() << "Index build: " << replState->buildUUID
                                                         << "; Database: " << replState->dbName));
            }

            Lock::CollectionLock collLock(abortCtx, dbAndUUID, MODE_X);
            _completeSelfAbort(abortCtx, replState, status);
        });
}

void IndexBuildsCoordinator::_runIndexBuildInner(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    const boost::optional<ResumeIndexInfo>& resumeInfo) {
    // This Status stays unchanged unless we catch an exception in the following try-catch block.
    auto status = Status::OK();
    try {
        while (MONGO_unlikely(hangAfterInitializingIndexBuild.shouldFail())) {
            hangAfterInitializingIndexBuild.pauseWhileSet(opCtx);
        }

        if (resumeInfo) {
            _resumeIndexBuildFromPhase(opCtx, replState, indexBuildOptions, resumeInfo.get());
        } else {
            _buildIndex(opCtx, replState, indexBuildOptions);
        }

    } catch (const DBException& ex) {
        status = ex.toStatus();
    }

    if (status.isOK()) {
        return;
    }

    {
        // If the index build has already been cleaned-up because it encountered an error at
        // commit-time, there is no work to do. This is the most routine case, since index
        // constraint checking happens at commit-time for index builds.
        stdx::unique_lock<Latch> lk(replState->mutex);
        if (replState->indexBuildState.isAborted()) {
            uassertStatusOK(status);
        }
    }

    // We do not hold a collection lock here, but we are protected against the collection being
    // dropped while the index build is still registered for the collection -- until abortIndexBuild
    // is called. The collection can be renamed, but it is OK for the name to be stale just for
    // logging purposes.
    auto collectionSharedPtr = CollectionCatalog::get(opCtx).lookupCollectionByUUIDForRead(
        opCtx, replState->collectionUUID);
    CollectionPtr collection(collectionSharedPtr.get(), CollectionPtr::NoYieldTag{});
    invariant(collection,
              str::stream() << "Collection with UUID " << replState->collectionUUID
                            << " should exist because an index build is in progress: "
                            << replState->buildUUID);
    NamespaceString nss = collection->ns();
    logFailure(status, nss, replState);

    // If we received an external abort, the caller should have already set our state to kAborted.
    invariant(status.code() != ErrorCodes::IndexBuildAborted);

    if (MONGO_unlikely(hangIndexBuildBeforeAbortCleanUp.shouldFail())) {
        LOGV2(4753601, "Hanging due to hangIndexBuildBeforeAbortCleanUp fail point");
        hangIndexBuildBeforeAbortCleanUp.pauseWhileSet();
    }

    // Index builds only check index constraints when committing. If an error occurs at that point,
    // then the build is cleaned up while still holding the appropriate locks. The only errors that
    // we cannot anticipate are user interrupts and shutdown errors.
    invariant(status.isA<ErrorCategory::Interruption>() ||
                  status.isA<ErrorCategory::ShutdownError>(),
              str::stream() << "Unexpected error code during index build cleanup: " << status);
    if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
        _cleanUpSinglePhaseAfterFailure(opCtx, collection, replState, indexBuildOptions, status);
    } else {
        invariant(IndexBuildProtocol::kTwoPhase == replState->protocol,
                  str::stream() << replState->buildUUID);
        _cleanUpTwoPhaseAfterFailure(opCtx, collection, replState, indexBuildOptions, status);
    }

    // Any error that escapes at this point is not fatal and can be handled by the caller.
    uassertStatusOK(status);
}

void IndexBuildsCoordinator::_resumeIndexBuildFromPhase(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    const ResumeIndexInfo& resumeInfo) {
    if (MONGO_unlikely(hangAfterSettingUpResumableIndexBuild.shouldFail())) {
        LOGV2(4841704,
              "Hanging index build due to failpoint 'hangAfterSettingUpResumableIndexBuild'");
        hangAfterSettingUpResumableIndexBuild.pauseWhileSet();
    }

    if (resumeInfo.getPhase() == IndexBuildPhaseEnum::kInitialized ||
        resumeInfo.getPhase() == IndexBuildPhaseEnum::kCollectionScan) {
        _scanCollectionAndInsertSortedKeysIntoIndex(
            opCtx,
            replState,
            resumeInfo.getCollectionScanPosition()
                ? boost::make_optional<RecordId>(RecordId(*resumeInfo.getCollectionScanPosition()))
                : boost::none);
    } else if (resumeInfo.getPhase() == IndexBuildPhaseEnum::kBulkLoad) {
        _insertSortedKeysIntoIndexForResume(opCtx, replState);
    }

    _insertKeysFromSideTablesWithoutBlockingWrites(opCtx, replState);
    _signalPrimaryForCommitReadiness(opCtx, replState);
    _insertKeysFromSideTablesBlockingWrites(opCtx, replState, indexBuildOptions);
    _waitForNextIndexBuildActionAndCommit(opCtx, replState, indexBuildOptions);
}

void IndexBuildsCoordinator::_awaitLastOpTimeBeforeInterceptorsMajorityCommitted(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // The last optime could be null if the node is in initial sync while building the index.
    if (replState->lastOpTimeBeforeInterceptors.isNull()) {
        return;
    }

    auto timeoutMillis = gResumableIndexBuildMajorityOpTimeTimeoutMillis;
    if (timeoutMillis == 0) {
        // Disable resumable index build.
        replState->lastOpTimeBeforeInterceptors = {};
        return;
    }

    Milliseconds timeout;
    Date_t deadline;
    if (timeoutMillis > 0) {
        timeout = Milliseconds(timeoutMillis);
        deadline = opCtx->getServiceContext()->getFastClockSource()->now() + timeout;
    } else {
        // Wait indefinitely for majority commit point.
        // Setting 'deadline' to Date_t::max() achieves the same effect as boost::none in
        // ReplicationCoordinatorImpl::waitUntilMajorityOpTime(). Additionally, providing a
        // 'deadline' of Date_t::max() is given special treatment in
        // OperationContext::waitForConditionOrInterruptNoAssertUntil().
        timeout = Milliseconds::max();
        deadline = Date_t::max();
    }

    LOGV2(4847600,
          "Index build: waiting for last optime before interceptors to be majority committed",
          "buildUUID"_attr = replState->buildUUID,
          "deadline"_attr = deadline,
          "timeout"_attr = timeout,
          "lastOpTime"_attr = replState->lastOpTimeBeforeInterceptors);

    hangIndexBuildBeforeWaitingUntilMajorityOpTime.executeIf(
        [opCtx, buildUUID = replState->buildUUID](const BSONObj& data) {
            LOGV2(
                4940901,
                "Hanging index build before waiting for the last optime before interceptors to be "
                "majority committed due to hangIndexBuildBeforeWaitingUntilMajorityOpTime "
                "failpoint",
                "buildUUID"_attr = buildUUID);

            hangIndexBuildBeforeWaitingUntilMajorityOpTime.pauseWhileSet(opCtx);
        },
        [buildUUID = replState->buildUUID](const BSONObj& data) {
            auto buildUUIDs = data.getObjectField("buildUUIDs");
            return std::any_of(buildUUIDs.begin(), buildUUIDs.end(), [buildUUID](const auto& elem) {
                return UUID::parse(elem.String()) == buildUUID;
            });
        });

    auto status = replCoord->waitUntilMajorityOpTime(
        opCtx, replState->lastOpTimeBeforeInterceptors, deadline);
    if (!status.isOK()) {
        replState->lastOpTimeBeforeInterceptors = {};
        LOGV2(5053900,
              "Index build: timed out waiting for the last optime before interceptors to be "
              "majority committed. Continuing as a non-resumable index build.",
              "buildUUID"_attr = replState->buildUUID,
              "deadline"_attr = deadline,
              "timeout"_attr = timeout,
              "lastOpTime"_attr = replState->lastOpTimeBeforeInterceptors,
              "waitStatus"_attr = status);
        return;
    }

    // Since we waited for all the writes before the interceptors were established to be majority
    // committed, if we read at the majority commit point for the collection scan, then none of the
    // documents put into the sorter can be rolled back.
    opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kMajorityCommitted);
}

void IndexBuildsCoordinator::_buildIndex(OperationContext* opCtx,
                                         std::shared_ptr<ReplIndexBuildState> replState,
                                         const IndexBuildOptions& indexBuildOptions) {
    if (MONGO_unlikely(hangBeforeBuildingIndex.shouldFail())) {
        LOGV2(4940900,
              "Hanging before building index due to hangBeforeBuildingIndex failpoint",
              "buildUUID"_attr = replState->buildUUID);

        hangBeforeBuildingIndex.pauseWhileSet();
    }

    // Read without a timestamp. When we commit, we block writes which guarantees all writes are
    // visible.
    invariant(RecoveryUnit::ReadSource::kNoTimestamp ==
              opCtx->recoveryUnit()->getTimestampReadSource());
    // The collection scan might read with a kMajorityCommitted read source, but will restore
    // kNoTimestamp afterwards.
    _scanCollectionAndInsertSortedKeysIntoIndex(opCtx, replState);
    _insertKeysFromSideTablesWithoutBlockingWrites(opCtx, replState);
    _signalPrimaryForCommitReadiness(opCtx, replState);
    _insertKeysFromSideTablesBlockingWrites(opCtx, replState, indexBuildOptions);
    _waitForNextIndexBuildActionAndCommit(opCtx, replState, indexBuildOptions);
}

/*
 * First phase is doing a collection scan and inserting keys into sorter.
 * Second phase is extracting the sorted keys and writing them into the new index table.
 */
void IndexBuildsCoordinator::_scanCollectionAndInsertSortedKeysIntoIndex(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    boost::optional<RecordId> resumeAfterRecordId) {
    // Collection scan and insert into index.
    {

        auto scopeGuard = makeGuard([&] {
            opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        });

        // Wait for the last optime before the interceptors are established to be majority committed
        // while we aren't holding any locks. This will set the read source to be kMajorityCommitted
        // if it waited.
        _awaitLastOpTimeBeforeInterceptorsMajorityCommitted(opCtx, replState);

        Lock::DBLock autoDb(opCtx, replState->dbName, MODE_IX);
        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_IX);

        auto collection = _setUpForScanCollectionAndInsertSortedKeysIntoIndex(opCtx, replState);

        uassertStatusOK(_indexBuildsManager.startBuildingIndex(
            opCtx, collection, replState->buildUUID, resumeAfterRecordId));
    }

    if (MONGO_unlikely(hangAfterIndexBuildDumpsInsertsFromBulk.shouldFail())) {
        LOGV2(20665, "Hanging after dumping inserts from bulk builder");
        hangAfterIndexBuildDumpsInsertsFromBulk.pauseWhileSet();
    }
}

void IndexBuildsCoordinator::_insertSortedKeysIntoIndexForResume(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    {
        Lock::DBLock autoDb(opCtx, replState->dbName, MODE_IX);
        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_IX);

        auto collection = _setUpForScanCollectionAndInsertSortedKeysIntoIndex(opCtx, replState);
        uassertStatusOK(_indexBuildsManager.resumeBuildingIndexFromBulkLoadPhase(
            opCtx, collection, replState->buildUUID));
    }

    if (MONGO_unlikely(hangAfterIndexBuildDumpsInsertsFromBulk.shouldFail())) {
        LOGV2(4940800, "Hanging after dumping inserts from bulk builder");
        hangAfterIndexBuildDumpsInsertsFromBulk.pauseWhileSet();
    }
}

CollectionPtr IndexBuildsCoordinator::_setUpForScanCollectionAndInsertSortedKeysIntoIndex(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    // Rebuilding system indexes during startup using the IndexBuildsCoordinator is done by all
    // storage engines if they're missing.
    invariant(_indexBuildsManager.isBackgroundBuilding(replState->buildUUID));

    auto collection =
        CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, replState->collectionUUID);
    invariant(collection);

    // Set up the thread's currentOp information to display createIndexes cmd information.
    updateCurOpOpDescription(opCtx, collection->ns(), replState->indexSpecs);

    // Index builds can safely ignore prepare conflicts and perform writes. On secondaries,
    // prepare operations wait for index builds to complete.
    opCtx->recoveryUnit()->setPrepareConflictBehavior(
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

    return collection;
}

/*
 * Third phase is catching up on all the writes that occurred during the first two phases.
 */
void IndexBuildsCoordinator::_insertKeysFromSideTablesWithoutBlockingWrites(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    // Perform the first drain while holding an intent lock.
    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
    {
        Lock::DBLock autoDb(opCtx, replState->dbName, MODE_IX);
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_IX);

        uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(
            opCtx,
            replState->buildUUID,
            getReadSourceForDrainBeforeCommitQuorum(*replState),
            IndexBuildInterceptor::DrainYieldPolicy::kYield));
    }

    if (MONGO_unlikely(hangAfterIndexBuildFirstDrain.shouldFail())) {
        LOGV2(20666,
              "Hanging after index build first drain",
              "buildUUID"_attr = replState->buildUUID);
        hangAfterIndexBuildFirstDrain.pauseWhileSet(opCtx);
    }
}
void IndexBuildsCoordinator::_insertKeysFromSideTablesBlockingWrites(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions) {
    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
    // Perform the second drain while stopping writes on the collection.
    {
        Lock::DBLock autoDb(opCtx, replState->dbName, MODE_IX);

        // Unlock RSTL to avoid deadlocks with prepare conflicts and state transitions. See
        // SERVER-42621.
        unlockRSTL(opCtx);
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_S);

        uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(
            opCtx,
            replState->buildUUID,
            getReadSourceForDrainBeforeCommitQuorum(*replState),
            IndexBuildInterceptor::DrainYieldPolicy::kNoYield));
    }

    if (MONGO_unlikely(hangAfterIndexBuildSecondDrain.shouldFail())) {
        LOGV2(20667, "Hanging after index build second drain");
        hangAfterIndexBuildSecondDrain.pauseWhileSet();
    }
}

/**
 * Continue the third phase of catching up on all remaining writes that occurred and then commit.
 * Accepts a commit timestamp for the index (null if not available).
 */
IndexBuildsCoordinator::CommitResult IndexBuildsCoordinator::_insertKeysFromSideTablesAndCommit(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    IndexBuildAction action,
    const IndexBuildOptions& indexBuildOptions,
    const Timestamp& commitIndexBuildTimestamp) {

    if (MONGO_unlikely(hangIndexBuildBeforeCommit.shouldFail())) {
        LOGV2(4841706, "Hanging before committing index build");
        hangIndexBuildBeforeCommit.pauseWhileSet();
    }

    Lock::DBLock autoDb(opCtx, replState->dbName, MODE_IX);

    // Unlock RSTL to avoid deadlocks with prepare conflicts and state transitions caused by waiting
    // for a a strong collection lock. See SERVER-42621.
    unlockRSTL(opCtx);

    // Need to return the collection lock back to exclusive mode to complete the index build.
    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
    Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_X);

    // If we can't acquire the RSTL within a given time period, there is an active state transition
    // and we should release our locks and try again. We would otherwise introduce a deadlock with
    // step-up by holding the Collection lock in exclusive mode. After it has enqueued its RSTL X
    // lock, step-up tries to reacquire the Collection locks for prepared transactions, which will
    // conflict with the X lock we currently hold.
    repl::ReplicationStateTransitionLockGuard rstl(
        opCtx, MODE_IX, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    try {
        // Since this thread is not killable by state transitions, this deadline is effectively the
        // longest period of time we can block a step-up. State transitions are infrequent, but
        // need to happen quickly. It should be okay to set this to a low value because the RSTL is
        // rarely contended, and if this times out, we will retry and reacquire the RSTL again
        // without a deadline at the beginning of this function.
        auto deadline = Date_t::now() + Milliseconds(10);
        rstl.waitForLockUntil(deadline);
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>&) {
        return CommitResult::kLockTimeout;
    }

    // If we are no longer primary after receiving a commit quorum, we must restart and wait for a
    // new signal from a new primary because we cannot commit. Note that two-phase index builds can
    // retry because a new signal should be received. Single-phase builds will be unable to commit
    // and will self-abort.
    bool isMaster = replCoord->canAcceptWritesFor(opCtx, dbAndUUID) &&
        !replCoord->getSettings().shouldRecoverFromOplogAsStandalone();
    if (!isMaster && IndexBuildAction::kCommitQuorumSatisfied == action) {
        return CommitResult::kNoLongerPrimary;
    }

    if (IndexBuildAction::kOplogCommit == action) {
        // This signal can be received during primary (drain phase), secondary, startup (startup
        // recovery) and startup2 (initial sync).
        invariant(!isMaster && replState->indexBuildState.isCommitPrepared(),
                  str::stream() << "Index build: " << replState->buildUUID
                                << ",  index build state: "
                                << replState->indexBuildState.toString());
    }

    // The collection object should always exist while an index build is registered.
    CollectionWriter collection(opCtx, replState->collectionUUID);
    invariant(collection,
              str::stream() << "Collection not found after relocking. Index build: "
                            << replState->buildUUID
                            << ", collection UUID: " << replState->collectionUUID);

    // Perform the third and final drain after releasing a shared lock and reacquiring an exclusive
    // lock on the collection.
    uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(
        opCtx,
        replState->buildUUID,
        RecoveryUnit::ReadSource::kNoTimestamp,
        IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    try {
        failIndexBuildOnCommit.execute(
            [](const BSONObj&) { uasserted(4698903, "index build aborted due to failpoint"); });

        {
            auto dss = DatabaseShardingState::get(opCtx, replState->dbName);
            auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);
            dss->checkDbVersion(opCtx, dssLock);
        }

        // If we are no longer primary and a single phase index build started as primary attempts to
        // commit, trigger a self-abort.
        if (!isMaster && IndexBuildAction::kSinglePhaseCommit == action) {
            uassertStatusOK(
                {ErrorCodes::NotWritablePrimary,
                 str::stream() << "Unable to commit index build because we are no longer primary: "
                               << replState->buildUUID});
        }

        // Retry indexing records that failed key generation, but only if we are primary.
        // Secondaries rely on the primary's decision to commit as assurance that it has checked all
        // key generation errors on its behalf.
        if (isMaster) {
            uassertStatusOK(_indexBuildsManager.retrySkippedRecords(
                opCtx, replState->buildUUID, collection.get()));
        }

        // Duplicate key constraint checking phase. Duplicate key errors are tracked for
        // single-phase builds on primaries and two-phase builds in all replication states.
        // Single-phase builds on secondaries don't track duplicates so this call is a no-op. This
        // can be called for two-phase builds in all replication states except during initial sync
        // when this node is not guaranteed to be consistent.
        bool twoPhaseAndNotInitialSyncing = IndexBuildProtocol::kTwoPhase == replState->protocol &&
            !replCoord->getMemberState().startup2();
        if (IndexBuildProtocol::kSinglePhase == replState->protocol ||
            twoPhaseAndNotInitialSyncing) {
            uassertStatusOK(_indexBuildsManager.checkIndexConstraintViolations(
                opCtx, collection.get(), replState->buildUUID));
        }
    } catch (const ExceptionForCat<ErrorCategory::ShutdownError>& e) {
        logFailure(e.toStatus(), collection->ns(), replState);
        _completeAbortForShutdown(opCtx, replState, collection.get());
        throw;
    } catch (const DBException& e) {
        auto status = e.toStatus();
        logFailure(status, collection->ns(), replState);

        // It is illegal to abort the index build at this point. Note that Interruption exceptions
        // are allowed because we cannot control them as they bypass the routine abort machinery.
        invariant(e.code() != ErrorCodes::IndexBuildAborted);

        // Index build commit may not fail on secondaries because it implies diverenge with data on
        // the primary. The only exception is single-phase builds started on primaries, which may
        // fail after a state transition. In this case, we have not replicated anything to
        // roll-back. With two-phase index builds, if a primary replicated an abortIndexBuild oplog
        // entry, then this index build should have been interrupted before committing with an
        // IndexBuildAborted error code.
        const bool twoPhaseAndNotPrimary =
            IndexBuildProtocol::kTwoPhase == replState->protocol && !isMaster;
        if (twoPhaseAndNotPrimary) {
            LOGV2_FATAL(4698902,
                        "Index build failed while not primary",
                        "buildUUID"_attr = replState->buildUUID,
                        "collectionUUID"_attr = replState->collectionUUID,
                        "db"_attr = replState->dbName,
                        "error"_attr = status);
        }

        // This index build failed due to an indexing error in normal circumstances. Abort while
        // still holding the RSTL and collection locks.
        _completeSelfAbort(opCtx, replState, status);
        throw;
    }

    // If two phase index builds is enabled, index build will be coordinated using
    // startIndexBuild and commitIndexBuild oplog entries.
    auto onCommitFn = [&] { onCommitIndexBuild(opCtx, collection->ns(), replState); };

    auto onCreateEachFn = [&](const BSONObj& spec) {
        if (IndexBuildProtocol::kTwoPhase == replState->protocol) {
            return;
        }

        auto opObserver = opCtx->getServiceContext()->getOpObserver();
        auto fromMigrate = false;
        opObserver->onCreateIndex(
            opCtx, collection->ns(), replState->collectionUUID, spec, fromMigrate);
    };

    // Commit index build.
    TimestampBlock tsBlock(opCtx, commitIndexBuildTimestamp);
    uassertStatusOK(_indexBuildsManager.commitIndexBuild(
        opCtx, collection, collection->ns(), replState->buildUUID, onCreateEachFn, onCommitFn));
    removeIndexBuildEntryAfterCommitOrAbort(opCtx, dbAndUUID, *replState);
    replState->stats.numIndexesAfter = getNumIndexesTotal(opCtx, collection.get());
    LOGV2(20663,
          "Index build: completed successfully",
          "buildUUID"_attr = replState->buildUUID,
          "namespace"_attr = collection->ns(),
          "uuid"_attr = replState->collectionUUID,
          "indexesBuilt"_attr = replState->indexSpecs.size(),
          "numIndexesBefore"_attr = replState->stats.numIndexesBefore,
          "numIndexesAfter"_attr = replState->stats.numIndexesAfter);
    return CommitResult::kSuccess;
}

StatusWith<std::pair<long long, long long>> IndexBuildsCoordinator::_runIndexRebuildForRecovery(
    OperationContext* opCtx,
    CollectionWriter& collection,
    const UUID& buildUUID,
    RepairData repair) noexcept {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));

    auto replState = invariant(_getIndexBuild(buildUUID));

    // We rely on 'collection' for any collection information because no databases are open during
    // recovery.
    NamespaceString nss = collection->ns();
    invariant(!nss.isEmpty());

    auto status = Status::OK();

    long long numRecords = 0;
    long long dataSize = 0;

    ReplIndexBuildState::IndexCatalogStats indexCatalogStats;
    indexCatalogStats.numIndexesBefore = getNumIndexesTotal(opCtx, collection.get());

    try {
        LOGV2(20673,
              "Index builds manager starting: {buildUUID}: {namespace}",
              "Index builds manager starting",
              "buildUUID"_attr = buildUUID,
              logAttrs(nss));

        std::tie(numRecords, dataSize) =
            uassertStatusOK(_indexBuildsManager.startBuildingIndexForRecovery(
                opCtx, collection.get(), buildUUID, repair));

        // Since we are holding an exclusive collection lock to stop new writes, do not yield locks
        // while draining.
        uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(
            opCtx,
            replState->buildUUID,
            RecoveryUnit::ReadSource::kNoTimestamp,
            IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

        uassertStatusOK(_indexBuildsManager.checkIndexConstraintViolations(
            opCtx, collection.get(), replState->buildUUID));

        // Commit the index build.
        uassertStatusOK(_indexBuildsManager.commitIndexBuild(opCtx,
                                                             collection,
                                                             nss,
                                                             buildUUID,
                                                             MultiIndexBlock::kNoopOnCreateEachFn,
                                                             MultiIndexBlock::kNoopOnCommitFn));

        indexCatalogStats.numIndexesAfter = getNumIndexesTotal(opCtx, collection.get());

        LOGV2(20674,
              "Index builds manager completed successfully: {buildUUID}: {namespace}. Index specs "
              "requested: {indexSpecsRequested}. Indexes in catalog before build: "
              "{numIndexesBefore}. Indexes in catalog after build: {numIndexesAfter}",
              "Index builds manager completed successfully",
              "buildUUID"_attr = buildUUID,
              logAttrs(nss),
              "indexSpecsRequested"_attr = replState->indexSpecs.size(),
              "numIndexesBefore"_attr = indexCatalogStats.numIndexesBefore,
              "numIndexesAfter"_attr = indexCatalogStats.numIndexesAfter);
    } catch (const DBException& ex) {
        status = ex.toStatus();
        invariant(status != ErrorCodes::IndexAlreadyExists);
        LOGV2(20675,
              "Index builds manager failed: {buildUUID}: {namespace}: {error}",
              "Index builds manager failed",
              "buildUUID"_attr = buildUUID,
              logAttrs(nss),
              "error"_attr = status);
    }

    // Index build is registered in manager regardless of IndexBuildsManager::setUpIndexBuild()
    // result.
    if (!status.isOK()) {
        // An index build failure during recovery is fatal.
        logFailure(status, nss, replState);
        fassertNoTrace(51076, status);
    }

    // 'numIndexesBefore' was before we cleared any unfinished indexes, so it must be the same
    // as 'numIndexesAfter', since we're going to be building any unfinished indexes too.
    invariant(indexCatalogStats.numIndexesBefore == indexCatalogStats.numIndexesAfter);

    {
        stdx::unique_lock<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replState);
    }

    if (status.isOK()) {
        return std::make_pair(numRecords, dataSize);
    }
    return status;
}

StatusWith<std::shared_ptr<ReplIndexBuildState>> IndexBuildsCoordinator::_getIndexBuild(
    const UUID& buildUUID) const {
    stdx::unique_lock<Latch> lk(_mutex);
    auto it = _allIndexBuilds.find(buildUUID);
    if (it == _allIndexBuilds.end()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "No index build with UUID: " << buildUUID};
    }
    return it->second;
}

std::vector<std::shared_ptr<ReplIndexBuildState>> IndexBuildsCoordinator::_getIndexBuilds() const {
    stdx::unique_lock<Latch> lk(_mutex);
    auto filter = [](const auto& replState) { return true; };
    return _filterIndexBuilds_inlock(lk, filter);
}

std::vector<std::shared_ptr<ReplIndexBuildState>> IndexBuildsCoordinator::_filterIndexBuilds_inlock(
    WithLock lk, IndexBuildFilterFn indexBuildFilter) const {
    std::vector<std::shared_ptr<ReplIndexBuildState>> indexBuilds;
    for (auto pair : _allIndexBuilds) {
        auto replState = pair.second;
        if (!indexBuildFilter(*replState)) {
            continue;
        }
        indexBuilds.push_back(replState);
    }
    return indexBuilds;
}

int IndexBuildsCoordinator::getNumIndexesTotal(OperationContext* opCtx,
                                               const CollectionPtr& collection) {
    invariant(collection);
    const auto& nss = collection->ns();
    invariant(opCtx->lockState()->isLocked(),
              str::stream() << "Unable to get index count because collection was not locked"
                            << nss);

    auto indexCatalog = collection->getIndexCatalog();
    invariant(indexCatalog, str::stream() << "Collection is missing index catalog: " << nss);

    return indexCatalog->numIndexesTotal(opCtx);
}

std::vector<BSONObj> IndexBuildsCoordinator::prepareSpecListForCreate(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const NamespaceString& nss,
    const std::vector<BSONObj>& indexSpecs) {
    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx,
                                                                               collection->ns());
    invariant(collection);

    // During secondary oplog application, the index specs have already been normalized in the
    // oplog entries read from the primary. We should not be modifying the specs any further.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().usingReplSets() && !replCoord->canAcceptWritesFor(opCtx, nss)) {
        return indexSpecs;
    }

    // Normalize the specs' collations, wildcard projections, and partial filters as applicable.
    auto normalSpecs = normalizeIndexSpecs(opCtx, collection, indexSpecs);

    // Remove any index specifications which already exist in the catalog.
    auto indexCatalog = collection->getIndexCatalog();
    auto resultSpecs =
        indexCatalog->removeExistingIndexes(opCtx, normalSpecs, true /*removeIndexBuildsToo*/);

    // Verify that each spec is compatible with the collection's sharding state.
    for (const BSONObj& spec : resultSpecs) {
        if (spec[kUniqueFieldName].trueValue()) {
            checkShardKeyRestrictions(opCtx, nss, spec[kKeyFieldName].Obj());
        }
    }

    return resultSpecs;
}

std::vector<BSONObj> IndexBuildsCoordinator::normalizeIndexSpecs(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const std::vector<BSONObj>& indexSpecs) {
    // This helper function may be called before the collection is created, when we are attempting
    // to check whether the candidate index collides with any existing indexes. If 'collection' is
    // nullptr, skip normalization. Since the collection does not exist there cannot be a conflict,
    // and we will normalize once the candidate spec is submitted to the IndexBuildsCoordinator.
    if (!collection) {
        return indexSpecs;
    }

    // Add collection-default collation where needed and normalize the collation in each index spec.
    auto normalSpecs =
        uassertStatusOK(collection->addCollationDefaultsToIndexSpecsForCreate(opCtx, indexSpecs));

    // If the index spec has a partialFilterExpression, we normalize it by parsing to an optimized,
    // sorted MatchExpression tree, re-serialize it to BSON, and add it back into the index spec.
    const auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr, collection->ns());
    std::transform(normalSpecs.begin(), normalSpecs.end(), normalSpecs.begin(), [&](auto& spec) {
        const auto kPartialFilterName = IndexDescriptor::kPartialFilterExprFieldName;
        auto partialFilterExpr = spec.getObjectField(kPartialFilterName);
        if (partialFilterExpr.isEmpty()) {
            return spec;
        }
        // Parse, optimize and sort the MatchExpression to reduce it to its normalized form.
        // Serialize the normalized filter back into the index spec before returning.
        auto partialFilter = MatchExpressionParser::parseAndNormalize(partialFilterExpr, expCtx);
        return spec.addField(BSON(kPartialFilterName << partialFilter->serialize()).firstElement());
    });

    // If any of the specs describe wildcard indexes, normalize the wildcard projections if present.
    // This will change all specs of the form {"a.b.c": 1} to normalized form {a: {b: {c : 1}}}.
    std::transform(normalSpecs.begin(), normalSpecs.end(), normalSpecs.begin(), [](auto& spec) {
        const auto kProjectionName = IndexDescriptor::kPathProjectionFieldName;
        const auto pathProjectionSpec = spec.getObjectField(kProjectionName);
        static const auto kWildcardKeyPattern = BSON("$**" << 1);
        if (pathProjectionSpec.isEmpty()) {
            return spec;
        }
        auto wildcardProjection =
            WildcardKeyGenerator::createProjectionExecutor(kWildcardKeyPattern, pathProjectionSpec);
        auto normalizedProjection =
            wildcardProjection.exec()->serializeTransformation(boost::none).toBson();
        return spec.addField(BSON(kProjectionName << normalizedProjection).firstElement());
    });
    return normalSpecs;
}

bool IndexBuildsCoordinator::supportsResumableIndexBuilds() const {
    auto serviceContext = getGlobalServiceContext();
    invariant(serviceContext);

    auto storageEngine = serviceContext->getStorageEngine();
    invariant(storageEngine);

    return storageEngine->supportsResumableIndexBuilds();
}
}  // namespace mongo

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

#include "mongo/db/index_builds_coordinator.h"

#include <absl/container/node_hash_map.h>
#include <algorithm>
#include <boost/filesystem/operations.hpp>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <mutex>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#include "mongo/base/error_codes.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_yield_restore.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/index_build_entry_gen.h"
#include "mongo/db/catalog/index_builds_manager.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_build_entry_helpers.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/disk_space_util.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/two_phase_index_build_knobs_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/message.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildFirstDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildSecondDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildDumpsInsertsFromBulk);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildDumpsInsertsFromBulkLock);
MONGO_FAIL_POINT_DEFINE(hangAfterInitializingIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangBeforeCompletingAbort);
MONGO_FAIL_POINT_DEFINE(failIndexBuildOnCommit);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildBeforeAbortCleanUp);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildOnStepUp);
MONGO_FAIL_POINT_DEFINE(hangAfterSettingUpResumableIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildBeforeCommit);
MONGO_FAIL_POINT_DEFINE(hangBeforeBuildingIndex);
MONGO_FAIL_POINT_DEFINE(hangBeforeBuildingIndexSecond);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildBeforeWaitingUntilMajorityOpTime);
MONGO_FAIL_POINT_DEFINE(hangBeforeUnregisteringAfterCommit);
MONGO_FAIL_POINT_DEFINE(failSetUpResumeIndexBuild);
MONGO_FAIL_POINT_DEFINE(failIndexBuildWithError);
MONGO_FAIL_POINT_DEFINE(failIndexBuildWithErrorInSecondDrain);
MONGO_FAIL_POINT_DEFINE(hangInRemoveIndexBuildEntryAfterCommitOrAbort);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildOnSetupBeforeTakingLocks);
MONGO_FAIL_POINT_DEFINE(hangAbortIndexBuildByBuildUUIDAfterLocks);
MONGO_FAIL_POINT_DEFINE(hangOnStepUpAsyncTaskBeforeCheckingCommitQuorum);

extern FailPoint skipWriteConflictRetries;

IndexBuildsCoordinator::IndexBuildsSSS::IndexBuildsSSS()
    : ServerStatusSection("indexBuilds"),
      registered(0),
      scanCollection(0),
      drainSideWritesTable(0),
      drainSideWritesTablePreCommit(0),
      waitForCommitQuorum(0),
      drainSideWritesTableOnCommit(0),
      processConstraintsViolatonTableOnCommit(0),
      commit(0) {}

namespace {

constexpr StringData kCreateIndexesFieldName = "createIndexes"_sd;
constexpr StringData kCommitIndexBuildFieldName = "commitIndexBuild"_sd;
constexpr StringData kAbortIndexBuildFieldName = "abortIndexBuild"_sd;
constexpr StringData kIndexesFieldName = "indexes"_sd;
constexpr StringData kKeyFieldName = "key"_sd;
constexpr StringData kUniqueFieldName = "unique"_sd;
constexpr StringData kPrepareUniqueFieldName = "prepareUnique"_sd;

/**
 * Checks if unique index specification is compatible with sharding configuration.
 */
void checkShardKeyRestrictions(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& newIdxKey) {
    CollectionCatalog::get(opCtx)->invariantHasExclusiveAccessToCollection(opCtx, nss);

    const auto collDesc = CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)
                              ->getCollectionDescription(opCtx);
    if (!collDesc.isSharded())
        return;

    const ShardKeyPattern shardKeyPattern(collDesc.getKeyPattern());
    uassert(ErrorCodes::CannotCreateIndex,
            str::stream() << "cannot create index with 'unique' or 'prepareUnique' option over "
                          << newIdxKey << " with shard key pattern " << shardKeyPattern.toBSON(),
            shardKeyPattern.isIndexUniquenessCompatible(newIdxKey));
}

/**
 * Returns true if we should build the indexes an empty collection using the IndexCatalog and
 * bypass the index build registration.
 */
bool shouldBuildIndexesOnEmptyCollectionSinglePhased(OperationContext* opCtx,
                                                     const CollectionPtr& collection,
                                                     IndexBuildProtocol protocol) {
    const auto& nss = collection->ns();
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_X),
              str::stream() << nss.toStringForErrorMsg());

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // Check whether the replica set member's config has {buildIndexes:false} set, which means
    // we are not allowed to build non-_id indexes on this server.
    if (!replCoord->buildsIndexes()) {
        return false;
    }

    // Secondaries should not bypass index build registration (and _runIndexBuild()) for two phase
    // index builds because they need to report index build progress to the primary per commit
    // quorum.
    if (IndexBuildProtocol::kTwoPhase == protocol && !replCoord->canAcceptWritesFor(opCtx, nss)) {
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

    // This check happens before spawning the index build thread. So it does not race with the
    // replication recovery flag being modified.
    if (inReplicationRecovery(opCtx->getServiceContext()).load()) {
        return false;
    }

    // Now, it's fine to trust Collection::isEmpty().
    // Fast counts are prone to both false positives and false negatives on unclean shutdowns. False
    // negatives can cause to skip index building. And, false positives can cause mismatch in number
    // of index entries among the nodes in the replica set. So, verify the collection is really
    // empty by opening the WT cursor and reading the first document.
    return collection->isEmpty(opCtx);
}

/**
 * Removes the index build from the config.system.indexBuilds collection after the primary has
 * written the commitIndexBuild or abortIndexBuild oplog entry.
 */
void removeIndexBuildEntryAfterCommitOrAbort(OperationContext* opCtx,
                                             const NamespaceStringOrUUID& dbAndUUID,
                                             const CollectionPtr& indexBuildEntryCollection,
                                             const ReplIndexBuildState& replState) {
    if (IndexBuildProtocol::kSinglePhase == replState.protocol) {
        return;
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->canAcceptWritesFor(opCtx, dbAndUUID)) {
        return;
    }

    if (replCoord->getSettings().shouldRecoverFromOplogAsStandalone()) {
        // Writes to the 'config.system.indexBuilds' collection are replicated and the index entry
        // will be removed when the delete oplog entry is replayed at a later time.
        return;
    }

    if (replState.isSettingUp()) {
        // The index build document is not written to config.system.indexBuilds collection yet.
        return;
    }

    auto status = indexbuildentryhelpers::removeIndexBuildEntry(
        opCtx, indexBuildEntryCollection, replState.buildUUID);
    if (!status.isOK()) {
        LOGV2_FATAL_NOTRACE(4763501,
                            "Failed to remove index build from system collection",
                            "buildUUID"_attr = replState.buildUUID,
                            "collectionUUID"_attr = replState.collectionUUID,
                            logAttrs(replState.dbName),
                            "indexNames"_attr = replState.indexNames,
                            "indexSpecs"_attr = replState.indexSpecs,
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

    replState->commit(opCtx);

    if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
        return;
    }

    invariant(IndexBuildProtocol::kTwoPhase == replState->protocol,
              str::stream() << "onCommitIndexBuild: " << buildUUID);
    invariant(shard_role_details::getLocker(opCtx)->isWriteLocked(),
              str::stream() << "onCommitIndexBuild: " << buildUUID);

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    const auto& collUUID = replState->collectionUUID;
    const auto& indexSpecs = replState->indexSpecs;
    auto fromMigrate = false;

    // Since two phase index builds are allowed to survive replication state transitions, we should
    // check if the node is currently a primary before attempting to write to the oplog.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->canAcceptWritesFor(opCtx, nss)) {
        invariant(!shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp().isNull(),
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
                       ReplIndexBuildState& replState) {
    if (IndexBuildProtocol::kTwoPhase != replState.protocol) {
        return;
    }

    invariant(shard_role_details::getLocker(opCtx)->isWriteLocked(),
              replState.buildUUID.toString());

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    auto collUUID = replState.collectionUUID;
    auto fromMigrate = false;
    opObserver->onAbortIndexBuild(opCtx,
                                  nss,
                                  collUUID,
                                  replState.buildUUID,
                                  replState.indexSpecs,
                                  replState.getAbortStatus(),
                                  fromMigrate);
}

/**
 * Logs the index build failure error in a standard format.
 */
void logFailure(Status status,
                const NamespaceString& nss,
                std::shared_ptr<ReplIndexBuildState> replState) {
    LOGV2(20649,
          "Index build: failed",
          "buildUUID"_attr = replState->buildUUID,
          "collectionUUID"_attr = replState->collectionUUID,
          logAttrs(nss),
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
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    // Helpers::getLast will bypass the oplog visibility rules by doing a backwards collection
    // scan.
    BSONObj oplogEntryBSON;
    // This operation does not perform any writes, but the index building code is sensitive to
    // exceptions and we must protect it from unanticipated write conflicts from reads.
    writeConflictRetry(opCtx, "getLatestOplogOpTime", NamespaceString::kRsOplogNamespace, [&]() {
        invariant(Helpers::getLast(opCtx, NamespaceString::kRsOplogNamespace, oplogEntryBSON));
    });

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
    if (!replCoord->getSettings().isReplSet()) {
        return false;
    }

    // When we are applying a startIndexBuild oplog entry during the oplog application phase of
    // startup recovery, the last optime here derived from the local oplog may not be a valid
    // optime to wait on for the majority commit point since the rest of the replica set may
    // be on a different branch of history.
    if (inReplicationRecovery(opCtx->getServiceContext()).load()) {
        LOGV2(5039100,
              "Index build: in replication recovery. Not waiting for last optime before "
              "interceptors to be majority committed",
              "buildUUID"_attr = replState.buildUUID);
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
                  "Index build: cannot read commit quorum from config db, will not be resumable",
                  "buildUUID"_attr = replState.buildUUID,
                  "error"_attr = swCommitQuorum.getStatus());
            return false;
        }
        auto commitQuorum = swCommitQuorum.getValue();
        if (CommitQuorumOptions::kVotingMembers != commitQuorum.mode) {
            return false;
        }
    }

    return true;
}

/**
 * Returns the ReadSource to be used for a drain occurring before the commit quorum has been
 * satisfied.
 */
RecoveryUnit::ReadSource getReadSourceForDrainBeforeCommitQuorum(
    const ReplIndexBuildState& replState) {
    return replState.isResumable() ? RecoveryUnit::ReadSource::kMajorityCommitted
                                   : RecoveryUnit::ReadSource::kNoTimestamp;
}

/**
 * Returns an AutoGetCollection::Options configured to skip the RSTL if 'skipRSTL' is true.
 */
AutoGetCollection::Options makeAutoGetCollectionOptions(bool skipRSTL) {
    return AutoGetCollection::Options{}.globalLockSkipOptions(
        Lock::GlobalLockSkipOptions{.skipRSTLLock = skipRSTL});
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

Status IndexBuildsCoordinator::checkDiskSpaceSufficientToStartIndexBuild(OperationContext* opCtx) {
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    const bool filesNotAllInSameDirectory =
        storageEngine->isUsingDirectoryPerDb() || storageEngine->isUsingDirectoryForIndexes();
    if (filesNotAllInSameDirectory) {
        LOGV2(7333300,
              "Index build: skipping available disk space check before starting index build as "
              "storage engine stores data files in different directories");
        return Status::OK();
    }

    // Must hold the global lock to ensure safe access to storageGlobalParams.dbpath.
    Lock::GlobalLock globalLock{opCtx, MODE_IS};
    const auto availableBytes = getAvailableDiskSpaceBytesInDbPath(storageGlobalParams.dbpath);
    const int64_t requiredBytes = gIndexBuildMinAvailableDiskSpaceMB.load() * 1024 * 1024;
    if (availableBytes <= requiredBytes) {
        return Status(
            ErrorCodes::OutOfDiskSpace,
            fmt::format("available disk space of {} bytes is less than required minimum of {}",
                        availableBytes,
                        requiredBytes));
    }
    return Status::OK();
}

std::unique_ptr<DiskSpaceMonitor::Action>
IndexBuildsCoordinator::makeKillIndexBuildOnLowDiskSpaceAction() {
    class KillIndexBuildsAction : public DiskSpaceMonitor::Action {
    public:
        KillIndexBuildsAction(IndexBuildsCoordinator* coordinator) : _coord(coordinator) {}

        int64_t getThresholdBytes() noexcept final {
            // This parameter's validator ensures that this multiplication will not overflow.
            return gIndexBuildMinAvailableDiskSpaceMB.load() * 1024 * 1024;
        }

        void act(OperationContext* opCtx, int64_t availableBytes) noexcept final {
            if (!feature_flags::gIndexBuildGracefulErrorHandling
                     .isEnabledUseLastLTSFCVWhenUninitialized(
                         serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                LOGV2(6826200,
                      "Index build: disk space monitor detected we're low on storage space but "
                      "'featureFlagIndexBuildGracefulErrorHandling' is disabled. Ignoring it");
                return;
            }

            if (_coord->noIndexBuildInProgress()) {
                // Avoid excessive logging when no index builds are in progress. Nothing prevents an
                // index build from starting after this check.  Subsequent calls will see any
                // newly-registered builds.
                return;
            }
            LOGV2(7333502,
                  "Attempting to kill index builds because remaining disk space is less than "
                  "required minimum",
                  "availableBytes"_attr = availableBytes,
                  "requiredBytes"_attr = getThresholdBytes());
            try {
                _coord->abortAllIndexBuildsDueToDiskSpace(
                    opCtx, availableBytes, getThresholdBytes());
            } catch (...) {
                LOGV2(7333503, "Failed to kill index builds", "reason"_attr = exceptionToStatus());
            }
        }

    private:
        IndexBuildsCoordinator* _coord;
    };
    return std::make_unique<KillIndexBuildsAction>(this);
};

std::vector<std::string> IndexBuildsCoordinator::extractIndexNames(
    const std::vector<BSONObj>& specs) {
    std::vector<std::string> indexNames;
    for (const auto& spec : specs) {
        std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName).toString();
        invariant(!name.empty(),
                  str::stream() << "Bad spec passed into ReplIndexBuildState constructor, missing '"
                                << IndexDescriptor::kIndexNameFieldName << "' field: " << spec);
        indexNames.push_back(name);
    }
    return indexNames;
}

bool IndexBuildsCoordinator::isCreateIndexesErrorSafeToIgnore(
    const Status& status, IndexBuildsManager::IndexConstraints indexConstraints) {
    return (status == ErrorCodes::IndexAlreadyExists ||
            ((status == ErrorCodes::IndexOptionsConflict ||
              status == ErrorCodes::IndexKeySpecsConflict) &&
             IndexBuildsManager::IndexConstraints::kRelax == indexConstraints));
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

    CollectionWriter collection(opCtx, nss);

    // Complete the index build.
    return _runIndexRebuildForRecovery(opCtx, collection, buildUUID, repair);
}

Status IndexBuildsCoordinator::_startIndexBuildForRecovery(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           const std::vector<BSONObj>& specs,
                                                           const UUID& buildUUID,
                                                           IndexBuildProtocol protocol) {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_X));

    std::vector<std::string> indexNames;
    for (auto& spec : specs) {
        std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName).toString();
        if (name.empty()) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream()
                              << "Cannot create an index for a spec '" << spec
                              << "' without a non-empty string value for the 'name' field");
        }
        indexNames.push_back(name);
    }

    CollectionWriter collection(opCtx, nss);
    {
        // These steps are combined into a single WUOW to ensure there are no commits without the
        // indexes for repair.
        WriteUnitOfWork wuow(opCtx);

        // We need to initialize the collection to rebuild the indexes. The collection may already
        // be initialized when rebuilding multiple unfinished indexes on the same collection.
        if (!collection->isInitialized()) {
            collection.getWritableCollection(opCtx)->init(opCtx);
        }

        if (storageGlobalParams.repair) {
            Status status = _dropIndexesForRepair(opCtx, collection, indexNames);
            if (!status.isOK()) {
                return status;
            }
        } else {
            // Unfinished index builds that are not resumable will drop and recreate the index table
            // using the same ident to avoid doing untimestamped writes to the catalog.
            for (const auto& indexName : indexNames) {
                auto indexCatalog = collection.getWritableCollection(opCtx)->getIndexCatalog();
                auto writableEntry = indexCatalog->getWritableEntryByName(
                    opCtx,
                    indexName,
                    IndexCatalog::InclusionPolicy::kUnfinished |
                        IndexCatalog::InclusionPolicy::kFrozen);
                Status status = indexCatalog->resetUnfinishedIndexForRecovery(
                    opCtx, collection.getWritableCollection(opCtx), writableEntry);
                if (!status.isOK()) {
                    return status;
                }

                const auto durableBuildUUID = collection->getIndexBuildUUID(indexName);

                // A build UUID is present if and only if we are rebuilding a two-phase build.
                invariant((protocol == IndexBuildProtocol::kTwoPhase) ==
                          durableBuildUUID.has_value());
                // When a buildUUID is present, it must match the build UUID parameter to this
                // function.
                invariant(!durableBuildUUID || *durableBuildUUID == buildUUID,
                          str::stream() << "durable build UUID: " << durableBuildUUID
                                        << "buildUUID: " << buildUUID);
            }
        }

        auto replIndexBuildState = std::make_shared<ReplIndexBuildState>(
            buildUUID, collection->uuid(), nss.dbName(), specs, protocol);

        Status status = activeIndexBuilds.registerIndexBuild(replIndexBuildState);
        if (!status.isOK()) {
            return status;
        }
        indexBuildsSSS.registered.addAndFetch(1);

        IndexBuildsManager::SetupOptions options;
        options.protocol = protocol;
        // All indexes are dropped during repair and should be rebuilt normally.
        options.forRecovery = !storageGlobalParams.repair;
        status = _indexBuildsManager.setUpIndexBuild(
            opCtx, collection, specs, buildUUID, MultiIndexBlock::kNoopOnInitFn, options);
        if (!status.isOK()) {
            // An index build failure during recovery is fatal.
            logFailure(status, nss, replIndexBuildState);
            fassertNoTrace(51086, status);
        }

        wuow.commit();
        // Mark the index build setup as complete, from now on cleanup is required on failure/abort.
        replIndexBuildState->completeSetup();
    }

    return Status::OK();
}

Status IndexBuildsCoordinator::_dropIndexesForRepair(OperationContext* opCtx,
                                                     CollectionWriter& collection,
                                                     const std::vector<std::string>& indexNames) {
    invariant(collection->isInitialized());
    for (const auto& indexName : indexNames) {
        auto indexCatalog = collection.getWritableCollection(opCtx)->getIndexCatalog();
        auto writableEntry = indexCatalog->getWritableEntryByName(
            opCtx, indexName, IndexCatalog::InclusionPolicy::kReady);
        if (writableEntry->descriptor()) {
            Status s = indexCatalog->dropIndexEntry(
                opCtx, collection.getWritableCollection(opCtx), writableEntry);
            if (!s.isOK()) {
                return s;
            }
            continue;
        }

        // The index must be unfinished or frozen if it isn't ready.
        writableEntry = indexCatalog->getWritableEntryByName(
            opCtx,
            indexName,
            IndexCatalog::InclusionPolicy::kUnfinished | IndexCatalog::InclusionPolicy::kFrozen);
        invariant(writableEntry);
        Status s = indexCatalog->dropUnfinishedIndex(
            opCtx, collection.getWritableCollection(opCtx), writableEntry);
        if (!s.isOK()) {
            return s;
        }
    }

    return Status::OK();
}

Status IndexBuildsCoordinator::_setUpResumeIndexBuild(OperationContext* opCtx,
                                                      const DatabaseName& dbName,
                                                      const UUID& collectionUUID,
                                                      const std::vector<BSONObj>& specs,
                                                      const UUID& buildUUID,
                                                      const ResumeIndexInfo& resumeInfo) {
    NamespaceStringOrUUID nssOrUuid{dbName, collectionUUID};

    if (MONGO_unlikely(failSetUpResumeIndexBuild.shouldFail())) {
        return {ErrorCodes::FailPointEnabled, "failSetUpResumeIndexBuild fail point is enabled"};
    }

    // Don't use the AutoGet helpers because they require an open database, which may not be the
    // case when an index build is resumed during recovery.
    Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
    CollectionNamespaceOrUUIDLock collLock(opCtx, nssOrUuid, MODE_X);

    CollectionWriter collection(opCtx, resumeInfo.getCollectionUUID());
    invariant(collection);
    auto durableCatalog = DurableCatalog::get(opCtx);

    for (const auto& spec : specs) {
        std::string indexName =
            spec.getStringField(IndexDescriptor::kIndexNameFieldName).toString();
        if (indexName.empty()) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream()
                              << "Cannot create an index for a spec '" << spec
                              << "' without a non-empty string value for the 'name' field");
        }

        // Check that the information in the durable catalog matches the resume info.
        uassert(4841702,
                "Index not found in durable catalog while attempting to resume index build",
                collection->isIndexPresent(indexName));

        const auto durableBuildUUID = collection->getIndexBuildUUID(indexName);
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

        uassertStatusOK(collection->checkMetaDataForIndex(indexName, spec));
    }

    if (!collection->isInitialized()) {
        WriteUnitOfWork wuow(opCtx);
        collection.getWritableCollection(opCtx)->init(opCtx);
        wuow.commit();
    }

    auto protocol = IndexBuildProtocol::kTwoPhase;
    auto replIndexBuildState = std::make_shared<ReplIndexBuildState>(
        buildUUID, collection->uuid(), dbName, specs, protocol);

    Status status = activeIndexBuilds.registerIndexBuild(replIndexBuildState);
    if (!status.isOK()) {
        return status;
    }
    indexBuildsSSS.registered.addAndFetch(1);

    IndexBuildsManager::SetupOptions options;
    options.protocol = protocol;
    status = _indexBuildsManager.setUpIndexBuild(
        opCtx, collection, specs, buildUUID, MultiIndexBlock::kNoopOnInitFn, options, resumeInfo);
    if (!status.isOK()) {
        activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager, replIndexBuildState);
    }

    // Mark the index build setup as complete, from now on cleanup is required on failure/abort.
    replIndexBuildState->completeSetup();
    return status;
}

void IndexBuildsCoordinator::waitForAllIndexBuildsToStop(OperationContext* opCtx) {
    activeIndexBuilds.waitForAllIndexBuildsToStop(opCtx);
}

std::vector<UUID> IndexBuildsCoordinator::abortCollectionIndexBuilds(
    OperationContext* opCtx,
    const NamespaceString collectionNss,
    const UUID collectionUUID,
    const std::string& reason) {
    auto collIndexBuilds = [&]() -> std::vector<std::shared_ptr<ReplIndexBuildState>> {
        auto indexBuildFilter = [=](const auto& replState) {
            return collectionUUID == replState.collectionUUID;
        };
        return activeIndexBuilds.filterIndexBuilds(indexBuildFilter);
    }();

    if (collIndexBuilds.empty()) {
        return {};
    }

    LOGV2(23879,
          "About to abort all index builders",
          logAttrs(collectionNss),
          "uuid"_attr = collectionUUID,
          "reason"_attr = reason);

    std::vector<UUID> buildUUIDs;
    for (const auto& replState : collIndexBuilds) {
        if (abortIndexBuildByBuildUUID(
                opCtx, replState->buildUUID, IndexBuildAction::kPrimaryAbort, reason)) {
            buildUUIDs.push_back(replState->buildUUID);
        }
    }
    return buildUUIDs;
}

void IndexBuildsCoordinator::abortDatabaseIndexBuilds(OperationContext* opCtx,
                                                      const DatabaseName& dbName,
                                                      const std::string& reason) {
    LOGV2(4612302,
          "About to abort all index builders running for collections in the given database",
          "database"_attr = dbName,
          "reason"_attr = reason);

    auto builds = [&]() -> std::vector<std::shared_ptr<ReplIndexBuildState>> {
        auto indexBuildFilter = [=](const auto& replState) {
            return dbName == replState.dbName;
        };
        return activeIndexBuilds.filterIndexBuilds(indexBuildFilter);
    }();
    for (const auto& replState : builds) {
        if (!abortIndexBuildByBuildUUID(
                opCtx, replState->buildUUID, IndexBuildAction::kPrimaryAbort, reason)) {
            // The index build may already be in the midst of tearing down.
            LOGV2(5010502,
                  "Index build: failed to abort index build for database drop",
                  "buildUUID"_attr = replState->buildUUID,
                  "database"_attr = dbName,
                  "collectionUUID"_attr = replState->collectionUUID);
        }
    }
}

void IndexBuildsCoordinator::_abortTenantIndexBuilds(
    OperationContext* opCtx,
    const std::vector<std::shared_ptr<ReplIndexBuildState>>& builds,
    MigrationProtocolEnum protocol,
    const std::string& reason) {

    std::vector<std::shared_ptr<ReplIndexBuildState>> buildsWaitingToFinish;
    buildsWaitingToFinish.reserve(builds.size());
    const auto indexBuildActionStr =
        indexBuildActionToString(IndexBuildAction::kTenantMigrationAbort);
    for (const auto& replState : builds) {
        if (!abortIndexBuildByBuildUUID(
                opCtx, replState->buildUUID, IndexBuildAction::kTenantMigrationAbort, reason)) {
            // The index build may already be in the midst of tearing down.
            LOGV2(4886204,
                  "Index build: failed to abort index build for tenant migration",
                  "buildUUID"_attr = replState->buildUUID,
                  logAttrs(replState->dbName),
                  "collectionUUID"_attr = replState->collectionUUID,
                  "buildAction"_attr = indexBuildActionStr);
            buildsWaitingToFinish.push_back(replState);
        }
    }
    for (const auto& replState : buildsWaitingToFinish) {
        LOGV2(6221600,
              "Waiting on the index build to unregister before continuing the tenant migration.",
              "buildUUID"_attr = replState->buildUUID,
              logAttrs(replState->dbName),
              "collectionUUID"_attr = replState->collectionUUID,
              "buildAction"_attr = indexBuildActionStr);
        awaitNoIndexBuildInProgressForCollection(
            opCtx, replState->collectionUUID, replState->protocol);
    }
}

void IndexBuildsCoordinator::abortTenantIndexBuilds(OperationContext* opCtx,
                                                    MigrationProtocolEnum protocol,
                                                    const boost::optional<TenantId>& tenantId,
                                                    const std::string& reason) {
    const auto tenantIdStr = tenantId ? tenantId->toString() : "";
    LOGV2(4886205,
          "About to abort all index builders running for collections belonging to the given tenant",
          "tenantId"_attr = tenantIdStr,
          "reason"_attr = reason);

    auto builds = [&]() -> std::vector<std::shared_ptr<ReplIndexBuildState>> {
        auto indexBuildFilter = [=](const auto& replState) {
            // Abort *all* index builds at the start of shard merge.
            return repl::ClonerUtils::isDatabaseForTenant(replState.dbName, tenantId, protocol);
        };
        return activeIndexBuilds.filterIndexBuilds(indexBuildFilter);
    }();

    _abortTenantIndexBuilds(opCtx, builds, protocol, reason);
}

void IndexBuildsCoordinator::abortAllIndexBuildsForInitialSync(OperationContext* opCtx,
                                                               const std::string& reason) {
    _abortAllIndexBuildsWithReason(opCtx, IndexBuildAction::kInitialSyncAbort, reason);
}

namespace {

// Interrupts the index builder thread and waits for it to clean up. Returns true if the index was
// aborted, and false if it was already committed or aborted.
bool forceSelfAbortIndexBuild(OperationContext* opCtx,
                              std::shared_ptr<ReplIndexBuildState>& replState,
                              Status reason) {
    if (!replState->forceSelfAbort(opCtx, reason)) {
        return false;
    }

    auto fut = replState->sharedPromise.getFuture();
    auto waitStatus = fut.waitNoThrow();              // Result from waiting on future.
    auto buildStatus = fut.getNoThrow().getStatus();  // Result from _runIndexBuildInner().
    LOGV2(7419401,
          "Index build: joined after forceful abort",
          "buildUUID"_attr = replState->buildUUID,
          "waitResult"_attr = waitStatus,
          "status"_attr = buildStatus);
    return true;
}

}  // namespace

void IndexBuildsCoordinator::abortAllIndexBuildsDueToDiskSpace(OperationContext* opCtx,
                                                               std::int64_t availableBytes,
                                                               std::int64_t requiredBytes) {
    auto builds = activeIndexBuilds.getAllIndexBuilds();
    auto abortStatus =
        Status(ErrorCodes::OutOfDiskSpace,
               fmt::format("available disk space of {} bytes is less than required minimum of {}",
                           availableBytes,
                           requiredBytes));
    for (auto&& replState : builds) {
        // Signals the index build to abort iself, which may involve signalling the current primary.
        if (forceSelfAbortIndexBuild(opCtx, replState, abortStatus)) {
            // Increase metrics only if the build was actually aborted by the above call.
            indexBuildsSSS.killedDueToInsufficientDiskSpace.addAndFetch(1);
            LOGV2(7333601,
                  "Index build: aborted due to insufficient disk space",
                  "buildUUID"_attr = replState->buildUUID);
        }
    }
}

void IndexBuildsCoordinator::abortUserIndexBuildsForUserWriteBlocking(OperationContext* opCtx) {
    LOGV2(6511600,
          "About to abort index builders running on user databases for user write blocking");

    auto builds = [&]() -> std::vector<std::shared_ptr<ReplIndexBuildState>> {
        auto indexBuildFilter = [](const auto& replState) {
            return !NamespaceString(replState.dbName).isOnInternalDb();
        };
        return activeIndexBuilds.filterIndexBuilds(indexBuildFilter);
    }();

    std::vector<std::shared_ptr<ReplIndexBuildState>> buildsWaitingToFinish;

    for (const auto& replState : builds) {
        if (!abortIndexBuildByBuildUUID(opCtx,
                                        replState->buildUUID,
                                        IndexBuildAction::kPrimaryAbort,
                                        "User write blocking")) {
            // If the index build is already finishing and thus can't be aborted, we must wait on
            // it.
            LOGV2(6511601,
                  "Index build: failed to abort index build for write blocking, will wait for "
                  "completion instead",
                  "buildUUID"_attr = replState->buildUUID,
                  logAttrs(replState->dbName),
                  "collectionUUID"_attr = replState->collectionUUID);
            buildsWaitingToFinish.push_back(replState);
        }
    }

    // Before returning, we must wait on all index builds which could not be aborted to finish.
    // Otherwise, index builds started before enabling user write block mode could commit after
    // enabling it.
    for (const auto& replState : buildsWaitingToFinish) {
        LOGV2(6511602,
              "Waiting on index build to finish for user write blocking",
              "buildUUID"_attr = replState->buildUUID,
              logAttrs(replState->dbName),
              "collectionUUID"_attr = replState->collectionUUID);
        awaitNoIndexBuildInProgressForCollection(
            opCtx, replState->collectionUUID, replState->protocol);
    }
}

namespace {
NamespaceString getNsFromUUID(OperationContext* opCtx, const UUID& uuid) {
    auto catalog = CollectionCatalog::get(opCtx);
    auto nss = catalog->lookupNSSByUUID(opCtx, uuid);
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
        auto dbAndUUID = NamespaceStringOrUUID(nss.dbName(), collUUID);
        writeConflictRetry(opCtx, "IndexBuildsCoordinator::applyStartIndexBuild", nss, [&] {
            WriteUnitOfWork wuow(opCtx);

            AutoGetCollection coll(opCtx, dbAndUUID, MODE_X);
            invariant(coll,
                      str::stream() << "Collection with UUID " << collUUID << " was dropped.");

            IndexCatalog* indexCatalog = coll.getWritableCollection(opCtx)->getIndexCatalog();

            for (const auto& spec : oplogEntry.indexSpecs) {
                std::string name =
                    spec.getStringField(IndexDescriptor::kIndexNameFieldName).toString();
                uassert(ErrorCodes::BadValue,
                        str::stream() << "Index spec is missing the 'name' field " << spec,
                        !name.empty());

                if (auto writableEntry = indexCatalog->getWritableEntryByName(
                        opCtx, name, IndexCatalog::InclusionPolicy::kReady)) {
                    uassertStatusOK(indexCatalog->dropIndexEntry(
                        opCtx, coll.getWritableCollection(opCtx), writableEntry));
                }

                auto writableEntry = indexCatalog->getWritableEntryByKeyPatternAndOptions(
                    opCtx,
                    spec.getObjectField(IndexDescriptor::kKeyPatternFieldName),
                    spec,
                    IndexCatalog::InclusionPolicy::kReady);
                if (writableEntry) {
                    uassertStatusOK(indexCatalog->dropIndexEntry(
                        opCtx, coll.getWritableCollection(opCtx), writableEntry));
                }
            }

            wuow.commit();
        });
    }

    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    uassertStatusOK(
        indexBuildsCoord
            ->startIndexBuild(opCtx,
                              nss.dbName(),
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

    // If this node's replica set config uses buildIndexes:false, then do not attempt to commit an
    // index that would have never been started.
    if (!repl::ReplicationCoordinator::get(opCtx)->buildsIndexes()) {
        return;
    }

    uassert(31417,
            str::stream()
                << "No commit timestamp set while applying commitIndexBuild operation. Build UUID: "
                << buildUUID,
            !shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp().isNull());

    // There is a possibility that we cannot find an active index build with the given build UUID.
    // This can be the case when:
    //   - The index already exists during initial sync.
    //   - The index was dropped on the sync source before the collection was cloned during initial
    //   sync.
    //   - A node is restarted with unfinished index builds and --recoverFromOplogAsStandalone.
    // The oplog code will ignore the NoSuchKey error code.
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
    //
    // Case 3: Node has unfinished index builds that are not restarted:
    // +--------------------------------+-------------------------------------------------+
    // |         Before Shutdown        |        After restart in standalone with         |
    // |                                |         --recoverFromOplogAsStandalone          |
    // +--------------------------------+-------------------------------------------------+
    // | startIndexBuild 'x' at TS: 1.  | Recovery at TS: 1.                              |
    // |                                | - Unfinished index build is not restarted.      |
    // | ***** Checkpoint taken *****   |                                                 |
    // |                                | Oplog replay operations starting with TS: 2.    |
    // | commitIndexBuild 'x' at TS: 2. | Apply commitIndexBuild 'x' oplog entry at TS: 2.|
    // |                                |                                                 |
    // |                                | ------------ Index build not found ------------ |
    // +--------------------------------+-------------------------------------------------+

    auto swReplState = _getIndexBuild(buildUUID);
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // Index builds are not restarted in standalone mode. If the node is started with
    // recoverFromOplogAsStandalone and when replaying the commitIndexBuild oplog entry for a paused
    // index, there is no active index build thread to commit.
    if (!swReplState.isOK() && replCoord->getSettings().shouldRecoverFromOplogAsStandalone()) {
        // Restart the 'paused' index build in the background.
        IndexBuilds buildsToRestart;
        IndexBuildDetails details{collUUID};
        for (const auto& spec : oplogEntry.indexSpecs) {
            details.indexSpecs.emplace_back(spec.getOwned());
        }
        buildsToRestart.insert({buildUUID, details});

        restartIndexBuildsForRecovery(opCtx, buildsToRestart, /*buildsToResume=*/{});

        // Get the builder.
        swReplState = _getIndexBuild(buildUUID);
    }
    auto replState = uassertStatusOK(swReplState);

    // Retry until we are able to put the index build in the kApplyCommitOplogEntry state. None of
    // the conditions for retrying are common or expected to be long-lived, so we believe this to be
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
    return replState->tryCommit(opCtx);
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
            !shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp().isNull());

    std::string abortReason(str::stream()
                            << "abortIndexBuild oplog entry encountered: " << *oplogEntry.cause);
    if (abortIndexBuildByBuildUUID(opCtx, buildUUID, IndexBuildAction::kOplogAbort, abortReason)) {
        return;
    }

    // The index build may already be in the midst of tearing down.
    LOGV2(5010504,
          "Index build: failed to abort index build while applying abortIndexBuild operation",
          "buildUUID"_attr = buildUUID,
          logAttrs(nss),
          "collectionUUID"_attr = collUUID,
          "cause"_attr = *oplogEntry.cause);

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().shouldRecoverFromOplogAsStandalone()) {
        // Unfinished index builds are not restarted in standalone mode. That means there will be no
        // index builder threads to abort. Instead, we should drop the unfinished indexes that were
        // aborted.
        AutoGetCollection autoColl{opCtx, nss, MODE_X};

        WriteUnitOfWork wuow(opCtx);

        auto indexCatalog = autoColl.getWritableCollection(opCtx)->getIndexCatalog();
        for (const auto& indexSpec : oplogEntry.indexSpecs) {
            auto writableEntry = indexCatalog->getWritableEntryByName(
                opCtx,
                indexSpec.getStringField(IndexDescriptor::kIndexNameFieldName),
                IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished |
                    IndexCatalog::InclusionPolicy::kFrozen);

            LOGV2(6455400,
                  "Dropping unfinished index during oplog recovery as standalone",
                  "spec"_attr = indexSpec);

            invariant(writableEntry && writableEntry->isFrozen());
            invariant(indexCatalog->dropUnfinishedIndex(
                opCtx, autoColl.getWritableCollection(opCtx), writableEntry));
        }

        wuow.commit();
        return;
    }
}

boost::optional<UUID> IndexBuildsCoordinator::abortIndexBuildByIndexNames(
    OperationContext* opCtx,
    const UUID& collectionUUID,
    const std::vector<std::string>& indexNames,
    std::string reason) {
    boost::optional<UUID> buildUUID;
    auto indexBuilds = activeIndexBuilds.getAllIndexBuilds();
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

void IndexBuildsCoordinator::_abortAllIndexBuildsWithReason(OperationContext* opCtx,
                                                            IndexBuildAction action,
                                                            const std::string& reason) {
    LOGV2(7738702,
          "About to abort all running index builders",
          "reason"_attr = reason,
          "action"_attr = indexBuildActionToString(action));

    auto builds = activeIndexBuilds.getAllIndexBuilds();
    for (const auto& replState : builds) {
        if (!abortIndexBuildByBuildUUID(opCtx, replState->buildUUID, action, reason)) {
            // The index build may already be in the midst of tearing down.
            LOGV2(7738703,
                  "Index build: failed to abort index build, this is expected if the build is "
                  "already being committed or in the process of tearing down.",
                  "buildUUID"_attr = replState->buildUUID,
                  "database"_attr = replState->dbName,
                  "collectionUUID"_attr = replState->collectionUUID);
        }
    }
}

void IndexBuildsCoordinator::abortAllIndexBuildsWithReason(OperationContext* opCtx,
                                                           const std::string& reason) {
    _abortAllIndexBuildsWithReason(opCtx, IndexBuildAction::kPrimaryAbort, reason);
}

void IndexBuildsCoordinator::setNewIndexBuildsBlocked(const bool newValue,
                                                      boost::optional<std::string> reason) {
    stdx::unique_lock<Latch> lk(_newIndexBuildsBlockedMutex);
    invariant(newValue != _newIndexBuildsBlocked);
    invariant((newValue && reason) || (!newValue && !reason));

    _newIndexBuildsBlocked = newValue;
    _blockReason = reason;

    if (!_newIndexBuildsBlocked) {
        _newIndexBuildsBlockedCV.notify_all();
    }
}

bool IndexBuildsCoordinator::hasIndexBuilder(OperationContext* opCtx,
                                             const UUID& collectionUUID,
                                             const std::vector<std::string>& indexNames) const {
    bool foundIndexBuilder = false;
    boost::optional<UUID> buildUUID;
    auto indexBuilds = activeIndexBuilds.getAllIndexBuilds();
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
                  "Ignoring error while aborting index build",
                  "buildUUID"_attr = buildUUID,
                  "error"_attr = replStateResult.getStatus());
            return false;
        }

        replState = replStateResult.getValue();
        LOGV2(4656010, "Attempting to abort index build", "buildUUID"_attr = replState->buildUUID);

        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);

        // Only on single phase builds, skip RSTL to avoid deadlocks with prepare conflicts and
        // state transitions caused by taking a strong collection lock. See SERVER-42621.
        const auto lockOptions =
            makeAutoGetCollectionOptions(IndexBuildProtocol::kSinglePhase == replState->protocol);
        AutoGetCollection autoGetColl(opCtx, dbAndUUID, MODE_X, lockOptions);
        AutoGetCollection indexBuildEntryColl(
            opCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);

        hangAbortIndexBuildByBuildUUIDAfterLocks.pauseWhileSet();

        // If we are using two-phase index builds and are no longer primary after receiving an
        // abort, we cannot replicate an abortIndexBuild oplog entry. Continue holding the RSTL to
        // check the replication state and to prevent any state transitions from happening while
        // aborting the index build. Once an index build is put into kAborted, the index builder
        // thread will be torn down, and an oplog entry must be replicated. Single-phase builds do
        // not have this restriction and may be aborted after a stepDown. Initial syncing nodes need
        // to be able to abort two phase index builds during the oplog replay phase.
        if (IndexBuildProtocol::kTwoPhase == replState->protocol) {
            // The AutoGetCollection helper takes the RSTL implicitly.
            invariant(shard_role_details::getLocker(opCtx)->isRSTLLocked());

            // Override the 'signalAction' as this is an initial syncing node.
            // Don't override it if it's a rollback abort which would be explictly requested
            // by the initial sync code.
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            if (replCoord->getMemberState().startup2() &&
                IndexBuildAction::kRollbackAbort != signalAction) {
                LOGV2_DEBUG(4665902,
                            1,
                            "Overriding abort 'signalAction' for initial sync",
                            "from"_attr = signalAction,
                            "to"_attr = IndexBuildAction::kInitialSyncAbort);
                signalAction = IndexBuildAction::kInitialSyncAbort;
            }

            if ((IndexBuildAction::kPrimaryAbort == signalAction ||
                 IndexBuildAction::kTenantMigrationAbort == signalAction) &&
                !replCoord->canAcceptWritesFor(opCtx, dbAndUUID)) {
                uassertStatusOK({ErrorCodes::NotWritablePrimary,
                                 str::stream()
                                     << "Unable to abort index build because we are not primary: "
                                     << buildUUID});
            }
        }

        auto tryAbortResult = replState->tryAbort(opCtx, signalAction, reason);
        switch (tryAbortResult) {
            case ReplIndexBuildState::TryAbortResult::kNotAborted:
                return false;
            case ReplIndexBuildState::TryAbortResult::kAlreadyAborted:
                return true;
            case ReplIndexBuildState::TryAbortResult::kRetry:
            case ReplIndexBuildState::TryAbortResult::kContinueAbort:
                break;
        }

        if (ReplIndexBuildState::TryAbortResult::kRetry == tryAbortResult) {
            retry = true;
            continue;
        }

        invariant(ReplIndexBuildState::TryAbortResult::kContinueAbort == tryAbortResult);

        if (MONGO_unlikely(hangBeforeCompletingAbort.shouldFail())) {
            LOGV2(4806200, "Hanging before completing index build abort");
            hangBeforeCompletingAbort.pauseWhileSet();
        }

        // At this point we must continue aborting the index build.
        _completeExternalAbort(opCtx, replState, *indexBuildEntryColl, signalAction);
        break;
    }

    return true;
}

void IndexBuildsCoordinator::_completeAbort(OperationContext* opCtx,
                                            std::shared_ptr<ReplIndexBuildState> replState,
                                            const CollectionPtr& indexBuildEntryCollection,
                                            IndexBuildAction signalAction) {
    if (!replState->isAbortCleanUpRequired()) {
        LOGV2(7329402,
              "Index build: abort cleanup not required",
              "action"_attr = indexBuildActionToString(signalAction),
              "buildUUID"_attr = replState->buildUUID,
              "collectionUUID"_attr = replState->collectionUUID);
        return;
    }

    // At this point, the target collection and config.system.indexBuilds locks are acquired, no
    // direct lock acquisitions are expected and there are no interrupt checkpoints. However,
    // OpObservers may introduce lock acquisitions (i.e. sharding state locks) and cause an
    // interruption during cleanup. For correctness, we must perform these final writes. Temporarily
    // disable interrupts.
    UninterruptibleLockGuard noInterrupt(shard_role_details::getLocker(opCtx));  // NOLINT.

    CollectionWriter coll(opCtx, replState->collectionUUID);
    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
    auto nss = coll->ns();
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    switch (signalAction) {
        // Replicates an abortIndexBuild oplog entry and deletes the index from the durable catalog.
        case IndexBuildAction::kTenantMigrationAbort:
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
            auto onCleanUpFn = [&] {
                onAbortIndexBuild(opCtx, coll->ns(), *replState);
            };
            _indexBuildsManager.abortIndexBuild(opCtx, coll, replState->buildUUID, onCleanUpFn);
            removeIndexBuildEntryAfterCommitOrAbort(
                opCtx, dbAndUUID, indexBuildEntryCollection, *replState);
            break;
        }
        // Deletes the index from the durable catalog.
        case IndexBuildAction::kInitialSyncAbort: {
            invariant(replState->protocol == IndexBuildProtocol::kTwoPhase);
            invariant(replCoord->getMemberState().startup2());

            bool isPrimary = replCoord->canAcceptWritesFor(opCtx, nss);
            invariant(!isPrimary, str::stream() << "Index build: " << replState->buildUUID);

            auto abortReason = replState->getAbortReason();
            LOGV2(4665903,
                  "Aborting index build during initial sync",
                  "buildUUID"_attr = replState->buildUUID,
                  "abortReason"_attr = abortReason,
                  "collectionUUID"_attr = replState->collectionUUID);
            _indexBuildsManager.abortIndexBuild(
                opCtx, coll, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
            break;
        }
        // Deletes the index from the durable catalog.
        case IndexBuildAction::kOplogAbort: {
            invariant(IndexBuildProtocol::kTwoPhase == replState->protocol);
            replState->onOplogAbort(opCtx, nss);
            _indexBuildsManager.abortIndexBuild(
                opCtx, coll, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
            break;
        }
        case IndexBuildAction::kRollbackAbort: {
            invariant(replState->protocol == IndexBuildProtocol::kTwoPhase);
            // File copy based initial sync does a rollback-like operation, so we allow STARTUP2
            // to abort as well as rollback.
            invariant(replCoord->getMemberState().rollback() ||
                      replCoord->getMemberState().startup2());
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

void IndexBuildsCoordinator::_completeExternalAbort(OperationContext* opCtx,
                                                    std::shared_ptr<ReplIndexBuildState> replState,
                                                    const CollectionPtr& indexBuildEntryColl,
                                                    IndexBuildAction signalAction) {

    const auto status = replState->getAbortStatus();
    try {
        _completeAbort(opCtx, replState, indexBuildEntryColl, signalAction);
    } catch (const DBException& e) {
        // In production code, we should not encounter any write conflict exceptions from
        // the abort logic. It is only through the use of a fail point that the internal write
        // conflict retry logic is disabled and we may get a WriteConflict here.
        // This index build is now in an inconsistent state and we should continue to crash the
        // server. But we will log a warning message to alert users of the CI system that this is
        // fine.
        if (e.code() == ErrorCodes::WriteConflict &&
            MONGO_unlikely(skipWriteConflictRetries.shouldFail()) &&
            opCtx->getClient()->isFromUserConnection()) {
            LOGV2_WARNING(
                7912300,
                "Failed to abort index build due to write conflict. This is only possible "
                "in a test environment with the  skipWriteConflictRetries fail point "
                "enabled. Index build is now in  an inconsistent and unrecoverable state. "
                "Proceeding to shut down server with a fatal assertion.",
                "buildUUID"_attr = replState->buildUUID);
        }

        LOGV2_FATAL(4656011,
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
          "buildUUID"_attr = replState->buildUUID,
          "waitResult"_attr = waitStatus,
          "status"_attr = buildStatus);

    if (IndexBuildAction::kRollbackAbort == signalAction) {
        // Index builds interrupted for rollback may be resumed during recovery. We wait for the
        // builder thread to complete before persisting the in-memory state that will be used
        // to resume the index build.
        // No locks are required when aborting due to rollback. This performs no storage engine
        // writes, only cleans up the remaining in-memory state.
        CollectionWriter coll(opCtx, replState->collectionUUID);
        _indexBuildsManager.abortIndexBuildWithoutCleanup(
            opCtx, coll.get(), replState->buildUUID, replState->isResumable());
    }

    replState->completeAbort(opCtx);
    activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager, replState);
}

void IndexBuildsCoordinator::_completeSelfAbort(OperationContext* opCtx,
                                                std::shared_ptr<ReplIndexBuildState> replState,
                                                const CollectionPtr& indexBuildEntryCollection) {
    _completeAbort(opCtx, replState, indexBuildEntryCollection, IndexBuildAction::kPrimaryAbort);
    replState->completeAbort(opCtx);

    activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager, replState);
}

void IndexBuildsCoordinator::_completeAbortForShutdown(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const CollectionPtr& collection) {
    // Leave it as-if kill -9 happened. Startup recovery will restart the index build.
    _indexBuildsManager.abortIndexBuildWithoutCleanup(
        opCtx, collection, replState->buildUUID, replState->isResumable());

    replState->abortForShutdown(opCtx);

    activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager, replState);
}

std::size_t IndexBuildsCoordinator::getActiveIndexBuildCount(OperationContext* opCtx) {
    auto indexBuilds = activeIndexBuilds.getAllIndexBuilds();
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

    if (_stepUpThread.joinable()) {
        // Under normal circumstances this should not result in a wait. The thread's opCtx should
        // be interrupted on replication state change, or finish while being primary. If this
        // results in a wait, it means the thread which started in the previous stepUp did not yet
        // exit. It should eventually exit.
        _stepUpThread.join();
    }

    PromiseAndFuture<void> promiseAndFuture;
    _stepUpThread = stdx::thread([this, &promiseAndFuture] {
        Client::initThread("IndexBuildsCoordinator-StepUp",
                           getGlobalServiceContext()->getService(ClusterRole::ShardServer));
        auto threadCtx = Client::getCurrent()->makeOperationContext();
        threadCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        promiseAndFuture.promise.emplaceValue();

        _onStepUpAsyncTaskFn(threadCtx.get());
        return;
    });

    // Wait until the async thread has started and marked its opCtx to always be interrupted at
    // step-down. We ensure the RSTL is taken and no interrupts are lost.
    promiseAndFuture.future.wait(opCtx);
}

void IndexBuildsCoordinator::_onStepUpAsyncTaskFn(OperationContext* opCtx) {
    auto indexBuilds = activeIndexBuilds.getAllIndexBuilds();
    const auto signalCommitQuorumAndRetrySkippedRecords =
        [this, opCtx](const std::shared_ptr<ReplIndexBuildState>& replState) {
            try {
                if (replState->protocol != IndexBuildProtocol::kTwoPhase) {
                    return;
                }

                // We don't need to check if we are primary because the opCtx is interrupted at
                // stepdown, so it is guaranteed that if taking the locks succeeds, we are primary.
                // Take an intent lock, the actual index build should keep running in parallel.
                // This also prevents the concurrent index build from aborting or committing
                // while we check if the commit quorum has to be signaled or check the skipped
                // records.
                const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
                AutoGetCollection autoColl(opCtx, dbAndUUID, MODE_IX);

                // The index build hasn't yet completed its initial setup, and persisted state like
                // commit quorum information is absent. There's nothing to do here.
                if (replState->isSettingUp()) {
                    return;
                }

                // The index build might have committed or aborted while looping and not holding the
                // collection lock. Re-checking if it is still active after taking locks would not
                // solve the issue, as build can still be registered as active, even if it is in an
                // aborted or committed state.
                if (replState->isAborting() || replState->isAborted() || replState->isCommitted()) {
                    return;
                }

                if (!_signalIfCommitQuorumNotEnabled(opCtx, replState)) {
                    // This reads from system.indexBuilds collection to see if commit quorum got
                    // satisfied.
                    try {
                        hangOnStepUpAsyncTaskBeforeCheckingCommitQuorum.pauseWhileSet();

                        if (_signalIfCommitQuorumIsSatisfied(opCtx, replState)) {
                            // The index build has been signalled to commit. As retrying skipped
                            // records during step-up is done to prevent waiting until commit time,
                            // if the build has already been signalled to commit, we may skip the
                            // retry during step-up.
                            return;
                        }
                    } catch (DBException& ex) {
                        // If the operation context is interrupted (shutdown, stepdown, killOp),
                        // stop the verification process and exit.
                        opCtx->checkForInterrupt();

                        fassert(31440, ex.toStatus());
                    }
                }

                // Unlike the primary, secondaries cannot fail immediately when detecting key
                // generation errors; they instead temporarily store them in the 'skipped records'
                // table, to validate them on commit. As an optimisation to potentially detect
                // errors earlier, check the table on step-up. Unlike during commit, we only check
                // key generation here, we do not actually insert the keys.
                uassertStatusOK(_indexBuildsManager.retrySkippedRecords(
                    opCtx,
                    replState->buildUUID,
                    autoColl.getCollection(),
                    IndexBuildsManager::RetrySkippedRecordMode::kKeyGeneration));

            } catch (const DBException& ex) {
                // If the operation context is interrupted (shutdown, stepdown, killOp), stop the
                // verification process and exit.
                opCtx->checkForInterrupt();

                // Some of the checks might have opened a snapshot. Abandon it before acquiring
                // MODE_X lock during abort.
                shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

                // All other errors must be due to key generation. Abort the build now, instead of
                // failing later during the commit phase retry.
                auto status = ex.toStatus().withContext("Skipped records retry failed on step-up");
                abortIndexBuildByBuildUUID(
                    opCtx, replState->buildUUID, IndexBuildAction::kPrimaryAbort, status.reason());
            }
        };

    try {
        forEachIndexBuild(indexBuilds,
                          "IndexBuildsCoordinator::_onStepUpAsyncTaskFn"_sd,
                          signalCommitQuorumAndRetrySkippedRecords);
    } catch (const DBException& ex) {
        LOGV2_DEBUG(7333100,
                    1,
                    "Step-up retry of skipped records for all index builds interrupted",
                    "exception"_attr = ex);
    }
    LOGV2(7508300, "Finished performing asynchronous step-up checks on index builds");
}

IndexBuilds IndexBuildsCoordinator::stopIndexBuildsForRollback(OperationContext* opCtx) {
    LOGV2(20658, "Stopping index builds before rollback");

    IndexBuilds buildsStopped;

    auto indexBuilds = activeIndexBuilds.getAllIndexBuilds();
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
        for (const auto& spec : replState->indexSpecs) {
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
    auto catalog = CollectionCatalog::get(opCtx);

    stdx::unordered_set<UUID, UUID::Hash> successfullyResumed;

    for (const auto& resumeInfo : buildsToResume) {
        auto buildUUID = resumeInfo.getBuildUUID();
        auto collUUID = resumeInfo.getCollectionUUID();

        boost::optional<NamespaceString> nss =
            catalog->lookupNSSByUUID(opCtx, resumeInfo.getCollectionUUID());
        invariant(nss);

        std::vector<BSONObj> indexSpecs;
        indexSpecs.reserve(resumeInfo.getIndexes().size());

        for (const auto& index : resumeInfo.getIndexes()) {
            indexSpecs.push_back(index.getSpec());
        }

        LOGV2(4841700,
              "Index build: resuming",
              "buildUUID"_attr = buildUUID,
              "collectionUUID"_attr = collUUID,
              logAttrs(nss.value()),
              "details"_attr = resumeInfo.toBSON());

        try {
            // This spawns a new thread and returns immediately. These index builds will resume and
            // wait for a commit or abort to be replicated.
            [[maybe_unused]] auto fut = uassertStatusOK(resumeIndexBuild(
                opCtx, nss->dbName(), collUUID, indexSpecs, buildUUID, resumeInfo));
            successfullyResumed.insert(buildUUID);
        } catch (const DBException& e) {
            LOGV2(4841701,
                  "Index build: failed to resume, restarting instead",
                  "buildUUID"_attr = buildUUID,
                  "collectionUUID"_attr = collUUID,
                  logAttrs(*nss),
                  "error"_attr = e);

            // Clean up the persisted Sorter data since resuming failed.
            for (const auto& index : resumeInfo.getIndexes()) {
                if (!index.getFileName()) {
                    continue;
                }

                LOGV2(5043100,
                      "Index build: removing resumable temp file",
                      "buildUUID"_attr = buildUUID,
                      "collectionUUID"_attr = collUUID,
                      logAttrs(*nss),
                      "file"_attr = index.getFileName());

                boost::system::error_code ec;
                boost::filesystem::remove(
                    storageGlobalParams.dbpath + "/_tmp/" + index.getFileName()->toString(), ec);

                if (ec) {
                    LOGV2(5043101,
                          "Index build: failed to remove resumable temp file",
                          "buildUUID"_attr = buildUUID,
                          "collectionUUID"_attr = collUUID,
                          logAttrs(*nss),
                          "file"_attr = index.getFileName(),
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

        boost::optional<NamespaceString> nss = catalog->lookupNSSByUUID(opCtx, build.collUUID);
        invariant(nss);

        LOGV2(20660,
              "Index build: restarting",
              "buildUUID"_attr = buildUUID,
              "collectionUUID"_attr = build.collUUID,
              logAttrs(nss.value()));
        IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions;
        // Indicate that the initialization should not generate oplog entries or timestamps for the
        // first catalog write, and that the original durable catalog entries should be dropped and
        // replaced.
        indexBuildOptions.applicationMode = ApplicationMode::kStartupRepair;
        // This spawns a new thread and returns immediately. These index builds will start and wait
        // for a commit or abort to be replicated.
        [[maybe_unused]] auto fut = uassertStatusOK(startIndexBuild(opCtx,
                                                                    nss->dbName(),
                                                                    build.collUUID,
                                                                    build.indexSpecs,
                                                                    buildUUID,
                                                                    IndexBuildProtocol::kTwoPhase,
                                                                    indexBuildOptions));
    }
}

bool IndexBuildsCoordinator::noIndexBuildInProgress() const {
    return activeIndexBuilds.getActiveIndexBuildsCount() == 0;
}

int IndexBuildsCoordinator::numInProgForDb(const DatabaseName& dbName) const {
    auto indexBuildFilter = [dbName](const auto& replState) {
        return dbName == replState.dbName;
    };
    auto dbIndexBuilds = activeIndexBuilds.filterIndexBuilds(indexBuildFilter);
    return int(dbIndexBuilds.size());
}

bool IndexBuildsCoordinator::inProgForCollection(const UUID& collectionUUID,
                                                 IndexBuildProtocol protocol) const {
    auto indexBuildFilter = [=](const auto& replState) {
        return collectionUUID == replState.collectionUUID && protocol == replState.protocol;
    };
    auto indexBuilds = activeIndexBuilds.filterIndexBuilds(indexBuildFilter);
    return !indexBuilds.empty();
}

bool IndexBuildsCoordinator::inProgForCollection(const UUID& collectionUUID) const {
    auto indexBuilds = activeIndexBuilds.filterIndexBuilds(
        [=](const auto& replState) { return collectionUUID == replState.collectionUUID; });
    return !indexBuilds.empty();
}

bool IndexBuildsCoordinator::inProgForDb(const DatabaseName& dbName) const {
    return numInProgForDb(dbName) > 0;
}

void IndexBuildsCoordinator::assertNoIndexBuildInProgress() const {
    activeIndexBuilds.assertNoIndexBuildInProgress();
}

void IndexBuildsCoordinator::assertNoIndexBuildInProgForCollection(
    const UUID& collectionUUID) const {
    boost::optional<UUID> firstIndexBuildUUID;
    auto indexBuilds = activeIndexBuilds.filterIndexBuilds([&](const auto& replState) {
        auto isIndexBuildForCollection = (collectionUUID == replState.collectionUUID);
        if (isIndexBuildForCollection && !firstIndexBuildUUID) {
            firstIndexBuildUUID = replState.buildUUID;
        };
        return isIndexBuildForCollection;
    });

    uassert(ErrorCodes::BackgroundOperationInProgressForNamespace,
            fmt::format("cannot perform operation: an index build is currently running for "
                        "collection with UUID: {}. Found index build: {}",
                        collectionUUID.toString(),
                        firstIndexBuildUUID->toString()),
            indexBuilds.empty());
}

void IndexBuildsCoordinator::assertNoBgOpInProgForDb(const DatabaseName& dbName) const {
    boost::optional<UUID> firstIndexBuildUUID;
    auto indexBuilds = activeIndexBuilds.filterIndexBuilds([&](const auto& replState) {
        auto isIndexBuildForCollection = (dbName == replState.dbName);
        if (isIndexBuildForCollection && !firstIndexBuildUUID) {
            firstIndexBuildUUID = replState.buildUUID;
        };
        return isIndexBuildForCollection;
    });

    uassert(ErrorCodes::BackgroundOperationInProgressForDatabase,
            fmt::format("cannot perform operation: an index build is currently running for "
                        "database {}. Found index build: {}",
                        dbName.toStringForErrorMsg(),
                        firstIndexBuildUUID->toString()),
            indexBuilds.empty());
}

void IndexBuildsCoordinator::awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                                      const UUID& collectionUUID,
                                                                      IndexBuildProtocol protocol) {
    activeIndexBuilds.awaitNoIndexBuildInProgressForCollection(opCtx, collectionUUID, protocol);
}

void IndexBuildsCoordinator::awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                                      const UUID& collectionUUID) {
    activeIndexBuilds.awaitNoIndexBuildInProgressForCollection(opCtx, collectionUUID);
}

void IndexBuildsCoordinator::awaitNoBgOpInProgForDb(OperationContext* opCtx,
                                                    const DatabaseName& dbName) {
    activeIndexBuilds.awaitNoBgOpInProgForDb(opCtx, dbName);
}

void IndexBuildsCoordinator::waitUntilAnIndexBuildFinishes(OperationContext* opCtx) {
    activeIndexBuilds.waitUntilAnIndexBuildFinishes(opCtx);
}

void IndexBuildsCoordinator::appendBuildInfo(const UUID& buildUUID, BSONObjBuilder* builder) const {
    _indexBuildsManager.appendBuildInfo(buildUUID, builder);
    activeIndexBuilds.appendBuildInfo(buildUUID, builder);
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
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_X),
              str::stream() << "IndexBuildsCoordinator::createIndexes: " << collectionUUID);

    auto buildUUID = UUID::gen();

    // Rest of this function can throw, so ensure the build cleanup occurs.
    ON_BLOCK_EXIT([&] { _indexBuildsManager.tearDownAndUnregisterIndexBuild(buildUUID); });

    try {
        auto onInitFn = MultiIndexBlock::makeTimestampedIndexOnInitFn(opCtx, collection.get());
        IndexBuildsManager::SetupOptions options;
        options.indexConstraints = indexConstraints;
        // As the caller has a MODE_X lock on the collection, we can safely assume they want to
        // build the index in the foreground instead of yielding during element insertion.
        options.method = IndexBuildMethod::kForeground;
        uassertStatusOK(_indexBuildsManager.setUpIndexBuild(
            opCtx, collection, {spec}, buildUUID, onInitFn, options));
    } catch (DBException& ex) {
        const auto& status = ex.toStatus();
        if (IndexBuildsCoordinator::isCreateIndexesErrorSafeToIgnore(status, indexConstraints)) {
            LOGV2_DEBUG(4718200,
                        1,
                        "Ignoring indexing error",
                        "error"_attr = redact(status),
                        logAttrs(nss),
                        "collectionUUID"_attr = collectionUUID,
                        "spec"_attr = spec);
            return;
        }
        throw;
    }

    ScopeGuard abortOnExit([&] {
        // A timestamped transaction is needed to perform a catalog write that removes the index
        // entry when aborting the single-phase index build for tenant migrations only.
        auto onCleanUpFn = MultiIndexBlock::makeTimestampedOnCleanUpFn(opCtx, collection.get());
        _indexBuildsManager.abortIndexBuild(opCtx, collection, buildUUID, onCleanUpFn);
    });
    uassertStatusOK(_indexBuildsManager.startBuildingIndex(opCtx, collection.get(), buildUUID));

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
    CollectionCatalog::get(opCtx)->invariantHasExclusiveAccessToCollection(opCtx, collection->ns());

    auto opObserver = opCtx->getServiceContext()->getOpObserver();

    auto indexCatalog = collection.getWritableCollection(opCtx)->getIndexCatalog();
    // Always run single phase index build for empty collection. And, will be coordinated using
    // createIndexes oplog entry.
    for (const auto& spec : specs) {
        if (spec.hasField(IndexDescriptor::kClusteredFieldName) &&
            spec.getBoolField(IndexDescriptor::kClusteredFieldName)) {
            // The index is already built implicitly.
            continue;
        }

        // Each index will be added to the mdb catalog using the preceding createIndexes
        // timestamp.
        opObserver->onCreateIndex(opCtx, nss, collectionUUID, spec, fromMigrate);
        uassertStatusOK(indexCatalog->createIndexOnEmptyCollection(
            opCtx, collection.getWritableCollection(opCtx), spec));
    }
}

void IndexBuildsCoordinator::sleepIndexBuilds_forTestOnly(bool sleep) {
    activeIndexBuilds.sleepIndexBuilds_forTestOnly(sleep);
}

void IndexBuildsCoordinator::verifyNoIndexBuilds_forTestOnly() const {
    activeIndexBuilds.verifyNoIndexBuilds_forTestOnly();
}

// static
void IndexBuildsCoordinator::updateCurOpOpDescription(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const std::vector<BSONObj>& indexSpecs,
                                                      boost::optional<BSONObj> curOpDesc) {
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
    builder.appendElementsUnique(curOpDesc ? curOpDesc.value() : curOp->opDescription());
    auto opDescObj = builder.obj();
    curOp->setLogicalOp_inlock(LogicalOp::opCommand);
    curOp->setOpDescription_inlock(opDescObj);
    curOp->setNS_inlock(nss);
    curOp->ensureStarted();
}

Status IndexBuildsCoordinator::_setUpIndexBuildForTwoPhaseRecovery(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const UUID& collectionUUID,
    const std::vector<BSONObj>& specs,
    const UUID& buildUUID) {
    NamespaceStringOrUUID nssOrUuid{dbName, collectionUUID};

    if (shard_role_details::getRecoveryUnit(opCtx)->isActive()) {
        // This function is shared by multiple callers. Some of which have opened a transaction to
        // perform reads. This function may make mixed-mode writes. Mixed-mode assertions can only
        // be suppressed when beginning a fresh transaction.
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    }
    // Don't use the AutoGet helpers because they require an open database, which may not be the
    // case when an index build is restarted during recovery.

    Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
    CollectionNamespaceOrUUIDLock collLock(opCtx, nssOrUuid, MODE_X);
    auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, collectionUUID);
    invariant(collection);
    const auto& nss = collection->ns();
    const auto protocol = IndexBuildProtocol::kTwoPhase;
    return _startIndexBuildForRecovery(opCtx, nss, specs, buildUUID, protocol);
}

void IndexBuildsCoordinator::_waitIfNewIndexBuildsBlocked(OperationContext* opCtx,
                                                          const UUID& collectionUUID,
                                                          const std::vector<BSONObj>& specs,
                                                          const UUID& buildUUID) {
    stdx::unique_lock<Latch> lk(_newIndexBuildsBlockedMutex);
    bool messageLogged = false;

    opCtx->waitForConditionOrInterrupt(_newIndexBuildsBlockedCV, lk, [&] {
        if (_newIndexBuildsBlocked && !messageLogged) {
            LOGV2(7738700,
                  "Index build: new index builds are blocked, waiting",
                  "reason"_attr = *_blockReason,
                  "indexSpecs"_attr = specs,
                  "buildUUID"_attr = buildUUID,
                  "collectionUUID"_attr = collectionUUID);
            messageLogged = true;
        }
        return !_newIndexBuildsBlocked;
    });
    if (messageLogged) {
        LOGV2(7738701,
              "Index build: new index builds unblocked, continuing",
              "indexSpecs"_attr = specs,
              "buildUUID"_attr = buildUUID,
              "collectionUUID"_attr = collectionUUID);
    }
}

StatusWith<AutoGetCollection> IndexBuildsCoordinator::_autoGetCollectionExclusiveWithTimeout(
    OperationContext* opCtx, ReplIndexBuildState* replState, bool retry) {
    const Milliseconds kStateTransitionBlockedMaxMs{10};
    boost::optional<logv2::SeveritySuppressor> logSeveritySuppressor;
    int retryCount = 0;
    while (true) {
        try {
            AutoGetCollection::Options autoGetCollOptions;
            autoGetCollOptions.deadline(Date_t::now() + kStateTransitionBlockedMaxMs);
            return AutoGetCollection(
                opCtx, {replState->dbName, replState->collectionUUID}, MODE_X, autoGetCollOptions);
        } catch (const ExceptionFor<ErrorCodes::LockTimeout>& ex) {
            if (!retry) {
                return ex.toStatus();
            }
            if (!logSeveritySuppressor) {
                logSeveritySuppressor.emplace(
                    Seconds{1}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(2));
            }
            ++retryCount;
            LOGV2_DEBUG(7866200,
                        (*logSeveritySuppressor)().toInt(),
                        "Index build: collection lock acquisition timeout, retrying",
                        "retries"_attr = retryCount);
        }
    }
}


StatusWith<boost::optional<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>>
IndexBuildsCoordinator::_filterSpecsAndRegisterBuild(OperationContext* opCtx,
                                                     const DatabaseName& dbName,
                                                     const UUID& collectionUUID,
                                                     const std::vector<BSONObj>& specs,
                                                     const UUID& buildUUID,
                                                     IndexBuildProtocol protocol) {

    // AutoGetCollection throws an exception if it is unable to look up the collection by UUID.
    NamespaceStringOrUUID nssOrUuid{dbName, collectionUUID};
    AutoGetCollection autoColl(opCtx, nssOrUuid, MODE_X);
    CollectionWriter collection(opCtx, autoColl);

    const auto& nss = collection.get()->ns();

    {
        // Disallow index builds on drop-pending namespaces (system.drop.*) if we are primary.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (replCoord->getSettings().isReplSet() &&
            replCoord->canAcceptWritesFor(opCtx, nssOrUuid)) {
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "drop-pending collection: " << nss.toStringForErrorMsg(),
                    !nss.isDropPendingNamespace());
        }

        // This check is for optimization purposes only as since this lock is released after this,
        // and is acquired again when we build the index in _setUpIndexBuild.
        auto scopedCss = CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss);
        scopedCss->checkShardVersionOrThrow(opCtx);
        scopedCss->getCollectionDescription(opCtx).throwIfReshardingInProgress(nss);
    }

    std::vector<BSONObj> filteredSpecs;
    try {
        filteredSpecs = prepareSpecListForCreate(opCtx, collection.get(), nss, specs);
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
            writeConflictRetry(
                opCtx, "IndexBuildsCoordinator::_filterSpecsAndRegisterBuild", nss, [&] {
                    WriteUnitOfWork wuow(opCtx);
                    createIndexesOnEmptyCollection(opCtx, collection, filteredSpecs, false);
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
        buildUUID, collectionUUID, dbName, filteredSpecs, protocol);
    replIndexBuildState->stats.numIndexesBefore = getNumIndexesTotal(opCtx, collection.get());

    auto status = activeIndexBuilds.registerIndexBuild(replIndexBuildState);
    if (!status.isOK()) {
        return status;
    }
    indexBuildsSSS.registered.addAndFetch(1);

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

    hangIndexBuildOnSetupBeforeTakingLocks.pauseWhileSet(opCtx);

    auto autoGetColl =
        std::move(_autoGetCollectionExclusiveWithTimeout(opCtx, replState.get()).getValue());

    CollectionWriter collection(opCtx, replState->collectionUUID);

    const auto& nss = collection.get()->ns();

    CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)
        ->checkShardVersionOrThrow(opCtx);

    // We will not have a start timestamp if we are newly a secondary (i.e. we started as
    // primary but there was a stepdown). We will be unable to timestamp the initial catalog write,
    // so we must fail the index build. During initial sync, there is no commit timestamp set.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->canAcceptWritesFor(opCtx, nss) &&
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
            if (!(replCoord->getSettings().isReplSet() &&
                  replCoord->canAcceptWritesFor(opCtx, nss))) {
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
                                            indexBuildOptions.commitQuorum.value(),
                                            replState->indexNames);

            try {
                uassertStatusOK(indexbuildentryhelpers::addIndexBuildEntry(opCtx, indexBuildEntry));
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& e) {
                // If config.system.indexBuilds is not found, convert the NamespaceNotFound
                // exception to an anonymous error code. This is to distinguish from
                // a NamespaceNotFound exception on the user collection, which callers sometimes
                // interpret as not being an error condition.
                uasserted(6325700, e.reason());
            }

            opCtx->getServiceContext()->getOpObserver()->onStartIndexBuild(
                opCtx,
                nss,
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
        repl::ReplicationCoordinator::get(opCtx)->shouldRelaxIndexConstraints(opCtx, nss)
        ? IndexBuildsManager::IndexConstraints::kRelax
        : IndexBuildsManager::IndexConstraints::kEnforce;
    options.protocol = replState->protocol;

    try {
        if (replCoord->canAcceptWritesFor(opCtx, collection->ns()) &&
            !replCoord->getSettings().shouldRecoverFromOplogAsStandalone()) {
            // On standalones and primaries, call setUpIndexBuild(), which makes the initial catalog
            // write. On primaries, this replicates the startIndexBuild oplog entry. The start
            // timestamp is only set during oplog application.
            invariant(startTimestamp.isNull(), startTimestamp.toString());
            uassertStatusOK(_indexBuildsManager.setUpIndexBuild(
                opCtx, collection, replState->indexSpecs, replState->buildUUID, onInitFn, options));
        } else {
            // If we are starting the index build as a secondary, we must suppress calls to write
            // our initial oplog entry in setUpIndexBuild().
            repl::UnreplicatedWritesBlock uwb(opCtx);

            boost::optional<TimestampBlock> tsBlock;
            if (!startTimestamp.isNull()) {
                // Use the provided timestamp to write the initial catalog entry. This is also the
                // case when recovering from the oplog as a standalone. In general, if a timestamp
                // is provided, it should be used to avoid untimestamped writes.
                tsBlock.emplace(opCtx, startTimestamp);
            }

            uassertStatusOK(_indexBuildsManager.setUpIndexBuild(
                opCtx, collection, replState->indexSpecs, replState->buildUUID, onInitFn, options));
        }
    } catch (DBException& ex) {
        _indexBuildsManager.abortIndexBuild(
            opCtx, collection, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);

        const auto& status = ex.toStatus();
        if (IndexBuildsCoordinator::isCreateIndexesErrorSafeToIgnore(status,
                                                                     options.indexConstraints)) {
            LOGV2_DEBUG(20662, 1, "Ignoring indexing error", "error"_attr = redact(status));
            return PostSetupAction::kCompleteIndexBuildEarly;
        }

        throw;
    }

    // Mark the index build setup as complete, from now on cleanup is required on failure/abort.
    // _setUpIndexBuildInner must not throw after this point, or risk secondaries getting stuck
    // applying the 'startIndexBuild' oplog entry, because throwing here would cause the node to
    // vote for abort and subsequently await the 'abortIndexBuild' entry before fulfilling the start
    // promise, while the oplog applier is waiting for the start promise.
    replState->completeSetup();

    // Failing to establish lastOpTime before interceptors is not fatal, the index build will
    // continue as non-resumable. The build can continue as non-resumable even if this step
    // succeeds, if it timeouts during the wait for majority read concern on the timestamp
    // established here.
    try {
        if (isIndexBuildResumable(opCtx, *replState, indexBuildOptions)) {
            // We should only set this value if this is a hybrid index build.
            invariant(_indexBuildsManager.isBackgroundBuilding(replState->buildUUID));

            // After the interceptors are set, get the latest optime in the oplog that could have
            // contained a write to this collection. We need to be holding the collection lock in X
            // mode so that we ensure that there are not any uncommitted transactions on this
            // collection.
            replState->setLastOpTimeBeforeInterceptors(getLatestOplogOpTime(opCtx));
        }
    } catch (DBException& ex) {
        // It is fine to let the build continue even if we are interrupted, interrupt check before
        // actually starting the build will trigger the abort, after having signalled the start
        // promise.
        LOGV2(7484300,
              "Index build: failed to setup index build resumability, will continue as "
              "non-resumable.",
              "buildUUID"_attr = replState->buildUUID,
              logAttrs(replState->dbName),
              "collectionUUID"_attr = replState->collectionUUID,
              "reason"_attr = ex.toStatus());
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
        // After this point, concurrent aborts are not allowed, with the exception of a loopback
        // voteAbortIndexBuild.
        replState->setPostFailureState(ex.toStatus());
        // Hold reference to the catalog for collection lookup without locks to be safe.
        auto catalog = CollectionCatalog::get(opCtx);
        CollectionPtr collection(catalog->lookupCollectionByUUID(opCtx, replState->collectionUUID));
        invariant(collection,
                  str::stream() << "Collection with UUID " << replState->collectionUUID
                                << " should exist because an index build is in progress: "
                                << replState->buildUUID);
        _cleanUpAfterFailure(opCtx, collection, replState, indexBuildOptions);

        // Setup is done within the index builder thread, signal to any waiters that an error
        // occurred.
        replState->sharedPromise.setError(replState->getAbortStatus());
        return replState->getAbortStatus();
    }

    // The indexes are in the durable catalog in an unfinished state. Return an OK status so
    // that the caller can continue building the indexes by calling _runIndexBuild().
    if (PostSetupAction::kContinueIndexBuild == postSetupAction) {
        return Status::OK();
    }

    // Unregister the index build before setting the promise, so callers do not see the build again.
    activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager, replState);

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
    activeIndexBuilds.sleepIfNecessary_forTestOnly();

    // If the index build does not exist, do not continue building the index. This may happen if an
    // ignorable indexing error occurred during setup. The promise will have been fulfilled, but the
    // build has already been unregistered.
    auto swReplState = _getIndexBuild(buildUUID);
    if (swReplState.getStatus() == ErrorCodes::NoSuchKey) {
        return;
    }
    auto replState = invariant(swReplState);

    // Add build UUID to lock manager diagnostic output.
    auto locker = shard_role_details::getLocker(opCtx);
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
        hangBeforeUnregisteringAfterCommit.pauseWhileSet();
        // Unregister first so that when we fulfill the future, the build is not observed as active.
        activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager, replState);
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
    auto newClient =
        opCtx->getServiceContext()->getService(ClusterRole::ShardServer)->makeClient(name);

    // TODO(SERVER-74657): Please revisit if this thread could be made killable.
    {
        stdx::lock_guard<Client> lk(*newClient.get());
        newClient.get()->setSystemOperationUnkillableByStepdown(lk);
    }

    AlternativeClientRegion acr(newClient);
    const auto newCtx = cc().makeOperationContext();
    func(newCtx.get());
}
}  // namespace

void IndexBuildsCoordinator::_cleanUpAfterFailure(OperationContext* opCtx,
                                                  const CollectionPtr& collection,
                                                  std::shared_ptr<ReplIndexBuildState> replState,
                                                  const IndexBuildOptions& indexBuildOptions) {

    const auto status = replState->getAbortStatus();

    if (!replState->isAbortCleanUpRequired()) {
        // The index build aborted at an early stage before the 'startIndexBuild' oplog entry is
        // replicated: members replicating from this sync source are not aware of this index
        // build, nor has any build state been persisted locally. Unregister the index build
        // locally. In two phase index builds, any conditions causing secondaries to fail setting up
        // an index build (which must have succeeded in the primary) are assumed to eventually cause
        // the node to crash, so we do not attempt to verify this is a primary.
        LOGV2(7564400,
              "Index build: unregistering without cleanup",
              "buildUUD"_attr = replState->buildUUID,
              "error"_attr = status);
        activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager, replState);
        return;
    }

    if (!status.isA<ErrorCategory::ShutdownError>()) {
        try {
            // It is still possible to get a shutdown request while trying to clean-up. All shutdown
            // errors must be handled, or risk blocking shutdown due to the index build coordinator
            // waiting on index builds to finish because the index build state has not been updated
            // properly.

            if (status.code() == ErrorCodes::DataCorruptionDetected) {
                indexBuildsSSS.failedDueToDataCorruption.addAndFetch(1);
                LOGV2(7333600,
                      "Index build: data corruption detected",
                      "buildUUID"_attr = replState->buildUUID,
                      logAttrs(replState->dbName),
                      "collectionUUID"_attr = replState->collectionUUID,
                      "error"_attr = status);
            }

            if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
                _cleanUpSinglePhaseAfterNonShutdownFailure(
                    opCtx, collection, replState, indexBuildOptions);
            } else {
                _cleanUpTwoPhaseAfterNonShutdownFailure(
                    opCtx, collection, replState, indexBuildOptions);
            }
            return;
        } catch (const DBException& ex) {
            if (!ex.isA<ErrorCategory::ShutdownError>()) {
                // The only expected errors are shutdown errors.
                fassert(7329405, ex.toStatus());
            }
        }
    }

    _completeAbortForShutdown(opCtx, replState, collection);
    return;
}

void IndexBuildsCoordinator::_cleanUpSinglePhaseAfterNonShutdownFailure(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions) {

    invariant(replState->isAbortCleanUpRequired());

    // The index builder thread can abort on its own if it is interrupted by a user killop. This
    // would prevent us from taking locks. Use a new OperationContext to abort the index build.
    runOnAlternateContext(opCtx, "self-abort", [this, replState](OperationContext* abortCtx) {
        auto autoGetColl =
            std::move(_autoGetCollectionExclusiveWithTimeout(abortCtx, replState.get()).getValue());
        AutoGetCollection indexBuildEntryColl(
            abortCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);
        _completeSelfAbort(abortCtx, replState, *indexBuildEntryColl);
    });
}

void IndexBuildsCoordinator::_cleanUpTwoPhaseAfterNonShutdownFailure(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions) {

    invariant(replState->isAbortCleanUpRequired());
    const auto status = replState->getAbortStatus();

    // Use a new OperationContext to abort the index build since our current opCtx may be
    // interrupted. This is still susceptible to shutdown interrupts, but in that case, on server
    // restart the index build will also be restarted. This is also susceptible to user killops, but
    // in that case, we will let the error escape and the server will crash.
    runOnAlternateContext(
        opCtx, "self-abort", [this, replState, status](OperationContext* abortCtx) {
            // The index builder thread will need to reach out to the current primary to abort on
            // its own. This can happen if an error is thrown, it is interrupted by a user killop,
            // or is killed internally by something like the DiskSpaceMonitor. Voting for abort is
            // only allowed if the node did not previously attempt to vote for commit.

            // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
            if (feature_flags::gIndexBuildGracefulErrorHandling.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
                replState->canVoteForAbort()) {
                // Always request an abort to the primary node, even if we are primary. If
                // primary, the signal will loop back and cause an asynchronous external
                // index build abort.
                _signalPrimaryForAbortAndWaitForExternalAbort(abortCtx, replState.get());

                // The abort, and state clean-up, is done externally by the async
                // 'voteAbortIndexBuild' command if the node is primary itself, or by the
                // 'indexBuildAbort' oplog entry application thread on secondaries. We'll re-throw
                // our error without doing anything else, as the index build is already cleaned
                // up, and the server will terminate otherwise.
            } else {
                // Take RSTL to observe and prevent replication state from changing.
                auto autoGetcoll = std::move(
                    _autoGetCollectionExclusiveWithTimeout(abortCtx, replState.get()).getValue());

                const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
                auto replCoord = repl::ReplicationCoordinator::get(abortCtx);
                if (!replCoord->canAcceptWritesFor(abortCtx, dbAndUUID)) {
                    // Index builds may not fail on secondaries. If a primary replicated an
                    // abortIndexBuild oplog entry, then this index build would have been externally
                    // aborted.
                    fassert(51101,
                            status.withContext(str::stream()
                                               << "Index build: " << replState->buildUUID
                                               << "; Database: "
                                               << replState->dbName.toStringForErrorMsg()));
                }

                AutoGetCollection indexBuildEntryColl(
                    abortCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);
                _completeSelfAbort(abortCtx, replState, *indexBuildEntryColl);
            }
        });
}

void IndexBuildsCoordinator::_runIndexBuildInner(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    const boost::optional<ResumeIndexInfo>& resumeInfo) {
    try {
        // Try to set index build state to in-progress, if it has been aborted or interrupted the
        // attempt will fail.
        replState->setInProgress(opCtx);

        hangAfterInitializingIndexBuild.pauseWhileSet(opCtx);
        failIndexBuildWithError.executeIf(
            [](const BSONObj& data) {
                uasserted(data["error"].safeNumberInt(),
                          "failIndexBuildWithError failpoint triggered");
            },
            [&](const BSONObj& data) {
                return UUID::parse(data["buildUUID"]) == replState->buildUUID;
            });

        // Index builds can safely ignore prepare conflicts and perform writes. On secondaries,
        // prepare operations wait for index builds to complete.
        shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(
            PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

        if (resumeInfo) {
            _resumeIndexBuildFromPhase(opCtx, replState, indexBuildOptions, resumeInfo.value());
        } else {
            _buildIndex(opCtx, replState, indexBuildOptions);
        }

    } catch (const DBException& ex) {
        // After this point, concurrent aborts are not allowed, with the exception of a loopback
        // voteAbortIndexBuild. External aborters will retry until the build is actually aborted by
        // the builder, or until the builder goes into kAwaitPrimaryAbort state, in which case an
        // external abort is allowed.

        // Merge exception status with replication index build state status. When there was an
        // external abort, the index build state already contains the abort reason as specified by
        // the external aborter and this call does not override the status. In that case, the fact
        // that this opCtx was interrupted (due to killOp) is irrelevant, as it is the means by
        // which the builder is stopped, not the actual root cause. This returns a meaningful error
        // message to the createIndexes caller in case of an external abort, e.g. a secondary voting
        // to abort the index build. Not doing so would return a generic, not too helpful "operation
        // was interrupted" error message, because the 'voteAbortIndexBuild' command kills the index
        // build's operation context.
        replState->setPostFailureState(ex.toStatus());
    }

    const auto status = replState->getAbortStatus();
    // No abort detected, index build returned normally.
    if (status.isOK()) {
        return;
    }

    if (replState->isExternalAbort()) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        auto& collector = ResourceConsumption::MetricsCollector::get(opCtx);

        // Only report metrics for index builds on primaries. We are being aborted by an external
        // thread, thus we can assume it is holding the RSTL while waiting for us to exit.
        bool wasCollecting = collector.endScopedCollecting();
        bool isPrimary = replCoord->canAcceptWritesFor_UNSAFE(
            opCtx, {replState->dbName, replState->collectionUUID});
        if (isPrimary && wasCollecting && ResourceConsumption::isMetricsAggregationEnabled()) {
            ResourceConsumption::get(opCtx).merge(
                opCtx, collector.getDbName(), collector.getMetrics());
        }
    }

    // If the index build has already been cleaned-up because it encountered an error, there is no
    // work to do. If feature flag IndexBuildGracefulErrorHandling is not enabled, the most routine
    // case is for this to be due to a self-abort caused by constraint checking during the commit
    // phase. If an external abort was requested, cleanup is handled by the requester, and there is
    // nothing to do.
    if (replState->isAborted() || replState->isExternalAbort()) {
        uassertStatusOK(status);
    }

    invariant(replState->isFailureCleanUp());

    // We do not hold a collection lock here, but we are protected against the collection being
    // dropped while the index build is still registered for the collection -- until abortIndexBuild
    // is called. The collection can be renamed, but it is OK for the name to be stale just for
    // logging purposes.
    auto catalog = CollectionCatalog::get(opCtx);
    CollectionPtr collection(catalog->lookupCollectionByUUID(opCtx, replState->collectionUUID));
    invariant(collection,
              str::stream() << "Collection with UUID " << replState->collectionUUID
                            << " should exist because an index build is in progress: "
                            << replState->buildUUID);
    NamespaceString nss = collection->ns();
    logFailure(status, nss, replState);

    if (MONGO_unlikely(hangIndexBuildBeforeAbortCleanUp.shouldFail())) {
        LOGV2(4753601, "Hanging due to hangIndexBuildBeforeAbortCleanUp fail point");
        hangIndexBuildBeforeAbortCleanUp.pauseWhileSet();
    }

    // If IndexBuildGracefulErrorHandling is not enabled, crash on unexpected build errors. When the
    // feature flag is enabled, two-phase builds can handle unexpected errors by requesting an abort
    // to the primary node. Single-phase builds can also abort immediately, as the primary or
    // standalone is the only node aware of the build.
    if (!feature_flags::gIndexBuildGracefulErrorHandling.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // Index builds only check index constraints when committing. If an error occurs at that
        // point, then the build is cleaned up while still holding the appropriate locks. The only
        // errors that we cannot anticipate are user interrupts and shutdown errors.
        if (status == ErrorCodes::OutOfDiskSpace) {
            LOGV2_ERROR(5642401,
                        "Index build unable to proceed due to insufficient disk space",
                        "error"_attr = status);
            fassertFailedNoTrace(5642402);
        }

        // WARNING: Do not add new exemptions to this assertion! If this assertion is failing, an
        // exception escaped during this index build. The solution should not be to add an exemption
        // for that exception. We should instead address the problem by preventing that exception
        // from being thrown in the first place.
        //
        // Simultaneous index builds are not resilient to arbitrary exceptions being thrown.
        // Secondaries will only abort when the primary replicates an abortIndexBuild oplog entry,
        // and primaries should only abort when they can guarantee the node will not step down.
        //
        // At this point, an exception was thrown, we released our locks, and our index build state
        // is not resumable. If we were primary when the exception was thrown, we are no longer
        // guaranteed to be primary at this point. If we were never primary or are no longer
        // primary, we will fatally assert. If we are still primary, we can hope to quickly
        // re-acquire our locks and abort the index build without issue. We will always fatally
        // assert in debug builds.
        //
        // Solutions to fixing this failing assertion may include:
        // * Suppress the errors during the index build and re-check the assertions that lead to the
        //   error at commit time once we have acquired all of the appropriate locks in
        //   _insertKeysFromSideTablesAndCommit().
        // * Explicitly abort the index build with abortIndexBuildByBuildUUID() before performing an
        //   operation that causes the index build to throw an error.
        if (opCtx->checkForInterruptNoAssert().isOK()) {
            if (TestingProctor::instance().isEnabled()) {
                LOGV2_FATAL(6967700,
                            "Unexpected error code during index build cleanup",
                            "error"_attr = status);
            } else {
                // Note: Even if we don't fatally assert, if the node has stepped-down from being
                // primary, then we will still crash shortly after this. As a secondary, index
                // builds must succeed, and if we are in this path, the index build failed without
                // being explicitly aborted by the primary. Only if we're lucky enough to still be
                // primary will we abort the index build without any nodes crashing.
                LOGV2_WARNING(6967701,
                              "Unexpected error code during index build cleanup",
                              "error"_attr = status);
            }
        }
    }

    _cleanUpAfterFailure(opCtx, collection, replState, indexBuildOptions);

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
        boost::optional<RecordId> resumeAfterRecordId;
        if (resumeInfo.getCollectionScanPosition()) {
            resumeAfterRecordId = *resumeInfo.getCollectionScanPosition();
        }

        _scanCollectionAndInsertSortedKeysIntoIndex(opCtx, replState, resumeAfterRecordId);
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

    // The index build is not resumable if the node is in initial sync while building the index.
    if (!replState->isResumable()) {
        return;
    }

    auto timeoutMillis = gResumableIndexBuildMajorityOpTimeTimeoutMillis;
    if (timeoutMillis == 0) {
        // Disable resumable index build.
        replState->clearLastOpTimeBeforeInterceptors();
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

    auto lastOpTimeBeforeInterceptors = replState->getLastOpTimeBeforeInterceptors();
    LOGV2(4847600,
          "Index build: waiting for last optime before interceptors to be majority committed",
          "buildUUID"_attr = replState->buildUUID,
          "collectionUUID"_attr = replState->collectionUUID,
          "deadline"_attr = deadline,
          "timeout"_attr = timeout,
          "lastOpTime"_attr = lastOpTimeBeforeInterceptors);

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

    auto status = replCoord->waitUntilMajorityOpTime(opCtx, lastOpTimeBeforeInterceptors, deadline);
    if (!status.isOK()) {
        replState->clearLastOpTimeBeforeInterceptors();
        LOGV2(5053900,
              "Index build: timed out waiting for the last optime before interceptors to be "
              "majority committed, continuing as a non-resumable index build",
              "buildUUID"_attr = replState->buildUUID,
              "collectionUUID"_attr = replState->collectionUUID,
              "deadline"_attr = deadline,
              "timeout"_attr = timeout,
              "lastOpTime"_attr = lastOpTimeBeforeInterceptors,
              "waitStatus"_attr = status);
        return;
    }

    // Since we waited for all the writes before the interceptors were established to be majority
    // committed, if we read at the majority commit point for the collection scan, then none of the
    // documents put into the sorter can be rolled back.
    shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
        RecoveryUnit::ReadSource::kMajorityCommitted);
}

void IndexBuildsCoordinator::_buildIndex(OperationContext* opCtx,
                                         std::shared_ptr<ReplIndexBuildState> replState,
                                         const IndexBuildOptions& indexBuildOptions) {

    auto failPointHang = [buildUUID = replState->buildUUID](FailPoint* fp) {
        if (MONGO_unlikely(fp->shouldFail())) {
            LOGV2(4940900, "Hanging before building index", "buildUUID"_attr = buildUUID);
            fp->pauseWhileSet();
        }
    };
    failPointHang(&hangBeforeBuildingIndex);
    failPointHang(&hangBeforeBuildingIndexSecond);

    // Read without a timestamp. When we commit, we block writes which guarantees all writes are
    // visible.
    invariant(RecoveryUnit::ReadSource::kNoTimestamp ==
              shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource());
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
    const boost::optional<RecordId>& resumeAfterRecordId) {
    // Collection scan and insert into index.

    // The collection scan phase of an index build is marked as low priority in order to reduce
    // impact on user operations. Other steps of the index builds such as the draining phase have
    // normal priority because index builds are required to eventually catch-up with concurrent
    // writers. Otherwise we risk never finishing the index build.
    ScopedAdmissionPriority priority(opCtx, AdmissionContext::Priority::kLow);
    {
        indexBuildsSSS.scanCollection.addAndFetch(1);

        ScopeGuard scopeGuard([&] {
            shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kNoTimestamp);
        });

        // Wait for the last optime before the interceptors are established to be majority committed
        // while we aren't holding any locks. This will set the read source to be kMajorityCommitted
        // if it waited.
        _awaitLastOpTimeBeforeInterceptorsMajorityCommitted(opCtx, replState);

        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
        AutoGetCollection autoGetColl(opCtx, dbAndUUID, MODE_IX);

        auto collection = _setUpForScanCollectionAndInsertSortedKeysIntoIndex(opCtx, replState);

        uassertStatusOK(_indexBuildsManager.startBuildingIndex(
            opCtx, collection, replState->buildUUID, resumeAfterRecordId));

        if (MONGO_unlikely(hangAfterIndexBuildDumpsInsertsFromBulkLock.shouldFail())) {
            LOGV2(7490902,
                  "Hanging while locking on failpoint hangAfterIndexBuildDumpsInsertsFromBulkLock");
            hangAfterIndexBuildDumpsInsertsFromBulkLock.pauseWhileSet();
        }
    }

    if (MONGO_unlikely(hangAfterIndexBuildDumpsInsertsFromBulk.shouldFail())) {
        LOGV2(20665, "Hanging after dumping inserts from bulk builder");
        hangAfterIndexBuildDumpsInsertsFromBulk.pauseWhileSet();
    }
}

void IndexBuildsCoordinator::_insertSortedKeysIntoIndexForResume(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    // The collection scan phase of an index build is marked as low priority in order to reduce
    // impact on user operations. Other steps of the index builds such as the draining phase have
    // normal priority because index builds are required to eventually catch-up with concurrent
    // writers. Otherwise we risk never finishing the index build.
    ScopedAdmissionPriority priority(opCtx, AdmissionContext::Priority::kLow);
    {
        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
        AutoGetCollection collLock(opCtx, dbAndUUID, MODE_IX);

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

    CollectionPtr collection(
        CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, replState->collectionUUID));
    invariant(collection);
    collection.makeYieldable(opCtx, LockedCollectionYieldRestore(opCtx, collection));
    return collection;
}

/*
 * Third phase is catching up on all the writes that occurred during the first two phases.
 */
void IndexBuildsCoordinator::_insertKeysFromSideTablesWithoutBlockingWrites(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    indexBuildsSSS.drainSideWritesTable.addAndFetch(1);

    // Perform the first drain while holding an intent lock.
    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
    {
        AutoGetCollection autoGetColl(opCtx, dbAndUUID, MODE_IX);

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
    indexBuildsSSS.drainSideWritesTablePreCommit.addAndFetch(1);
    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);

    failIndexBuildWithErrorInSecondDrain.executeIf(
        [](const BSONObj& data) {
            uasserted(data["error"].safeNumberInt(),
                      "failIndexBuildWithErrorInSecondDrain failpoint triggered");
        },
        [&](const BSONObj& data) {
            return UUID::parse(data["buildUUID"]) == replState->buildUUID;
        });

    // Perform the second drain while stopping writes on the collection.
    {
        // Skip RSTL to avoid deadlocks with prepare conflicts and state transitions. See
        // SERVER-42621.
        const auto kAutoGetCollectionOptionsWithSkipRSTL =
            makeAutoGetCollectionOptions(/*skipRSTL=*/true);
        AutoGetCollection autoGetColl(
            opCtx, dbAndUUID, MODE_S, kAutoGetCollectionOptionsWithSkipRSTL);

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

    // Need to return the collection lock back to exclusive mode to complete the index build.
    auto locksOrStatus =
        _autoGetCollectionExclusiveWithTimeout(opCtx, replState.get(), /*retry=*/false);
    if (!locksOrStatus.isOK()) {
        return CommitResult::kLockTimeout;
    }

    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    AutoGetCollection indexBuildEntryColl(
        opCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);

    // If we are no longer primary after receiving a commit quorum, we must restart and wait for a
    // new signal from a new primary because we cannot commit. Note that two-phase index builds can
    // retry because a new signal should be received. Single-phase builds will be unable to commit
    // and will self-abort.
    bool isPrimary = replCoord->canAcceptWritesFor(opCtx, dbAndUUID) &&
        !replCoord->getSettings().shouldRecoverFromOplogAsStandalone();
    if (!isPrimary && IndexBuildAction::kCommitQuorumSatisfied == action) {
        return CommitResult::kNoLongerPrimary;
    }

    if (IndexBuildAction::kOplogCommit == action) {
        replState->onOplogCommit(isPrimary);
    }

    // While we are still holding the RSTL and before returning, ensure the metrics collected for
    // this index build are attributed to the primary that commits or aborts the index build.
    ScopeGuard metricsGuard([&]() {
        auto& collector = ResourceConsumption::MetricsCollector::get(opCtx);
        bool wasCollecting = collector.endScopedCollecting();
        if (!isPrimary || !wasCollecting || !ResourceConsumption::isMetricsAggregationEnabled()) {
            return;
        }

        ResourceConsumption::get(opCtx).merge(opCtx, collector.getDbName(), collector.getMetrics());
    });

    // The collection object should always exist while an index build is registered.
    CollectionWriter collection(opCtx, replState->collectionUUID);
    invariant(collection,
              str::stream() << "Collection not found after relocking. Index build: "
                            << replState->buildUUID
                            << ", collection UUID: " << replState->collectionUUID);

    {
        indexBuildsSSS.drainSideWritesTableOnCommit.addAndFetch(1);
        // Perform the third and final drain after releasing a shared lock and reacquiring an
        // exclusive lock on the collection.
        uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(
            opCtx,
            replState->buildUUID,
            RecoveryUnit::ReadSource::kNoTimestamp,
            IndexBuildInterceptor::DrainYieldPolicy::kNoYield));
    }
    try {
        failIndexBuildOnCommit.execute(
            [](const BSONObj&) { uasserted(4698903, "index build aborted due to failpoint"); });

        // If we are no longer primary and a single phase index build started as primary attempts to
        // commit, trigger a self-abort.
        if (!isPrimary && IndexBuildAction::kSinglePhaseCommit == action) {
            uassertStatusOK(
                {ErrorCodes::NotWritablePrimary,
                 str::stream() << "Unable to commit index build because we are no longer primary: "
                               << replState->buildUUID});
        }

        // Retry indexing records that failed key generation, but only if we are primary.
        // Secondaries rely on the primary's decision to commit as assurance that it has checked all
        // key generation errors on its behalf.
        if (isPrimary) {
            uassertStatusOK(_indexBuildsManager.retrySkippedRecords(
                opCtx, replState->buildUUID, collection.get()));
        }

        // Duplicate key constraint checking phase. Duplicate key errors are tracked for
        // single-phase builds on primaries and two-phase builds in all replication states.
        // Single-phase builds on secondaries don't track duplicates so this call is a no-op. This
        // can be called for two-phase builds in all replication states except during initial sync
        // when this node is not guaranteed to be consistent.
        {
            indexBuildsSSS.processConstraintsViolatonTableOnCommit.addAndFetch(1);
            bool twoPhaseAndNotInitialSyncing =
                IndexBuildProtocol::kTwoPhase == replState->protocol &&
                !replCoord->getMemberState().startup2();
            if (IndexBuildProtocol::kSinglePhase == replState->protocol ||
                twoPhaseAndNotInitialSyncing) {
                uassertStatusOK(_indexBuildsManager.checkIndexConstraintViolations(
                    opCtx, collection.get(), replState->buildUUID));
            }
        }
        indexBuildsSSS.commit.addAndFetch(1);

        // If two phase index builds is enabled, index build will be coordinated using
        // startIndexBuild and commitIndexBuild oplog entries.
        auto onCommitFn = [&] {
            onCommitIndexBuild(opCtx, collection->ns(), replState);
        };

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
    } catch (const ExceptionForCat<ErrorCategory::ShutdownError>& e) {
        logFailure(e.toStatus(), collection->ns(), replState);
        _completeAbortForShutdown(opCtx, replState, collection.get());
        throw;
    } catch (const DBException& e) {
        // There already is clean-up handling code up the stack, but this redundancy is introduced
        // to make sure we abort the index build as primary, by doing so while we still have the
        // locks. The caller's handling code will detect this condition and do nothing.
        auto status = e.toStatus();
        logFailure(status, collection->ns(), replState);

        // It is illegal to abort the index build at this point. Note that Interruption exceptions
        // are allowed because we cannot control them as they bypass the routine abort machinery.
        invariant(!replState->isExternalAbort());

        // Index build commit may not fail on secondaries because it implies diverenge with data on
        // the primary. The only exception is single-phase builds started on primaries, which may
        // fail after a state transition. In this case, we have not replicated anything to
        // roll-back. With two-phase index builds, if a primary replicated an abortIndexBuild oplog
        // entry, then this index build should have been interrupted before committing.
        const bool twoPhaseAndNotPrimary =
            IndexBuildProtocol::kTwoPhase == replState->protocol && !isPrimary;
        if (twoPhaseAndNotPrimary) {
            LOGV2_FATAL(4698902,
                        "Index build failed while not primary",
                        "buildUUID"_attr = replState->buildUUID,
                        "collectionUUID"_attr = replState->collectionUUID,
                        logAttrs(replState->dbName),
                        "error"_attr = status);
        }

        replState->setPostFailureState(status);
        // This index build failed due to an indexing error in normal circumstances. Abort while
        // still holding the RSTL and collection locks.
        _completeSelfAbort(opCtx, replState, *indexBuildEntryColl);
        throw;
    }

    // At this point, the commitIndexBuild entry has already been written and replicated. For
    // correctness, we must perform these final writes. Temporarily disable interrupts.
    UninterruptibleLockGuard noInterrupt(shard_role_details::getLocker(opCtx));  // NOLINT.

    removeIndexBuildEntryAfterCommitOrAbort(opCtx, dbAndUUID, *indexBuildEntryColl, *replState);
    replState->stats.numIndexesAfter = getNumIndexesTotal(opCtx, collection.get());
    LOGV2(20663,
          "Index build: completed successfully",
          "buildUUID"_attr = replState->buildUUID,
          "collectionUUID"_attr = replState->collectionUUID,
          logAttrs(collection->ns()),
          "indexesBuilt"_attr = replState->indexNames,
          "numIndexesBefore"_attr = replState->stats.numIndexesBefore,
          "numIndexesAfter"_attr = replState->stats.numIndexesAfter);
    return CommitResult::kSuccess;
}

StatusWith<std::pair<long long, long long>> IndexBuildsCoordinator::_runIndexRebuildForRecovery(
    OperationContext* opCtx,
    CollectionWriter& collection,
    const UUID& buildUUID,
    RepairData repair) noexcept {
    invariant(
        shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(collection->ns(), MODE_X));

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
        LOGV2(20673, "Index builds manager starting", "buildUUID"_attr = buildUUID, logAttrs(nss));

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

    activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager, replState);

    if (status.isOK()) {
        return std::make_pair(numRecords, dataSize);
    }
    return status;
}

StatusWith<std::shared_ptr<ReplIndexBuildState>> IndexBuildsCoordinator::_getIndexBuild(
    const UUID& buildUUID) const {
    return activeIndexBuilds.getIndexBuild(buildUUID);
}

int IndexBuildsCoordinator::getNumIndexesTotal(OperationContext* opCtx,
                                               const CollectionPtr& collection) {
    invariant(collection);
    const auto& nss = collection->ns();
    invariant(shard_role_details::getLocker(opCtx)->isLocked(),
              str::stream() << "Unable to get index count because collection was not locked"
                            << nss.toStringForErrorMsg());

    auto indexCatalog = collection->getIndexCatalog();
    invariant(indexCatalog,
              str::stream() << "Collection is missing index catalog: "
                            << nss.toStringForErrorMsg());

    return indexCatalog->numIndexesTotal();
}

std::vector<BSONObj> IndexBuildsCoordinator::prepareSpecListForCreate(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const NamespaceString& nss,
    const std::vector<BSONObj>& indexSpecs) {
    CollectionCatalog::get(opCtx)->invariantHasExclusiveAccessToCollection(opCtx, collection->ns());
    invariant(collection);

    // During secondary oplog application, the index specs have already been normalized in the
    // oplog entries read from the primary. We should not be modifying the specs any further.
    auto indexCatalog = collection->getIndexCatalog();
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->canAcceptWritesFor(opCtx, nss)) {
        // A secondary node with a subset of the indexes already built will not vote for the commit
        // quorum, which can stall the index build indefinitely on a replica set.
        auto specsToBuild = indexCatalog->removeExistingIndexes(
            opCtx, collection, indexSpecs, /*removeIndexBuildsToo=*/true);
        if (indexSpecs.size() != specsToBuild.size()) {
            if (specsToBuild.size() == 0) {
                LOGV2_WARNING(
                    7731100,
                    "Secondary node already has all indexes built, which can happen as a result of "
                    "a previous, incomplete rolling index build. The node will not proceed with "
                    "the index build, and consequently will not participate in voting towards the "
                    "commit quorum. Use the 'setIndexCommitQuorum' command to adjust the commit "
                    "quorum accordingly. Caveat: to ensure the index build completes, this node "
                    "should not become primary for the duration of the build; step it down if it "
                    "happens",
                    logAttrs(nss),
                    logAttrs(collection->uuid()),
                    "requestedSpecs"_attr = indexSpecs,
                    "specsToBuild"_attr = specsToBuild);
            } else {
                LOGV2_WARNING(
                    7731101,
                    "Secondary node already has a subset of indexes built, which can happen as a "
                    "result of a previous, incomplete rolling index build. The node will not "
                    "proceed with the index build, and consequently will not participate in voting "
                    "towards the commit quorum. Use the 'setIndexCommitQuorum' command to adjust "
                    "the commit quorum accordingly. Caveat: to ensure the index build completes, "
                    "this node should not become primary for the duration of the build; step it "
                    "down if it happens. Additionally, this node will be missing a subset of the "
                    "indices present in the rest of the replica set. To remediate this, manually "
                    "build the missing indexes on this node as a standalone.",
                    logAttrs(nss),
                    logAttrs(collection->uuid()),
                    "requestedSpecs"_attr = indexSpecs,
                    "specsToBuild"_attr = specsToBuild);
            }
        }
        return indexSpecs;
    }

    // Normalize the specs' collations, wildcard projections, and any other fields as applicable.
    auto normalSpecs = indexCatalog->normalizeIndexSpecs(opCtx, collection, indexSpecs);

    // Remove any index specifications which already exist in the catalog.
    auto resultSpecs = indexCatalog->removeExistingIndexes(
        opCtx, collection, normalSpecs, false /*removeIndexBuildsToo*/);

    // Verify that each spec is compatible with the collection's sharding state.
    for (const BSONObj& spec : resultSpecs) {
        if (spec[kUniqueFieldName].trueValue() || spec[kPrepareUniqueFieldName].trueValue()) {
            checkShardKeyRestrictions(opCtx, nss, spec[kKeyFieldName].Obj());
        }
    }

    return resultSpecs;
}

}  // namespace mongo

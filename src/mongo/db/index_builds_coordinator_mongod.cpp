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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/index_builds_coordinator_mongod.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_build_entry_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {

using namespace indexbuildentryhelpers;

namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeInitializingIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangAfterInitializingIndexBuild);

/**
 * Constructs the options for the loader thread pool.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "IndexBuildsCoordinatorMongod";
    options.minThreads = 0;
    // We depend on thread pool sizes being equal between primaries and secondaries. If a secondary
    // has fewer resources than a primary, index build oplog entries can replicate in an order that
    // the secondary is unable to fulfill, leading to deadlocks. See SERVER-44250.
    options.maxThreads = 10;

    // Ensure all threads have a client.
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };

    return options;
}

}  // namespace

IndexBuildsCoordinatorMongod::IndexBuildsCoordinatorMongod()
    : _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();
}

void IndexBuildsCoordinatorMongod::shutdown() {
    // Stop new scheduling.
    _threadPool.shutdown();

    // Wait for all active builds to stop.
    waitForAllIndexBuildsToStopForShutdown();

    // Wait for active threads to finish.
    _threadPool.join();
}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorMongod::startIndexBuild(OperationContext* opCtx,
                                              std::string dbName,
                                              CollectionUUID collectionUUID,
                                              const std::vector<BSONObj>& specs,
                                              const UUID& buildUUID,
                                              IndexBuildProtocol protocol,
                                              IndexBuildOptions indexBuildOptions) {
    if (indexBuildOptions.twoPhaseRecovery) {
        // Two phase index build recovery goes though a different set-up procedure because the
        // original index will be dropped first.
        invariant(protocol == IndexBuildProtocol::kTwoPhase);
        auto status =
            _setUpIndexBuildForTwoPhaseRecovery(opCtx, dbName, collectionUUID, specs, buildUUID);
        if (!status.isOK()) {
            return status;
        }
    } else {
        auto statusWithOptionalResult =
            _filterSpecsAndRegisterBuild(opCtx,
                                         dbName,
                                         collectionUUID,
                                         specs,
                                         buildUUID,
                                         protocol,
                                         indexBuildOptions.commitQuorum);
        if (!statusWithOptionalResult.isOK()) {
            return statusWithOptionalResult.getStatus();
        }

        if (statusWithOptionalResult.getValue()) {
            // TODO (SERVER-37644): when joining is implemented, the returned Future will no longer
            // always be set.
            invariant(statusWithOptionalResult.getValue()->isReady());
            // The requested index (specs) are already built or are being built. Return success
            // early (this is v4.0 behavior compatible).
            return statusWithOptionalResult.getValue().get();
        }
    }

    invariant(!opCtx->lockState()->isRSTLExclusive(), buildUUID.toString());

    // Copy over all necessary OperationContext state.

    // Task in thread pool should retain the caller's deadline.
    const auto deadline = opCtx->getDeadline();
    const auto timeoutError = opCtx->getTimeoutError();

    const NamespaceStringOrUUID nssOrUuid{dbName, collectionUUID};
    const auto nss = CollectionCatalog::get(opCtx).resolveNamespaceStringOrUUID(opCtx, nssOrUuid);

    const auto& oss = OperationShardingState::get(opCtx);
    const auto shardVersion = oss.getShardVersion(nss);
    const auto dbVersion = oss.getDbVersion(dbName);

    // Task in thread pool should have similar CurOp representation to the caller so that it can be
    // identified as a createIndexes operation.
    LogicalOp logicalOp = LogicalOp::opInvalid;
    BSONObj opDesc;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        auto curOp = CurOp::get(opCtx);
        logicalOp = curOp->getLogicalOp();
        opDesc = curOp->opDescription().getOwned();
    }

    // If this index build was started during secondary batch application, it will have a commit
    // timestamp that must be copied over to timestamp the write to initialize the index build.
    const auto startTimestamp = opCtx->recoveryUnit()->getCommitTimestamp();

    // Use a promise-future pair to wait until the index build has been started. This future will
    // only return when the index build thread has started and the initial catalog write has been
    // written, or an error has been encountered otherwise.
    auto [startPromise, startFuture] = makePromiseFuture<void>();

    auto replState = invariant(_getIndexBuild(buildUUID));
    _threadPool.schedule([
        this,
        buildUUID,
        collectionUUID,
        dbName,
        nss,
        deadline,
        indexBuildOptions,
        logicalOp,
        opDesc,
        replState,
        startPromise = std::move(startPromise),
        startTimestamp,
        timeoutError,
        shardVersion,
        dbVersion
    ](auto status) mutable noexcept {
        // Clean up if we failed to schedule the task.
        if (!status.isOK()) {
            stdx::unique_lock<Latch> lk(_mutex);
            _unregisterIndexBuild(lk, replState);
            startPromise.setError(status);
            return;
        }

        auto opCtx = Client::getCurrent()->makeOperationContext();
        opCtx->setDeadlineByDate(deadline, timeoutError);

        auto& oss = OperationShardingState::get(opCtx.get());
        oss.initializeClientRoutingVersions(nss, shardVersion, dbVersion);

        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            auto curOp = CurOp::get(opCtx.get());
            curOp->setLogicalOp_inlock(logicalOp);
            curOp->setOpDescription_inlock(opDesc);
        }

        while (MONGO_unlikely(hangBeforeInitializingIndexBuild.shouldFail())) {
            sleepmillis(100);
        }

        // Index builds should never take the PBWM lock, even on a primary. This allows the
        // index build to continue running after the node steps down to a secondary.
        ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
            opCtx->lockState());

        if (!indexBuildOptions.twoPhaseRecovery) {
            status =
                _setUpIndexBuild(opCtx.get(), dbName, collectionUUID, buildUUID, startTimestamp);
            if (!status.isOK()) {
                startPromise.setError(status);
                return;
            }
        }

        // Signal that the index build started successfully.
        startPromise.setWith([] {});

        while (MONGO_unlikely(hangAfterInitializingIndexBuild.shouldFail())) {
            sleepmillis(100);
        }

        // Runs the remainder of the index build. Sets the promise result and cleans up the index
        // build.
        _runIndexBuild(opCtx.get(), buildUUID, indexBuildOptions);

        // Do not exit with an incomplete future.
        invariant(replState->sharedPromise.getFuture().isReady());
    });

    // Waits until the index build has either been started or failed to start.
    auto status = startFuture.getNoThrow(opCtx);
    if (!status.isOK()) {
        return status;
    }
    return replState->sharedPromise.getFuture();
}

Status IndexBuildsCoordinatorMongod::voteCommitIndexBuild(const UUID& buildUUID,
                                                          const HostAndPort& hostAndPort) {
    // TODO: not yet implemented.
    return Status::OK();
}

Status IndexBuildsCoordinatorMongod::setCommitQuorum(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     const std::vector<StringData>& indexNames,
                                                     const CommitQuorumOptions& newCommitQuorum) {
    if (indexNames.empty()) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream()
                          << "Cannot set a new commit quorum on an index build in collection '"
                          << nss << "' without providing any indexes.");
    }

    AutoGetCollectionForRead autoColl(opCtx, nss);
    Collection* collection = autoColl.getCollection();
    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Collection '" << nss << "' was not found.");
    }

    UUID collectionUUID = collection->uuid();

    stdx::unique_lock<Latch> lk(_mutex);
    auto collectionIt = _collectionIndexBuilds.find(collectionUUID);
    if (collectionIt == _collectionIndexBuilds.end()) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream() << "No index builds found on collection '" << nss << "'.");
    }

    if (!collectionIt->second->hasIndexBuildState(lk, indexNames.front())) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream() << "Cannot find an index build on collection '" << nss
                                    << "' with the provided index names");
    }

    // Use the first index to get the ReplIndexBuildState.
    std::shared_ptr<ReplIndexBuildState> buildState =
        collectionIt->second->getIndexBuildState(lk, indexNames.front());

    // Ensure the ReplIndexBuildState has the same indexes as 'indexNames'.
    bool equal = std::equal(
        buildState->indexNames.begin(), buildState->indexNames.end(), indexNames.begin());
    if (buildState->indexNames.size() != indexNames.size() || !equal) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream()
                          << "Provided indexes are not all being "
                          << "built by the same index builder in collection '" << nss << "'.");
    }

    // See if the new commit quorum is satisfiable.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    Status status = replCoord->checkIfCommitQuorumCanBeSatisfied(newCommitQuorum);
    if (!status.isOK()) {
        return status;
    }

    // Persist the new commit quorum for the index build and write it to the collection.
    buildState->commitQuorum = newCommitQuorum;
    // TODO (SERVER-40807): disabling the following code for the v4.2 release so it does not have
    // downstream impact.
    /*
    return indexbuildentryhelpers::setCommitQuorum(opCtx, buildState->buildUUID, newCommitQuorum);
    */
    return Status::OK();
}

Status IndexBuildsCoordinatorMongod::_finishScanningPhase() {
    // TODO: implement.
    return Status::OK();
}

Status IndexBuildsCoordinatorMongod::_finishVerificationPhase() {
    // TODO: implement.
    return Status::OK();
}

Status IndexBuildsCoordinatorMongod::_finishCommitPhase() {
    // TODO: implement.
    return Status::OK();
}

StatusWith<bool> IndexBuildsCoordinatorMongod::_checkCommitQuorum(
    const BSONObj& commitQuorum, const std::vector<HostAndPort>& confirmedMembers) {
    // TODO: not yet implemented.
    return false;
}

void IndexBuildsCoordinatorMongod::_refreshReplStateFromPersisted(OperationContext* opCtx,
                                                                  const UUID& buildUUID) {
    // TODO: not yet implemented.
}

}  // namespace mongo

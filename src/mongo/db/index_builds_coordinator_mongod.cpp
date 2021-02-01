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

#include "mongo/db/index_builds_coordinator_mongod.h"

#include <algorithm>

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/index_build_entry_gen.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_build_entry_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/two_phase_index_build_knobs_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterAcquiringIndexBuildSlot);
MONGO_FAIL_POINT_DEFINE(hangBeforeInitializingIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildAfterSignalPrimaryForCommitReadiness);

const StringData kMaxNumActiveUserIndexBuildsServerParameterName = "maxNumActiveUserIndexBuilds"_sd;

/**
 * Constructs the options for the loader thread pool.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "IndexBuildsCoordinatorMongod";
    options.minThreads = 0;
    // Both the primary and secondary nodes will have an unlimited thread pool size. This is done to
    // allow secondary nodes to startup as many index builders as necessary in order to prevent
    // scheduling deadlocks during initial sync or oplog application. When commands are run from
    // user connections that need to create indexes, those commands will hang until there are less
    // than 'maxNumActiveUserIndexBuilds' running index build threads, or until the operation is
    // interrupted.
    options.maxThreads = ThreadPool::Options::kUnlimited;

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

    // Change the 'setOnUpdate' function for the server parameter to signal the condition variable
    // when the value changes.
    ServerParameter* serverParam =
        ServerParameterSet::getGlobal()->get(kMaxNumActiveUserIndexBuildsServerParameterName);
    static_cast<
        IDLServerParameterWithStorage<ServerParameterType::kStartupAndRuntime, AtomicWord<int>>*>(
        serverParam)
        ->setOnUpdate([this](const int) -> Status {
            _indexBuildFinished.notify_all();
            return Status::OK();
        });
}

void IndexBuildsCoordinatorMongod::shutdown(OperationContext* opCtx) {
    // Stop new scheduling.
    _threadPool.shutdown();

    // Wait for all active builds to stop.
    waitForAllIndexBuildsToStopForShutdown(opCtx);

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
    return _startIndexBuild(
        opCtx, dbName, collectionUUID, specs, buildUUID, protocol, indexBuildOptions, boost::none);
}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorMongod::resumeIndexBuild(OperationContext* opCtx,
                                               std::string dbName,
                                               CollectionUUID collectionUUID,
                                               const std::vector<BSONObj>& specs,
                                               const UUID& buildUUID,
                                               const ResumeIndexInfo& resumeInfo) {
    IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions;
    indexBuildOptions.applicationMode = ApplicationMode::kStartupRepair;
    return _startIndexBuild(opCtx,
                            dbName,
                            collectionUUID,
                            specs,
                            buildUUID,
                            IndexBuildProtocol::kTwoPhase,
                            indexBuildOptions,
                            resumeInfo);
}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorMongod::_startIndexBuild(OperationContext* opCtx,
                                               std::string dbName,
                                               CollectionUUID collectionUUID,
                                               const std::vector<BSONObj>& specs,
                                               const UUID& buildUUID,
                                               IndexBuildProtocol protocol,
                                               IndexBuildOptions indexBuildOptions,
                                               const boost::optional<ResumeIndexInfo>& resumeInfo) {
    const NamespaceStringOrUUID nssOrUuid{dbName, collectionUUID};

    {
        // Only operations originating from user connections need to wait while there are more than
        // 'maxNumActiveUserIndexBuilds' index builds currently running.
        if (opCtx->getClient()->isFromUserConnection()) {
            {
                // The global lock acquires the RSTL lock which we use to assert that we're the
                // primary node when running user operations. Additionally, releasing this lock
                // allows the node to step down after we have checked the replication state. If this
                // node steps down after this check, similar assertions will cause the index build
                // to fail later on when locks are reacquired. Therefore, this assertion is not
                // required for correctness, but only intended to rate limit index builds started on
                // primaries.
                ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
                    opCtx->lockState());
                Lock::GlobalLock globalLk(opCtx, MODE_IX);

                auto replCoord = repl::ReplicationCoordinator::get(opCtx);
                uassert(ErrorCodes::NotWritablePrimary,
                        "Not primary while waiting to start an index build",
                        replCoord->canAcceptWritesFor(opCtx, nssOrUuid));
            }

            // The check here catches empty index builds and also allows us to stop index
            // builds before waiting for throttling. It may race with the abort at the start
            // of migration so we do check again later.
            uassertStatusOK(tenant_migration_access_blocker::checkIfCanBuildIndex(opCtx, dbName));

            stdx::unique_lock<Latch> lk(_throttlingMutex);
            opCtx->waitForConditionOrInterrupt(_indexBuildFinished, lk, [&] {
                const int maxActiveBuilds = maxNumActiveUserIndexBuilds.load();
                if (_numActiveIndexBuilds < maxActiveBuilds) {
                    _numActiveIndexBuilds++;
                    return true;
                }

                LOGV2(4715500,
                      "Too many index builds running simultaneously, waiting until the number of "
                      "active index builds is below the threshold",
                      "numActiveIndexBuilds"_attr = _numActiveIndexBuilds,
                      "maxNumActiveUserIndexBuilds"_attr = maxActiveBuilds,
                      "indexSpecs"_attr = specs,
                      "buildUUID"_attr = buildUUID,
                      "collectionUUID"_attr = collectionUUID);
                return false;
            });
        } else {
            // System index builds have no limit and never wait, but do consume a slot.
            stdx::unique_lock<Latch> lk(_throttlingMutex);
            _numActiveIndexBuilds++;
        }
    }

    auto onScopeExitGuard = makeGuard([&] {
        stdx::unique_lock<Latch> lk(_throttlingMutex);
        _numActiveIndexBuilds--;
        _indexBuildFinished.notify_one();
    });

    if (MONGO_unlikely(hangAfterAcquiringIndexBuildSlot.shouldFail())) {
        LOGV2(4886201, "Hanging index build due to failpoint 'hangAfterAcquiringIndexBuildSlot'");
        hangAfterAcquiringIndexBuildSlot.pauseWhileSet();
    }

    if (indexBuildOptions.applicationMode == ApplicationMode::kStartupRepair) {
        // Two phase index build recovery goes though a different set-up procedure because we will
        // either resume the index build or the original index will be dropped first.
        invariant(protocol == IndexBuildProtocol::kTwoPhase);
        auto status = Status::OK();
        if (resumeInfo) {
            status = _setUpResumeIndexBuild(
                opCtx, dbName, collectionUUID, specs, buildUUID, resumeInfo.get());
        } else {
            status = _setUpIndexBuildForTwoPhaseRecovery(
                opCtx, dbName, collectionUUID, specs, buildUUID);
        }
        if (!status.isOK()) {
            return status;
        }
    } else {
        auto statusWithOptionalResult =
            _filterSpecsAndRegisterBuild(opCtx, dbName, collectionUUID, specs, buildUUID, protocol);
        if (!statusWithOptionalResult.isOK()) {
            return statusWithOptionalResult.getStatus();
        }

        if (statusWithOptionalResult.getValue()) {
            invariant(statusWithOptionalResult.getValue()->isReady());
            // The requested index (specs) are already built or are being built. Return success
            // early (this is v4.0 behavior compatible).
            return statusWithOptionalResult.getValue().get();
        }

        if (opCtx->getClient()->isFromUserConnection()) {
            auto migrationStatus =
                tenant_migration_access_blocker::checkIfCanBuildIndex(opCtx, dbName);
            if (!migrationStatus.isOK()) {
                LOGV2(4886200,
                      "Aborted index build before start due to tenant migration",
                      "error"_attr = migrationStatus,
                      "buildUUID"_attr = buildUUID,
                      "collectionUUID"_attr = collectionUUID);
                activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager,
                                                       invariant(_getIndexBuild(buildUUID)));
                return migrationStatus;
            }
        }
    }

    invariant(!opCtx->lockState()->isRSTLExclusive(), buildUUID.toString());

    const auto nss = CollectionCatalog::get(opCtx)->resolveNamespaceStringOrUUID(opCtx, nssOrUuid);

    auto& oss = OperationShardingState::get(opCtx);
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

    // The thread pool task will be responsible for signalling the condition variable when the index
    // build thread is done running.
    onScopeExitGuard.dismiss();
    _threadPool.schedule([
        this,
        buildUUID,
        dbName,
        nss,
        indexBuildOptions,
        logicalOp,
        opDesc,
        replState,
        startPromise = std::move(startPromise),
        startTimestamp,
        shardVersion,
        dbVersion,
        resumeInfo
    ](auto status) mutable noexcept {
        auto onScopeExitGuard = makeGuard([&] {
            stdx::unique_lock<Latch> lk(_throttlingMutex);
            _numActiveIndexBuilds--;
            _indexBuildFinished.notify_one();
        });

        // Clean up if we failed to schedule the task.
        if (!status.isOK()) {
            activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager, replState);
            startPromise.setError(status);
            return;
        }

        auto opCtx = Client::getCurrent()->makeOperationContext();

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

        // Start collecting metrics for the index build. The metrics for this operation will only be
        // aggregated globally if the node commits or aborts while it is primary.
        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx.get());
        if (ResourceConsumption::shouldCollectMetricsForDatabase(dbName) &&
            ResourceConsumption::isMetricsCollectionEnabled()) {
            metricsCollector.beginScopedCollecting(opCtx.get(), dbName);
        }

        // Index builds should never take the PBWM lock, even on a primary. This allows the
        // index build to continue running after the node steps down to a secondary.
        ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
            opCtx->lockState());

        if (indexBuildOptions.applicationMode != ApplicationMode::kStartupRepair) {
            status = _setUpIndexBuild(opCtx.get(), buildUUID, startTimestamp, indexBuildOptions);
            if (!status.isOK()) {
                startPromise.setError(status);
                return;
            }
        }

        // Signal that the index build started successfully.
        startPromise.setWith([] {});

        // Runs the remainder of the index build. Sets the promise result and cleans up the index
        // build.
        _runIndexBuild(opCtx.get(), buildUUID, indexBuildOptions, resumeInfo);

        // Do not exit with an incomplete future.
        invariant(replState->sharedPromise.getFuture().isReady());

        try {
            // Logs the index build statistics if it took longer than the server parameter `slowMs`
            // to complete.
            CurOp::get(opCtx.get())
                ->completeAndLogOperation(opCtx.get(), MONGO_LOGV2_DEFAULT_COMPONENT);
        } catch (const DBException& e) {
            LOGV2(4656002, "unable to log operation", "error"_attr = e);
        }
    });

    // Waits until the index build has either been started or failed to start.
    // Ignore any interruption state in 'opCtx'.
    // If 'opCtx' is interrupted, the caller will be notified after startIndexBuild() returns when
    // it checks the future associated with 'sharedPromise'.
    auto status = startFuture.getNoThrow(Interruptible::notInterruptible());
    if (!status.isOK()) {
        return status;
    }
    return replState->sharedPromise.getFuture();
}

Status IndexBuildsCoordinatorMongod::voteCommitIndexBuild(OperationContext* opCtx,
                                                          const UUID& buildUUID,
                                                          const HostAndPort& votingNode) {
    auto swReplState = _getIndexBuild(buildUUID);
    if (!swReplState.isOK()) {
        // Index build might have got torn down.
        return swReplState.getStatus();
    }

    auto replState = swReplState.getValue();

    {
        // Secondary nodes will always try to vote regardless of the commit quorum value. If the
        // commit quorum is disabled, do not record their entry into the commit ready nodes.
        // If we fail to retrieve the persisted commit quorum, the index build might be in the
        // middle of tearing down.
        Lock::SharedLock commitQuorumLk(opCtx->lockState(), replState->commitQuorumLock.get());
        auto commitQuorum =
            uassertStatusOK(indexbuildentryhelpers::getCommitQuorum(opCtx, buildUUID));
        if (commitQuorum.numNodes == CommitQuorumOptions::kDisabled) {
            return Status::OK();
        }
    }

    // Our current contract is that commit quorum can't be disabled for an active index build with
    // commit quorum on (i.e., commit value set as non-zero or a valid tag) and vice-versa. So,
    // after this point, it's not possible for the index build's commit quorum value to get updated
    // to CommitQuorumOptions::kDisabled.
    Status persistStatus = Status::OK();

    IndexBuildEntry indexbuildEntry(
        buildUUID, replState->collectionUUID, CommitQuorumOptions(), replState->indexNames);
    std::vector<HostAndPort> votersList{votingNode};
    indexbuildEntry.setCommitReadyMembers(votersList);

    {
        // Updates don't need to acquire pbwm lock.
        ShouldNotConflictWithSecondaryBatchApplicationBlock noPBWMBlock(opCtx->lockState());
        persistStatus =
            indexbuildentryhelpers::persistCommitReadyMemberInfo(opCtx, indexbuildEntry);
    }

    if (persistStatus.isOK()) {
        _signalIfCommitQuorumIsSatisfied(opCtx, replState);
    }
    return persistStatus;
}

void IndexBuildsCoordinatorMongod::_sendCommitQuorumSatisfiedSignal(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    replState->setCommitQuorumSatisfied(opCtx);
}

void IndexBuildsCoordinatorMongod::_signalIfCommitQuorumIsSatisfied(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {

    // Acquire the commitQuorumLk in shared mode to make sure commit quorum value did not change
    // after reading it from config.system.indexBuilds collection.
    Lock::SharedLock commitQuorumLk(opCtx->lockState(), replState->commitQuorumLock.get());

    // Read the index builds entry from config.system.indexBuilds collection.
    auto swIndexBuildEntry =
        indexbuildentryhelpers::getIndexBuildEntry(opCtx, replState->buildUUID);
    auto indexBuildEntry = uassertStatusOK(swIndexBuildEntry);

    auto voteMemberList = indexBuildEntry.getCommitReadyMembers();
    // This can occur when no vote got received and stepup tries to check if commit quorum is
    // satisfied.
    if (!voteMemberList)
        return;

    bool commitQuorumSatisfied = repl::ReplicationCoordinator::get(opCtx)->isCommitQuorumSatisfied(
        indexBuildEntry.getCommitQuorum(), voteMemberList.get());

    if (!commitQuorumSatisfied)
        return;

    LOGV2(
        3856201, "Index build: commit quorum satisfied", "indexBuildEntry"_attr = indexBuildEntry);
    _sendCommitQuorumSatisfiedSignal(opCtx, replState);
}

bool IndexBuildsCoordinatorMongod::_signalIfCommitQuorumNotEnabled(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
        replState->setSinglePhaseCommit(opCtx);
        return true;
    }

    invariant(IndexBuildProtocol::kTwoPhase == replState->protocol);

    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
    repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);

    // Secondaries should always try to vote even if the commit quorum is disabled. Secondaries
    // must not read the on-disk commit quorum value as it may not be present at all times, such as
    // during initial sync.
    if (!replCoord->canAcceptWritesFor(opCtx, dbAndUUID)) {
        return false;
    }

    // Acquire the commitQuorumLk in shared mode to make sure commit quorum value did not change
    // after reading it from config.system.indexBuilds collection.
    Lock::SharedLock commitQuorumLk(opCtx->lockState(), replState->commitQuorumLock.get());

    // Read the commit quorum value from config.system.indexBuilds collection.
    auto commitQuorum = uassertStatusOKWithContext(
        indexbuildentryhelpers::getCommitQuorum(opCtx, replState->buildUUID),
        str::stream() << "failed to get commit quorum before committing index build: "
                      << replState->buildUUID);

    // Check if the commit quorum is disabled for the index build.
    if (commitQuorum.numNodes != CommitQuorumOptions::kDisabled) {
        return false;
    }

    _sendCommitQuorumSatisfiedSignal(opCtx, replState);
    return true;
}

bool IndexBuildsCoordinatorMongod::_checkVoteCommitIndexCmdSucceeded(const BSONObj& response,
                                                                     const UUID& indexBuildUUID) {
    auto commandStatus = getStatusFromCommandResult(response);
    auto wcStatus = getWriteConcernStatusFromCommandResult(response);
    if (commandStatus.isOK() && wcStatus.isOK()) {
        return true;
    }
    LOGV2(3856202,
          "'voteCommitIndexBuild' command failed.",
          "indexBuildUUID"_attr = indexBuildUUID,
          "responseStatus"_attr = response);
    return false;
}

void IndexBuildsCoordinatorMongod::_signalPrimaryForCommitReadiness(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // Before voting see if we are eligible to skip voting and signal
    // to commit index build if the node is primary.
    if (_signalIfCommitQuorumNotEnabled(opCtx, replState)) {
        return;
    }

    // No locks should be held.
    invariant(!opCtx->lockState()->isLocked());

    Backoff exponentialBackoff(Seconds(1), Seconds(2));

    auto onRemoteCmdScheduled = [opCtx, replState](executor::TaskExecutor::CallbackHandle handle) {
        replState->onVoteRequestScheduled(opCtx, handle);
    };

    auto onRemoteCmdComplete = [replState](executor::TaskExecutor::CallbackHandle) {
        replState->clearVoteRequestCbk();
    };

    auto needToVote = [replState]() -> bool { return !replState->getNextActionNoWait(); };

    // Retry 'voteCommitIndexBuild' command on error until we have been signaled either with commit
    // or abort. This way, we can make sure majority of nodes will never stop voting and wait for
    // commit or abort signal until they have received commit or abort signal.
    while (needToVote()) {
        // Check for any interrupts, including shutdown-related ones, before starting the voting
        // process.
        opCtx->checkForInterrupt();

        // Don't hammer the network.
        sleepFor(exponentialBackoff.nextSleep());
        // When index build started during startup recovery can try to get it's address when
        // rsConfig is uninitialized. So, retry till it gets initialized. Also, it's important, when
        // we retry, we check if we have received commit or abort signal to ensure liveness. For
        // e.g., consider a case where  index build gets restarted on startup recovery and
        // indexBuildsCoordinator thread waits for valid address w/o checking commit or abort
        // signal. Now, things can go wrong if we try to replay commitIndexBuild oplog entry for
        // that index build on startup recovery. Oplog applier would get stuck waiting on the
        // indexBuildsCoordinator thread. As a result, we won't be able to transition to secondary
        // state, get stuck on startup state.
        auto myAddress = replCoord->getMyHostAndPort();
        if (myAddress.empty()) {
            continue;
        }
        auto const voteCmdRequest =
            BSON("voteCommitIndexBuild" << replState->buildUUID << "hostAndPort"
                                        << myAddress.toString() << "writeConcern"
                                        << BSON("w"
                                                << "majority"));

        BSONObj voteCmdResponse;
        try {
            voteCmdResponse = replCoord->runCmdOnPrimaryAndAwaitResponse(
                opCtx, "admin", voteCmdRequest, onRemoteCmdScheduled, onRemoteCmdComplete);
        } catch (DBException& ex) {
            // All errors, including CallbackCanceled and network errors, should be retried.
            // If ErrorCodes::CallbackCanceled is due to shutdown, then checkForInterrupt() at the
            // beginning of this loop will catch it and throw an error to the caller. Or, if we
            // received the CallbackCanceled error because the index build was signaled with abort
            // or commit signal, then needToVote() would return false and we don't retry the voting
            // process.
            LOGV2_DEBUG(4666400,
                        1,
                        "Failed to run 'voteCommitIndexBuild' command.",
                        "indexBuildUUID"_attr = replState->buildUUID,
                        "errorMsg"_attr = ex);
            continue;
        }

        // Command error and write concern error have to be retried.
        if (_checkVoteCommitIndexCmdSucceeded(voteCmdResponse, replState->buildUUID)) {
            break;
        }
    }

    if (MONGO_unlikely(hangIndexBuildAfterSignalPrimaryForCommitReadiness.shouldFail())) {
        LOGV2(4841707, "Hanging index build after signaling the primary for commit readiness");
        hangIndexBuildAfterSignalPrimaryForCommitReadiness.pauseWhileSet(opCtx);
    }
    return;
}

IndexBuildAction IndexBuildsCoordinatorMongod::_drainSideWritesUntilNextActionIsAvailable(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    auto future = replState->getNextActionFuture();

    // Waits until the promise is fulfilled or the deadline expires.
    IndexBuildAction nextAction;
    auto waitUntilNextActionIsReady = [&]() {
        auto deadline = Date_t::now() + Milliseconds(1000);
        auto timeoutError = opCtx->getTimeoutError();

        try {
            nextAction =
                opCtx->runWithDeadline(deadline, timeoutError, [&] { return future.get(opCtx); });
        } catch (const ExceptionForCat<ErrorCategory::ExceededTimeLimitError>& e) {
            if (e.code() == timeoutError) {
                return false;
            }
            throw;
        }
        return true;
    };

    // Continuously drain incoming writes until the future is ready. This is an optimization that
    // allows the critical section of committing, which must drain the remainder of the side writes,
    // to be as short as possible.
    while (!waitUntilNextActionIsReady()) {
        _insertKeysFromSideTablesWithoutBlockingWrites(opCtx, replState);
    }
    return nextAction;
}

void IndexBuildsCoordinatorMongod::_waitForNextIndexBuildActionAndCommit(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions) {
    LOGV2(3856203,
          "Index build: waiting for next action before completing final phase",
          "buildUUID"_attr = replState->buildUUID);

    while (true) {
        // Future wait should hold no locks.
        invariant(!opCtx->lockState()->isLocked(),
                  str::stream() << "holding locks while waiting for commit or abort: "
                                << replState->buildUUID);
        // Future wait can be interrupted.
        const auto nextAction = _drainSideWritesUntilNextActionIsAvailable(opCtx, replState);

        LOGV2(3856204,
              "Index build: received signal",
              "buildUUID"_attr = replState->buildUUID,
              "action"_attr = indexBuildActionToString(nextAction));

        // If the index build was aborted, this serves as a final interruption point. Since the
        // index builder thread is interrupted before the action is set, this must fail if the build
        // was aborted.
        opCtx->checkForInterrupt();

        bool needsToRetryWait = false;

        auto commitTimestamp = replState->getCommitTimestamp();

        switch (nextAction) {
            case IndexBuildAction::kOplogCommit: {
                invariant(replState->protocol == IndexBuildProtocol::kTwoPhase);
                invariant(!commitTimestamp.isNull(), replState->buildUUID.toString());
                LOGV2(3856205,
                      "Index build: committing from oplog entry",
                      "buildUUID"_attr = replState->buildUUID,
                      "commitTimestamp"_attr = commitTimestamp,
                      "collectionUUID"_attr = replState->collectionUUID);
                break;
            }
            case IndexBuildAction::kCommitQuorumSatisfied: {
                invariant(commitTimestamp.isNull(),
                          str::stream() << "commit ts: " << commitTimestamp.toString()
                                        << "; index build: " << replState->buildUUID.toString());
                break;
            }
            case IndexBuildAction::kSinglePhaseCommit:
                invariant(replState->protocol == IndexBuildProtocol::kSinglePhase,
                          str::stream() << "commit ts: " << commitTimestamp.toString()
                                        << "; index build: " << replState->buildUUID.toString());
                break;
            case IndexBuildAction::kOplogAbort:
            case IndexBuildAction::kRollbackAbort:
            case IndexBuildAction::kInitialSyncAbort:
            case IndexBuildAction::kPrimaryAbort:
                // The calling thread should have interrupted us before signaling an abort action.
                LOGV2_FATAL(4698901, "Index build abort should have interrupted this operation");
            case IndexBuildAction::kNoAction:
                return;
        }

        auto result = _insertKeysFromSideTablesAndCommit(
            opCtx, replState, nextAction, indexBuildOptions, commitTimestamp);
        switch (result) {
            case CommitResult::kNoLongerPrimary:
                invariant(nextAction != IndexBuildAction::kOplogCommit);
                // Reset the promise as the node has stepped down. Wait for the new primary to
                // coordinate the index build and send the new signal/action.
                LOGV2(3856207,
                      "No longer primary while attempting to commit. Waiting again for next action "
                      "before completing final phase",
                      "buildUUID"_attr = replState->buildUUID);
                replState->resetNextActionPromise();
                needsToRetryWait = true;
                break;
            case CommitResult::kLockTimeout:
                LOGV2(4698900,
                      "Unable to acquire RSTL for commit within deadline. Releasing locks and "
                      "trying again",
                      "buildUUID"_attr = replState->buildUUID);
                needsToRetryWait = true;
                break;
            case CommitResult::kSuccess:
                break;
        }

        if (!needsToRetryWait) {
            break;
        }
    }
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

    AutoGetCollectionForRead collection(opCtx, nss);
    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Collection '" << nss << "' was not found.");
    }

    UUID collectionUUID = collection->uuid();
    std::shared_ptr<ReplIndexBuildState> replState;

    auto pred = [&](const auto& replState) {
        if (collectionUUID != replState.collectionUUID) {
            return false;
        }
        if (indexNames.size() != replState.indexNames.size()) {
            return false;
        }
        // Ensure the ReplIndexBuildState has the same indexes as 'indexNames'.
        return std::equal(
            replState.indexNames.begin(), replState.indexNames.end(), indexNames.begin());
    };
    auto collIndexBuilds = activeIndexBuilds.filterIndexBuilds(pred);
    if (collIndexBuilds.empty()) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream() << "Cannot find an index build on collection '" << nss
                                    << "' with the provided index names");
    }
    invariant(
        1U == collIndexBuilds.size(),
        str::stream() << "Found multiple index builds with the same index names on collection "
                      << nss << " (" << collectionUUID
                      << "): first index name: " << indexNames.front());

    replState = collIndexBuilds.front();

    // See if the new commit quorum is satisfiable.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    Status status = replCoord->checkIfCommitQuorumCanBeSatisfied(newCommitQuorum);
    if (!status.isOK()) {
        return status;
    }

    // Read the index builds entry from config.system.indexBuilds collection.
    auto swOnDiskCommitQuorum =
        indexbuildentryhelpers::getCommitQuorum(opCtx, replState->buildUUID);
    // Index build has not yet started.
    if (swOnDiskCommitQuorum == ErrorCodes::NoMatchingDocument) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream()
                          << "Index build not yet started for the provided indexes in collection '"
                          << nss << "'.");
    }

    auto currentCommitQuorum = invariantStatusOK(swOnDiskCommitQuorum);
    if (currentCommitQuorum.numNodes == CommitQuorumOptions::kDisabled ||
        newCommitQuorum.numNodes == CommitQuorumOptions::kDisabled) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Commit quorum value can be changed only for index builds "
                                    << "with commit quorum enabled, nss: '" << nss
                                    << "' first index name: '" << indexNames.front()
                                    << "' currentCommitQuorum: " << currentCommitQuorum.toBSON()
                                    << " providedCommitQuorum: " << newCommitQuorum.toBSON());
    }

    invariant(opCtx->lockState()->isRSTLLocked());
    // About to update the commit quorum value on-disk. So, take the lock in exclusive mode to
    // prevent readers from reading the commit quorum value and making decision on commit quorum
    // satisfied with the stale read commit quorum value.
    Lock::ExclusiveLock commitQuorumLk(opCtx->lockState(), replState->commitQuorumLock.get());
    {
        if (auto action = replState->getNextActionNoWait()) {
            return Status(ErrorCodes::CommandFailed,
                          str::stream() << "Commit quorum can't be changed as index build is "
                                           "ready to commit or abort: "
                                        << indexBuildActionToString(*action));
        }
    }

    IndexBuildEntry indexbuildEntry(
        replState->buildUUID, replState->collectionUUID, newCommitQuorum, replState->indexNames);
    status = indexbuildentryhelpers::persistIndexCommitQuorum(opCtx, indexbuildEntry);

    {
        // Check to see the index build hasn't received commit index build signal while updating
        // the commit quorum value on-disk.
        if (auto action = replState->getNextActionNoWait()) {
            invariant(*action != IndexBuildAction::kCommitQuorumSatisfied);
        }
    }
    return status;
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

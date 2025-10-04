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


#include "mongo/db/index_builds/index_builds_coordinator_mongod.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/index_builds/active_index_builds.h"
#include "mongo/db/index_builds/index_build_entry_gen.h"
#include "mongo/db/index_builds/index_build_entry_helpers.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/two_phase_index_build_knobs_gen.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/replication_state_transition_lock_guard.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/user_write_block/global_user_write_block_state.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(failIndexBuildWithErrorInSecondDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterRegisteringIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangBeforeInitializingIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildBeforeSignalPrimaryForCommitReadiness);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildAfterSignalPrimaryForCommitReadiness);
MONGO_FAIL_POINT_DEFINE(hangBeforeRunningIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildBeforeSignalingPrimaryForAbort);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildBeforeTransitioningReplStateTokAwaitPrimaryAbort);
MONGO_FAIL_POINT_DEFINE(hangBeforeVoteCommitIndexBuild);

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
        Client::initThread(threadName,
                           getGlobalServiceContext()->getService(ClusterRole::ShardServer),
                           Client::noSession(),
                           ClientOperationKillableByStepdown{false});
    };

    return options;
}

void runVoteCommand(OperationContext* opCtx,
                    ReplIndexBuildState* replState,
                    std::function<BSONObj(const UUID&, const std::string&)> generateCommand,
                    std::function<bool(BSONObj, const UUID)> validateFn) {

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // No locks should be held.
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    Backoff exponentialBackoff(Seconds(1), Seconds(2));

    auto onRemoteCmdScheduled = [opCtx, replState](executor::TaskExecutor::CallbackHandle handle) {
        replState->onVoteRequestScheduled(opCtx, handle);
    };

    auto onRemoteCmdComplete = [replState](executor::TaskExecutor::CallbackHandle) {
        replState->clearVoteRequestCbk();
    };

    auto needToVote = [replState]() -> bool {
        return !replState->getNextActionNoWait();
    };

    // Retry command on error until we have been signaled either with commit or abort. This way, we
    // can make sure majority of nodes will never stop voting and wait for commit or abort signal
    // until they have received commit or abort signal.
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

        const BSONObj voteCmdRequest = generateCommand(replState->buildUUID, myAddress.toString());

        BSONObj voteCmdResponse;
        try {
            voteCmdResponse = replCoord->runCmdOnPrimaryAndAwaitResponse(opCtx,
                                                                         DatabaseName::kAdmin,
                                                                         voteCmdRequest,
                                                                         onRemoteCmdScheduled,
                                                                         onRemoteCmdComplete);
        } catch (DBException& ex) {
            // All errors, including CallbackCanceled and network errors, should be retried.
            // If ErrorCodes::CallbackCanceled is due to shutdown, then checkForInterrupt() at the
            // beginning of this loop will catch it and throw an error to the caller. Or, if we
            // received the CallbackCanceled error because the index build was signaled with abort
            // or commit signal, then needToVote() would return false and we don't retry the voting
            // process.
            LOGV2_DEBUG(4666400,
                        1,
                        "Failed to run index build vote command.",
                        "command"_attr = voteCmdRequest.firstElement().fieldName(),
                        "indexBuildUUID"_attr = replState->buildUUID,
                        "errorMsg"_attr = ex);
            continue;
        }

        // Check if the command has to be retried.
        if (validateFn(voteCmdResponse, replState->buildUUID)) {
            break;
        }
    }
};
}  // namespace

IndexBuildsCoordinatorMongod::IndexBuildsCoordinatorMongod()
    : _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();

    // Change the 'setOnUpdate' function for the server parameter to signal the condition variable
    // when the value changes.
    using ParamT =
        IDLServerParameterWithStorage<ServerParameterType::kStartupAndRuntime, AtomicWord<int>>;
    ServerParameterSet::getNodeParameterSet()
        ->get<ParamT>(kMaxNumActiveUserIndexBuildsServerParameterName)
        ->setOnUpdate([this](const int) -> Status {
            _indexBuildFinished.notify_all();
            return Status::OK();
        });
}

void IndexBuildsCoordinatorMongod::shutdown(OperationContext*) {
    // Stop new scheduling.
    _threadPool.shutdown();

    // Wait for all active builds to stop.
    activeIndexBuilds.waitForAllIndexBuildsToStopForShutdown();

    // Wait for active threads to finish.
    _threadPool.join();
}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorMongod::startIndexBuild(OperationContext* opCtx,
                                              const DatabaseName& dbName,
                                              const UUID& collectionUUID,
                                              const std::vector<IndexBuildInfo>& indexes,
                                              const UUID& buildUUID,
                                              IndexBuildProtocol protocol,
                                              IndexBuildOptions indexBuildOptions) {
    return _startIndexBuild(opCtx,
                            dbName,
                            collectionUUID,
                            indexes,
                            buildUUID,
                            protocol,
                            indexBuildOptions,
                            boost::none);
}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorMongod::resumeIndexBuild(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               const UUID& collectionUUID,
                                               const std::vector<IndexBuildInfo>& indexes,
                                               const UUID& buildUUID,
                                               const ResumeIndexInfo& resumeInfo) {
    IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions;
    indexBuildOptions.applicationMode = ApplicationMode::kStartupRepair;
    return _startIndexBuild(opCtx,
                            dbName,
                            collectionUUID,
                            indexes,
                            buildUUID,
                            IndexBuildProtocol::kTwoPhase,
                            indexBuildOptions,
                            resumeInfo);
}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorMongod::_startIndexBuild(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               const UUID& collectionUUID,
                                               const std::vector<IndexBuildInfo>& indexes,
                                               const UUID& buildUUID,
                                               IndexBuildProtocol protocol,
                                               IndexBuildOptions indexBuildOptions,
                                               const boost::optional<ResumeIndexInfo>& resumeInfo) {
    const NamespaceStringOrUUID nssOrUuid{dbName, collectionUUID};

    auto writeBlockState = GlobalUserWriteBlockState::get(opCtx);

    invariant(!shard_role_details::getLocker(opCtx)->isRSTLExclusive(), buildUUID.toString());

    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (protocol == IndexBuildProtocol::kTwoPhase && fcvSnapshot.isVersionInitialized() &&
        feature_flags::gFeatureFlagPrimaryDrivenIndexBuilds.isEnabled(
            VersionContext::getDecoration(opCtx), fcvSnapshot)) {
        invariant(indexBuildOptions.indexBuildMethod == IndexBuildMethodEnum::kPrimaryDriven);
    }

    const auto nss = CollectionCatalog::get(opCtx)->resolveNamespaceStringOrUUID(opCtx, nssOrUuid);

    std::vector<BSONObj> indexSpecs;
    indexSpecs.reserve(indexes.size());
    for (const auto& indexBuildInfo : indexes) {
        indexSpecs.push_back(indexBuildInfo.spec);
    }

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
                if (gFeatureFlagIntentRegistration.isEnabled()) {
                    auto intent = nss.isReplicated()
                        ? rss::consensus::IntentRegistry::Intent::Write
                        : rss::consensus::IntentRegistry::Intent::LocalWrite;
                    uassert(ErrorCodes::NotWritablePrimary,
                            "Not primary while waiting to start an index build",
                            rss::consensus::IntentRegistry::get(opCtx->getServiceContext())
                                .canDeclareIntent(intent, opCtx));
                } else {
                    repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
                    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
                    uassert(ErrorCodes::NotWritablePrimary,
                            "Not primary while waiting to start an index build",
                            replCoord->canAcceptWritesFor(opCtx, nssOrUuid));
                }
            }

            // The checks here catch empty index builds and also allow us to stop index
            // builds before waiting for throttling.
            uassertStatusOK(writeBlockState->checkIfIndexBuildAllowedToStart(opCtx, nss));

            stdx::unique_lock<stdx::mutex> lk(_throttlingMutex);
            bool messageLogged = false;
            opCtx->waitForConditionOrInterrupt(_indexBuildFinished, lk, [&] {
                const int maxActiveBuilds = maxNumActiveUserIndexBuilds.load();
                if (_numActiveIndexBuilds < maxActiveBuilds) {
                    _numActiveIndexBuilds++;
                    return true;
                }

                if (!messageLogged) {
                    LOGV2(
                        4715500,
                        "Too many index builds running simultaneously, waiting until the number of "
                        "active index builds is below the threshold",
                        "numActiveIndexBuilds"_attr = _numActiveIndexBuilds,
                        "maxNumActiveUserIndexBuilds"_attr = maxActiveBuilds,
                        "indexSpecs"_attr = indexSpecs,
                        "buildUUID"_attr = buildUUID,
                        "collectionUUID"_attr = collectionUUID);
                    messageLogged = true;
                }
                return false;
            });
        } else {
            // System index builds have no limit and never wait, but do consume a slot.
            stdx::unique_lock<stdx::mutex> lk(_throttlingMutex);
            _numActiveIndexBuilds++;
        }
    }

    ScopeGuard onScopeExitGuard([&] {
        stdx::unique_lock<stdx::mutex> lk(_throttlingMutex);
        _numActiveIndexBuilds--;
        _indexBuildFinished.notify_one();
    });

    ScopeGuard unregisterUnscheduledIndexBuild([&] {
        auto replIndexBuildState = _getIndexBuild(buildUUID);
        if (replIndexBuildState.isOK()) {
            auto replState = invariant(replIndexBuildState);
            if (replState->isSettingUp()) {
                activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager, replState);
            }
        }
    });

    if (indexBuildOptions.applicationMode == ApplicationMode::kStartupRepair) {
        // Two phase index build recovery goes through a different set-up procedure because we will
        // either resume the index build or the original index will be dropped first.
        invariant(protocol == IndexBuildProtocol::kTwoPhase);
        auto status = Status::OK();
        if (resumeInfo) {
            status = _setUpResumeIndexBuild(
                opCtx, dbName, collectionUUID, indexes, buildUUID, resumeInfo.value());
        } else {
            status = _setUpIndexBuildForTwoPhaseRecovery(
                opCtx, dbName, collectionUUID, indexes, buildUUID);
        }
        if (!status.isOK()) {
            return status;
        }
    } else {
        auto statusWithOptionalResult = _filterSpecsAndRegisterBuild(
            opCtx, dbName, collectionUUID, indexes, buildUUID, protocol);
        if (!statusWithOptionalResult.isOK()) {
            return statusWithOptionalResult.getStatus();
        }

        if (MONGO_unlikely(hangAfterRegisteringIndexBuild.shouldFail())) {
            LOGV2(8296700, "Hanging due to hangAfterRegisteringIndexBuild");
            hangAfterRegisteringIndexBuild.pauseWhileSet(opCtx);
        }

        if (statusWithOptionalResult.getValue()) {
            invariant(statusWithOptionalResult.getValue()->isReady());
            // The requested index (specs) are already built or are being built. Return success
            // early (this is v4.0 behavior compatible).
            return statusWithOptionalResult.getValue().value();
        }

        if (opCtx->getClient()->isFromUserConnection()) {
            auto buildBlockedStatus = writeBlockState->checkIfIndexBuildAllowedToStart(opCtx, nss);
            if (!buildBlockedStatus.isOK()) {
                LOGV2(6511603,
                      "Aborted index build due to user index builds being blocked",
                      "error"_attr = buildBlockedStatus,
                      "buildUUID"_attr = buildUUID,
                      "collectionUUID"_attr = collectionUUID);
                return buildBlockedStatus;
            }
        }
    }

    auto& oss = OperationShardingState::get(opCtx);

    // The builder thread updates to curOp description to be that of a createIndexes command, but we
    // still want to transfer whatever extra information there is available from the caller.
    BSONObj opDesc;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        auto curOp = CurOp::get(opCtx);
        opDesc = curOp->opDescription().getOwned();
    }

    // If this index build was started during secondary batch application, it will have a commit
    // timestamp that must be copied over to timestamp the write to initialize the index build.
    const auto startTimestamp = shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp();

    // Use a promise-future pair to wait until the index build has been started. This future will
    // only return when the index build thread has started and the initial catalog write has been
    // written, or an error has been encountered otherwise.
    auto [startPromise, startFuture] = makePromiseFuture<void>();

    auto replState = invariant(_getIndexBuild(buildUUID));

    ForwardableOperationMetadata forwardableOpMetadata(opCtx);

    // The thread pool task will be responsible for signalling the condition variable when the index
    // build thread is done running.
    onScopeExitGuard.dismiss();
    unregisterUnscheduledIndexBuild.dismiss();
    _threadPool.schedule([this,
                          buildUUID,
                          dbName,
                          nss,
                          indexBuildOptions,
                          opDesc,
                          replState,
                          startPromise = std::move(startPromise),
                          startTimestamp,
                          shardVersion = oss.getShardVersion(nss),
                          dbVersion = oss.getDbVersion(dbName),
                          resumeInfo,
                          forwardableOpMetadata =
                              std::move(forwardableOpMetadata)](auto status) mutable {
        ScopeGuard onScopeExitGuard([&] {
            stdx::unique_lock<stdx::mutex> lk(_throttlingMutex);
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
        // Indicate that the index build is scheduled and running under this opCtx.
        replState->onThreadScheduled(opCtx.get());

        // Set up the thread's currentOp information to display createIndexes cmd information,
        // merged with the caller's opDesc.
        updateCurOpOpDescription(opCtx.get(), nss, toIndexSpecs(replState->getIndexes()), opDesc);

        // Forward the forwardable operation metadata from the external client to this thread's
        // client.
        forwardableOpMetadata.setOn(opCtx.get());

        while (MONGO_unlikely(hangBeforeInitializingIndexBuild.shouldFail())) {
            sleepmillis(100);
        }

        if (indexBuildOptions.applicationMode != ApplicationMode::kStartupRepair) {
            // The shard version protocol is only required when setting up the index build and
            // writing the 'startIndexBuild' oplog entry. If a chunk migration is in-progress while
            // an index build is started, it will be aborted. A recipient shard will copy
            // in-progress indexes from the donor shard, and if the index build is aborted on the
            // donor, the client running createIndexes will receive an error requiring them to retry
            // the command, and the indexes will become consistent.
            ScopedSetShardRole scopedSetShardRole(opCtx.get(), nss, shardVersion, dbVersion);
            status = _setUpIndexBuild(opCtx.get(), buildUUID, startTimestamp, indexBuildOptions);
            if (!status.isOK()) {
                startPromise.setError(status);
                // Do not exit with an incomplete future, even if setup fails, we should still
                // signal waiters.
                invariant(replState->sharedPromise.getFuture().isReady());
                return;
            }
        }

        // Signal that the index build started successfully.
        startPromise.setWith([] {});

        hangBeforeRunningIndexBuild.pauseWhileSet();

        // Runs the remainder of the index build. Sets the promise result and cleans up the
        // index build.
        _runIndexBuild(opCtx.get(), buildUUID, indexBuildOptions, resumeInfo);

        // Do not exit with an incomplete future.
        invariant(replState->sharedPromise.getFuture().isReady());

        try {
            // Logs the index build statistics if it took longer than the server parameter
            // `slowMs` to complete.
            CurOp::get(opCtx.get())
                ->completeAndLogOperation(
                    {MONGO_LOGV2_DEFAULT_COMPONENT, toLogService(opCtx->getService())},
                    DatabaseProfileSettings::get(opCtx->getServiceContext())
                        .getDatabaseProfileSettings(nss.dbName())
                        .filter);
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

Status IndexBuildsCoordinatorMongod::voteAbortIndexBuild(OperationContext* opCtx,
                                                         const UUID& buildUUID,
                                                         const HostAndPort& votingNode,
                                                         StringData reason) {

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto memberConfig = replCoord->findConfigMemberByHostAndPort_deprecated(votingNode);
    if (!memberConfig || memberConfig->isArbiter()) {
        return Status{ErrorCodes::Error{7329201},
                      "Non-member and arbiter nodes cannot vote to abort an index build"};
    }

    if (replCoord->getMyHostAndPort() == votingNode) {
        LOGV2_DEBUG(7329404, 2, "'voteAbortIndexBuild' loopback", "node"_attr = votingNode);
    }

    bool aborted = abortIndexBuildByBuildUUID(
        opCtx,
        buildUUID,
        IndexBuildAction::kPrimaryAbort,
        Status{ErrorCodes::IndexBuildAborted,
               fmt::format(
                   "'voteAbortIndexBuild' received from '{}': {}", votingNode.toString(), reason)});

    if (aborted) {
        return Status::OK();
    }

    // Index build does not exist or cannot be aborted because it is committing.
    // No need to wait for write concern.
    return Status{ErrorCodes::Error{7329202}, "Index build cannot be aborted"};
}

Status IndexBuildsCoordinatorMongod::voteCommitIndexBuild(OperationContext* opCtx,
                                                          const UUID& buildUUID,
                                                          const HostAndPort& votingNode) {
    hangBeforeVoteCommitIndexBuild.pauseWhileSet(opCtx);

    auto swReplState = _getIndexBuild(buildUUID);
    if (!swReplState.isOK()) {
        // Index build might have got torn down.
        return swReplState.getStatus();
    }

    auto replState = swReplState.getValue();

    {
        // TODO SERVER-99706: Investigate if this is safe. Other commit quorum operations take the
        // RSTL lock before locking the commit quorum lock. However, this operation follows the
        // inverse order.
        DisableLockerRuntimeOrderingChecks disable{opCtx};
        // Secondary nodes will always try to vote regardless of the commit quorum value. If the
        // commit quorum is disabled, do not record their entry into the commit ready nodes.
        // If we fail to retrieve the persisted commit quorum, the index build might be in the
        // middle of tearing down.
        Lock::SharedLock commitQuorumLk(opCtx, *replState->commitQuorumLock);
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

    IndexBuildEntry indexbuildEntry(buildUUID,
                                    replState->collectionUUID,
                                    CommitQuorumOptions(),
                                    toIndexNames(replState->getIndexes()));
    std::vector<HostAndPort> votersList{votingNode};
    indexbuildEntry.setCommitReadyMembers(votersList);

    auto persistStatus =
        indexbuildentryhelpers::persistCommitReadyMemberInfo(opCtx, indexbuildEntry);
    if (persistStatus.isOK()) {
        _signalIfCommitQuorumIsSatisfied(opCtx, replState);
    }
    return persistStatus;
}

void IndexBuildsCoordinatorMongod::_sendCommitQuorumSatisfiedSignal(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    replState->setCommitQuorumSatisfied(opCtx);
}

bool IndexBuildsCoordinatorMongod::_signalIfCommitQuorumIsSatisfied(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {

    // TODO SERVER-99706: Investigate if this is safe. Other commit quorum operations take the
    // RSTL lock before locking the commit quorum lock. However, this operation follows the
    // inverse order.
    DisableLockerRuntimeOrderingChecks disable{opCtx};

    // Acquire the commitQuorumLk in shared mode to make sure commit quorum value did not change
    // after reading it from config.system.indexBuilds collection.
    Lock::SharedLock commitQuorumLk(opCtx, *replState->commitQuorumLock);

    // Read the index builds entry from config.system.indexBuilds collection.
    auto swIndexBuildEntry =
        indexbuildentryhelpers::getIndexBuildEntry(opCtx, replState->buildUUID);
    auto indexBuildEntry = uassertStatusOK(swIndexBuildEntry);

    auto voteMemberList = indexBuildEntry.getCommitReadyMembers();
    // This can occur when no vote got received and stepup tries to check if commit quorum is
    // satisfied.
    if (!voteMemberList)
        return false;

    bool commitQuorumSatisfied = repl::ReplicationCoordinator::get(opCtx)->isCommitQuorumSatisfied(
        indexBuildEntry.getCommitQuorum(), voteMemberList.value());

    if (!commitQuorumSatisfied)
        return false;

    LOGV2(
        3856201, "Index build: commit quorum satisfied", "indexBuildEntry"_attr = indexBuildEntry);
    _sendCommitQuorumSatisfiedSignal(opCtx, replState);
    return true;
}

bool IndexBuildsCoordinatorMongod::_signalIfCommitQuorumNotEnabled(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
        replState->setSinglePhaseCommit(opCtx);
        return true;
    }

    invariant(IndexBuildProtocol::kTwoPhase == replState->protocol);

    if (gFeatureFlagIntentRegistration.isEnabled()) {
        if (!rss::consensus::IntentRegistry::get(opCtx->getServiceContext())
                 .canDeclareIntent(rss::consensus::IntentRegistry::Intent::Write, opCtx)) {
            return false;
        }
    } else {
        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
        repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);

        // Secondaries should always try to vote even if the commit quorum is disabled. Secondaries
        // must not read the on-disk commit quorum value as it may not be present at all times, such
        // as during initial sync.
        if (!replCoord->canAcceptWritesFor(opCtx, dbAndUUID)) {
            return false;
        }
    }

    // TODO SERVER-99706: Investigate if this is safe. Other commit quorum operations take the
    // RSTL lock before locking the commit quorum lock. However, this operation follows the
    // inverse order.
    DisableLockerRuntimeOrderingChecks disable{opCtx};

    // Acquire the commitQuorumLk in shared mode to make sure commit quorum value did not change
    // after reading it from config.system.indexBuilds collection.
    Lock::SharedLock commitQuorumLk(opCtx, *replState->commitQuorumLock);

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

void IndexBuildsCoordinatorMongod::_signalPrimaryForAbortAndWaitForExternalAbort(
    OperationContext* opCtx, ReplIndexBuildState* replState) {
    hangIndexBuildBeforeTransitioningReplStateTokAwaitPrimaryAbort.pauseWhileSet(opCtx);

    const auto abortStatus = replState->getAbortStatus();
    LOGV2(7419402,
          "Index build: signaling primary to abort index build",
          "buildUUID"_attr = replState->buildUUID,
          logAttrs(replState->dbName),
          "collectionUUID"_attr = replState->collectionUUID,
          "reason"_attr = abortStatus);
    replState->requestAbortFromPrimary();

    hangIndexBuildBeforeSignalingPrimaryForAbort.pauseWhileSet(opCtx);

    // The abort command might loop back to the same node, resulting in the command killing the
    // index builder thread (current), causing the cancellation of the command callback handle due
    // to the opCtx being interrupted. De-registering the callback due to cancellation means the
    // command did actually abort the index, so it is a non issue to just let it be cancelled. This
    // might be reflected in the logs as a cancelled command request, even if the command did
    // actually abort the build.
    const auto reason = replState->getAbortReason();

    const auto generateCmd = [reason](const UUID& uuid, const std::string& address) {
        return BSON("voteAbortIndexBuild" << uuid << "hostAndPort" << address << "reason" << reason
                                          << "writeConcern" << BSON("w" << "majority"));
    };

    const auto checkVoteAbortIndexCmdDone = [](const BSONObj& response,
                                               const UUID& indexBuildUUID) {
        auto commandStatus = getStatusFromCommandResult(response);
        auto wcStatus = getWriteConcernStatusFromCommandResult(response);
        if (commandStatus.isOK() && wcStatus.isOK()) {
            return true;
        }
        if (!commandStatus.isA<ErrorCategory::NotPrimaryError>()) {
            // If the index build failed to abort on the primary, retrying won't solve the issue.
            // This may only happen if the build cannot be aborted because it is being committed, or
            // because it is already aborted or committed, meaning an active index build is not
            // registered. In any of these cases, the primary will eventually replicate either a
            // 'commitIndexBuild' or 'abortIndexBuild' oplog entry. So it is safe to just wait for
            // the next action after the vote request is done.
            LOGV2(7329400,
                  "Index build: 'voteAbortIndexBuild' command failed, index build was not found or "
                  "is in the process of committing. The command won't be retried.",
                  "buildUUID"_attr = indexBuildUUID,
                  "responseStatus"_attr = response);
            return true;
        }

        // We should retry if the error is due to primary change.
        LOGV2(7329401,
              "Index build: 'voteAbortIndexBuild' command failed due to primary change and will be "
              "retried",
              "buildUUID"_attr = indexBuildUUID,
              "responseStatus"_attr = response);
        return false;
    };

    runVoteCommand(opCtx, replState, generateCmd, checkVoteAbortIndexCmdDone);

    // Wait until the index build is externally aborted in this node, either through loopback or
    // replication, causing the index building thread to be interrupted, or the promise to be
    // fulfilled. Cleanup is done by the async command or oplog applier thread. If a
    // 'commitIndexBuild' is replicated in this state, the secondary will crash.
    try {
        if (!replState->getNextActionFuture().isReady()) {
            LOGV2(7329406,
                  "Index build: waiting for primary to abort the index build after "
                  "'voteAbortIndexBuild' request",
                  "buildUUID"_attr = replState->buildUUID);
            replState->getNextActionFuture().wait(opCtx);
        }
        // The promise was fullfilled before waiting.
        return;
    } catch (const DBException&) {
        // External aborts must wait for the builder thread, so we cannot be in an already aborted
        // state.
        if (replState->isExternalAbort()) {
            // The build was aborted, and the opCtx interrupted, before the thread checked the
            // future.
            return;
        }
        throw;
    }
}

void IndexBuildsCoordinatorMongod::_signalPrimaryForCommitReadiness(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    if (MONGO_unlikely(hangIndexBuildBeforeSignalPrimaryForCommitReadiness.shouldFail())) {
        LOGV2(10528500, "Hanging index build after signaling the primary for commit readiness");
        hangIndexBuildBeforeSignalPrimaryForCommitReadiness.pauseWhileSet(opCtx);
    }

    // Before voting see if we are eligible to skip voting and signal
    // to commit index build if the node is primary.
    if (_signalIfCommitQuorumNotEnabled(opCtx, replState)) {
        LOGV2(7568001,
              "Index build: skipping vote for commit readiness",
              "buildUUID"_attr = replState->buildUUID,
              logAttrs(replState->dbName),
              "collectionUUID"_attr = replState->collectionUUID);
        return;
    }

    LOGV2(7568000,
          "Index build: vote for commit readiness",
          "buildUUID"_attr = replState->buildUUID,
          logAttrs(replState->dbName),
          "collectionUUID"_attr = replState->collectionUUID);

    // Indicate that the index build in this node has already tried to vote for commit readiness.
    // We do not try to determine whether the vote has actually succeeded or not, as it is
    // challenging due to the asynchronous request and potential concurrent interrupts. After this
    // point, the node cannot vote to abort this index build, and if it needs to abort the index
    // build it must try to do so independently. Meaning, as a primary it will succeed, but as a
    // secondary it will fassert.
    replState->setVotedForCommitReadiness(opCtx);

    const auto generateCmd = [](const UUID& uuid, const std::string& address) {
        return BSON("voteCommitIndexBuild" << uuid << "hostAndPort" << address << "writeConcern"
                                           << BSON("w" << "majority"));
    };

    const auto checkVoteCommitIndexCmdSucceeded = [](const BSONObj& response,
                                                     const UUID& indexBuildUUID) {
        // Command error and write concern error have to be retried.
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
    };

    runVoteCommand(opCtx, replState.get(), generateCmd, checkVoteCommitIndexCmdSucceeded);

    if (MONGO_unlikely(hangIndexBuildAfterSignalPrimaryForCommitReadiness.shouldFail())) {
        LOGV2(4841707, "Hanging index build after signaling the primary for commit readiness");
        hangIndexBuildAfterSignalPrimaryForCommitReadiness.pauseWhileSet(opCtx);
    }
    return;
}

IndexBuildAction IndexBuildsCoordinatorMongod::_waitForNextIndexBuildAction(
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
        } catch (const ExceptionFor<ErrorCategory::ExceededTimeLimitError>& e) {
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
        if (replState->getGenerateTableWrites()) {
            _insertKeysFromSideTablesWithoutBlockingWrites(opCtx, replState);
        }
    }

    if (replState->getGenerateTableWrites()) {
        // Final chance to catch up before taking an X lock.
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

    failIndexBuildWithErrorInSecondDrain.executeIf(
        [](const BSONObj& data) {
            uasserted(data["error"].safeNumberInt(),
                      "failIndexBuildWithErrorInSecondDrain failpoint triggered");
        },
        [&](const BSONObj& data) {
            return UUID::parse(data["buildUUID"]) == replState->buildUUID;
        });

    while (true) {
        // Future wait should hold no locks.
        invariant(!shard_role_details::getLocker(opCtx)->isLocked(),
                  str::stream() << "holding locks while waiting for commit or abort: "
                                << replState->buildUUID);

        auto const nextAction = [&] {
            _incWaitForCommitQuorum();
            // Future wait can be interrupted.
            return _waitForNextIndexBuildAction(opCtx, replState);
        }();
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
                LOGV2(7866201,
                      "Unable to acquire locks for commit within deadline. Releasing locks and "
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
                          << nss.toStringForErrorMsg() << "' without providing any indexes.");
    }

    // Take the MODE_IX lock now, so that when we actually persist the value later, we don't need to
    // upgrade the lock.
    AutoGetCollection collection(opCtx, nss, MODE_IX);
    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream()
                          << "Collection '" << nss.toStringForErrorMsg() << "' was not found.");
    }

    UUID collectionUUID = collection->uuid();
    std::shared_ptr<ReplIndexBuildState> replState;

    auto pred = [&](const auto& replState) {
        if (collectionUUID != replState.collectionUUID) {
            return false;
        }

        auto replStateIndexNames = toIndexNames(replState.getIndexes());
        if (indexNames.size() != replStateIndexNames.size()) {
            return false;
        }
        // Ensure the ReplIndexBuildState has the same indexes as 'indexNames'.
        return std::equal(
            replStateIndexNames.begin(), replStateIndexNames.end(), indexNames.begin());
    };
    auto collIndexBuilds = activeIndexBuilds.filterIndexBuilds(pred);
    if (collIndexBuilds.empty()) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream()
                          << "Cannot find an index build on collection '"
                          << nss.toStringForErrorMsg() << "' with the provided index names");
    }
    invariant(
        1U == collIndexBuilds.size(),
        str::stream() << "Found multiple index builds with the same index names on collection "
                      << nss.toStringForErrorMsg() << " (" << collectionUUID
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
                          << nss.toStringForErrorMsg() << "'.");
    }

    auto currentCommitQuorum = invariantStatusOK(swOnDiskCommitQuorum);
    if (currentCommitQuorum.numNodes == CommitQuorumOptions::kDisabled ||
        newCommitQuorum.numNodes == CommitQuorumOptions::kDisabled) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Commit quorum value can be changed only for index builds "
                          << "with commit quorum enabled, nss: '" << nss.toStringForErrorMsg()
                          << "' first index name: '" << indexNames.front()
                          << "' currentCommitQuorum: " << currentCommitQuorum.toBSON()
                          << " providedCommitQuorum: " << newCommitQuorum.toBSON());
    }

    invariant(shard_role_details::getLocker(opCtx)->isRSTLLocked() ||
              gFeatureFlagIntentRegistration.isEnabled());
    // About to update the commit quorum value on-disk. So, take the lock in exclusive mode to
    // prevent readers from reading the commit quorum value and making decision on commit quorum
    // satisfied with the stale read commit quorum value.
    Lock::ExclusiveLock commitQuorumLk(opCtx, *replState->commitQuorumLock);
    {
        if (auto action = replState->getNextActionNoWait()) {
            return Status(ErrorCodes::CommandFailed,
                          str::stream() << "Commit quorum can't be changed as index build is "
                                           "ready to commit or abort: "
                                        << indexBuildActionToString(*action));
        }
    }

    IndexBuildEntry indexbuildEntry(replState->buildUUID,
                                    replState->collectionUUID,
                                    newCommitQuorum,
                                    toIndexNames(replState->getIndexes()));
    status = indexbuildentryhelpers::persistIndexCommitQuorum(opCtx, indexbuildEntry);
    if (!status.isOK()) {
        return status;
    }

    // Check to see the index build hasn't received commit index build signal while updating
    // the commit quorum value on-disk.
    if (auto action = replState->getNextActionNoWait()) {
        uassert(ErrorCodes::CommandFailed,
                str::stream() << "Commit quorum is already satisfied, this command has no effect",
                *action != IndexBuildAction::kCommitQuorumSatisfied);
    }

    // If the index builder is already waiting for the commit quorum to be satisfied and the commit
    // quorum changes, we need to signal the index builder to make it aware of the change.
    _signalIfCommitQuorumIsSatisfied(opCtx, replState);

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

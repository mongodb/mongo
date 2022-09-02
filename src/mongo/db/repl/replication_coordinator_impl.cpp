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


#define LOGV2_FOR_ELECTION(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                             \
        ID, DLEVEL, {logv2::LogComponent::kReplicationElection}, MESSAGE, ##__VA_ARGS__)

#include "mongo/db/repl/replication_coordinator_impl.h"

#include <algorithm>
#include <fmt/format.h>
#include <functional>
#include <limits>

#include "mongo/base/status.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/mongod_options_storage_gen.h"
#include "mongo/db/prepare_conflict_tracker.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/always_allow_non_local_writes.h"
#include "mongo/db/repl/check_quorum_for_config_change.h"
#include "mongo/db/repl/data_replicator_external_state_initial_sync.h"
#include "mongo/db/repl/hello_response.h"
#include "mongo/db/repl/initial_syncer_factory.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config_checks.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator_impl_gen.h"
#include "mongo/db/repl/replication_metrics.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/shutdown_in_progress_quiesce_info.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/network_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(stepdownHangBeforePerformingPostMemberStateUpdateActions);
MONGO_FAIL_POINT_DEFINE(stepdownHangBeforeRSTLEnqueue);
// Fail setMaintenanceMode with ErrorCodes::NotSecondary to simulate a concurrent election.
MONGO_FAIL_POINT_DEFINE(setMaintenanceModeFailsWithNotSecondary);
MONGO_FAIL_POINT_DEFINE(forceSyncSourceRetryWaitForInitialSync);
// Signals that a hello request has started waiting.
MONGO_FAIL_POINT_DEFINE(waitForHelloResponse);
// Will cause a hello request to hang as it starts waiting.
MONGO_FAIL_POINT_DEFINE(hangWhileWaitingForHelloResponse);
// Will cause a hello request to hang after it times out waiting for a topology change.
MONGO_FAIL_POINT_DEFINE(hangAfterWaitingForTopologyChangeTimesOut);
MONGO_FAIL_POINT_DEFINE(skipDurableTimestampUpdates);
// Skip sending heartbeats to pre-check that a quorum is available before a reconfig.
MONGO_FAIL_POINT_DEFINE(omitConfigQuorumCheck);
// Will cause signal drain complete to hang right before acquiring the RSTL.
MONGO_FAIL_POINT_DEFINE(hangBeforeRSTLOnDrainComplete);
// Will cause signal drain complete to hang before reconfig.
MONGO_FAIL_POINT_DEFINE(hangBeforeReconfigOnDrainComplete);
// Will cause signal drain complete to hang after reconfig.
MONGO_FAIL_POINT_DEFINE(hangAfterReconfigOnDrainComplete);
MONGO_FAIL_POINT_DEFINE(doNotRemoveNewlyAddedOnHeartbeats);
// Will hang right after setting the currentOp info associated with an automatic reconfig.
MONGO_FAIL_POINT_DEFINE(hangDuringAutomaticReconfig);
// Make reconfig command hang before validating new config.
MONGO_FAIL_POINT_DEFINE(ReconfigHangBeforeConfigValidationCheck);
// Blocks after reconfig runs.
MONGO_FAIL_POINT_DEFINE(hangAfterReconfig);
// Allows skipping fetching the config from ping sender.
MONGO_FAIL_POINT_DEFINE(skipBeforeFetchingConfig);
// Hang after grabbing the RSTL but before we start rejecting writes.
MONGO_FAIL_POINT_DEFINE(stepdownHangAfterGrabbingRSTL);
// Hang before making checks on the new config relative to the current one.
MONGO_FAIL_POINT_DEFINE(hangBeforeNewConfigValidationChecks);
// Simulates returning a specified error in the hello response.
MONGO_FAIL_POINT_DEFINE(setCustomErrorInHelloResponseMongoD);
// Throws right before the call into recoverTenantMigrationAccessBlockers.
MONGO_FAIL_POINT_DEFINE(throwBeforeRecoveringTenantMigrationAccessBlockers);

// Number of times we tried to go live as a secondary.
CounterMetric attemptsToBecomeSecondary("repl.apply.attemptsToBecomeSecondary");

// Tracks the last state transition performed in this replica set.
auto& lastStateTransition =
    makeSynchronizedMetric<std::string>("repl.stateTransition.lastStateTransition");

// Tracks the number of operations killed on state transition.
CounterMetric userOpsKilled("repl.stateTransition.userOperationsKilled");

// Tracks the number of operations left running on state transition.
CounterMetric userOpsRunning("repl.stateTransition.userOperationsRunning");

// Tracks the number of times we have successfully performed automatic reconfigs to remove
// 'newlyAdded' fields.
CounterMetric numAutoReconfigsForRemovalOfNewlyAddedFields(
    "repl.reconfig.numAutoReconfigsForRemovalOfNewlyAddedFields");

using namespace fmt::literals;

using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using CallbackFn = executor::TaskExecutor::CallbackFn;
using CallbackHandle = executor::TaskExecutor::CallbackHandle;
using EventHandle = executor::TaskExecutor::EventHandle;
using executor::NetworkInterface;
using NextAction = Fetcher::NextAction;

namespace {

const char kLocalDB[] = "local";

void lockAndCall(stdx::unique_lock<Latch>* lk, const std::function<void()>& fn) {
    if (!lk->owns_lock()) {
        lk->lock();
    }
    fn();
}

template <typename T>
StatusOrStatusWith<T> futureGetNoThrowWithDeadline(OperationContext* opCtx,
                                                   SharedSemiFuture<T>& f,
                                                   Date_t deadline,
                                                   ErrorCodes::Error error) {
    try {
        return opCtx->runWithDeadline(deadline, error, [&] { return f.getNoThrow(opCtx); });
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

constexpr StringData kQuiesceModeShutdownMessage =
    "The server is in quiesce mode and will shut down"_sd;

}  // namespace

void ReplicationCoordinatorImpl::WaiterList::add_inlock(const OpTime& opTime,
                                                        SharedWaiterHandle waiter) {
    _waiters.emplace(opTime, std::move(waiter));
}

SharedSemiFuture<void> ReplicationCoordinatorImpl::WaiterList::add_inlock(
    const OpTime& opTime, boost::optional<WriteConcernOptions> wc) {
    auto pf = makePromiseFuture<void>();
    _waiters.emplace(opTime, std::make_shared<Waiter>(std::move(pf.promise), std::move(wc)));
    return std::move(pf.future);
}

bool ReplicationCoordinatorImpl::WaiterList::remove_inlock(SharedWaiterHandle waiter) {
    for (auto iter = _waiters.begin(); iter != _waiters.end(); iter++) {
        if (iter->second == waiter) {
            _waiters.erase(iter);
            return true;
        }
    }
    return false;
}

template <typename Func>
void ReplicationCoordinatorImpl::WaiterList::setValueIf_inlock(Func&& func,
                                                               boost::optional<OpTime> opTime) {
    for (auto it = _waiters.begin(); it != _waiters.end() && (!opTime || it->first <= *opTime);) {
        const auto& waiter = it->second;
        try {
            if (func(it->first, waiter)) {
                waiter->promise.emplaceValue();
                it = _waiters.erase(it);
            } else {
                ++it;
            }
        } catch (const DBException& e) {
            waiter->promise.setError(e.toStatus());
            it = _waiters.erase(it);
        }
    }
}

void ReplicationCoordinatorImpl::WaiterList::setValueAll_inlock() {
    for (auto& [opTime, waiter] : _waiters) {
        waiter->promise.emplaceValue();
    }
    _waiters.clear();
}

void ReplicationCoordinatorImpl::WaiterList::setErrorAll_inlock(Status status) {
    invariant(!status.isOK());
    for (auto& [opTime, waiter] : _waiters) {
        waiter->promise.setError(status);
    }
    _waiters.clear();
}

namespace {
ReplicationCoordinator::Mode getReplicationModeFromSettings(const ReplSettings& settings) {
    if (settings.usingReplSets()) {
        return ReplicationCoordinator::modeReplSet;
    }
    return ReplicationCoordinator::modeNone;
}

InitialSyncerInterface::Options createInitialSyncerOptions(
    ReplicationCoordinator* replCoord, ReplicationCoordinatorExternalState* externalState) {
    InitialSyncerInterface::Options options;
    options.getMyLastOptime = [replCoord]() { return replCoord->getMyLastAppliedOpTime(); };
    options.setMyLastOptime = [replCoord,
                               externalState](const OpTimeAndWallTime& opTimeAndWallTime) {
        // Note that setting the last applied opTime forward also advances the global timestamp.
        replCoord->setMyLastAppliedOpTimeAndWallTimeForward(opTimeAndWallTime);
        signalOplogWaiters();
        // The oplog application phase of initial sync starts timestamping writes, causing
        // WiredTiger to pin this data in memory. Advancing the oldest timestamp in step with the
        // last applied optime here will permit WiredTiger to evict this data as it sees fit.
        replCoord->getServiceContext()->getStorageEngine()->setOldestTimestamp(
            opTimeAndWallTime.opTime.getTimestamp());
    };
    options.resetOptimes = [replCoord]() { replCoord->resetMyLastOpTimes(); };
    options.syncSourceSelector = replCoord;
    options.oplogFetcherMaxFetcherRestarts =
        externalState->getOplogFetcherInitialSyncMaxFetcherRestarts();

    // If this failpoint is set, override the default sync source retry interval for initial sync.
    forceSyncSourceRetryWaitForInitialSync.execute([&](const BSONObj& data) {
        auto retryMS = data["retryMS"].numberInt();
        options.syncSourceRetryWait = Milliseconds(retryMS);
    });

    return options;
}
}  // namespace

ReplicationCoordinatorImpl::ReplicationCoordinatorImpl(
    ServiceContext* service,
    const ReplSettings& settings,
    std::unique_ptr<ReplicationCoordinatorExternalState> externalState,
    std::unique_ptr<executor::TaskExecutor> executor,
    std::unique_ptr<TopologyCoordinator> topCoord,
    ReplicationProcess* replicationProcess,
    StorageInterface* storage,
    int64_t prngSeed)
    : _service(service),
      _settings(settings),
      _replMode(getReplicationModeFromSettings(settings)),
      _topCoord(std::move(topCoord)),
      _replExecutor(std::move(executor)),
      _externalState(std::move(externalState)),
      _inShutdown(false),
      _memberState(MemberState::RS_STARTUP),
      _rsConfigState(kConfigPreStart),
      _selfIndex(-1),
      _sleptLastElection(false),
      _readWriteAbility(std::make_unique<ReadWriteAbility>(!settings.usingReplSets())),
      _replicationProcess(replicationProcess),
      _storage(storage),
      _handleLivenessTimeoutCallback(_replExecutor.get(),
                                     [this](const executor::TaskExecutor::CallbackArgs& args) {
                                         _handleLivenessTimeout(args);
                                     }),
      _handleElectionTimeoutCallback(
          _replExecutor.get(),
          [this](const executor::TaskExecutor::CallbackArgs&) {
              _startElectSelfIfEligibleV1(StartElectionReasonEnum::kElectionTimeout);
          },
          [this](int64_t limit) { return _nextRandomInt64_inlock(limit); }),
      _random(prngSeed) {

    _termShadow.store(OpTime::kUninitializedTerm);

    invariant(_service);

    if (!isReplEnabled()) {
        return;
    }

    // If this is a config server, then we set the periodic no-op interval to 1 second. This is to
    // ensure that the config server will not unduly hold up change streams running on the cluster.
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        periodicNoopIntervalSecs.store(1);
    }

    _externalState->setupNoopWriter(Seconds(periodicNoopIntervalSecs.load()));
}

ReplicationCoordinatorImpl::~ReplicationCoordinatorImpl() = default;

void ReplicationCoordinatorImpl::waitForStartUpComplete_forTest() {
    _waitForStartUpComplete();
}

void ReplicationCoordinatorImpl::_waitForStartUpComplete() {
    CallbackHandle handle;
    {
        stdx::unique_lock<Latch> lk(_mutex);
        while (_rsConfigState == kConfigPreStart || _rsConfigState == kConfigStartingUp) {
            _rsConfigStateChange.wait(lk);
        }
        handle = _finishLoadLocalConfigCbh;
    }
    if (handle.isValid()) {
        _replExecutor->wait(handle);
    }
}

ReplSetConfig ReplicationCoordinatorImpl::getReplicaSetConfig_forTest() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _rsConfig;
}

Date_t ReplicationCoordinatorImpl::getElectionTimeout_forTest() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _handleElectionTimeoutCallback.getNextCall();
}

Milliseconds ReplicationCoordinatorImpl::getRandomizedElectionOffset_forTest() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _getRandomizedElectionOffset_inlock();
}

boost::optional<Date_t> ReplicationCoordinatorImpl::getPriorityTakeover_forTest() const {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_priorityTakeoverCbh.isValid()) {
        return boost::none;
    }
    return _priorityTakeoverWhen;
}

boost::optional<Date_t> ReplicationCoordinatorImpl::getCatchupTakeover_forTest() const {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_catchupTakeoverCbh.isValid()) {
        return boost::none;
    }
    return _catchupTakeoverWhen;
}

executor::TaskExecutor::CallbackHandle ReplicationCoordinatorImpl::getCatchupTakeoverCbh_forTest()
    const {
    return _catchupTakeoverCbh;
}

OpTime ReplicationCoordinatorImpl::getCurrentCommittedSnapshotOpTime() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _getCurrentCommittedSnapshotOpTime_inlock();
}

OpTime ReplicationCoordinatorImpl::_getCurrentCommittedSnapshotOpTime_inlock() const {
    return _currentCommittedSnapshot.value_or(OpTime());
}

void ReplicationCoordinatorImpl::appendDiagnosticBSON(mongo::BSONObjBuilder* bob) {
    BSONObjBuilder eBuilder(bob->subobjStart("executor"));
    _replExecutor->appendDiagnosticBSON(&eBuilder);
}

void ReplicationCoordinatorImpl::appendConnectionStats(executor::ConnectionPoolStats* stats) const {
    _replExecutor->appendConnectionStats(stats);
}

bool ReplicationCoordinatorImpl::_startLoadLocalConfig(
    OperationContext* opCtx, StorageEngine::LastShutdownState lastShutdownState) {
    LOGV2(4280500, "Attempting to create internal replication collections");
    // Create necessary replication collections to guarantee that if a checkpoint sees data after
    // initial sync has completed, it also sees these collections.
    fassert(50708, _replicationProcess->getConsistencyMarkers()->createInternalCollections(opCtx));

    // Ensure (update if needed) the in-memory count for the oplogTruncateAfterPoint collection
    // matches the collection contents.
    _replicationProcess->getConsistencyMarkers()->ensureFastCountOnOplogTruncateAfterPoint(opCtx);

    _replicationProcess->getConsistencyMarkers()->initializeMinValidDocument(opCtx);

    fassert(51240, _externalState->createLocalLastVoteCollection(opCtx));

    LOGV2(4280501, "Attempting to load local voted for document");
    StatusWith<LastVote> lastVote = _externalState->loadLocalLastVoteDocument(opCtx);
    if (!lastVote.isOK()) {
        LOGV2_FATAL_NOTRACE(40367,
                            "Error loading local voted for document at startup; {error}",
                            "Error loading local voted for document at startup",
                            "error"_attr = lastVote.getStatus());
    }
    if (lastVote.getValue().getTerm() == OpTime::kInitialTerm) {
        // This log line is checked in unit tests.
        LOGV2(21311, "Did not find local initialized voted for document at startup");
    }
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _topCoord->loadLastVote(lastVote.getValue());
    }

    LOGV2(4280502, "Searching for local Rollback ID document");
    // Check that we have a local Rollback ID. If we do not have one, create one.
    auto status = _replicationProcess->refreshRollbackID(opCtx);
    if (!status.isOK()) {
        if (status == ErrorCodes::NamespaceNotFound) {
            LOGV2(21312, "Did not find local Rollback ID document at startup. Creating one");
            auto initializingStatus = _replicationProcess->initializeRollbackID(opCtx);
            fassert(40424, initializingStatus);
        } else {
            LOGV2_FATAL_NOTRACE(40428,
                                "Error loading local Rollback ID document at startup; {error}",
                                "Error loading local Rollback ID document at startup",
                                "error"_attr = status);
        }
    } else if (lastShutdownState == StorageEngine::LastShutdownState::kUnclean) {
        LOGV2(501401, "Incrementing the rollback ID after unclean shutdown");
        fassert(501402, _replicationProcess->incrementRollbackID(opCtx));
    }

    LOGV2_DEBUG(4280503, 1, "Attempting to load local replica set configuration document");
    StatusWith<BSONObj> cfg = _externalState->loadLocalConfigDocument(opCtx);
    if (!cfg.isOK()) {
        LOGV2(21313,
              "Did not find local replica set configuration document at startup;  {error}",
              "Did not find local replica set configuration document at startup",
              "error"_attr = cfg.getStatus());
        return true;
    }
    ReplSetConfig localConfig;
    try {
        localConfig = ReplSetConfig::parse(cfg.getValue());
    } catch (const DBException& e) {
        auto status = e.toStatus();
        if (status.code() == ErrorCodes::RepairedReplicaSetNode) {
            LOGV2_FATAL_NOTRACE(
                50923,
                "This instance has been repaired and may contain modified replicated data that "
                "would not match other replica set members. To see your repaired data, start "
                "mongod without the --replSet option. When you are finished recovering your "
                "data and would like to perform a complete re-sync, please refer to the "
                "documentation here: "
                "https://docs.mongodb.com/manual/tutorial/resync-replica-set-member/");
        }
        LOGV2_FATAL_NOTRACE(
            28545,
            "Locally stored replica set configuration does not parse; See "
            "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config "
            "for information on how to recover from this. Got \"{error}\" while parsing "
            "{config}",
            "Locally stored replica set configuration does not parse; See "
            "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config "
            "for information on how to recover from this",
            "error"_attr = status,
            "config"_attr = cfg.getValue());
    }

    LOGV2(4280504, "Cleaning up any partially applied oplog batches & reading last op from oplog");
    // Read the last op from the oplog after cleaning up any partially applied batches.
    const auto stableTimestamp = boost::none;
    _replicationProcess->getReplicationRecovery()->recoverFromOplog(opCtx, stableTimestamp);
    LOGV2(4280505,
          "Creating any necessary TenantMigrationAccessBlockers for unfinished migrations");

    if (MONGO_unlikely(throwBeforeRecoveringTenantMigrationAccessBlockers.shouldFail())) {
        uasserted(6111700,
                  "Failpoint 'throwBeforeRecoveringTenantMigrationAccessBlockers' triggered. "
                  "Throwing exception.");
    }

    tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx);
    LOGV2(4280506, "Reconstructing prepared transactions");
    reconstructPreparedTransactions(opCtx, OplogApplication::Mode::kRecovering);

    const auto lastOpTimeAndWallTimeResult = _externalState->loadLastOpTimeAndWallTime(opCtx);

    // Use a callback here, because _finishLoadLocalConfig calls isself() which requires
    // that the server's networking layer be up and running and accepting connections, which
    // doesn't happen until startReplication finishes.
    auto handle =
        _replExecutor->scheduleWork([=](const executor::TaskExecutor::CallbackArgs& args) {
            _finishLoadLocalConfig(args, localConfig, lastOpTimeAndWallTimeResult, lastVote);
        });
    if (handle == ErrorCodes::ShutdownInProgress) {
        handle = CallbackHandle{};
    }
    fassert(40446, handle);
    stdx::lock_guard<Latch> lk(_mutex);
    _finishLoadLocalConfigCbh = std::move(handle.getValue());

    LOGV2(4280507, "Loaded replica set config, scheduled callback to set local config");
    return false;
}

void ReplicationCoordinatorImpl::_createHorizonTopologyChangePromiseMapping(WithLock) {
    auto horizonMappings = _rsConfig.getMemberAt(_selfIndex).getHorizonMappings();
    // Create a new horizon to promise mapping since it is possible for the horizons
    // to change after a replica set reconfig.
    _horizonToTopologyChangePromiseMap.clear();
    for (auto const& [horizon, hostAndPort] : horizonMappings) {
        _horizonToTopologyChangePromiseMap.emplace(
            horizon, std::make_shared<SharedPromise<std::shared_ptr<const HelloResponse>>>());
    }
}

void ReplicationCoordinatorImpl::_finishLoadLocalConfig(
    const executor::TaskExecutor::CallbackArgs& cbData,
    const ReplSetConfig& localConfig,
    const StatusWith<OpTimeAndWallTime>& lastOpTimeAndWallTimeStatus,
    const StatusWith<LastVote>& lastVoteStatus) {
    if (!cbData.status.isOK()) {
        LOGV2_DEBUG(21314,
                    1,
                    "Loading local replica set configuration failed due to {error}",
                    "Loading local replica set configuration failed",
                    "error"_attr = cbData.status);
        return;
    }

    LOGV2(4280508, "Attempting to set local replica set config; validating config for startup");
    StatusWith<int> myIndex =
        validateConfigForStartUp(_externalState.get(), localConfig, getServiceContext());
    if (!myIndex.isOK()) {
        if (myIndex.getStatus() == ErrorCodes::NodeNotFound ||
            myIndex.getStatus() == ErrorCodes::InvalidReplicaSetConfig) {
            LOGV2_WARNING(21405,
                          "Locally stored replica set configuration does not have a valid entry "
                          "for the current node; waiting for reconfig or remote heartbeat; Got "
                          "\"{error}\" while validating {localConfig}",
                          "Locally stored replica set configuration does not have a valid entry "
                          "for the current node; waiting for reconfig or remote heartbeat",
                          "error"_attr = myIndex.getStatus(),
                          "localConfig"_attr = localConfig.toBSON());
            myIndex = StatusWith<int>(-1);
        } else {
            LOGV2_ERROR(21415,
                        "Locally stored replica set configuration is invalid; See "
                        "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config"
                        " for information on how to recover from this. Got \"{error}\" "
                        "while validating {localConfig}",
                        "Locally stored replica set configuration is invalid; See "
                        "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config"
                        " for information on how to recover from this",
                        "error"_attr = myIndex.getStatus(),
                        "localConfig"_attr = localConfig.toBSON());
            fassertFailedNoTrace(28544);
        }
    }

    if (!_settings.isServerless() && localConfig.getReplSetName() != _settings.ourSetName()) {
        LOGV2_WARNING(21406,
                      "Local replica set configuration document reports set name of "
                      "{localConfigSetName}, but command line reports "
                      "{commandLineSetName}; waiting for reconfig or remote heartbeat",
                      "Local replica set configuration document set name differs from command line "
                      "set name; waiting for reconfig or remote heartbeat",
                      "localConfigSetName"_attr = localConfig.getReplSetName(),
                      "commandLineSetName"_attr = _settings.ourSetName());
        myIndex = StatusWith<int>(-1);
    }
    LOGV2(4280509, "Local configuration validated for startup");

    // Do not check optime, if this node is an arbiter.
    bool isArbiter =
        myIndex.getValue() != -1 && localConfig.getMemberAt(myIndex.getValue()).isArbiter();
    OpTimeAndWallTime lastOpTimeAndWallTime = OpTimeAndWallTime();
    if (!isArbiter) {
        if (!lastOpTimeAndWallTimeStatus.isOK()) {
            LOGV2_WARNING(
                21407,
                "Failed to load timestamp and/or wall clock time of most recently applied "
                "operation: {error}",
                "Failed to load timestamp and/or wall clock time of most recently applied "
                "operation",
                "error"_attr = lastOpTimeAndWallTimeStatus.getStatus());
        } else {
            lastOpTimeAndWallTime = lastOpTimeAndWallTimeStatus.getValue();
        }
    } else {
        ReplicaSetAwareServiceRegistry::get(_service).onBecomeArbiter();
    }

    const auto lastOpTime = lastOpTimeAndWallTime.opTime;
    // Restore the current term according to the terms of last oplog entry and last vote.
    // The initial term of OpTime() is 0.
    long long term = lastOpTime.getTerm();
    if (lastVoteStatus.isOK()) {
        long long lastVoteTerm = lastVoteStatus.getValue().getTerm();
        if (term < lastVoteTerm) {
            term = lastVoteTerm;
        }
    }

    auto opCtx = cc().makeOperationContext();
    if (!lastOpTime.isNull()) {

        // If we have an oplog, it is still possible that our data is not in a consistent state. For
        // example, if we are starting up after a crash following a post-rollback RECOVERING state.
        // To detect this, we see if our last optime is >= the 'minValid' optime, which
        // should be persistent across node crashes.
        OpTime minValid = _replicationProcess->getConsistencyMarkers()->getMinValid(opCtx.get());

        // It is not safe to take stable checkpoints until we reach minValid, so we set our
        // initialDataTimestamp to prevent this. It is expected that this is only necessary when
        // enableMajorityReadConcern:false.
        if (lastOpTime < minValid) {
            LOGV2_DEBUG(4916700,
                        2,
                        "Setting initialDataTimestamp to minValid since our last optime is less "
                        "than minValid",
                        "lastOpTime"_attr = lastOpTime,
                        "minValid"_attr = minValid);
            _storage->setInitialDataTimestamp(getServiceContext(), minValid.getTimestamp());
        }
    }

    // Update the global timestamp before setting the last applied opTime forward so the last
    // applied optime is never greater than the latest cluster time in the logical clock.
    _externalState->setGlobalTimestamp(getServiceContext(), lastOpTime.getTimestamp());

    stdx::unique_lock<Latch> lock(_mutex);
    invariant(_rsConfigState == kConfigStartingUp);
    const PostMemberStateUpdateAction action =
        _setCurrentRSConfig(lock, opCtx.get(), localConfig, myIndex.getValue());

    // Set our last applied and durable optimes to the top of the oplog, if we have one.
    if (!lastOpTime.isNull()) {
        LOGV2_DEBUG(4280510,
                    1,
                    "Setting this node's last applied and durable opTimes to the top of the oplog");
        bool isRollbackAllowed = false;
        _setMyLastAppliedOpTimeAndWallTime(lock, lastOpTimeAndWallTime, isRollbackAllowed);
        _setMyLastDurableOpTimeAndWallTime(lock, lastOpTimeAndWallTime, isRollbackAllowed);
        _reportUpstream_inlock(std::move(lock));  // unlocks _mutex.
    } else {
        lock.unlock();
    }

    {
        stdx::lock_guard<Latch> lk(_mutex);
        // Step down is impossible, so we don't need to wait for the returned event.
        _updateTerm_inlock(term);
    }
    LOGV2(21320, "Current term is now {term}", "Updated term", "term"_attr = term);
    _performPostMemberStateUpdateAction(action);

    if (!isArbiter && myIndex.getValue() != -1) {
        _externalState->startThreads();
        _startDataReplication(opCtx.get());
    }
    LOGV2(4280511, "Set local replica set config");
}

void ReplicationCoordinatorImpl::_startInitialSync(
    OperationContext* opCtx,
    InitialSyncerInterface::OnCompletionFn onCompletion,
    bool fallbackToLogical) {
    std::shared_ptr<InitialSyncerInterface> initialSyncerCopy;

    // Initial sync may take locks during startup; make sure there is no possibility of conflict.
    dassert(!opCtx->lockState()->isLocked());
    try {
        {
            // Must take the lock to set _initialSyncer, but not call it.
            stdx::lock_guard<Latch> lock(_mutex);
            if (_inShutdown || _inTerminalShutdown) {
                LOGV2(21326, "Initial Sync not starting because replication is shutting down");
                return;
            }

            auto initialSyncerFactory = InitialSyncerFactory::get(opCtx->getServiceContext());
            auto createInitialSyncer = [&](const std::string& method) {
                return initialSyncerFactory->makeInitialSyncer(
                    method,
                    createInitialSyncerOptions(this, _externalState.get()),
                    std::make_unique<DataReplicatorExternalStateInitialSync>(this,
                                                                             _externalState.get()),
                    _externalState->getDbWorkThreadPool(),
                    _storage,
                    _replicationProcess,
                    onCompletion);
            };

            if (!fallbackToLogical) {
                auto swInitialSyncer = createInitialSyncer(initialSyncMethod);
                if (swInitialSyncer.getStatus().code() == ErrorCodes::NotImplemented &&
                    initialSyncMethod != "logical") {
                    LOGV2_WARNING(58154,
                                  "No such initial sync method was available. Falling back to "
                                  "logical initial sync.",
                                  "initialSyncMethod"_attr = initialSyncMethod,
                                  "error"_attr = swInitialSyncer.getStatus().reason());
                    swInitialSyncer = createInitialSyncer(std::string("logical"));
                }
                initialSyncerCopy = uassertStatusOK(swInitialSyncer);
            } else {
                auto swInitialSyncer = createInitialSyncer(std::string("logical"));
                initialSyncerCopy = uassertStatusOK(swInitialSyncer);
            }
            _initialSyncer = initialSyncerCopy;
        }
        // InitialSyncer::startup() must be called outside lock because it uses features (eg.
        // setting the initial sync flag) which depend on the ReplicationCoordinatorImpl.
        uassertStatusOK(initialSyncerCopy->startup(opCtx, numInitialSyncAttempts.load()));
        LOGV2(4280514, "Initial sync started");
    } catch (const DBException& e) {
        auto status = e.toStatus();
        LOGV2(21327,
              "Initial Sync failed to start: {error}",
              "Initial Sync failed to start",
              "error"_attr = status);
        if (ErrorCodes::CallbackCanceled == status || ErrorCodes::isShutdownError(status.code())) {
            return;
        }
        fassertFailedWithStatusNoTrace(40354, status);
    }
}

void ReplicationCoordinatorImpl::_initialSyncerCompletionFunction(
    const StatusWith<OpTimeAndWallTime>& opTimeStatus) {
    {
        stdx::unique_lock<Latch> lock(_mutex);
        if (opTimeStatus == ErrorCodes::CallbackCanceled) {
            LOGV2(21324,
                  "Initial Sync has been cancelled: {error}",
                  "Initial Sync has been cancelled",
                  "error"_attr = opTimeStatus.getStatus());
            return;
        } else if (!opTimeStatus.isOK()) {
            if (_inShutdown || _inTerminalShutdown) {
                LOGV2(21325,
                      "Initial Sync failed during shutdown due to {error}",
                      "Initial Sync failed during shutdown",
                      "error"_attr = opTimeStatus.getStatus());
                return;
            } else if (opTimeStatus == ErrorCodes::InvalidSyncSource &&
                       _initialSyncer->getInitialSyncMethod() != "logical") {
                LOGV2(5780600,
                      "Falling back to logical initial sync: {error}",
                      "Falling back to logical initial sync",
                      "error"_attr = opTimeStatus.getStatus());
                lock.unlock();
                clearSyncSourceDenylist();
                _scheduleWorkAt(_replExecutor->now(),
                                [=](const mongo::executor::TaskExecutor::CallbackArgs& cbData) {
                                    _startInitialSync(
                                        cc().makeOperationContext().get(),
                                        [this](const StatusWith<OpTimeAndWallTime>& opTimeStatus) {
                                            _initialSyncerCompletionFunction(opTimeStatus);
                                        },
                                        true /* fallbackToLogical */);
                                });
                return;
            } else {
                LOGV2_ERROR(21416,
                            "Initial sync failed, shutting down now. Restart the server "
                            "to attempt a new initial sync");
                fassertFailedWithStatusNoTrace(40088, opTimeStatus.getStatus());
            }
        }


        const auto lastApplied = opTimeStatus.getValue();
        _setMyLastAppliedOpTimeAndWallTime(lock, lastApplied, false);
        signalOplogWaiters();

        _topCoord->resetMaintenanceCount();
    }

    ReplicaSetAwareServiceRegistry::get(_service).onInitialDataAvailable(
        cc().makeOperationContext().get(), false /* isMajorityDataAvailable */);

    // Transition from STARTUP2 to RECOVERING and start the producer and the applier.
    // If the member state is REMOVED, this will do nothing until we receive a config with
    // ourself in it.
    const auto memberState = getMemberState();
    invariant(memberState.startup2() || memberState.removed());
    invariant(setFollowerMode(MemberState::RS_RECOVERING));
    auto opCtxHolder = cc().makeOperationContext();
    _externalState->startSteadyStateReplication(opCtxHolder.get(), this);
    // This log is used in tests to ensure we made it to this point.
    LOGV2_DEBUG(4853000, 1, "initial sync complete.");
}

void ReplicationCoordinatorImpl::_startDataReplication(OperationContext* opCtx) {
    if (_startedSteadyStateReplication.swap(true)) {
        // This is not the first call.
        return;
    }

    // Make sure we're not holding any locks; existing locks might conflict with operations
    // we take during initial sync or replication steady state startup.
    dassert(!opCtx->lockState()->isLocked());

    // Check to see if we need to do an initial sync.
    const auto lastOpTime = getMyLastAppliedOpTime();
    const auto needsInitialSync =
        lastOpTime.isNull() || _externalState->isInitialSyncFlagSet(opCtx);
    if (!needsInitialSync) {
        LOGV2(4280512, "No initial sync required. Attempting to begin steady replication");
        // Start steady replication, since we already have data.
        // ReplSetConfig has been installed, so it's either in STARTUP2 or REMOVED.
        auto memberState = getMemberState();
        invariant(memberState.startup2() || memberState.removed());
        invariant(setFollowerMode(MemberState::RS_RECOVERING));
        // Set an initial sync ID, in case we were upgraded or restored from backup without doing
        // an initial sync.
        _replicationProcess->getConsistencyMarkers()->setInitialSyncIdIfNotSet(opCtx);
        _externalState->startSteadyStateReplication(opCtx, this);
        return;
    }

    LOGV2(4280513, "Initial sync required. Attempting to start initial sync...");
    // Do initial sync.
    if (!_externalState->getTaskExecutor()) {
        LOGV2(21323, "Not running initial sync during test");
        return;
    }

    _startInitialSync(opCtx, [this](const StatusWith<OpTimeAndWallTime>& opTimeStatus) {
        _initialSyncerCompletionFunction(opTimeStatus);
    });
}

void ReplicationCoordinatorImpl::startup(OperationContext* opCtx,
                                         StorageEngine::LastShutdownState lastShutdownState) {
    if (!isReplEnabled()) {
        if (ReplSettings::shouldRecoverFromOplogAsStandalone()) {
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "Cannot set parameter 'recoverToOplogTimestamp' "
                                  << "when recovering from the oplog as a standalone",
                    recoverToOplogTimestamp.empty());
            _replicationProcess->getReplicationRecovery()->recoverFromOplogAsStandalone(opCtx);
        }

        if (storageGlobalParams.queryableBackupMode && !recoverToOplogTimestamp.empty()) {
            BSONObj recoverToTimestampObj = fromjson(recoverToOplogTimestamp);
            uassert(ErrorCodes::BadValue,
                    str::stream() << "'recoverToOplogTimestamp' needs to have a 'timestamp' field",
                    recoverToTimestampObj.hasField("timestamp"));

            Timestamp recoverToTimestamp = recoverToTimestampObj.getField("timestamp").timestamp();
            uassert(ErrorCodes::BadValue,
                    str::stream() << "'recoverToOplogTimestamp' needs to be a valid timestamp",
                    !recoverToTimestamp.isNull());

            StatusWith<BSONObj> cfg = _externalState->loadLocalConfigDocument(opCtx);
            uassert(ErrorCodes::InvalidReplicaSetConfig,
                    str::stream()
                        << "No replica set config document was found, 'recoverToOplogTimestamp' "
                        << "must be used with a node that was previously part of a replica set",
                    cfg.isOK());

            // Need to perform replication recovery up to and including the given timestamp.
            _replicationProcess->getReplicationRecovery()->recoverFromOplogUpTo(opCtx,
                                                                                recoverToTimestamp);
        }

        stdx::lock_guard<Latch> lk(_mutex);
        _setConfigState_inlock(kConfigReplicationDisabled);
        return;
    }

    invariant(_settings.usingReplSets());
    invariant(!ReplSettings::shouldRecoverFromOplogAsStandalone());

    _storage->initializeStorageControlsForReplication(opCtx->getServiceContext());

    // We are expected to be able to transition out of the kConfigStartingUp state by the end
    // of this function. Any uncaught exceptions here leave us in an invalid state and we will
    // not be able to shut down by normal means, as clean shutdown assumes we can leave that state.
    try {
        {
            stdx::lock_guard<Latch> lk(_mutex);
            fassert(18822, !_inShutdown);
            _setConfigState_inlock(kConfigStartingUp);
            _topCoord->setStorageEngineSupportsReadCommitted(
                _externalState->isReadCommittedSupportedByStorageEngine(opCtx));
        }

        // Initialize the cached pointer to the oplog collection.
        acquireOplogCollectionForLogging(opCtx);

        _replExecutor->startup();

        LOGV2(6005300, "Starting up replica set aware services");
        ReplicaSetAwareServiceRegistry::get(_service).onStartup(opCtx);

        bool doneLoadingConfig = _startLoadLocalConfig(opCtx, lastShutdownState);
        if (doneLoadingConfig) {
            // If we're not done loading the config, then the config state will be set by
            // _finishLoadLocalConfig.
            stdx::lock_guard<Latch> lk(_mutex);
            invariant(!_rsConfig.isInitialized());
            _setConfigState_inlock(kConfigUninitialized);
        }
    } catch (DBException& e) {
        auto status = e.toStatus();
        LOGV2_FATAL_NOTRACE(
            6111701, "Failed to load local replica set config on startup", "status"_attr = status);
    }
}

void ReplicationCoordinatorImpl::_setImplicitDefaultWriteConcern(OperationContext* opCtx,
                                                                 WithLock lk) {
    auto& rwcDefaults = ReadWriteConcernDefaults::get(opCtx);
    bool isImplicitDefaultWriteConcernMajority = _rsConfig.isImplicitDefaultWriteConcernMajority();
    rwcDefaults.setImplicitDefaultWriteConcernMajority(isImplicitDefaultWriteConcernMajority);
}

void ReplicationCoordinatorImpl::enterTerminalShutdown() {
    std::shared_ptr<InitialSyncerInterface> initialSyncerCopy;
    {
        stdx::lock_guard lk(_mutex);
        _inTerminalShutdown = true;
        initialSyncerCopy = _initialSyncer;
    }
    // Shutting down the initial syncer early works around an issue where an initial syncer may not
    // be able to shut down with an opCtx active.  No opCtx is active when enterTerminalShutdown is
    // called due to a signal.
    if (initialSyncerCopy) {
        const auto status = initialSyncerCopy->shutdown();
        if (!status.isOK()) {
            LOGV2_WARNING(6137700,
                          "InitialSyncer shutdown failed: {error}",
                          "InitialSyncer shutdown failed",
                          "error"_attr = status);
        }
    }
}

bool ReplicationCoordinatorImpl::enterQuiesceModeIfSecondary(Milliseconds quiesceTime) {
    LOGV2_INFO(4794602, "Attempting to enter quiesce mode");

    stdx::lock_guard lk(_mutex);

    if (!_memberState.secondary()) {
        return false;
    }

    _inQuiesceMode = true;
    _quiesceDeadline = _replExecutor->now() + quiesceTime;

    // Increment the topology version and respond to all waiting hello requests with an error.
    _fulfillTopologyChangePromise(lk);

    return true;
}

bool ReplicationCoordinatorImpl::inQuiesceMode() const {
    stdx::lock_guard lk(_mutex);
    return _inQuiesceMode;
}

void ReplicationCoordinatorImpl::shutdown(OperationContext* opCtx) {
    // Shutdown must:
    // * prevent new threads from blocking in awaitReplication
    // * wake up all existing threads blocking in awaitReplication
    // * Shut down and join the execution resources it owns.

    if (!_settings.usingReplSets()) {
        return;
    }

    LOGV2(5074000, "Shutting down the replica set aware services.");
    ReplicaSetAwareServiceRegistry::get(_service).onShutdown();

    LOGV2(21328, "Shutting down replication subsystems");

    // Used to shut down outside of the lock.
    std::shared_ptr<InitialSyncerInterface> initialSyncerCopy;
    {
        stdx::unique_lock<Latch> lk(_mutex);
        fassert(28533, !_inShutdown);
        _inShutdown = true;
        if (_rsConfigState == kConfigPreStart) {
            LOGV2_WARNING(21409,
                          "ReplicationCoordinatorImpl::shutdown() called before startup() "
                          "finished. Shutting down without cleaning up the replication system");
            return;
        }
        if (_rsConfigState == kConfigStartingUp) {
            // Wait until we are finished starting up, so that we can cleanly shut everything down.
            lk.unlock();
            _waitForStartUpComplete();
            lk.lock();
            fassert(18823, _rsConfigState != kConfigStartingUp);
        }
        _replicationWaiterList.setErrorAll_inlock(
            {ErrorCodes::ShutdownInProgress, "Replication is being shut down"});
        _opTimeWaiterList.setErrorAll_inlock(
            {ErrorCodes::ShutdownInProgress, "Replication is being shut down"});
        _currentCommittedSnapshotCond.notify_all();
        _initialSyncer.swap(initialSyncerCopy);
    }

    // joining the replication executor is blocking so it must be run outside of the mutex
    if (initialSyncerCopy) {
        LOGV2_DEBUG(
            21329, 1, "ReplicationCoordinatorImpl::shutdown calling InitialSyncer::shutdown");
        const auto status = initialSyncerCopy->shutdown();
        if (!status.isOK()) {
            LOGV2_WARNING(21410,
                          "InitialSyncer shutdown failed: {error}",
                          "InitialSyncer shutdown failed",
                          "error"_attr = status);
        }
        initialSyncerCopy->join();
        initialSyncerCopy.reset();
    }

    {
        stdx::unique_lock<Latch> lk(_mutex);
        if (_finishedDrainingPromise) {
            _finishedDrainingPromise->setError(
                {ErrorCodes::InterruptedAtShutdown,
                 "Cancelling wait for drain mode to complete due to shutdown"});
            _finishedDrainingPromise = boost::none;
        }
    }

    _externalState->shutdown(opCtx);
    _replExecutor->shutdown();
    _replExecutor->join();
}

const ReplSettings& ReplicationCoordinatorImpl::getSettings() const {
    return _settings;
}

ReplicationCoordinator::Mode ReplicationCoordinatorImpl::getReplicationMode() const {
    return _replMode;
}

MemberState ReplicationCoordinatorImpl::getMemberState() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _getMemberState_inlock();
}

std::vector<MemberData> ReplicationCoordinatorImpl::getMemberData() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _topCoord->getMemberData();
}

MemberState ReplicationCoordinatorImpl::_getMemberState_inlock() const {
    return _memberState;
}

Status ReplicationCoordinatorImpl::waitForMemberState(Interruptible* interruptible,
                                                      MemberState expectedState,
                                                      Milliseconds timeout) {
    if (timeout < Milliseconds(0)) {
        return Status(ErrorCodes::BadValue, "Timeout duration cannot be negative");
    }

    stdx::unique_lock<Latch> lk(_mutex);
    auto pred = [this, expectedState]() { return _memberState == expectedState; };
    if (!interruptible->waitForConditionOrInterruptFor(_memberStateChange, lk, timeout, pred)) {
        return Status(ErrorCodes::ExceededTimeLimit,
                      str::stream()
                          << "Timed out waiting for state to become " << expectedState.toString()
                          << ". Current state is " << _memberState.toString());
    }
    return Status::OK();
}

Seconds ReplicationCoordinatorImpl::getSecondaryDelaySecs() const {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_rsConfig.isInitialized());
    if (_selfIndex == -1) {
        // We aren't currently in the set. Return 0 seconds so we can clear out the applier's
        // queue of work.
        return Seconds(0);
    }
    return _rsConfig.getMemberAt(_selfIndex).getSecondaryDelay();
}

void ReplicationCoordinatorImpl::clearSyncSourceDenylist() {
    stdx::lock_guard<Latch> lk(_mutex);
    _topCoord->clearSyncSourceDenylist();
}

Status ReplicationCoordinatorImpl::setFollowerModeRollback(OperationContext* opCtx) {
    invariant(opCtx);
    invariant(opCtx->lockState()->isRSTLExclusive());
    return _setFollowerMode(opCtx, MemberState::RS_ROLLBACK);
}

Status ReplicationCoordinatorImpl::setFollowerMode(const MemberState& newState) {
    // Switching to rollback should call setFollowerModeRollback instead.
    invariant(newState != MemberState::RS_ROLLBACK);
    return _setFollowerMode(nullptr, newState);
}

Status ReplicationCoordinatorImpl::_setFollowerMode(OperationContext* opCtx,
                                                    const MemberState& newState) {
    stdx::unique_lock<Latch> lk(_mutex);
    if (newState == _topCoord->getMemberState()) {
        return Status::OK();
    }
    if (_topCoord->getRole() == TopologyCoordinator::Role::kLeader) {
        return Status(ErrorCodes::NotSecondary,
                      "Cannot set follower mode when node is currently the leader");
    }

    if (auto electionFinishedEvent = _cancelElectionIfNeeded(lk)) {
        // We were a candidate, which means _topCoord believed us to be in state RS_SECONDARY, and
        // we know that newState != RS_SECONDARY because we would have returned early, above if
        // the old and new state were equal. So, try again after the election is over to
        // finish setting the follower mode.  We cannot wait for the election to finish here as we
        // may be holding a global X lock, so we return a bad status and rely on the caller to
        // retry.
        return Status(ErrorCodes::ElectionInProgress,
                      str::stream() << "Cannot set follower mode to " << newState.toString()
                                    << " because we are in the middle of running an election");
    }

    _topCoord->setFollowerMode(newState.s);

    if (_memberState.secondary() && newState == MemberState::RS_ROLLBACK) {
        // If we are switching out of SECONDARY and to ROLLBACK, we must make sure that we hold the
        // RSTL in mode X to prevent readers that have the RSTL in intent mode from reading.
        _readWriteAbility->setCanServeNonLocalReads(opCtx, 0U);
    }

    const PostMemberStateUpdateAction action = _updateMemberStateFromTopologyCoordinator(lk);
    lk.unlock();
    _performPostMemberStateUpdateAction(action);

    return Status::OK();
}

ReplicationCoordinator::ApplierState ReplicationCoordinatorImpl::getApplierState() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _applierState;
}

void ReplicationCoordinatorImpl::signalDrainComplete(OperationContext* opCtx,
                                                     long long termWhenBufferIsEmpty) noexcept {
    {
        stdx::unique_lock<Latch> lk(_mutex);
        if (_applierState == ReplicationCoordinator::ApplierState::DrainingForShardSplit) {
            _applierState = ApplierState::Stopped;
            auto memberState = _getMemberState_inlock();
            invariant(memberState.secondary() || memberState.startup());
            _externalState->onDrainComplete(opCtx);

            if (_finishedDrainingPromise) {
                _finishedDrainingPromise->emplaceValue();
                _finishedDrainingPromise = boost::none;
            }

            return;
        }
    }

    // This logic is a little complicated in order to avoid acquiring the RSTL in mode X
    // unnecessarily.  This is important because the applier may call signalDrainComplete()
    // whenever it wants, not only when the ReplicationCoordinator is expecting it.
    //
    // The steps are:
    // 1.) Check to see if we're waiting for this signal.  If not, return early.
    // 2.) Otherwise, release the mutex while acquiring the RSTL in mode X, since that might take a
    //     while (NB there's a deadlock cycle otherwise, too).
    // 3.) Re-check to see if we've somehow left drain mode.  If we have not, clear
    //     producer and applier's states, set the flag allowing non-local database writes and
    //     drop the mutex.  At this point, no writes can occur from other threads, due to the RSTL
    //     in mode X.
    // 4.) Drop all temp collections, and log the drops to the oplog.
    // 5.) Log transition to primary in the oplog and set that OpTime as the floor for what we will
    //     consider to be committed.
    // 6.) Drop the RSTL.
    //
    // Because replicatable writes are forbidden while in drain mode, and we don't exit drain
    // mode until we have the RSTL in mode X, which forbids all other threads from making
    // writes, we know that from the time that _canAcceptNonLocalWrites is set until
    // this method returns, no external writes will be processed.  This is important so that a new
    // temp collection isn't introduced on the new primary before we drop all the temp collections.

    // When we go to drop all temp collections, we must replicate the drops.
    invariant(opCtx->writesAreReplicated());

    stdx::unique_lock<Latch> lk(_mutex);
    if (_applierState != ApplierState::Draining) {
        LOGV2(6015306, "Applier already left draining state, exiting.");
        return;
    }
    lk.unlock();

    _externalState->onDrainComplete(opCtx);
    ReplicaSetAwareServiceRegistry::get(_service).onStepUpBegin(opCtx, termWhenBufferIsEmpty);

    if (MONGO_unlikely(hangBeforeRSTLOnDrainComplete.shouldFail())) {
        LOGV2(4712800, "Hanging due to hangBeforeRSTLOnDrainComplete failpoint");
        hangBeforeRSTLOnDrainComplete.pauseWhileSet(opCtx);
    }

    // Kill all user writes and user reads that encounter a prepare conflict. Also kills select
    // internal operations. Although secondaries cannot accept writes, a step up can kill writes
    // that were blocked behind the RSTL lock held by a step down attempt. These writes will be
    // killed with a retryable error code during step up.
    AutoGetRstlForStepUpStepDown arsu(
        this, opCtx, ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepUp);
    lk.lock();

    // Exit drain mode only if we're actually in draining mode, the apply buffer is empty in the
    // current term, and we're allowed to become the writable primary.
    if (_applierState != ApplierState::Draining ||
        !_topCoord->canCompleteTransitionToPrimary(termWhenBufferIsEmpty)) {
        LOGV2(6015308,
              "Applier left draining state or not allowed to become writeable primary, exiting");
        return;
    }
    _applierState = ApplierState::Stopped;

    invariant(_getMemberState_inlock().primary());
    invariant(!_readWriteAbility->canAcceptNonLocalWrites(opCtx));

    {
        // If the config doesn't have a term, don't change it.
        auto needBumpConfigTerm = _rsConfig.getConfigTerm() != OpTime::kUninitializedTerm;
        auto currConfigVersionAndTerm = _rsConfig.getConfigVersionAndTerm();
        lk.unlock();

        if (needBumpConfigTerm) {
            if (MONGO_unlikely(hangBeforeReconfigOnDrainComplete.shouldFail())) {
                LOGV2(5726200, "Hanging due to hangBeforeReconfigOnDrainComplete failpoint");
                hangBeforeReconfigOnDrainComplete.pauseWhileSet(opCtx);
            }
            // We re-write the term but keep version the same. This conceptually a no-op
            // in the config consensus group, analogous to writing a new oplog entry
            // in Raft log state machine on step up.
            auto getNewConfig = [&](const ReplSetConfig& oldConfig,
                                    long long primaryTerm) -> StatusWith<ReplSetConfig> {
                if (oldConfig.getConfigVersionAndTerm() != currConfigVersionAndTerm) {
                    return {ErrorCodes::ConfigurationInProgress,
                            "reconfig on step up was preempted by another reconfig"};
                }
                auto config = oldConfig.getMutable();
                config.setConfigTerm(primaryTerm);
                return ReplSetConfig(std::move(config));
            };
            LOGV2(4508103, "Increment the config term via reconfig");
            // Since we are only bumping the config term, we can skip the config replication and
            // quorum checks in reconfig.
            auto reconfigStatus = doOptimizedReconfig(opCtx, getNewConfig);
            if (!reconfigStatus.isOK()) {
                LOGV2(4508100,
                      "Automatic reconfig to increment the config term on stepup failed",
                      "error"_attr = reconfigStatus);
                // If the node stepped down after we released the lock, we can just return.
                if (ErrorCodes::isNotPrimaryError(reconfigStatus.code())) {
                    return;
                }
                // Writing this new config with a new term is somewhat "best effort", and if we get
                // preempted by a concurrent reconfig, that is fine since that new config will have
                // occurred after the node became primary and so the concurrent reconfig has updated
                // the term appropriately.
                if (reconfigStatus != ErrorCodes::ConfigurationInProgress) {
                    LOGV2_FATAL_CONTINUE(4508101,
                                         "Reconfig on stepup failed for unknown reasons",
                                         "error"_attr = reconfigStatus);
                    fassertFailedWithStatus(31477, reconfigStatus);
                }
            }
        }
        if (MONGO_unlikely(hangAfterReconfigOnDrainComplete.shouldFail())) {
            LOGV2(4508102, "Hanging due to hangAfterReconfigOnDrainComplete failpoint");
            hangAfterReconfigOnDrainComplete.pauseWhileSet(opCtx);
        }

        LOGV2(6015310, "Starting to transition to primary.");
        AllowNonLocalWritesBlock writesAllowed(opCtx);
        OpTime firstOpTime = _externalState->onTransitionToPrimary(opCtx);
        ReplicaSetAwareServiceRegistry::get(_service).onStepUpComplete(opCtx,
                                                                       firstOpTime.getTerm());
        lk.lock();

        _topCoord->completeTransitionToPrimary(firstOpTime);
        invariant(firstOpTime.getTerm() == _topCoord->getTerm());
        invariant(termWhenBufferIsEmpty == _topCoord->getTerm());
    }

    // Must calculate the commit level again because firstOpTimeOfMyTerm wasn't set when we logged
    // our election in onTransitionToPrimary(), above.
    _updateLastCommittedOpTimeAndWallTime(lk);
    _wakeReadyWaiters(lk);

    // Update _canAcceptNonLocalWrites.
    _updateWriteAbilityFromTopologyCoordinator(lk, opCtx);
    _updateMemberStateFromTopologyCoordinator(lk);

    LOGV2(21331,
          "Transition to primary complete; database writes are now permitted",
          "term"_attr = _termShadow.load());
    _externalState->startNoopWriter(_getMyLastAppliedOpTime_inlock());
}

void ReplicationCoordinatorImpl::signalUpstreamUpdater() {
    _externalState->forwardSecondaryProgress();
}

void ReplicationCoordinatorImpl::setMyHeartbeatMessage(const std::string& msg) {
    stdx::unique_lock<Latch> lock(_mutex);
    _topCoord->setMyHeartbeatMessage(_replExecutor->now(), msg);
}

void ReplicationCoordinatorImpl::setMyLastAppliedOpTimeAndWallTimeForward(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    // Update the global timestamp before setting the last applied opTime forward so the last
    // applied optime is never greater than the latest cluster time in the logical clock.
    const auto opTime = opTimeAndWallTime.opTime;
    _externalState->setGlobalTimestamp(getServiceContext(), opTime.getTimestamp());

    stdx::unique_lock<Latch> lock(_mutex);
    auto myLastAppliedOpTime = _getMyLastAppliedOpTime_inlock();
    if (opTime > myLastAppliedOpTime) {
        _setMyLastAppliedOpTimeAndWallTime(lock, opTimeAndWallTime, false);
        _reportUpstream_inlock(std::move(lock));
    } else {
        if (opTime != myLastAppliedOpTime) {
            // In pv1, oplog entries are ordered by non-decreasing term and strictly increasing
            // timestamp. So, in pv1, its not possible for us to get opTime with lower term and
            // timestamp higher than or equal to our current lastAppliedOptime.
            invariant(opTime.getTerm() == OpTime::kUninitializedTerm ||
                      myLastAppliedOpTime.getTerm() == OpTime::kUninitializedTerm ||
                      opTime.getTimestamp() < myLastAppliedOpTime.getTimestamp());
        }

        if (_readWriteAbility->canAcceptNonLocalWrites(lock) && _rsConfig.getWriteMajority() == 1) {
            // Single vote primaries may have a lagged stable timestamp due to paring back the
            // stable timestamp to the all committed timestamp.
            _setStableTimestampForStorage(lock);
        }
    }
}

void ReplicationCoordinatorImpl::setMyLastDurableOpTimeAndWallTimeForward(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    stdx::unique_lock<Latch> lock(_mutex);

    if (MONGO_unlikely(skipDurableTimestampUpdates.shouldFail())) {
        return;
    }

    if (opTimeAndWallTime.opTime > _getMyLastDurableOpTime_inlock()) {
        _setMyLastDurableOpTimeAndWallTime(lock, opTimeAndWallTime, false);
        _reportUpstream_inlock(std::move(lock));
    }
}

void ReplicationCoordinatorImpl::setMyLastAppliedOpTimeAndWallTime(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    const auto opTime = opTimeAndWallTime.opTime;
    // Update the global timestamp before setting the last applied opTime forward so the last
    // applied optime is never greater than the latest cluster time in the logical clock.
    _externalState->setGlobalTimestamp(getServiceContext(), opTime.getTimestamp());

    stdx::unique_lock<Latch> lock(_mutex);
    // The optime passed to this function is required to represent a consistent database state.
    _setMyLastAppliedOpTimeAndWallTime(lock, opTimeAndWallTime, false);
    signalOplogWaiters();
    _reportUpstream_inlock(std::move(lock));
}

void ReplicationCoordinatorImpl::setMyLastDurableOpTimeAndWallTime(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    stdx::unique_lock<Latch> lock(_mutex);
    _setMyLastDurableOpTimeAndWallTime(lock, opTimeAndWallTime, false);
    _reportUpstream_inlock(std::move(lock));
}

void ReplicationCoordinatorImpl::resetMyLastOpTimes() {
    stdx::unique_lock<Latch> lock(_mutex);
    _resetMyLastOpTimes(lock);
    _reportUpstream_inlock(std::move(lock));
}

void ReplicationCoordinatorImpl::_resetMyLastOpTimes(WithLock lk) {
    LOGV2_DEBUG(21332, 1, "Resetting durable/applied optimes");
    // Reset to uninitialized OpTime
    bool isRollbackAllowed = true;
    _setMyLastAppliedOpTimeAndWallTime(lk, OpTimeAndWallTime(), isRollbackAllowed);
    _setMyLastDurableOpTimeAndWallTime(lk, OpTimeAndWallTime(), isRollbackAllowed);
}

void ReplicationCoordinatorImpl::_reportUpstream_inlock(stdx::unique_lock<Latch> lock) {
    invariant(lock.owns_lock());

    if (getReplicationMode() != modeReplSet) {
        return;
    }

    if (_getMemberState_inlock().primary()) {
        return;
    }

    lock.unlock();

    _externalState->forwardSecondaryProgress();  // Must do this outside _mutex
}

void ReplicationCoordinatorImpl::_setMyLastAppliedOpTimeAndWallTime(
    WithLock lk, const OpTimeAndWallTime& opTimeAndWallTime, bool isRollbackAllowed) {
    const auto opTime = opTimeAndWallTime.opTime;

    // The last applied opTime should never advance beyond the global timestamp (i.e. the latest
    // cluster time). Not enforced if the logical clock is disabled, e.g. for arbiters.
    dassert(!VectorClock::get(getServiceContext())->isEnabled() ||
            _externalState->getGlobalTimestamp(getServiceContext()) >= opTime.getTimestamp());

    _topCoord->setMyLastAppliedOpTimeAndWallTime(
        opTimeAndWallTime, _replExecutor->now(), isRollbackAllowed);
    // If we are using applied times to calculate the commit level, update it now.
    if (!_rsConfig.getWriteConcernMajorityShouldJournal()) {
        _updateLastCommittedOpTimeAndWallTime(lk);
    }
    // No need to wake up replication waiters because there should not be any replication waiters
    // waiting on our own lastApplied.

    // Update the storage engine's lastApplied snapshot before updating the stable timestamp on the
    // storage engine. New transactions reading from the lastApplied snapshot should start before
    // the oldest timestamp is advanced to avoid races. Additionally, update this snapshot before
    // signaling optime waiters. This avoids a race that would allow optime waiters to open
    // transactions on stale lastApplied values because they do not hold or reacquire the
    // replication coordinator mutex when signaled.
    _externalState->updateLastAppliedSnapshot(opTime);

    // Signal anyone waiting on optime changes.
    _opTimeWaiterList.setValueIf_inlock(
        [opTime](const OpTime& waitOpTime, const SharedWaiterHandle& waiter) {
            return waitOpTime <= opTime;
        },
        opTime);

    if (opTime.isNull()) {
        return;
    }

    // Advance the stable timestamp if necessary. Stable timestamps are used to determine the latest
    // timestamp that it is safe to revert the database to, in the event of a rollback via the
    // 'recover to timestamp' method.
    invariant(opTime.getTimestamp().getInc() > 0,
              str::stream() << "Impossible optime received: " << opTime.toString());
    // If we are lagged behind the commit optime, set a new stable timestamp here. When majority
    // read concern is disabled, the stable timestamp is set to lastApplied.
    if (opTime <= _topCoord->getLastCommittedOpTime() ||
        !serverGlobalParams.enableMajorityReadConcern) {
        _setStableTimestampForStorage(lk);
    }
}

void ReplicationCoordinatorImpl::_setMyLastDurableOpTimeAndWallTime(
    WithLock lk, const OpTimeAndWallTime& opTimeAndWallTime, bool isRollbackAllowed) {
    _topCoord->setMyLastDurableOpTimeAndWallTime(
        opTimeAndWallTime, _replExecutor->now(), isRollbackAllowed);
    // If we are using durable times to calculate the commit level, update it now.
    if (_rsConfig.getWriteConcernMajorityShouldJournal()) {
        _updateLastCommittedOpTimeAndWallTime(lk);
    }
    // There could be replication waiters waiting for our lastDurable for {j: true}, wake up those
    // that now have their write concern satisfied.
    _wakeReadyWaiters(lk, opTimeAndWallTime.opTime);
}

OpTime ReplicationCoordinatorImpl::getMyLastAppliedOpTime() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _getMyLastAppliedOpTime_inlock();
}

OpTimeAndWallTime ReplicationCoordinatorImpl::getMyLastAppliedOpTimeAndWallTime(
    bool rollbackSafe) const {
    stdx::lock_guard<Latch> lock(_mutex);
    if (rollbackSafe && _getMemberState_inlock().rollback()) {
        return {};
    }
    return _getMyLastAppliedOpTimeAndWallTime_inlock();
}

OpTimeAndWallTime ReplicationCoordinatorImpl::getMyLastDurableOpTimeAndWallTime() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _getMyLastDurableOpTimeAndWallTime_inlock();
}

OpTime ReplicationCoordinatorImpl::getMyLastDurableOpTime() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _getMyLastDurableOpTime_inlock();
}

Status ReplicationCoordinatorImpl::_validateReadConcern(OperationContext* opCtx,
                                                        const ReadConcernArgs& readConcern) {
    if (readConcern.getArgsAfterClusterTime() &&
        readConcern.getLevel() != ReadConcernLevel::kMajorityReadConcern &&
        readConcern.getLevel() != ReadConcernLevel::kLocalReadConcern &&
        readConcern.getLevel() != ReadConcernLevel::kSnapshotReadConcern) {
        return {
            ErrorCodes::BadValue,
            "Only readConcern level 'majority', 'local', or 'snapshot' is allowed when specifying "
            "afterClusterTime"};
    }

    if (readConcern.getArgsAtClusterTime() &&
        readConcern.getLevel() != ReadConcernLevel::kSnapshotReadConcern) {
        return {ErrorCodes::BadValue,
                "readConcern level 'snapshot' is required when specifying atClusterTime"};
    }

    // We cannot support read concern 'majority' by means of reading from a historical snapshot if
    // the storage layer doesn't support it. In this case, we can support it by using "speculative"
    // majority reads instead.
    if (readConcern.getLevel() == ReadConcernLevel::kMajorityReadConcern &&
        readConcern.getMajorityReadMechanism() ==
            ReadConcernArgs::MajorityReadMechanism::kMajoritySnapshot &&
        !_externalState->isReadCommittedSupportedByStorageEngine(opCtx)) {
        return {ErrorCodes::ReadConcernMajorityNotEnabled,
                str::stream() << "Storage engine does not support read concern: "
                              << readConcern.toString()};
    }

    if (readConcern.getLevel() == ReadConcernLevel::kSnapshotReadConcern &&
        !_externalState->isReadConcernSnapshotSupportedByStorageEngine(opCtx)) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "Storage engine does not support read concern: "
                              << readConcern.toString()};
    }

    if (readConcern.getArgsAtClusterTime() && !serverGlobalParams.enableMajorityReadConcern) {
        return {ErrorCodes::InvalidOptions,
                "readConcern level 'snapshot' is not supported in sharded clusters when "
                "enableMajorityReadConcern=false. See "
                "https://dochub.mongodb.org/core/"
                "disabled-read-concern-majority-snapshot-restrictions."};
    }

    return Status::OK();
}

Status ReplicationCoordinatorImpl::waitUntilOpTimeForRead(OperationContext* opCtx,
                                                          const ReadConcernArgs& readConcern) {
    auto verifyStatus = _validateReadConcern(opCtx, readConcern);
    if (!verifyStatus.isOK()) {
        return verifyStatus;
    }

    // nothing to wait for
    if (!readConcern.getArgsAfterClusterTime() && !readConcern.getArgsOpTime() &&
        !readConcern.getArgsAtClusterTime()) {
        return Status::OK();
    }

    // We should never wait for replication if we are holding any locks, because this can
    // potentially block for long time while doing network activity.
    if (opCtx->lockState()->isLocked()) {
        return {ErrorCodes::IllegalOperation,
                "Waiting for replication not allowed while holding a lock"};
    }

    return waitUntilOpTimeForReadUntil(opCtx, readConcern, boost::none);
}

Status ReplicationCoordinatorImpl::waitUntilOpTimeForReadUntil(OperationContext* opCtx,
                                                               const ReadConcernArgs& readConcern,
                                                               boost::optional<Date_t> deadline) {
    if (getReplicationMode() != repl::ReplicationCoordinator::modeReplSet) {
        // 'afterOpTime', 'afterClusterTime', and 'atClusterTime' are only supported for replica
        // sets.
        return {ErrorCodes::NotAReplicaSet,
                "node needs to be a replica set member to use read concern"};
    }

    if (_rsConfigState == kConfigUninitialized || _rsConfigState == kConfigInitiating) {
        return {ErrorCodes::NotYetInitialized,
                "Cannot use non-local read concern until replica set is finished initializing."};
    }

    if (readConcern.getArgsAfterClusterTime() || readConcern.getArgsAtClusterTime()) {
        return _waitUntilClusterTimeForRead(opCtx, readConcern, deadline);
    } else {
        return _waitUntilOpTimeForReadDeprecated(opCtx, readConcern);
    }
}

Status ReplicationCoordinatorImpl::_waitUntilOpTime(OperationContext* opCtx,
                                                    OpTime targetOpTime,
                                                    boost::optional<Date_t> deadline) {
    if (!_externalState->oplogExists(opCtx)) {
        return {ErrorCodes::NotYetInitialized, "The oplog does not exist."};
    }

    {
        stdx::unique_lock lock(_mutex);
        if (targetOpTime > _getMyLastAppliedOpTime_inlock()) {
            if (_inShutdown) {
                return {ErrorCodes::ShutdownInProgress, "Shutdown in progress"};
            }

            // We just need to wait for the opTime to catch up to what we need (not majority RC).
            auto future = _opTimeWaiterList.add_inlock(targetOpTime);

            LOGV2_DEBUG(21333,
                        3,
                        "waitUntilOpTime: OpID {OpID} is waiting for OpTime "
                        "{targetOpTime} until {deadline}",
                        "waitUntilOpTime is waiting for OpTime",
                        "OpID"_attr = opCtx->getOpID(),
                        "targetOpTime"_attr = targetOpTime,
                        "deadline"_attr = deadline.value_or(opCtx->getDeadline()));

            lock.unlock();
            auto waitStatus = futureGetNoThrowWithDeadline(
                opCtx, future, deadline.value_or(Date_t::max()), opCtx->getTimeoutError());
            if (!waitStatus.isOK()) {
                lock.lock();
                return waitStatus.withContext(
                    str::stream() << "Error waiting for optime " << targetOpTime.toString()
                                  << ", current relevant optime is "
                                  << _getMyLastAppliedOpTime_inlock().toString() << ".");
            }
        }
    }

    // We need to wait for all committed writes to be visible, even in the oplog (which uses
    // special visibility rules).  We must do this after waiting for our target optime, because
    // only then do we know that it will fill in all "holes" before that time.  If we do it
    // earlier, we may return when the requested optime has been reached, but other writes
    // at optimes before that time are not yet visible.
    //
    // We wait only on primaries, because on secondaries, other mechanisms assure that the
    // last applied optime is always hole-free, and waiting for all earlier writes to be visible
    // can deadlock against secondary command application.
    //
    // Note that oplog queries by secondary nodes depend on this behavior to wait for
    // all oplog holes to be filled in, despite providing an afterClusterTime field
    // with Timestamp(0,1).
    _storage->waitForAllEarlierOplogWritesToBeVisible(opCtx, /* primaryOnly =*/true);

    return Status::OK();
}

Status ReplicationCoordinatorImpl::waitUntilMajorityOpTime(mongo::OperationContext* opCtx,
                                                           mongo::repl::OpTime targetOpTime,
                                                           boost::optional<Date_t> deadline) {
    if (!_externalState->snapshotsEnabled()) {
        return {ErrorCodes::CommandNotSupported,
                "Current storage engine does not support majority committed reads"};
    }

    stdx::unique_lock lock(_mutex);

    LOGV2_DEBUG(21334,
                1,
                "waitUntilOpTime: waiting for optime:{targetOpTime} to be in a snapshot -- current "
                "snapshot: {currentCommittedSnapshotOpTime}",
                "waitUntilOpTime: waiting for target OpTime to be in a snapshot",
                "targetOpTime"_attr = targetOpTime,
                "currentCommittedSnapshotOpTime"_attr =
                    _getCurrentCommittedSnapshotOpTime_inlock());

    LOGV2_DEBUG(21335,
                3,
                "waitUntilOpTime: waiting for a new snapshot until {deadline}",
                "waitUntilOpTime: waiting for a new snapshot",
                "deadline"_attr = deadline.value_or(opCtx->getDeadline()));

    try {
        auto ok = opCtx->waitForConditionOrInterruptUntil(
            _currentCommittedSnapshotCond, lock, deadline.value_or(Date_t::max()), [&] {
                return _inShutdown || (targetOpTime <= _getCurrentCommittedSnapshotOpTime_inlock());
            });
        uassert(opCtx->getTimeoutError(), "operation exceeded time limit", ok);
    } catch (const DBException& e) {
        return e.toStatus().withContext(
            str::stream() << "Error waiting for snapshot not less than " << targetOpTime.toString()
                          << ", current relevant optime is "
                          << _getCurrentCommittedSnapshotOpTime_inlock().toString() << ".");
    }

    if (_inShutdown) {
        return {ErrorCodes::ShutdownInProgress, "Shutdown in progress"};
    }

    if (_currentCommittedSnapshot) {
        // It seems that targetOpTime can sometimes be default OpTime{}. When there is no
        // _currentCommittedSnapshot, _getCurrentCommittedSnapshotOpTime_inlock() also returns
        // default OpTime{}. Hence this branch only runs if _currentCommittedSnapshot actually
        // exists.
        LOGV2_DEBUG(21336,
                    3,
                    "Got notified of new snapshot: {currentCommittedSnapshot}",
                    "Got notified of new snapshot",
                    "currentCommittedSnapshot"_attr = _currentCommittedSnapshot->toString());
    }
    return Status::OK();
}

Status ReplicationCoordinatorImpl::_waitUntilClusterTimeForRead(OperationContext* opCtx,
                                                                const ReadConcernArgs& readConcern,
                                                                boost::optional<Date_t> deadline) {
    invariant(readConcern.getArgsAfterClusterTime() || readConcern.getArgsAtClusterTime());
    invariant(!readConcern.getArgsAfterClusterTime() || !readConcern.getArgsAtClusterTime());
    auto clusterTime = readConcern.getArgsAfterClusterTime()
        ? *readConcern.getArgsAfterClusterTime()
        : *readConcern.getArgsAtClusterTime();
    invariant(clusterTime != LogicalTime::kUninitialized);

    // convert clusterTime to opTime so it can be used by the _opTimeWaiterList for wait on
    // readConcern level local.
    auto targetOpTime = OpTime(clusterTime.asTimestamp(), OpTime::kUninitializedTerm);
    invariant(!readConcern.getArgsOpTime());

    // We don't set isMajorityCommittedRead for transactions because snapshots are always
    // speculative; we wait for majority when the transaction commits.
    //
    // Speculative majority reads do not need to wait for the commit point to advance to satisfy
    // afterClusterTime reads. Waiting for the lastApplied to advance past the given target optime
    // ensures the recency guarantee for the afterClusterTime read. At the end of the command, we
    // will wait for the lastApplied optime to become majority committed, which then satisfies the
    // durability guarantee.
    //
    // Majority and snapshot reads outside of transactions should non-speculatively wait for the
    // majority committed snapshot.
    const bool isMajorityCommittedRead = !readConcern.isSpeculativeMajority() &&
        !opCtx->inMultiDocumentTransaction() &&
        (readConcern.getLevel() == ReadConcernLevel::kMajorityReadConcern ||
         readConcern.getLevel() == ReadConcernLevel::kSnapshotReadConcern);

    if (isMajorityCommittedRead) {
        return waitUntilMajorityOpTime(opCtx, targetOpTime, deadline);
    } else {
        return _waitUntilOpTime(opCtx, targetOpTime, deadline);
    }
}

// TODO: remove when SERVER-29729 is done
Status ReplicationCoordinatorImpl::_waitUntilOpTimeForReadDeprecated(
    OperationContext* opCtx, const ReadConcernArgs& readConcern) {
    const bool isMajorityCommittedRead =
        readConcern.getLevel() == ReadConcernLevel::kMajorityReadConcern;

    const auto targetOpTime = readConcern.getArgsOpTime().value_or(OpTime());
    if (isMajorityCommittedRead) {
        return waitUntilMajorityOpTime(opCtx, targetOpTime);
    } else {
        return _waitUntilOpTime(opCtx, targetOpTime);
    }
}

Status ReplicationCoordinatorImpl::awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) {
    // Using an uninitialized term means that this optime will be compared to other optimes only by
    // its timestamp. This allows us to wait only on the timestamp of the commit point surpassing
    // this timestamp, without worrying about terms.
    OpTime waitOpTime(ts, OpTime::kUninitializedTerm);
    return waitUntilMajorityOpTime(opCtx, waitOpTime);
}

OpTimeAndWallTime ReplicationCoordinatorImpl::_getMyLastAppliedOpTimeAndWallTime_inlock() const {
    return _topCoord->getMyLastAppliedOpTimeAndWallTime();
}

OpTime ReplicationCoordinatorImpl::_getMyLastAppliedOpTime_inlock() const {
    return _topCoord->getMyLastAppliedOpTime();
}

OpTimeAndWallTime ReplicationCoordinatorImpl::_getMyLastDurableOpTimeAndWallTime_inlock() const {
    return _topCoord->getMyLastDurableOpTimeAndWallTime();
}

OpTime ReplicationCoordinatorImpl::_getMyLastDurableOpTime_inlock() const {
    return _topCoord->getMyLastDurableOpTime();
}

Status ReplicationCoordinatorImpl::setLastDurableOptime_forTest(long long cfgVer,
                                                                long long memberId,
                                                                const OpTime& opTime,
                                                                Date_t wallTime) {
    stdx::lock_guard<Latch> lock(_mutex);
    invariant(getReplicationMode() == modeReplSet);

    if (wallTime == Date_t()) {
        wallTime = Date_t() + Seconds(opTime.getSecs());
    }

    const UpdatePositionArgs::UpdateInfo update(
        OpTime(), Date_t(), opTime, wallTime, cfgVer, memberId);
    const auto statusWithOpTime = _setLastOptimeForMember(lock, update);
    _updateStateAfterRemoteOpTimeUpdates(lock, statusWithOpTime.getValue());
    return statusWithOpTime.getStatus();
}

Status ReplicationCoordinatorImpl::setLastAppliedOptime_forTest(long long cfgVer,
                                                                long long memberId,
                                                                const OpTime& opTime,
                                                                Date_t wallTime) {
    stdx::lock_guard<Latch> lock(_mutex);
    invariant(getReplicationMode() == modeReplSet);

    if (wallTime == Date_t()) {
        wallTime = Date_t() + Seconds(opTime.getSecs());
    }

    const UpdatePositionArgs::UpdateInfo update(
        opTime, wallTime, OpTime(), Date_t(), cfgVer, memberId);
    const auto statusWithOpTime = _setLastOptimeForMember(lock, update);
    _updateStateAfterRemoteOpTimeUpdates(lock, statusWithOpTime.getValue());
    return statusWithOpTime.getStatus();
}

StatusWith<OpTime> ReplicationCoordinatorImpl::_setLastOptimeForMember(
    WithLock lk, const UpdatePositionArgs::UpdateInfo& args) {
    auto result = _topCoord->setLastOptimeForMember(args, _replExecutor->now());
    if (!result.isOK())
        return result.getStatus();
    const bool advancedOpTime = result.getValue();
    _rescheduleLivenessUpdate_inlock(args.memberId);
    return advancedOpTime ? std::max(args.appliedOpTime, args.durableOpTime) : OpTime();
}

void ReplicationCoordinatorImpl::_updateStateAfterRemoteOpTimeUpdates(
    WithLock lk, const OpTime& maxRemoteOpTime) {
    // Only update committed optime if the remote optimes increased.
    if (!maxRemoteOpTime.isNull()) {
        _updateLastCommittedOpTimeAndWallTime(lk);
        // Wait up replication waiters on optime changes.
        _wakeReadyWaiters(lk, maxRemoteOpTime);
    }
}

bool ReplicationCoordinatorImpl::isCommitQuorumSatisfied(
    const CommitQuorumOptions& commitQuorum, const std::vector<mongo::HostAndPort>& members) const {
    stdx::lock_guard<Latch> lock(_mutex);

    if (commitQuorum.mode.empty()) {
        return _haveNumNodesSatisfiedCommitQuorum(lock, commitQuorum.numNodes, members);
    }

    StringData patternName;
    if (commitQuorum.mode == CommitQuorumOptions::kMajority) {
        patternName = ReplSetConfig::kMajorityWriteConcernModeName;
    } else if (commitQuorum.mode == CommitQuorumOptions::kVotingMembers) {
        patternName = ReplSetConfig::kVotingMembersWriteConcernModeName;
    } else {
        patternName = commitQuorum.mode;
    }

    auto tagPattern = uassertStatusOK(_rsConfig.findCustomWriteMode(patternName));
    return _haveTaggedNodesSatisfiedCommitQuorum(lock, tagPattern, members);
}

bool ReplicationCoordinatorImpl::_haveNumNodesSatisfiedCommitQuorum(
    WithLock lk, int numNodes, const std::vector<mongo::HostAndPort>& members) const {
    for (auto&& member : members) {
        auto memberConfig = _rsConfig.findMemberByHostAndPort(member);
        // We do not count arbiters and members that aren't part of replica set config,
        // towards the commit quorum.
        if (!memberConfig || memberConfig->isArbiter())
            continue;

        --numNodes;

        if (numNodes <= 0) {
            return true;
        }
    }
    return false;
}

bool ReplicationCoordinatorImpl::_haveTaggedNodesSatisfiedCommitQuorum(
    WithLock lk,
    const ReplSetTagPattern& tagPattern,
    const std::vector<mongo::HostAndPort>& members) const {
    ReplSetTagMatch matcher(tagPattern);

    for (auto&& member : members) {
        auto memberConfig = _rsConfig.findMemberByHostAndPort(member);
        // We do not count arbiters and members that aren't part of replica set config,
        // towards the commit quorum.
        if (!memberConfig || memberConfig->isArbiter())
            continue;
        for (auto&& it = memberConfig->tagsBegin(); it != memberConfig->tagsEnd(); ++it) {
            if (matcher.update(*it)) {
                return true;
            }
        }
    }
    return false;
}

bool ReplicationCoordinatorImpl::_doneWaitingForReplication_inlock(
    const OpTime& opTime, const WriteConcernOptions& writeConcern) {
    // The syncMode cannot be unset.
    invariant(writeConcern.syncMode != WriteConcernOptions::SyncMode::UNSET);

    const bool useDurableOpTime = writeConcern.syncMode == WriteConcernOptions::SyncMode::JOURNAL;
    if (!stdx::holds_alternative<std::string>(writeConcern.w)) {
        if (auto wTags = stdx::get_if<WTags>(&writeConcern.w)) {
            auto tagPattern = uassertStatusOK(_rsConfig.makeCustomWriteMode(*wTags));
            return _topCoord->haveTaggedNodesReachedOpTime(opTime, tagPattern, useDurableOpTime);
        }

        return _topCoord->haveNumNodesReachedOpTime(
            opTime, stdx::get<int64_t>(writeConcern.w), useDurableOpTime);
    }

    StringData patternName;
    auto wMode = stdx::get<std::string>(writeConcern.w);
    if (wMode == WriteConcernOptions::kMajority) {
        if (_externalState->snapshotsEnabled() && !gTestingSnapshotBehaviorInIsolation) {
            // Make sure we have a valid "committed" snapshot up to the needed optime.
            if (!_currentCommittedSnapshot) {
                return false;
            }

            // Wait for the "current" snapshot to advance to/past the opTime.
            const auto haveSnapshot = _currentCommittedSnapshot >= opTime;
            if (!haveSnapshot) {
                LOGV2_DEBUG(
                    21337,
                    1,
                    "Required snapshot optime: {opTime} is not yet part of the current "
                    "'committed' snapshot: {currentCommittedSnapshotOpTime}",
                    "Required snapshot optime is not yet part of the current 'committed' snapshot",
                    "opTime"_attr = opTime,
                    "currentCommittedSnapshotOpTime"_attr = _currentCommittedSnapshot);
                return false;
            }

            // Fallthrough to wait for "majority" write concern.
        }

        // Wait for all drop pending collections with drop optime before and at 'opTime' to be
        // removed from storage.
        if (auto dropOpTime = _externalState->getEarliestDropPendingOpTime()) {
            if (*dropOpTime <= opTime) {
                LOGV2_DEBUG(
                    21338,
                    1,
                    "Unable to satisfy the requested majority write concern at "
                    "'committed' optime {opTime}. There are still drop pending collections "
                    "(earliest drop optime: {earliestDropOpTime}) that have to be removed from "
                    "storage before we can "
                    "satisfy the write concern {writeConcern}",
                    "Unable to satisfy the requested majority write concern at 'committed' optime. "
                    "There are still drop pending collections that have to be removed from storage "
                    "before we can satisfy the write concern",
                    "opTime"_attr = opTime,
                    "earliestDropOpTime"_attr = *dropOpTime,
                    "writeConcern"_attr = writeConcern.toBSON());
                return false;
            }
        }

        // Continue and wait for replication to the majority (of voters).
        // *** Needed for J:True, writeConcernMajorityShouldJournal:False (appliedOpTime snapshot).
        patternName = ReplSetConfig::kMajorityWriteConcernModeName;
    } else {
        patternName = wMode;
    }

    auto tagPattern = uassertStatusOK(_rsConfig.findCustomWriteMode(patternName));
    if (writeConcern.checkCondition == WriteConcernOptions::CheckCondition::OpTime) {
        return _topCoord->haveTaggedNodesReachedOpTime(opTime, tagPattern, useDurableOpTime);
    } else {
        invariant(writeConcern.checkCondition == WriteConcernOptions::CheckCondition::Config);
        auto pred = _topCoord->makeConfigPredicate();
        return _topCoord->haveTaggedNodesSatisfiedCondition(pred, tagPattern);
    }
}

ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorImpl::awaitReplication(
    OperationContext* opCtx, const OpTime& opTime, const WriteConcernOptions& writeConcern) {
    // It is illegal to wait for replication with a session checked out because it can lead to
    // deadlocks.
    invariant(OperationContextSession::get(opCtx) == nullptr);

    Timer timer;
    WriteConcernOptions fixedWriteConcern = populateUnsetWriteConcernOptionsSyncMode(writeConcern);

    // We should never wait for replication if we are holding any locks, because this can
    // potentially block for long time while doing network activity.
    invariant(!opCtx->lockState()->isLocked());

    auto interruptStatus = opCtx->checkForInterruptNoAssert();
    if (!interruptStatus.isOK()) {
        return {interruptStatus, duration_cast<Milliseconds>(timer.elapsed())};
    }

    auto wTimeoutDate = [&]() -> Date_t {
        auto clockSource = opCtx->getServiceContext()->getFastClockSource();
        if (writeConcern.wDeadline != Date_t::max()) {
            return writeConcern.wDeadline;
        }
        if (writeConcern.wTimeout == WriteConcernOptions::kNoTimeout) {
            return Date_t::max();
        }
        return clockSource->now() + clockSource->getPrecision() +
            Milliseconds{writeConcern.wTimeout};
    }();

    const auto opCtxDeadline = opCtx->getDeadline();
    const auto timeoutError = opCtx->getTimeoutError();

    auto future = [&] {
        stdx::lock_guard lock(_mutex);
        return _startWaitingForReplication(lock, opTime, fixedWriteConcern);
    }();
    auto status = futureGetNoThrowWithDeadline(opCtx, future, wTimeoutDate, timeoutError);

    // If we get a timeout error and the opCtx deadline is >= the writeConcern wtimeout, then we
    // know the timeout was due to wtimeout (not opCtx deadline) and thus we return
    // ErrorCodes::WriteConcernFailed.
    if (status.code() == timeoutError && opCtxDeadline >= wTimeoutDate) {
        status = Status{ErrorCodes::WriteConcernFailed, "waiting for replication timed out"};
    }

    if (TestingProctor::instance().isEnabled() && !status.isOK()) {
        stdx::lock_guard lock(_mutex);
        LOGV2(21339,
              "Replication failed for write concern: {writeConcern}, waiting for optime: {opTime}, "
              "opID: {opID}, all_durable: {allDurable}, progress: {progress}",
              "Replication failed for write concern",
              "status"_attr = redact(status),
              "writeConcern"_attr = writeConcern.toBSON(),
              "opTime"_attr = opTime,
              "opID"_attr = opCtx->getOpID(),
              "allDurable"_attr = _storage->getAllDurableTimestamp(_service),
              "progress"_attr = _getReplicationProgress(lock));
    }
    return {std::move(status), duration_cast<Milliseconds>(timer.elapsed())};
}

SharedSemiFuture<void> ReplicationCoordinatorImpl::awaitReplicationAsyncNoWTimeout(
    const OpTime& opTime, const WriteConcernOptions& writeConcern) {
    WriteConcernOptions fixedWriteConcern = populateUnsetWriteConcernOptionsSyncMode(writeConcern);

    // The returned future won't account for wTimeout or wDeadline, so reject any write concerns
    // with either option to avoid misuse.
    invariant(fixedWriteConcern.wDeadline == Date_t::max());
    invariant(fixedWriteConcern.wTimeout == WriteConcernOptions::kNoTimeout);

    stdx::lock_guard lg(_mutex);
    return _startWaitingForReplication(lg, opTime, fixedWriteConcern);
}

BSONObj ReplicationCoordinatorImpl::_getReplicationProgress(WithLock wl) const {
    BSONObjBuilder progress;

    const auto lastCommittedOpTime = _topCoord->getLastCommittedOpTime();
    progress.append("lastCommittedOpTime", lastCommittedOpTime.toBSON());

    const auto currentCommittedSnapshotOpTime = _getCurrentCommittedSnapshotOpTime_inlock();
    progress.append("currentCommittedSnapshotOpTime", currentCommittedSnapshotOpTime.toBSON());

    const auto earliestDropPendingOpTime = _externalState->getEarliestDropPendingOpTime();
    if (earliestDropPendingOpTime) {
        progress.append("earliestDropPendingOpTime", earliestDropPendingOpTime->toBSON());
    }

    _topCoord->fillMemberData(&progress);
    return progress.obj();
}

SharedSemiFuture<void> ReplicationCoordinatorImpl::_startWaitingForReplication(
    WithLock wl, const OpTime& opTime, const WriteConcernOptions& writeConcern) {

    const Mode replMode = getReplicationMode();
    if (replMode == modeNone) {
        // no replication check needed (validated above)
        return Future<void>::makeReady();
    }
    if (opTime.isNull()) {
        // If waiting for the empty optime, always say it's been replicated.
        return Future<void>::makeReady();
    }
    if (_inShutdown) {
        return Future<void>::makeReady(
            Status{ErrorCodes::ShutdownInProgress, "Replication is being shut down"});
    }

    auto checkForStepDown = [&]() -> Status {
        if (replMode == modeReplSet && !_memberState.primary()) {
            return {ErrorCodes::PrimarySteppedDown,
                    "Primary stepped down while waiting for replication"};
        }

        if (opTime.getTerm() != _topCoord->getTerm()) {
            return {
                ErrorCodes::PrimarySteppedDown,
                str::stream() << "Term changed from " << opTime.getTerm() << " to "
                              << _topCoord->getTerm()
                              << " while waiting for replication, indicating that this node must "
                                 "have stepped down."};
        }

        if (_topCoord->isSteppingDown()) {
            return {ErrorCodes::PrimarySteppedDown,
                    "Received stepdown request while waiting for replication"};
        }
        return Status::OK();
    };

    Status stepdownStatus = checkForStepDown();
    if (!stepdownStatus.isOK()) {
        return Future<void>::makeReady(stepdownStatus);
    }

    // Check if the given write concern is satisfiable before we add ourself to
    // _replicationWaiterList. On replSetReconfig, waiters that are no longer satisfiable will be
    // notified. See _setCurrentRSConfig.
    auto satisfiableStatus = _checkIfWriteConcernCanBeSatisfied_inlock(writeConcern);
    if (!satisfiableStatus.isOK()) {
        return Future<void>::makeReady(satisfiableStatus);
    }

    try {
        if (_doneWaitingForReplication_inlock(opTime, writeConcern)) {
            return Future<void>::makeReady();
        }
    } catch (const DBException& e) {
        return Future<void>::makeReady(e.toStatus());
    }

    if (!writeConcern.needToWaitForOtherNodes() &&
        writeConcern.syncMode != WriteConcernOptions::SyncMode::JOURNAL) {
        // We are only waiting for our own lastApplied, add this to _opTimeWaiterList instead. This
        // is because waiters in _replicationWaiterList are not notified on self's lastApplied
        // updates.
        return _opTimeWaiterList.add_inlock(opTime);
    }

    // From now on, we are either waiting for replication or local journaling. And waiters in
    // _replicationWaiterList will be checked and notified on remote opTime updates and on self's
    // lastDurable updates (but not on self's lastApplied updates, in which case use
    // _opTimeWaiterList instead).
    return _replicationWaiterList.add_inlock(opTime, writeConcern);
}

void ReplicationCoordinatorImpl::waitForStepDownAttempt_forTest() {
    auto isSteppingDown = [&]() {
        stdx::unique_lock<Latch> lk(_mutex);
        // If true, we know that a stepdown is underway.
        return (_topCoord->isSteppingDown());
    };

    while (!isSteppingDown()) {
        sleepFor(Milliseconds{10});
    }
}

void ReplicationCoordinatorImpl::updateAndLogStateTransitionMetrics(
    const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
    const size_t numOpsKilled,
    const size_t numOpsRunning) const {

    // Clear the current metrics before setting.
    userOpsKilled.decrement(userOpsKilled.get());
    userOpsRunning.decrement(userOpsRunning.get());

    switch (stateTransition) {
        case ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepUp:
            lastStateTransition = "stepUp";
            break;
        case ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown:
            lastStateTransition = "stepDown";
            break;
        case ReplicationCoordinator::OpsKillingStateTransitionEnum::kRollback:
            lastStateTransition = "rollback";
            break;
        default:
            MONGO_UNREACHABLE;
    }

    userOpsKilled.increment(numOpsKilled);
    userOpsRunning.increment(numOpsRunning);

    BSONObjBuilder bob;
    bob.append("lastStateTransition", **lastStateTransition);
    bob.appendNumber("userOpsKilled", userOpsKilled.get());
    bob.appendNumber("userOpsRunning", userOpsRunning.get());

    LOGV2(21340,
          "State transition ops metrics: {metrics}",
          "State transition ops metrics",
          "metrics"_attr = bob.obj());
}

long long ReplicationCoordinatorImpl::_calculateRemainingQuiesceTimeMillis() const {
    auto remainingQuiesceTimeMillis =
        std::max(Milliseconds::zero(), _quiesceDeadline - _replExecutor->now());
    // Turn remainingQuiesceTimeMillis into an int64 so that it's a supported BSONElement.
    long long remainingQuiesceTimeLong = durationCount<Milliseconds>(remainingQuiesceTimeMillis);
    return remainingQuiesceTimeLong;
}

std::shared_ptr<HelloResponse> ReplicationCoordinatorImpl::_makeHelloResponse(
    boost::optional<StringData> horizonString, WithLock lock, const bool hasValidConfig) const {

    uassert(ShutdownInProgressQuiesceInfo(_calculateRemainingQuiesceTimeMillis()),
            kQuiesceModeShutdownMessage,
            !_inQuiesceMode);

    if (!hasValidConfig) {
        auto response = std::make_shared<HelloResponse>();
        response->setTopologyVersion(_topCoord->getTopologyVersion());
        response->markAsNoConfig();
        return response;
    }

    // horizonString must be passed in if we are a valid member of the config.
    invariant(horizonString);
    auto response = std::make_shared<HelloResponse>();
    invariant(isReplEnabled());
    _topCoord->fillHelloForReplSet(response, *horizonString);

    OpTime lastOpTime = _getMyLastAppliedOpTime_inlock();

    response->setLastWrite(lastOpTime, lastOpTime.getTimestamp().getSecs());
    if (_currentCommittedSnapshot) {
        response->setLastMajorityWrite(*_currentCommittedSnapshot,
                                       _currentCommittedSnapshot->getTimestamp().getSecs());
    }

    if (response->isWritablePrimary() && !_readWriteAbility->canAcceptNonLocalWrites(lock)) {
        // Report that we are secondary and not accepting writes until drain completes.
        response->setIsWritablePrimary(false);
        response->setIsSecondary(true);
    }

    if (_waitingForRSTLAtStepDown) {
        response->setIsWritablePrimary(false);
    }

    if (_inShutdown) {
        response->setIsWritablePrimary(false);
        response->setIsSecondary(false);
    }
    return response;
}

SharedSemiFuture<ReplicationCoordinatorImpl::SharedHelloResponse>
ReplicationCoordinatorImpl::_getHelloResponseFuture(
    WithLock lk,
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<StringData> horizonString,
    boost::optional<TopologyVersion> clientTopologyVersion) {

    uassert(ShutdownInProgressQuiesceInfo(_calculateRemainingQuiesceTimeMillis()),
            kQuiesceModeShutdownMessage,
            !_inQuiesceMode);

    const bool hasValidConfig = horizonString != boost::none;

    if (!clientTopologyVersion) {
        // The client is not using awaitable hello so we respond immediately.
        return SharedSemiFuture<SharedHelloResponse>(
            SharedHelloResponse(_makeHelloResponse(horizonString, lk, hasValidConfig)));
    }

    const TopologyVersion topologyVersion = _topCoord->getTopologyVersion();
    if (clientTopologyVersion->getProcessId() != topologyVersion.getProcessId()) {
        // Getting a different process id indicates that the server has restarted so we return
        // immediately with the updated process id.
        return SharedSemiFuture<SharedHelloResponse>(
            SharedHelloResponse(_makeHelloResponse(horizonString, lk, hasValidConfig)));
    }

    auto prevCounter = clientTopologyVersion->getCounter();
    auto topologyVersionCounter = topologyVersion.getCounter();
    uassert(31382,
            str::stream() << "Received a topology version with counter: " << prevCounter
                          << " which is greater than the server topology version counter: "
                          << topologyVersionCounter,
            prevCounter <= topologyVersionCounter);

    if (prevCounter < topologyVersionCounter) {
        // The received hello command contains a stale topology version so we respond
        // immediately with a more current topology version.
        return SharedSemiFuture<SharedHelloResponse>(
            SharedHelloResponse(_makeHelloResponse(horizonString, lk, hasValidConfig)));
    }

    if (!hasValidConfig) {
        // An empty SNI will correspond to kDefaultHorizon.
        const auto sni = horizonParams.sniName ? *horizonParams.sniName : "";
        auto sniIter =
            _sniToValidConfigPromiseMap
                .emplace(sni,
                         std::make_shared<SharedPromise<std::shared_ptr<const HelloResponse>>>())
                .first;
        return sniIter->second->getFuture();
    }
    // Each awaitable hello will wait on their specific horizon. We always expect horizonString
    // to exist in _horizonToTopologyChangePromiseMap.
    auto horizonIter = _horizonToTopologyChangePromiseMap.find(*horizonString);
    invariant(horizonIter != end(_horizonToTopologyChangePromiseMap));
    return horizonIter->second->getFuture();
}

SharedSemiFuture<ReplicationCoordinatorImpl::SharedHelloResponse>
ReplicationCoordinatorImpl::getHelloResponseFuture(
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion) {
    stdx::lock_guard lk(_mutex);
    const auto horizonString = _getHorizonString(lk, horizonParams);
    return _getHelloResponseFuture(lk, horizonParams, horizonString, clientTopologyVersion);
}

boost::optional<StringData> ReplicationCoordinatorImpl::_getHorizonString(
    WithLock, const SplitHorizon::Parameters& horizonParams) const {
    const auto myState = _topCoord->getMemberState();
    const bool hasValidConfig = _rsConfig.isInitialized() && !myState.removed();
    boost::optional<StringData> horizonString;
    if (hasValidConfig) {
        const auto& self = _rsConfig.getMemberAt(_selfIndex);
        horizonString = self.determineHorizon(horizonParams);
    }
    // A horizonString that is boost::none indicates that we do not have a valid config.
    return horizonString;
}

std::shared_ptr<const HelloResponse> ReplicationCoordinatorImpl::awaitHelloResponse(
    OperationContext* opCtx,
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion,
    boost::optional<Date_t> deadline) {
    stdx::unique_lock lk(_mutex);

    const auto horizonString = _getHorizonString(lk, horizonParams);
    auto future = _getHelloResponseFuture(lk, horizonParams, horizonString, clientTopologyVersion);
    if (future.isReady()) {
        return future.get();
    }

    // If clientTopologyVersion is not none, deadline must also be not none.
    invariant(deadline);
    const TopologyVersion topologyVersion = _topCoord->getTopologyVersion();

    HelloMetrics::get(opCtx)->incrementNumAwaitingTopologyChanges();
    lk.unlock();

    if (MONGO_unlikely(waitForHelloResponse.shouldFail())) {
        // Used in tests that wait for this failpoint to be entered before triggering a topology
        // change.
        LOGV2(31464, "waitForHelloResponse failpoint enabled");
    }
    if (MONGO_unlikely(hangWhileWaitingForHelloResponse.shouldFail())) {
        LOGV2(21341, "Hanging due to hangWhileWaitingForHelloResponse failpoint");
        hangWhileWaitingForHelloResponse.pauseWhileSet(opCtx);
    }

    // Wait for a topology change with timeout set to deadline.
    LOGV2_DEBUG(21342,
                1,
                "Waiting for a hello response from a topology change or until deadline: "
                "{deadline}. Current TopologyVersion counter is {currentTopologyVersionCounter}",
                "Waiting for a hello response from a topology change or until deadline",
                "deadline"_attr = deadline.value(),
                "currentTopologyVersionCounter"_attr = topologyVersion.getCounter());
    auto statusWithHello =
        futureGetNoThrowWithDeadline(opCtx, future, deadline.value(), opCtx->getTimeoutError());
    auto status = statusWithHello.getStatus();

    if (MONGO_unlikely(hangAfterWaitingForTopologyChangeTimesOut.shouldFail())) {
        LOGV2(4783200, "Hanging due to hangAfterWaitingForTopologyChangeTimesOut failpoint");
        hangAfterWaitingForTopologyChangeTimesOut.pauseWhileSet(opCtx);
    }

    setCustomErrorInHelloResponseMongoD.execute([&](const BSONObj& data) {
        auto errorCode = data["errorType"].safeNumberInt();
        LOGV2(6208200,
              "Triggered setCustomErrorInHelloResponseMongoD fail point.",
              "errorCode"_attr = errorCode);

        status = Status(ErrorCodes::Error(errorCode),
                        "Set by setCustomErrorInHelloResponseMongoD fail point.");
    });

    if (!status.isOK()) {
        LOGV2_DEBUG(6208204, 1, "Error while waiting for hello response", "status"_attr = status);

        // We decrement the counter on most errors. Note that some errors may already be covered
        // by calls to resetNumAwaitingTopologyChanges(), which sets the counter to zero, so we
        // only decrement non-zero counters. This is safe so long as:
        // 1) Increment + decrement calls always occur at a 1:1 ratio and in that order.
        // 2) All callers to increment/decrement/reset take locks.
        stdx::lock_guard lk(_mutex);
        if (status != ErrorCodes::SplitHorizonChange &&
            HelloMetrics::get(opCtx)->getNumAwaitingTopologyChanges() > 0) {
            HelloMetrics::get(opCtx)->decrementNumAwaitingTopologyChanges();
        }

        // Return a HelloResponse with the current topology version on timeout when waiting for
        // a topology change.
        if (status == ErrorCodes::ExceededTimeLimit) {
            // A topology change has not occured within the deadline so horizonString is still a
            // good indicator of whether we have a valid config.
            const bool hasValidConfig = horizonString != boost::none;
            return _makeHelloResponse(horizonString, lk, hasValidConfig);
        }
    }

    // A topology change has happened so we return a HelloResponse with the updated
    // topology version.
    uassertStatusOK(status);
    return statusWithHello.getValue();
}

StatusWith<OpTime> ReplicationCoordinatorImpl::getLatestWriteOpTime(OperationContext* opCtx) const
    noexcept try {
    ShouldNotConflictWithSecondaryBatchApplicationBlock noPBWMBlock(opCtx->lockState());
    Lock::GlobalLock globalLock(opCtx, MODE_IS);
    // Check if the node is primary after acquiring global IS lock.
    if (!canAcceptNonLocalWrites()) {
        return {ErrorCodes::NotWritablePrimary, "Not primary so can't get latest write optime"};
    }
    const auto& oplog = LocalOplogInfo::get(opCtx)->getCollection();
    if (!oplog) {
        return {ErrorCodes::NamespaceNotFound, "oplog collection does not exist"};
    }
    auto latestOplogTimestampSW = oplog->getRecordStore()->getLatestOplogTimestamp(opCtx);
    if (!latestOplogTimestampSW.isOK()) {
        return latestOplogTimestampSW.getStatus();
    }
    return OpTime(latestOplogTimestampSW.getValue(), getTerm());
} catch (const DBException& e) {
    return e.toStatus();
}

HostAndPort ReplicationCoordinatorImpl::getCurrentPrimaryHostAndPort() const {
    stdx::lock_guard<Latch> lock(_mutex);
    auto primary = _topCoord->getCurrentPrimaryMember();
    return primary ? primary->getHostAndPort() : HostAndPort();
}

void ReplicationCoordinatorImpl::cancelCbkHandle(CallbackHandle activeHandle) {
    _replExecutor->cancel(activeHandle);
}

BSONObj ReplicationCoordinatorImpl::runCmdOnPrimaryAndAwaitResponse(
    OperationContext* opCtx,
    const std::string& dbName,
    const BSONObj& cmdObj,
    OnRemoteCmdScheduledFn onRemoteCmdScheduled,
    OnRemoteCmdCompleteFn onRemoteCmdComplete) {
    // About to make network and DBDirectClient (recursive) calls, so we should not hold any locks.
    invariant(!opCtx->lockState()->isLocked());

    const auto primaryHostAndPort = getCurrentPrimaryHostAndPort();
    if (primaryHostAndPort.empty()) {
        uassertStatusOK(Status{ErrorCodes::NoConfigPrimary, "Primary is unknown/down."});
    }

    // Run the command via AsyncDBClient which performs a network call. This is also the desired
    // behaviour when running this command locally as to avoid using the DBDirectClient which would
    // provide additional management when trying to cancel the request with differing clients.
    executor::RemoteCommandRequest request(primaryHostAndPort, dbName, cmdObj, nullptr);
    executor::RemoteCommandResponse cbkResponse(
        Status{ErrorCodes::InternalError, "Uninitialized value"});

    // Schedule the remote command.
    auto&& scheduleResult = _replExecutor->scheduleRemoteCommand(
        request, [&cbkResponse](const executor::TaskExecutor::RemoteCommandCallbackArgs& cbk) {
            cbkResponse = cbk.response;
        });

    uassertStatusOK(scheduleResult.getStatus());
    CallbackHandle cbkHandle = scheduleResult.getValue();

    try {
        onRemoteCmdScheduled(cbkHandle);

        // Wait for the response in an interruptible mode.
        _replExecutor->wait(cbkHandle, opCtx);
    } catch (const DBException&) {
        // If waiting for the response is interrupted, then we still have a callback out and
        // registered with the TaskExecutor to run when the response finally does come back. Since
        // the callback references local state, cbkResponse, it would be invalid for the callback to
        // run after leaving the this function. Therefore, we cancel the callback and wait
        // uninterruptably for the callback to be run.
        _replExecutor->cancel(cbkHandle);
        _replExecutor->wait(cbkHandle);
        throw;
    }

    onRemoteCmdComplete(cbkHandle);
    uassertStatusOK(cbkResponse.status);
    return cbkResponse.data;
}

void ReplicationCoordinatorImpl::_killConflictingOpsOnStepUpAndStepDown(
    AutoGetRstlForStepUpStepDown* arsc, ErrorCodes::Error reason) {
    const OperationContext* rstlOpCtx = arsc->getOpCtx();
    ServiceContext* serviceCtx = rstlOpCtx->getServiceContext();
    invariant(serviceCtx);

    for (ServiceContext::LockedClientsCursor cursor(serviceCtx); Client* client = cursor.next();) {
        stdx::lock_guard<Client> lk(*client);
        if (client->isFromSystemConnection() && !client->canKillSystemOperationInStepdown(lk)) {
            continue;
        }

        OperationContext* toKill = client->getOperationContext();

        // Don't kill step up/step down thread.
        if (toKill && !toKill->isKillPending() && toKill->getOpID() != rstlOpCtx->getOpID()) {
            auto locker = toKill->lockState();
            if (toKill->shouldAlwaysInterruptAtStepDownOrUp() ||
                locker->wasGlobalLockTakenInModeConflictingWithWrites() ||
                PrepareConflictTracker::get(toKill).isWaitingOnPrepareConflict()) {
                serviceCtx->killOperation(lk, toKill, reason);
                arsc->incrementUserOpsKilled();
            } else {
                arsc->incrementUserOpsRunning();
            }
        }
    }
}

ReplicationCoordinatorImpl::AutoGetRstlForStepUpStepDown::AutoGetRstlForStepUpStepDown(
    ReplicationCoordinatorImpl* repl,
    OperationContext* opCtx,
    const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
    Date_t deadline)
    : _replCord(repl), _opCtx(opCtx), _stateTransition(stateTransition) {
    invariant(_replCord && _opCtx);

    // The state transition should never be rollback within this class.
    invariant(_stateTransition != ReplicationCoordinator::OpsKillingStateTransitionEnum::kRollback);

    int rstlTimeout = fassertOnLockTimeoutForStepUpDown.load();
    Date_t start{Date_t::now()};
    if (rstlTimeout > 0 && deadline - start > Seconds(rstlTimeout)) {
        deadline = start + Seconds(rstlTimeout);  // cap deadline
    }

    try {
        // Enqueues RSTL in X mode.
        _rstlLock.emplace(_opCtx, MODE_X, ReplicationStateTransitionLockGuard::EnqueueOnly());

        ON_BLOCK_EXIT([&] { _stopAndWaitForKillOpThread(); });
        _startKillOpThread();

        // Wait for RSTL to be acquired.
        _rstlLock->waitForLockUntil(deadline);

    } catch (const ExceptionFor<ErrorCodes::LockTimeout>&) {
        if (rstlTimeout > 0 && Date_t::now() - start >= Seconds(rstlTimeout)) {
            // Dump all locks to identify which thread(s) are holding RSTL.
            getGlobalLockManager()->dump();

            auto lockerInfo =
                opCtx->lockState()->getLockerInfo(CurOp::get(opCtx)->getLockStatsBase());
            BSONObjBuilder lockRep;
            lockerInfo->stats.report(&lockRep);
            LOGV2_FATAL(5675600,
                        "Time out exceeded waiting for RSTL, stepUp/stepDown is not possible "
                        "thus calling abort() to allow cluster to progress.",
                        "lockRep"_attr = lockRep.obj());
        }
        // Rethrow to keep processing as before at a higher layer.
        throw;
    }
};

void ReplicationCoordinatorImpl::AutoGetRstlForStepUpStepDown::_startKillOpThread() {
    invariant(!_killOpThread);
    _killOpThread = std::make_unique<stdx::thread>([this] { _killOpThreadFn(); });
}

void ReplicationCoordinatorImpl::AutoGetRstlForStepUpStepDown::_killOpThreadFn() {
    Client::initThread("RstlKillOpThread");

    invariant(!cc().isFromUserConnection());

    LOGV2(21343, "Starting to kill user operations");
    auto uniqueOpCtx = cc().makeOperationContext();
    OperationContext* opCtx = uniqueOpCtx.get();

    // Set the reason for killing operations.
    ErrorCodes::Error killReason = ErrorCodes::InterruptedDueToReplStateChange;

    while (true) {
        // Reset the value before killing user operations as we only want to track the number
        // of operations that's running after step down.
        _userOpsRunning = 0;
        _replCord->_killConflictingOpsOnStepUpAndStepDown(this, killReason);

        // Destroy all stashed transaction resources, in order to release locks.
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        killSessionsAbortUnpreparedTransactions(opCtx, matcherAllSessions, killReason);

        // Operations (like batch insert) that have currently yielded the global lock during step
        // down can reacquire global lock in IX mode when this node steps back up after a brief
        // network partition. And, this can lead to data inconsistency (see SERVER-27534). So,
        // its important we mark operations killed at least once after enqueuing the RSTL lock in
        // X mode for the first time. This ensures that no writing operations will continue
        // after the node's term change.
        {
            stdx::unique_lock<Latch> lock(_mutex);
            if (_stopKillingOps.wait_for(
                    lock, Milliseconds(10).toSystemDuration(), [this] { return _killSignaled; })) {
                LOGV2(21344, "Stopped killing user operations");
                _replCord->updateAndLogStateTransitionMetrics(
                    _stateTransition, getUserOpsKilled(), getUserOpsRunning());
                _killSignaled = false;
                return;
            }
        }
    }
}

void ReplicationCoordinatorImpl::AutoGetRstlForStepUpStepDown::_stopAndWaitForKillOpThread() {
    if (!(_killOpThread && _killOpThread->joinable()))
        return;

    {
        stdx::unique_lock<Latch> lock(_mutex);
        _killSignaled = true;
        _stopKillingOps.notify_all();
    }
    _killOpThread->join();
    _killOpThread.reset();
}

size_t ReplicationCoordinatorImpl::AutoGetRstlForStepUpStepDown::getUserOpsKilled() const {
    return _userOpsKilled;
}

void ReplicationCoordinatorImpl::AutoGetRstlForStepUpStepDown::incrementUserOpsKilled(size_t val) {
    _userOpsKilled += val;
}

size_t ReplicationCoordinatorImpl::AutoGetRstlForStepUpStepDown::getUserOpsRunning() const {
    return _userOpsRunning;
}

void ReplicationCoordinatorImpl::AutoGetRstlForStepUpStepDown::incrementUserOpsRunning(size_t val) {
    _userOpsRunning += val;
}

void ReplicationCoordinatorImpl::AutoGetRstlForStepUpStepDown::rstlRelease() {
    _rstlLock->release();
}

void ReplicationCoordinatorImpl::AutoGetRstlForStepUpStepDown::rstlReacquire() {
    // Ensure that we are not holding the RSTL lock in any mode.
    invariant(!_opCtx->lockState()->isRSTLLocked());

    // Since we have released the RSTL lock at this point, there can be some conflicting
    // operations sneaked in here. We need to kill those operations to acquire the RSTL lock.
    // Also, its ok to start "RstlKillOpthread" thread before RSTL lock enqueue as we kill
    // operations in a loop.
    ON_BLOCK_EXIT([&] { _stopAndWaitForKillOpThread(); });
    _startKillOpThread();
    _rstlLock->reacquire();
}

const OperationContext* ReplicationCoordinatorImpl::AutoGetRstlForStepUpStepDown::getOpCtx() const {
    return _opCtx;
}

void ReplicationCoordinatorImpl::stepDown(OperationContext* opCtx,
                                          const bool force,
                                          const Milliseconds& waitTime,
                                          const Milliseconds& stepdownTime) {
    const Date_t startTime = _replExecutor->now();
    const Date_t stepDownUntil = startTime + stepdownTime;
    const Date_t waitUntil = startTime + waitTime;

    // Note this check is inherently racy - it's always possible for the node to stepdown from some
    // other path before we acquire the global exclusive lock.  This check is just to try to save us
    // from acquiring the global X lock unnecessarily.
    uassert(ErrorCodes::NotWritablePrimary,
            "not primary so can't step down",
            getMemberState().primary());

    // This makes us tell the 'hello' command we can't accept writes (though in fact we can,
    // it is not valid to disable writes until we actually acquire the RSTL).
    {
        stdx::lock_guard lk(_mutex);
        _waitingForRSTLAtStepDown++;
        _fulfillTopologyChangePromise(lk);
    }
    ScopeGuard clearStepDownFlag([&] {
        stdx::lock_guard lk(_mutex);
        _waitingForRSTLAtStepDown--;
        _fulfillTopologyChangePromise(lk);
    });

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &stepdownHangBeforeRSTLEnqueue, opCtx, "stepdownHangBeforeRSTLEnqueue");

    // Using 'force' sets the default for the wait time to zero, which means the stepdown will
    // fail if it does not acquire the lock immediately. In such a scenario, we use the
    // stepDownUntil deadline instead.
    auto deadline = force ? stepDownUntil : waitUntil;
    AutoGetRstlForStepUpStepDown arsd(
        this, opCtx, ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown, deadline);

    stepdownHangAfterGrabbingRSTL.pauseWhileSet();

    stdx::unique_lock<Latch> lk(_mutex);

    opCtx->checkForInterrupt();

    const long long termAtStart = _topCoord->getTerm();

    // This will cause us to fail if we're already in the process of stepping down, or if we've
    // already successfully stepped down via another path.
    auto abortFn = uassertStatusOK(_topCoord->prepareForStepDownAttempt());

    // Update _canAcceptNonLocalWrites from the TopologyCoordinator now that we're in the middle
    // of a stepdown attempt.  This will prevent us from accepting writes so that if our stepdown
    // attempt fails later we can release the RSTL and go to sleep to allow secondaries to
    // catch up without allowing new writes in.
    _updateWriteAbilityFromTopologyCoordinator(lk, opCtx);
    auto action = _updateMemberStateFromTopologyCoordinator(lk);
    invariant(action == PostMemberStateUpdateAction::kActionNone);
    invariant(!_readWriteAbility->canAcceptNonLocalWrites(lk));

    // We truly cannot accept writes now, and we've updated the topology version to say so, so
    // no need for this flag any more, nor to increment the topology version again.
    _waitingForRSTLAtStepDown--;
    clearStepDownFlag.dismiss();

    auto updateMemberState = [&] {
        invariant(lk.owns_lock());
        invariant(opCtx->lockState()->isRSTLExclusive());

        // Make sure that we leave _canAcceptNonLocalWrites in the proper state.
        _updateWriteAbilityFromTopologyCoordinator(lk, opCtx);
        auto action = _updateMemberStateFromTopologyCoordinator(lk);
        lk.unlock();

        if (MONGO_unlikely(stepdownHangBeforePerformingPostMemberStateUpdateActions.shouldFail())) {
            LOGV2(21345,
                  "stepping down from primary - "
                  "stepdownHangBeforePerformingPostMemberStateUpdateActions fail point enabled. "
                  "Blocking until fail point is disabled");
            while (MONGO_unlikely(
                stepdownHangBeforePerformingPostMemberStateUpdateActions.shouldFail())) {
                mongo::sleepsecs(1);
                {
                    stdx::lock_guard<Latch> lock(_mutex);
                    if (_inShutdown) {
                        break;
                    }
                }
            }
        }

        _performPostMemberStateUpdateAction(action);
    };
    ScopeGuard onExitGuard([&] {
        abortFn();
        updateMemberState();
    });

    auto waitTimeout = std::min(waitTime, stepdownTime);

    // Set up a waiter which will be signaled when we process a heartbeat or updatePosition
    // and have a majority of nodes at our optime.
    const WriteConcernOptions waiterWriteConcern(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::NONE, waitTimeout);

    // If attemptStepDown() succeeds, we are guaranteed that no concurrent step up or
    // step down can happen afterwards. So, it's safe to release the mutex before
    // yieldLocksForPreparedTransactions().
    while (!_topCoord->tryToStartStepDown(
        termAtStart, _replExecutor->now(), waitUntil, stepDownUntil, force)) {

        // The stepdown attempt failed. We now release the RSTL to allow secondaries to read the
        // oplog, then wait until enough secondaries are caught up for us to finish stepdown.
        arsd.rstlRelease();
        invariant(!opCtx->lockState()->isLocked());

        // Make sure we re-acquire the RSTL before returning so that we're always holding the
        // RSTL when the onExitGuard set up earlier runs.
        ON_BLOCK_EXIT([&] {
            // Need to release _mutex before re-acquiring the RSTL to preserve lock acquisition
            // order rules.
            lk.unlock();

            // Need to re-acquire the RSTL before re-attempting stepdown. We use no timeout here
            // even though that means the lock acquisition could take longer than the stepdown
            // window. Since we'll need the RSTL no matter what to clean up a failed stepdown
            // attempt, we might as well spend whatever time we need to acquire it now.  For
            // the same reason, we also disable lock acquisition interruption, to guarantee that
            // we get the lock eventually.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());

            // Since we have released the RSTL lock at this point, there can be some read
            // operations sneaked in here, that might hold global lock in S mode or blocked on
            // prepare conflict. We need to kill those operations to avoid 3-way deadlock
            // between read, prepared transaction and step down thread. And, any write
            // operations that gets sneaked in here will fail as we have updated
            // _canAcceptNonLocalWrites to false after our first successful RSTL lock
            // acquisition. So, we won't get into problems like SERVER-27534.
            arsd.rstlReacquire();
            lk.lock();
        });

        auto lastAppliedOpTime = _getMyLastAppliedOpTime_inlock();
        auto currentTerm = _topCoord->getTerm();
        // If termAtStart != currentTerm, tryToStartStepDown would have thrown.
        invariant(termAtStart == currentTerm);
        // As we should not wait for secondaries to catch up if this node has not yet written in
        // this term, invariant that the lastAppliedOpTime we will wait for has the same term as the
        // current term. Also see TopologyCoordinator::isSafeToStepDown.
        invariant(lastAppliedOpTime.getTerm() == currentTerm);

        auto future = _replicationWaiterList.add_inlock(lastAppliedOpTime, waiterWriteConcern);

        lk.unlock();
        auto status = futureGetNoThrowWithDeadline(
            opCtx, future, std::min(stepDownUntil, waitUntil), ErrorCodes::ExceededTimeLimit);
        lk.lock();

        // We ignore the case where runWithDeadline returns timeoutError because in that case
        // coming back around the loop and calling tryToStartStepDown again will cause
        // tryToStartStepDown to return ExceededTimeLimit with the proper error message.
        if (!status.isOK() && status.code() != ErrorCodes::ExceededTimeLimit) {
            opCtx->checkForInterrupt();
        }
    }

    // Prepare for unconditional stepdown success!
    // We need to release the mutex before yielding locks for prepared transactions, which might
    // check out sessions, to avoid deadlocks with checked-out sessions accessing this mutex.
    lk.unlock();

    yieldLocksForPreparedTransactions(opCtx);
    invalidateSessionsForStepdown(opCtx);

    lk.lock();

    // Clear the node's election candidate metrics since it is no longer primary.
    ReplicationMetrics::get(opCtx).clearElectionCandidateMetrics();

    _topCoord->finishUnconditionalStepDown();

    onExitGuard.dismiss();
    updateMemberState();

    // Schedule work to (potentially) step back up once the stepdown period has ended.
    _scheduleWorkAt(stepDownUntil, [=](const executor::TaskExecutor::CallbackArgs& cbData) {
        _handleTimePassing(cbData);
    });

    // If election handoff is enabled, schedule a step-up immediately instead of waiting for the
    // election timeout to expire.
    if (!force && enableElectionHandoff.load()) {
        _performElectionHandoff();
    }
}

void ReplicationCoordinatorImpl::_performElectionHandoff() {
    stdx::lock_guard<Latch> lock(_mutex);
    auto candidateIndex = _topCoord->chooseElectionHandoffCandidate();

    if (candidateIndex < 0) {
        LOGV2(21346, "Could not find node to hand off election to");
        return;
    }

    auto target = _rsConfig.getMemberAt(candidateIndex).getHostAndPort();
    executor::RemoteCommandRequest request(
        target, "admin", BSON("replSetStepUp" << 1 << "skipDryRun" << true), nullptr);
    LOGV2(
        21347, "Handing off election to {target}", "Handing off election", "target"_attr = target);

    auto callbackHandleSW = _replExecutor->scheduleRemoteCommand(
        request, [target](const executor::TaskExecutor::RemoteCommandCallbackArgs& callbackData) {
            auto status = callbackData.response.status;

            if (status.isOK()) {
                LOGV2_DEBUG(21348,
                            1,
                            "replSetStepUp request to {target} succeeded with response -- "
                            "{response}",
                            "replSetStepUp request succeeded",
                            "target"_attr = target,
                            "response"_attr = callbackData.response.data);
            } else {
                LOGV2(21349,
                      "replSetStepUp request to {target} failed due to {error}",
                      "replSetStepUp request failed",
                      "target"_attr = target,
                      "error"_attr = status);
            }
        });

    auto callbackHandleStatus = callbackHandleSW.getStatus();
    if (!callbackHandleStatus.isOK()) {
        LOGV2_ERROR(21417,
                    "Failed to schedule ReplSetStepUp request to {target} for election handoff: "
                    "{error}",
                    "Failed to schedule replSetStepUp request for election handoff",
                    "target"_attr = target,
                    "error"_attr = callbackHandleStatus);
    }
}

void ReplicationCoordinatorImpl::_handleTimePassing(
    const executor::TaskExecutor::CallbackArgs& cbData) {
    if (!cbData.status.isOK()) {
        return;
    }

    // For election protocol v1, call _startElectSelfIfEligibleV1 to avoid race
    // against other elections caused by events like election timeout, replSetStepUp etc.
    _startElectSelfIfEligibleV1(StartElectionReasonEnum::kSingleNodePromptElection);
}

bool ReplicationCoordinatorImpl::isWritablePrimaryForReportingPurposes() {
    if (!_settings.usingReplSets()) {
        return true;
    }

    stdx::lock_guard<Latch> lock(_mutex);
    invariant(getReplicationMode() == modeReplSet);
    return _getMemberState_inlock().primary();
}

bool ReplicationCoordinatorImpl::canAcceptWritesForDatabase(OperationContext* opCtx,
                                                            StringData dbName) {
    // The answer isn't meaningful unless we hold the ReplicationStateTransitionLock.
    invariant(opCtx->lockState()->isRSTLLocked() || opCtx->isLockFreeReadsOp());
    return canAcceptWritesForDatabase_UNSAFE(opCtx, dbName);
}

bool ReplicationCoordinatorImpl::canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                                                   StringData dbName) {
    // _canAcceptNonLocalWrites is always true for standalone nodes, and adjusted based on
    // primary+drain state in replica sets.
    //
    // Stand-alone nodes and drained replica set primaries can always accept writes.  Writes are
    // always permitted to the "local" database.
    if (_readWriteAbility->canAcceptNonLocalWrites_UNSAFE() || alwaysAllowNonLocalWrites(opCtx)) {
        return true;
    }
    if (dbName == kLocalDB) {
        return true;
    }
    return false;
}

bool ReplicationCoordinatorImpl::canAcceptNonLocalWrites() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _readWriteAbility->canAcceptNonLocalWrites(lk);
}

bool ReplicationCoordinatorImpl::canAcceptWritesFor(OperationContext* opCtx,
                                                    const NamespaceStringOrUUID& nsOrUUID) {
    if (!isReplEnabled() || nsOrUUID.db() == kLocalDB) {
        // Writes on stand-alone nodes or "local" database are always permitted.
        return true;
    }

    invariant(opCtx->lockState()->isRSTLLocked(), nsOrUUID.toString());
    return canAcceptWritesFor_UNSAFE(opCtx, nsOrUUID);
}

bool ReplicationCoordinatorImpl::canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                                           const NamespaceStringOrUUID& nsOrUUID) {
    bool canWriteToDB = canAcceptWritesForDatabase_UNSAFE(opCtx, nsOrUUID.db());

    if (!canWriteToDB) {
        if (auto ns = nsOrUUID.nss()) {
            if (!ns->isSystemDotProfile()) {
                return false;
            }
        } else {
            auto uuid = nsOrUUID.uuid();
            invariant(uuid, nsOrUUID.toString());
            if (auto ns = CollectionCatalog::get(opCtx)->lookupNSSByUUID(opCtx, *uuid)) {
                if (!ns->isSystemDotProfile()) {
                    return false;
                }
            }
        }
    }

    // Even if we think we can write to the database we need to make sure we're not trying
    // to write to the oplog in ROLLBACK.
    // If we can accept non local writes (ie we're PRIMARY) then we must not be in ROLLBACK.
    // This check is redundant of the check of _memberState below, but since this can be checked
    // without locking, we do it as an optimization.
    if (_readWriteAbility->canAcceptNonLocalWrites_UNSAFE() || alwaysAllowNonLocalWrites(opCtx)) {
        return true;
    }

    if (auto ns = nsOrUUID.nss()) {
        if (!ns->isOplog()) {
            return true;
        }
    } else if (const auto& oplogCollection = LocalOplogInfo::get(opCtx)->getCollection()) {
        auto uuid = nsOrUUID.uuid();
        invariant(uuid, nsOrUUID.toString());
        if (oplogCollection->uuid() != *uuid) {
            return true;
        }
    }

    stdx::lock_guard<Latch> lock(_mutex);
    if (_memberState.rollback()) {
        return false;
    }
    return true;
}

Status ReplicationCoordinatorImpl::checkCanServeReadsFor(OperationContext* opCtx,
                                                         const NamespaceString& ns,
                                                         bool secondaryOk) {
    invariant(opCtx->lockState()->isRSTLLocked() || opCtx->isLockFreeReadsOp());
    return checkCanServeReadsFor_UNSAFE(opCtx, ns, secondaryOk);
}

Status ReplicationCoordinatorImpl::checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                                const NamespaceString& ns,
                                                                bool secondaryOk) {
    auto client = opCtx->getClient();
    bool isPrimaryOrSecondary = _readWriteAbility->canServeNonLocalReads_UNSAFE();

    // Always allow reads from the direct client, no matter what.
    if (client->isInDirectClient()) {
        return Status::OK();
    }

    // Oplog reads are not allowed during STARTUP state, but we make an exception for internal
    // reads. Internal reads are required for cleaning up unfinished apply batches.
    if (!isPrimaryOrSecondary && getReplicationMode() == modeReplSet && ns.isOplog()) {
        stdx::lock_guard<Latch> lock(_mutex);
        if ((_memberState.startup() && client->isFromUserConnection()) || _memberState.startup2() ||
            _memberState.rollback()) {
            return Status{ErrorCodes::NotPrimaryOrSecondary,
                          "Oplog collection reads are not allowed while in the rollback or "
                          "startup state."};
        }
    }

    // Non-oplog local reads from the user are not allowed during initial sync when the initial
    // sync method disallows it.  "isFromUserConnection" means DBDirectClient reads are not blocked;
    // "isInternalClient" means reads from other cluster members are not blocked.
    if (!isPrimaryOrSecondary && getReplicationMode() == modeReplSet && ns.db() == kLocalDB &&
        client->isFromUserConnection()) {
        stdx::lock_guard<Latch> lock(_mutex);
        auto isInternalClient = !client->session() ||
            (client->session()->getTags() & transport::Session::kInternalClient);
        if (!isInternalClient && _memberState.startup2() && _initialSyncer &&
            !_initialSyncer->allowLocalDbAccess()) {
            return Status{ErrorCodes::NotPrimaryOrSecondary,
                          str::stream() << "Local reads are not allowed during initial sync with "
                                           "current initial sync method: "
                                        << _initialSyncer->getInitialSyncMethod()};
        }
    }


    if (canAcceptWritesFor_UNSAFE(opCtx, ns)) {
        return Status::OK();
    }

    if (opCtx->inMultiDocumentTransaction()) {
        if (!_readWriteAbility->canAcceptNonLocalWrites_UNSAFE()) {
            return Status(ErrorCodes::NotWritablePrimary,
                          "Multi-document transactions are only allowed on replica set primaries.");
        }
    }

    if (secondaryOk) {
        if (isPrimaryOrSecondary) {
            return Status::OK();
        }
        const auto msg = client->supportsHello()
            ? "not primary or secondary; cannot currently read from this replSet member"_sd
            : "not master or secondary; cannot currently read from this replSet member"_sd;
        return Status(ErrorCodes::NotPrimaryOrSecondary, msg);
    }

    const auto msg = client->supportsHello() ? "not primary and secondaryOk=false"_sd
                                             : "not master and slaveOk=false"_sd;
    return Status(ErrorCodes::NotPrimaryNoSecondaryOk, msg);
}

bool ReplicationCoordinatorImpl::isInPrimaryOrSecondaryState(OperationContext* opCtx) const {
    return _readWriteAbility->canServeNonLocalReads(opCtx);
}

bool ReplicationCoordinatorImpl::isInPrimaryOrSecondaryState_UNSAFE() const {
    return _readWriteAbility->canServeNonLocalReads_UNSAFE();
}

bool ReplicationCoordinatorImpl::shouldRelaxIndexConstraints(OperationContext* opCtx,
                                                             const NamespaceString& ns) {
    if (ReplSettings::shouldRecoverFromOplogAsStandalone() || !recoverToOplogTimestamp.empty() ||
        tenantMigrationInfo(opCtx)) {
        return true;
    }
    return !canAcceptWritesFor(opCtx, ns);
}

OID ReplicationCoordinatorImpl::getElectionId() {
    stdx::lock_guard<Latch> lock(_mutex);
    return _electionId;
}

int ReplicationCoordinatorImpl::getMyId() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _getMyId_inlock();
}

HostAndPort ReplicationCoordinatorImpl::getMyHostAndPort() const {
    stdx::unique_lock<Latch> lk(_mutex);
    if (_selfIndex == -1) {
        return HostAndPort();
    }
    return _rsConfig.getMemberAt(_selfIndex).getHostAndPort();
}

int ReplicationCoordinatorImpl::_getMyId_inlock() const {
    const MemberConfig& self = _rsConfig.getMemberAt(_selfIndex);
    return self.getId().getData();
}

StatusWith<BSONObj> ReplicationCoordinatorImpl::prepareReplSetUpdatePositionCommand() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _topCoord->prepareReplSetUpdatePositionCommand(
        _getCurrentCommittedSnapshotOpTime_inlock());
}

Status ReplicationCoordinatorImpl::processReplSetGetStatus(
    OperationContext* opCtx,
    BSONObjBuilder* response,
    ReplSetGetStatusResponseStyle responseStyle) {

    BSONObj initialSyncProgress;
    if (responseStyle == ReplSetGetStatusResponseStyle::kInitialSync) {
        std::shared_ptr<InitialSyncerInterface> initialSyncerCopy;
        {
            stdx::lock_guard<Latch> lk(_mutex);
            initialSyncerCopy = _initialSyncer;
        }

        // getInitialSyncProgress must be called outside the ReplicationCoordinatorImpl::_mutex
        // lock. Else it might deadlock with InitialSyncer::_multiApplierCallback where it first
        // acquires InitialSyncer::_mutex and then ReplicationCoordinatorImpl::_mutex.
        if (initialSyncerCopy) {
            initialSyncProgress = initialSyncerCopy->getInitialSyncProgress();
        }
    }

    BSONObj electionCandidateMetrics =
        ReplicationMetrics::get(getServiceContext()).getElectionCandidateMetricsBSON();
    BSONObj electionParticipantMetrics =
        ReplicationMetrics::get(getServiceContext()).getElectionParticipantMetricsBSON();

    boost::optional<Timestamp> lastStableRecoveryTimestamp = boost::none;
    try {
        // Retrieving last stable recovery timestamp should not be blocked by oplog
        // application.
        ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
            opCtx->lockState());
        opCtx->lockState()->skipAcquireTicket();
        // We need to hold the lock so that we don't run when storage is being shutdown.
        Lock::GlobalLock lk(opCtx,
                            MODE_IS,
                            Date_t::now() + Milliseconds(5),
                            Lock::InterruptBehavior::kLeaveUnlocked,
                            true /* skipRSTLLock */);
        if (lk.isLocked()) {
            lastStableRecoveryTimestamp = _storage->getLastStableRecoveryTimestamp(_service);
        } else {
            LOGV2_WARNING(6100702,
                          "Failed to get last stable recovery timestamp due to {error}",
                          "error"_attr = "lock acquire timeout"_sd);
        }
    } catch (const ExceptionForCat<ErrorCategory::CancellationError>& ex) {
        LOGV2_WARNING(6100703,
                      "Failed to get last stable recovery timestamp due to {error}",
                      "error"_attr = redact(ex));
    }

    stdx::lock_guard<Latch> lk(_mutex);
    if (_inShutdown) {
        return Status(ErrorCodes::ShutdownInProgress, "shutdown in progress");
    }

    Status result(ErrorCodes::InternalError, "didn't set status in prepareStatusResponse");
    _topCoord->prepareStatusResponse(
        TopologyCoordinator::ReplSetStatusArgs{
            _replExecutor->now(),
            static_cast<unsigned>(time(nullptr) - serverGlobalParams.started),
            _getCurrentCommittedSnapshotOpTime_inlock(),
            initialSyncProgress,
            electionCandidateMetrics,
            electionParticipantMetrics,
            lastStableRecoveryTimestamp,
            _externalState->tooStale()},
        response,
        &result);
    return result;
}

void ReplicationCoordinatorImpl::appendSecondaryInfoData(BSONObjBuilder* result) {
    stdx::lock_guard<Latch> lock(_mutex);
    _topCoord->fillMemberData(result);
}

ReplSetConfig ReplicationCoordinatorImpl::getConfig() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig;
}

ConnectionString ReplicationCoordinatorImpl::getConfigConnectionString() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.getConnectionString();
}

Milliseconds ReplicationCoordinatorImpl::getConfigElectionTimeoutPeriod() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.getElectionTimeoutPeriod();
}

std::vector<MemberConfig> ReplicationCoordinatorImpl::getConfigVotingMembers() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.votingMembers();
}

std::int64_t ReplicationCoordinatorImpl::getConfigTerm() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.getConfigTerm();
}

std::int64_t ReplicationCoordinatorImpl::getConfigVersion() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.getConfigVersion();
}

ConfigVersionAndTerm ReplicationCoordinatorImpl::getConfigVersionAndTerm() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.getConfigVersionAndTerm();
}

int ReplicationCoordinatorImpl::getConfigNumMembers() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.getNumMembers();
}

Milliseconds ReplicationCoordinatorImpl::getConfigHeartbeatTimeoutPeriodMillis() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.getHeartbeatTimeoutPeriodMillis();
}

BSONObj ReplicationCoordinatorImpl::getConfigBSON() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.toBSON();
}

const MemberConfig* ReplicationCoordinatorImpl::findConfigMemberByHostAndPort(
    const HostAndPort& hap) const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.findMemberByHostAndPort(hap);
}

bool ReplicationCoordinatorImpl::isConfigLocalHostAllowed() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.isLocalHostAllowed();
}

Milliseconds ReplicationCoordinatorImpl::getConfigHeartbeatInterval() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.getHeartbeatInterval();
}

Status ReplicationCoordinatorImpl::validateWriteConcern(
    const WriteConcernOptions& writeConcern) const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.validateWriteConcern(writeConcern);
}

WriteConcernOptions ReplicationCoordinatorImpl::_getOplogCommitmentWriteConcern(WithLock lk) {
    auto syncMode = getWriteConcernMajorityShouldJournal_inlock()
        ? WriteConcernOptions::SyncMode::JOURNAL
        : WriteConcernOptions::SyncMode::NONE;
    WriteConcernOptions oplogWriteConcern(
        ReplSetConfig::kMajorityWriteConcernModeName, syncMode, WriteConcernOptions::kNoTimeout);
    return oplogWriteConcern;
}

WriteConcernOptions ReplicationCoordinatorImpl::_getConfigReplicationWriteConcern() {
    WriteConcernOptions configWriteConcern(ReplSetConfig::kConfigMajorityWriteConcernModeName,
                                           WriteConcernOptions::SyncMode::NONE,
                                           WriteConcernOptions::kNoTimeout);
    configWriteConcern.checkCondition = WriteConcernOptions::CheckCondition::Config;
    return configWriteConcern;
}

void ReplicationCoordinatorImpl::processReplSetGetConfig(BSONObjBuilder* result,
                                                         bool commitmentStatus,
                                                         bool includeNewlyAdded) {
    stdx::lock_guard<Latch> lock(_mutex);
    if (includeNewlyAdded) {
        result->append("config", _rsConfig.toBSON());
    } else {
        result->append("config", _rsConfig.toBSONWithoutNewlyAdded());
    }

    if (commitmentStatus) {
        uassert(ErrorCodes::NotWritablePrimary,
                "commitmentStatus is only supported on primary.",
                _readWriteAbility->canAcceptNonLocalWrites(lock));
        auto configWriteConcern = _getConfigReplicationWriteConcern();
        auto configOplogCommitmentOpTime = _topCoord->getConfigOplogCommitmentOpTime();
        auto oplogWriteConcern = _getOplogCommitmentWriteConcern(lock);

        // OpTime isn't used when checking for config replication.
        OpTime ignored;
        auto committed = _doneWaitingForReplication_inlock(ignored, configWriteConcern) &&
            _doneWaitingForReplication_inlock(configOplogCommitmentOpTime, oplogWriteConcern);
        result->append("commitmentStatus", committed);
    }
}

void ReplicationCoordinatorImpl::processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) {
    EventHandle evh;

    if (_needToUpdateTerm(replMetadata.getTerm())) {
        {
            stdx::lock_guard<Latch> lock(_mutex);
            evh = _processReplSetMetadata_inlock(replMetadata);
        }

        if (evh) {
            _replExecutor->waitForEvent(evh);
        }
    }
}

void ReplicationCoordinatorImpl::cancelAndRescheduleElectionTimeout() {
    stdx::lock_guard<Latch> lock(_mutex);
    _cancelAndRescheduleElectionTimeout_inlock();
}

EventHandle ReplicationCoordinatorImpl::_processReplSetMetadata_inlock(
    const rpc::ReplSetMetadata& replMetadata) {
    // Note that public method processReplSetMetadata() above depends on this method not needing
    // to do anything when the term is up to date.  If that changes, be sure to update that
    // method as well.
    return _updateTerm_inlock(replMetadata.getTerm());
}

bool ReplicationCoordinatorImpl::getMaintenanceMode() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _topCoord->getMaintenanceCount() > 0;
}

Status ReplicationCoordinatorImpl::setMaintenanceMode(OperationContext* opCtx, bool activate) {
    if (getReplicationMode() != modeReplSet) {
        return Status(ErrorCodes::NoReplicationEnabled,
                      "can only set maintenance mode on replica set members");
    }

    // It is possible that we change state to or from RECOVERING. Thus, we need the RSTL in X mode.
    ReplicationStateTransitionLockGuard transitionGuard(opCtx, MODE_X);

    stdx::unique_lock<Latch> lk(_mutex);
    if (_topCoord->getRole() == TopologyCoordinator::Role::kCandidate ||
        MONGO_unlikely(setMaintenanceModeFailsWithNotSecondary.shouldFail())) {
        return Status(ErrorCodes::NotSecondary, "currently running for election");
    }

    if (_getMemberState_inlock().primary()) {
        return Status(ErrorCodes::NotSecondary, "primaries can't modify maintenance mode");
    }

    int curMaintenanceCalls = _topCoord->getMaintenanceCount();
    if (activate) {
        LOGV2(21350,
              "going into maintenance mode with {otherMaintenanceModeTasksInProgress} "
              "other maintenance mode tasks in progress",
              "Going into maintenance mode",
              "otherMaintenanceModeTasksInProgress"_attr = curMaintenanceCalls);
        _topCoord->adjustMaintenanceCountBy(1);
    } else if (curMaintenanceCalls > 0) {
        invariant(_topCoord->getRole() == TopologyCoordinator::Role::kFollower);

        _topCoord->adjustMaintenanceCountBy(-1);

        LOGV2(21351,
              "leaving maintenance mode ({otherMaintenanceModeTasksOngoing} other maintenance mode "
              "tasks ongoing)",
              "Leaving maintenance mode",
              "otherMaintenanceModeTasksOngoing"_attr = curMaintenanceCalls - 1);
    } else {
        LOGV2_WARNING(21411, "Attempted to leave maintenance mode but it is not currently active");
        return Status(ErrorCodes::OperationFailed, "already out of maintenance mode");
    }

    const PostMemberStateUpdateAction action = _updateMemberStateFromTopologyCoordinator(lk);
    lk.unlock();
    _performPostMemberStateUpdateAction(action);
    return Status::OK();
}

bool ReplicationCoordinatorImpl::shouldDropSyncSourceAfterShardSplit(const OID replicaSetId) const {
    if (!_settings.isServerless()) {
        return false;
    }

    stdx::lock_guard<Latch> lg(_mutex);

    return replicaSetId != _rsConfig.getReplicaSetId();
}

Status ReplicationCoordinatorImpl::processReplSetSyncFrom(OperationContext* opCtx,
                                                          const HostAndPort& target,
                                                          BSONObjBuilder* resultObj) {
    Status result(ErrorCodes::InternalError, "didn't set status in prepareSyncFromResponse");
    std::shared_ptr<InitialSyncerInterface> initialSyncerCopy;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _topCoord->prepareSyncFromResponse(target, resultObj, &result);
        // _initialSyncer must not be called with repl mutex held.
        initialSyncerCopy = _initialSyncer;
    }

    // If we are in the middle of an initial sync, do a resync.
    if (result.isOK() && initialSyncerCopy) {
        initialSyncerCopy->cancelCurrentAttempt();
    }
    return result;
}

Status ReplicationCoordinatorImpl::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
    auto result = [=]() {
        stdx::lock_guard<Latch> lock(_mutex);
        return _topCoord->prepareFreezeResponse(_replExecutor->now(), secs, resultObj);
    }();
    if (!result.isOK()) {
        return result.getStatus();
    }

    if (TopologyCoordinator::PrepareFreezeResponseResult::kSingleNodeSelfElect ==
        result.getValue()) {
        // For election protocol v1, call _startElectSelfIfEligibleV1 to avoid race
        // against other elections caused by events like election timeout, replSetStepUp etc.
        _startElectSelfIfEligibleV1(StartElectionReasonEnum::kSingleNodePromptElection);
    }

    return Status::OK();
}

Status ReplicationCoordinatorImpl::processReplSetReconfig(OperationContext* opCtx,
                                                          const ReplSetReconfigArgs& args,
                                                          BSONObjBuilder* resultObj) {
    LOGV2(21352,
          "replSetReconfig admin command received from client; new config: {newConfig}",
          "replSetReconfig admin command received from client",
          "newConfig"_attr = args.newConfigObj);

    auto getNewConfig = [&](const ReplSetConfig& oldConfig,
                            long long currentTerm) -> StatusWith<ReplSetConfig> {
        ReplSetConfig newConfig;

        // Only explicitly set configTerm to this node's term for non-force reconfigs.
        // Otherwise, use -1.
        auto term = (!args.force) ? currentTerm : OpTime::kUninitializedTerm;

        // When initializing a new config through the replSetReconfig command, ignore the term
        // field passed in through its args. Instead, use this node's term.
        try {
            newConfig = ReplSetConfig::parse(args.newConfigObj, term, oldConfig.getReplicaSetId());
        } catch (const DBException& e) {
            auto status = e.toStatus();
            LOGV2_ERROR(21418,
                        "replSetReconfig got {error} while parsing {newConfig}",
                        "replSetReconfig error parsing new config",
                        "error"_attr = status,
                        "newConfig"_attr = args.newConfigObj);
            return Status(ErrorCodes::InvalidReplicaSetConfig, status.reason());
        }

        if (newConfig.getReplSetName() != oldConfig.getReplSetName()) {
            static constexpr char errmsg[] =
                "Rejecting reconfig where new config set name differs from command line set name";
            LOGV2_ERROR(21419,
                        errmsg,
                        "newConfigSetName"_attr = newConfig.getReplSetName(),
                        "oldConfigSetName"_attr = oldConfig.getReplSetName());
            return Status(ErrorCodes::InvalidReplicaSetConfig,
                          str::stream()
                              << errmsg << ", new config set name: " << newConfig.getReplSetName()
                              << ", old config set name: " << oldConfig.getReplSetName());
        }

        if (args.force) {
            // Increase the config version for force reconfig.
            auto version = std::max(oldConfig.getConfigVersion(), newConfig.getConfigVersion());
            version += 10'000 + SecureRandom().nextInt32(100'000);
            auto newMutableConfig = newConfig.getMutable();
            newMutableConfig.setConfigVersion(version);
            newConfig = ReplSetConfig(std::move(newMutableConfig));
        }

        boost::optional<MutableReplSetConfig> newMutableConfig;

        // Set the 'newlyAdded' field to true for all new voting nodes.
        for (int i = 0; i < newConfig.getNumMembers(); i++) {
            const auto newMem = newConfig.getMemberAt(i);

            // In a reconfig, the 'newlyAdded' flag should never already be set for
            // this member. If it is set, throw an error.
            if (newMem.isNewlyAdded()) {
                str::stream errmsg;
                errmsg << "Cannot provide " << MemberConfig::kNewlyAddedFieldName
                       << " field to member config during reconfig.";
                LOGV2_ERROR(4634900,
                            "Initializing 'newlyAdded' field to member has failed with bad status.",
                            "errmsg"_attr = std::string(errmsg));
                return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
            }

            // We should never set the 'newlyAdded' field for arbiters or during force reconfigs.
            if (newMem.isArbiter() || args.force) {
                continue;
            }

            const auto newMemId = newMem.getId();
            const auto oldMem = oldConfig.findMemberByID(newMemId.getData());

            const bool isNewVotingMember = (oldMem == nullptr && newMem.isVoter());
            const bool isCurrentlyNewlyAdded = (oldMem != nullptr && oldMem->isNewlyAdded());

            // Append the 'newlyAdded' field if the node:
            // 1) Is a new, voting node
            // 2) Already has a 'newlyAdded' field in the old config
            if (isNewVotingMember || isCurrentlyNewlyAdded) {
                if (!newMutableConfig) {
                    newMutableConfig = newConfig.getMutable();
                }
                newMutableConfig->addNewlyAddedFieldForMember(newMemId);
            }
        }

        if (newMutableConfig) {
            newConfig = ReplSetConfig(*std::move(newMutableConfig));
            LOGV2(4634400,
                  "Appended the 'newlyAdded' field to a node in the new config. Nodes with "
                  "the 'newlyAdded' field will be considered to have 'votes:0'. Upon "
                  "transition to SECONDARY, this field will be automatically removed.",
                  "newConfigObj"_attr = newConfig.toBSON(),
                  "userProvidedConfig"_attr = args.newConfigObj,
                  "oldConfig"_attr = oldConfig.toBSON());
        }

        return newConfig;
    };

    return doReplSetReconfig(opCtx, getNewConfig, args.force);
}

Status ReplicationCoordinatorImpl::doOptimizedReconfig(OperationContext* opCtx,
                                                       GetNewConfigFn getNewConfig) {
    return _doReplSetReconfig(opCtx, getNewConfig, false /* force */, true /* skipSafetyChecks*/);
}

Status ReplicationCoordinatorImpl::doReplSetReconfig(OperationContext* opCtx,
                                                     GetNewConfigFn getNewConfig,
                                                     bool force) {
    return _doReplSetReconfig(opCtx, getNewConfig, force, false /* skipSafetyChecks*/);
}

Status ReplicationCoordinatorImpl::_doReplSetReconfig(OperationContext* opCtx,
                                                      GetNewConfigFn getNewConfig,
                                                      bool force,
                                                      bool skipSafetyChecks) {
    stdx::unique_lock<Latch> lk(_mutex);

    while (_rsConfigState == kConfigPreStart || _rsConfigState == kConfigStartingUp) {
        _rsConfigStateChange.wait(lk);
    }

    switch (_rsConfigState) {
        case kConfigSteady:
            break;
        case kConfigUninitialized:
            return Status(ErrorCodes::NotYetInitialized,
                          "Node not yet initialized; use the replSetInitiate command");
        case kConfigReplicationDisabled:
            invariant(
                false);       // should be unreachable due to !_settings.usingReplSets() check above
            [[fallthrough]];  // Placate clang.
        case kConfigInitiating:
        case kConfigReconfiguring:
        case kConfigHBReconfiguring:
            return Status(ErrorCodes::ConfigurationInProgress,
                          "Cannot run replSetReconfig because the node is currently updating "
                          "its configuration");
        default:
            LOGV2_FATAL(18914,
                        "Unexpected _rsConfigState {_rsConfigState}",
                        "Unexpected _rsConfigState",
                        "_rsConfigState"_attr = int(_rsConfigState));
    }

    LOGV2(6015313, "Replication config state is Steady, starting reconfig");
    invariant(_rsConfig.isInitialized());

    if (!force && !_readWriteAbility->canAcceptNonLocalWrites(lk) && !skipSafetyChecks) {
        return Status(
            ErrorCodes::NotWritablePrimary,
            str::stream()
                << "Safe reconfig is only allowed on a writable PRIMARY. Current state is "
                << _getMemberState_inlock().toString());
    }
    auto topCoordTerm = _topCoord->getTerm();

    if (!force && !skipSafetyChecks) {
        // For safety of reconfig, since we must commit a config in our own term before executing a
        // reconfig, so we should never have a config in an older term. If the current config was
        // installed via a force reconfig, we aren't concerned about this safety guarantee.
        invariant(_rsConfig.getConfigTerm() == OpTime::kUninitializedTerm ||
                  _rsConfig.getConfigTerm() == topCoordTerm);
    }

    auto configWriteConcern = _getConfigReplicationWriteConcern();
    // Construct a fake OpTime that can be accepted but isn't used.
    OpTime fakeOpTime(Timestamp(1, 1), topCoordTerm);

    if (!force && !skipSafetyChecks) {
        if (!_doneWaitingForReplication_inlock(fakeOpTime, configWriteConcern)) {
            return Status(ErrorCodes::CurrentConfigNotCommittedYet,
                          str::stream()
                              << "Cannot run replSetReconfig because the current config: "
                              << _rsConfig.getConfigVersionAndTerm().toString() << " is not "
                              << "majority committed.");
        }

        // Make sure that the latest committed optime from the previous config is committed in the
        // current config. If this is the initial reconfig, then we don't need to check this
        // condition, since there were no prior configs. Also, for force reconfigs we bypass this
        // safety check condition.
        auto isInitialReconfig = (_rsConfig.getConfigVersion() == 1);
        // If our config was installed via a "force" reconfig, we bypass the oplog commitment check.
        auto leavingForceConfig = (_rsConfig.getConfigTerm() == OpTime::kUninitializedTerm);
        auto configOplogCommitmentOpTime = _topCoord->getConfigOplogCommitmentOpTime();
        auto oplogWriteConcern = _getOplogCommitmentWriteConcern(lk);

        if (!leavingForceConfig && !isInitialReconfig &&
            !_doneWaitingForReplication_inlock(configOplogCommitmentOpTime, oplogWriteConcern)) {
            LOGV2(51816,
                  "Oplog config commitment condition failed to be satisfied. The last committed "
                  "optime in the previous config ({configOplogCommitmentOpTime}) is not committed "
                  "in current config",
                  "Oplog config commitment condition failed to be satisfied. The last committed "
                  "optime in the previous config is not committed in current config",
                  "configOplogCommitmentOpTime"_attr = configOplogCommitmentOpTime);
            return Status(ErrorCodes::CurrentConfigNotCommittedYet,
                          str::stream() << "Last committed optime from previous config ("
                                        << configOplogCommitmentOpTime.toString()
                                        << ") is not committed in the current config.");
        }
    }

    _setConfigState_inlock(kConfigReconfiguring);
    auto configStateGuard =
        ScopeGuard([&] { lockAndCall(&lk, [=] { _setConfigState_inlock(kConfigSteady); }); });

    ReplSetConfig oldConfig = _rsConfig;
    int myIndex = _selfIndex;
    lk.unlock();

    // Call the callback to get the new config given the old one.
    auto newConfigStatus = getNewConfig(oldConfig, topCoordTerm);
    Status status = newConfigStatus.getStatus();
    if (!status.isOK())
        return status;
    ReplSetConfig newConfig = newConfigStatus.getValue();

    // Synchronize this change with potential changes to the default write concern.
    auto wcChanges = getWriteConcernTagChanges();
    auto mustReleaseWCChange = false;
    if (!oldConfig.areWriteConcernModesTheSame(&newConfig)) {
        if (!wcChanges->reserveConfigWriteConcernTagChange()) {
            return Status(
                ErrorCodes::ConflictingOperationInProgress,
                "Default write concern change(s) in progress. Please retry the reconfig later.");
        }
        // Reservation OK.
        mustReleaseWCChange = true;
    }

    ON_BLOCK_EXIT([&] {
        if (mustReleaseWCChange) {
            wcChanges->releaseConfigWriteConcernTagChange();
        }
    });

    hangBeforeNewConfigValidationChecks.pauseWhileSet();

    // Excluding reconfigs that bump the config term during step-up from checking against changing
    // the implicit default write concern, as it is not needed.
    if (!skipSafetyChecks /* skipping step-up reconfig */) {
        bool currIDWC = oldConfig.isImplicitDefaultWriteConcernMajority();
        bool newIDWC = newConfig.isImplicitDefaultWriteConcernMajority();

        // If the new config changes the replica set's implicit default write concern, we fail the
        // reconfig command. This includes force reconfigs.
        // The user should set a cluster-wide write concern and attempt the reconfig command again.
        if (serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
            if (!repl::enableDefaultWriteConcernUpdatesForInitiate.load() && currIDWC != newIDWC &&
                !ReadWriteConcernDefaults::get(opCtx).isCWWCSet(opCtx)) {
                return Status(
                    ErrorCodes::NewReplicaSetConfigurationIncompatible,
                    str::stream()
                        << "Reconfig attempted to install a config that would change the implicit "
                           "default write concern. Use the setDefaultRWConcern command to set a "
                           "cluster-wide write concern and try the reconfig again.");
            }
        } else {
            // Allow all reconfigs if the shard is not part of a sharded cluster yet, however
            // prevent changing the implicit default write concern to (w: 1) after it becomes part
            // of a sharded cluster and CWWC is not set on the cluster.
            // Remote call to the configServer should be done to check if CWWC is set on the
            // cluster.
            if (_externalState->isShardPartOfShardedCluster(opCtx) && currIDWC != newIDWC &&
                !newIDWC) {
                try {
                    // Initiates a remote call to the config server.
                    if (!_externalState->isCWWCSetOnConfigShard(opCtx)) {
                        return Status(
                            ErrorCodes::NewReplicaSetConfigurationIncompatible,
                            str::stream()
                                << "Reconfig attempted to install a config that would change the "
                                   "implicit default write concern on the shard to {w: 1}. Use the "
                                   "setDefaultRWConcern command to set a cluster-wide write "
                                   "concern on the cluster and try the reconfig again.");
                    }
                } catch (const DBException& ex) {
                    return Status(
                        ErrorCodes::ConfigServerUnreachable,
                        str::stream()
                            << "Reconfig attempted to install a config that would change the "
                               "implicit default write concern on the shard to {w: 1}, but the "
                               "shard can not check if CWWC is set on the cluster, as the request "
                               "to the config server is failing with error: " +
                                ex.toString());
                }
            }
        }

        // If we are currently using a custom write concern as the default, check that the
        // corresponding definition still exists in the new config.
        if (serverGlobalParams.clusterRole == ClusterRole::None) {
            try {
                const auto rwcDefaults =
                    ReadWriteConcernDefaults::get(opCtx->getServiceContext()).getDefault(opCtx);
                const auto wcDefault = rwcDefaults.getDefaultWriteConcern();
                if (wcDefault) {
                    auto validateWCStatus = newConfig.validateWriteConcern(wcDefault.value());
                    if (!validateWCStatus.isOK()) {
                        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                                      str::stream() << "May not remove custom write concern "
                                                       "definition from config while it is in "
                                                       "use as the default write concern. "
                                                       "Change the default write concern to a "
                                                       "non-conflicting setting and try the "
                                                       "reconfig again.");
                    }
                }
            } catch (const DBException& e) {
                return e.toStatus("Exception while loading write concern default during reconfig");
            }
        }
    }

    BSONObj oldConfigObj = oldConfig.toBSON();
    BSONObj newConfigObj = newConfig.toBSON();
    audit::logReplSetReconfig(opCtx->getClient(), &oldConfigObj, &newConfigObj);

    bool allowSplitHorizonIP = !opCtx->getClient()->hasRemote();

    Status validateStatus =
        validateConfigForReconfig(oldConfig, newConfig, force, allowSplitHorizonIP);
    if (!validateStatus.isOK()) {
        LOGV2_ERROR(21420,
                    "replSetReconfig got {error} while validating {newConfig}",
                    "replSetReconfig error while validating new config",
                    "error"_attr = validateStatus,
                    "newConfig"_attr = newConfigObj,
                    "oldConfig"_attr = oldConfigObj);
        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible, validateStatus.reason());
    }

    if (MONGO_unlikely(ReconfigHangBeforeConfigValidationCheck.shouldFail())) {
        LOGV2(4637900,
              "ReconfigHangBeforeConfigValidationCheck fail point "
              "enabled. Blocking until fail point is disabled.");
        ReconfigHangBeforeConfigValidationCheck.pauseWhileSet(opCtx);
    }

    // Make sure we can find ourselves in the config. If the config contents have not changed, then
    // we bypass the check for finding ourselves in the config, since we know it should already be
    // satisfied. There is also one further optimization here: if we have a valid _selfIndex, we can
    // do a quick and cheap pass first to see if host and port exist in the new config. This is safe
    // as we are not allowed to have the same HostAndPort in the config twice. Matching HostandPort
    // implies matching isSelf, and it is actually preferrable to avoid checking the latter as it is
    // succeptible to transient DNS errors.
    auto quickIndex =
        _selfIndex >= 0 ? findOwnHostInConfigQuick(newConfig, getMyHostAndPort()) : -1;
    if (quickIndex >= 0) {
        if (!force) {
            auto electableStatus = checkElectable(newConfig, quickIndex);
            if (!electableStatus.isOK()) {
                LOGV2_ERROR(6475002,
                            "Not electable in new config and force=false, rejecting",
                            "error"_attr = electableStatus,
                            "newConfig"_attr = newConfigObj);
                return electableStatus;
            }
        }
        LOGV2(6475000,
              "Was able to quickly find new index in config. Skipping full checks.",
              "index"_attr = quickIndex,
              "force"_attr = force);
        myIndex = quickIndex;
    } else {
        // Either our HostAndPort changed in the config or we didn't have a _selfIndex.
        if (skipSafetyChecks) {
            LOGV2_ERROR(5986700,
                        "Configuration changed substantially in a config change that should have "
                        "only changed term",
                        "oldConfig"_attr = oldConfig,
                        "newConfig"_attr = newConfig);
            // Always debug assert if we reach this point.
            dassert(!skipSafetyChecks);
        }
        LOGV2(6015314, "Finding self in new config");
        StatusWith<int> myIndexSw = force
            ? findSelfInConfig(_externalState.get(), newConfig, opCtx->getServiceContext())
            : findSelfInConfigIfElectable(
                  _externalState.get(), newConfig, opCtx->getServiceContext());
        if (!myIndexSw.getStatus().isOK()) {
            LOGV2_ERROR(4751504,
                        "replSetReconfig error while trying to find self in config",
                        "error"_attr = myIndexSw.getStatus(),
                        "force"_attr = force,
                        "newConfig"_attr = newConfigObj);
            return myIndexSw.getStatus();
        }
        myIndex = myIndexSw.getValue();
    }

    LOGV2(21353,
          "replSetReconfig config object with {numMembers} members parses ok",
          "replSetReconfig config object parses ok",
          "numMembers"_attr = newConfig.getNumMembers());

    if (!force && !skipSafetyChecks && !MONGO_unlikely(omitConfigQuorumCheck.shouldFail())) {
        LOGV2(4509600, "Executing quorum check for reconfig");
        status =
            checkQuorumForReconfig(_replExecutor.get(), newConfig, myIndex, _topCoord->getTerm());
        if (!status.isOK()) {
            LOGV2_ERROR(21421,
                        "replSetReconfig failed; {error}",
                        "replSetReconfig failed",
                        "error"_attr = status);
            return status;
        }
    }

    LOGV2(51814, "Persisting new config to disk");
    {
        Lock::GlobalLock globalLock(opCtx, LockMode::MODE_IX);
        if (!force && !_readWriteAbility->canAcceptNonLocalWrites(opCtx) && !skipSafetyChecks) {
            return {ErrorCodes::NotWritablePrimary, "Stepped down when persisting new config"};
        }

        // Don't write no-op for internal and external force reconfig.
        // For non-force reconfigs with 'skipSafetyChecks' set to false, we are guaranteed that the
        // node is a writable primary.
        // When 'skipSafetyChecks' is true, it is possible the node is not yet a writable primary
        // (eg. in the case where reconfig is called during stepup). In all other cases, we should
        // still do the no-op write when possible.
        status = _externalState->storeLocalConfigDocument(
            opCtx,
            newConfig.toBSON(),
            !force && _readWriteAbility->canAcceptNonLocalWrites(opCtx) /* writeOplog */);
        if (!status.isOK()) {
            LOGV2_ERROR(21422,
                        "replSetReconfig failed to store config document; {error}",
                        "replSetReconfig failed to store config document",
                        "error"_attr = status);
            return status;
        }
    }
    // Wait for durability of the new config document.
    JournalFlusher::get(opCtx)->waitForJournalFlush();
    LOGV2(6015315, "Persisted new config to disk");

    configStateGuard.dismiss();
    _finishReplSetReconfig(opCtx, newConfig, force, myIndex);

    if (MONGO_unlikely(hangAfterReconfig.shouldFail())) {
        LOGV2(5940904, "Hanging after reconfig on fail point");
        hangAfterReconfig.pauseWhileSet();
    }

    return Status::OK();
}

void ReplicationCoordinatorImpl::_finishReplSetReconfig(OperationContext* opCtx,
                                                        const ReplSetConfig& newConfig,
                                                        const bool isForceReconfig,
                                                        int myIndex) {
    // Do not conduct an election during a reconfig, as the node may not be electable post-reconfig.
    executor::TaskExecutor::EventHandle electionFinishedEvent;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        electionFinishedEvent = _cancelElectionIfNeeded(lk);
    }

    // If there is an election in-progress, there can be at most one. No new election can happen as
    // we have already set our ReplicationCoordinatorImpl::_rsConfigState state to
    // "kConfigReconfiguring" which prevents new elections from happening.
    if (electionFinishedEvent) {
        LOGV2(21354,
              "Waiting for election to complete before finishing reconfig to config with "
              "{configVersionAndTerm}",
              "Waiting for election to complete before finishing reconfig",
              "configVersionAndTerm"_attr = newConfig.getConfigVersionAndTerm());
        // Wait for the election to complete and the node's Role to be set to follower.
        _replExecutor->waitForEvent(electionFinishedEvent);
    }

    boost::optional<AutoGetRstlForStepUpStepDown> arsd;
    stdx::unique_lock<Latch> lk(_mutex);
    if (isForceReconfig && _shouldStepDownOnReconfig(lk, newConfig, myIndex)) {
        _topCoord->prepareForUnconditionalStepDown();
        lk.unlock();

        // Primary node won't be electable or removed after the configuration change.
        // So, finish the reconfig under RSTL, so that the step down occurs safely.
        arsd.emplace(this, opCtx, ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown);

        lk.lock();
        if (_topCoord->isSteppingDownUnconditionally()) {
            invariant(opCtx->lockState()->isRSTLExclusive());
            LOGV2(21355, "Stepping down from primary, because we received a new config");
            // We need to release the mutex before yielding locks for prepared transactions, which
            // might check out sessions, to avoid deadlocks with checked-out sessions accessing
            // this mutex.
            lk.unlock();

            yieldLocksForPreparedTransactions(opCtx);
            invalidateSessionsForStepdown(opCtx);

            lk.lock();

            // Clear the node's election candidate metrics since it is no longer primary.
            ReplicationMetrics::get(opCtx).clearElectionCandidateMetrics();

            // Update _canAcceptNonLocalWrites.
            _updateWriteAbilityFromTopologyCoordinator(lk, opCtx);
        } else {
            // Release the rstl lock as the node might have stepped down due to
            // other unconditional step down code paths like learning new term via heartbeat &
            // liveness timeout. And, no new election can happen as we have already set our
            // ReplicationCoordinatorImpl::_rsConfigState state to "kConfigReconfiguring" which
            // prevents new elections from happening. So, its safe to release the RSTL lock.
            arsd.reset();
        }
    }

    invariant(_rsConfigState == kConfigReconfiguring);
    invariant(_rsConfig.isInitialized());

    const ReplSetConfig oldConfig = _rsConfig;
    const PostMemberStateUpdateAction action = _setCurrentRSConfig(lk, opCtx, newConfig, myIndex);

    // Record the latest committed optime in the current config atomically with the new config
    // taking effect. Once we have acquired the replication mutex above, we are ensured that no new
    // writes will be committed in the previous config, since any other system operation must
    // acquire the mutex to advance the commit point.
    _topCoord->updateLastCommittedInPrevConfig();

    // Safe reconfig guarantees that all committed entries are safe, so we can keep our commit
    // point. One exception is when we change the meaning of the "committed" snapshot from applied
    // -> durable. We have to drop all snapshots so we don't mistakenly read from the wrong one.
    auto defaultDurableChanged = oldConfig.getWriteConcernMajorityShouldJournal() !=
        newConfig.getWriteConcernMajorityShouldJournal();
    // If the new config has the same content but different version and term, like on stepup, we
    // don't need to drop snapshots either, since the quorum condition is still the same.
    auto newConfigCopy = newConfig.getMutable();
    newConfigCopy.setConfigTerm(oldConfig.getConfigTerm());
    newConfigCopy.setConfigVersion(oldConfig.getConfigVersion());
    auto contentChanged =
        SimpleBSONObjComparator::kInstance.evaluate(oldConfig.toBSON() != newConfigCopy.toBSON());
    if (defaultDurableChanged || (isForceReconfig && contentChanged)) {
        _clearCommittedSnapshot_inlock();
    }

    // If we have a split config, schedule heartbeats to each recipient member. It informs them of
    // the new split config.
    if (newConfig.isSplitConfig()) {
        const auto now = _replExecutor->now();
        const auto recipientConfig = newConfig.getRecipientConfig();
        for (const auto& member : recipientConfig->members()) {
            _scheduleHeartbeatToTarget_inlock(
                member.getHostAndPort(), now, newConfig.getReplSetName().toString());
        }
    }

    lk.unlock();
    _performPostMemberStateUpdateAction(action);
}

Status ReplicationCoordinatorImpl::awaitConfigCommitment(OperationContext* opCtx,
                                                         bool waitForOplogCommitment) {
    stdx::unique_lock<Latch> lk(_mutex);
    // Check writable primary before waiting.
    if (!_readWriteAbility->canAcceptNonLocalWrites(lk)) {
        return {
            ErrorCodes::PrimarySteppedDown,
            "replSetReconfig should only be run on a writable PRIMARY. Current state {};"_format(
                _memberState.toString())};
    }
    auto configOplogCommitmentOpTime = _topCoord->getConfigOplogCommitmentOpTime();
    auto oplogWriteConcern = _getOplogCommitmentWriteConcern(lk);
    OpTime fakeOpTime(Timestamp(1, 1), _topCoord->getTerm());
    auto currConfig = _rsConfig;
    lk.unlock();

    // Wait for the config document to be replicated to a majority of nodes in the current config.
    LOGV2(4508702, "Waiting for the current config to propagate to a majority of nodes");
    StatusAndDuration configAwaitStatus =
        awaitReplication(opCtx, fakeOpTime, _getConfigReplicationWriteConcern());

    logv2::DynamicAttributes attr;
    attr.add("configVersion", currConfig.getConfigVersion());
    attr.add("configTerm", currConfig.getConfigTerm());
    attr.add("configWaitDuration", configAwaitStatus.duration);
    if (!configAwaitStatus.status.isOK()) {
        LOGV2_WARNING(4714200, "Current config hasn't propagated to a majority of nodes", attr);
        std::stringstream ss;
        ss << "Current config with " << currConfig.getConfigVersionAndTerm().toString()
           << " has not yet propagated to a majority of nodes";
        return configAwaitStatus.status.withContext(ss.str());
    }

    if (!waitForOplogCommitment) {
        LOGV2(4689401, "Propagated current replica set config to a majority of nodes", attr);
        return Status::OK();
    }

    // Wait for the latest committed optime in the previous config to be committed in the
    // current config.
    LOGV2(51815,
          "Waiting for the last committed optime in the previous config "
          "to be committed in the current config",
          "configOplogCommitmentOpTime"_attr = configOplogCommitmentOpTime);
    StatusAndDuration oplogAwaitStatus =
        awaitReplication(opCtx, configOplogCommitmentOpTime, oplogWriteConcern);
    attr.add("oplogWaitDuration", oplogAwaitStatus.duration);
    attr.add("configOplogCommitmentOpTime", configOplogCommitmentOpTime);
    if (!oplogAwaitStatus.status.isOK()) {
        LOGV2_WARNING(4714201,
                      "Last committed optime in previous config isn't committed in current config",
                      attr);
        std::stringstream ss;
        ss << "Last committed optime in the previous config ("
           << configOplogCommitmentOpTime.toString()
           << ") has not yet become committed in the current config with "
           << currConfig.getConfigVersionAndTerm().toString();
        return oplogAwaitStatus.status.withContext(ss.str());
    }
    LOGV2(4508701, "The current replica set config is committed", attr);
    return Status::OK();
}


void ReplicationCoordinatorImpl::_reconfigToRemoveNewlyAddedField(
    const executor::TaskExecutor::CallbackArgs& cbData,
    MemberId memberId,
    ConfigVersionAndTerm versionAndTerm) {
    if (cbData.status == ErrorCodes::CallbackCanceled) {
        LOGV2_DEBUG(4634502,
                    2,
                    "Failed to remove 'newlyAdded' config field",
                    "memberId"_attr = memberId.getData(),
                    "error"_attr = cbData.status);
        // We will retry on the next heartbeat.
        return;
    }

    if (MONGO_unlikely(doNotRemoveNewlyAddedOnHeartbeats.shouldFail())) {
        LOGV2(
            4709200,
            "Not removing 'newlyAdded' field due to 'doNotremoveNewlyAddedOnHeartbeats' failpoint",
            "memberId"_attr = memberId.getData());
        return;
    }

    LOGV2(4634505,
          "Beginning automatic reconfig to remove 'newlyAdded' config field",
          "memberId"_attr = memberId.getData());

    auto getNewConfig = [&](const repl::ReplSetConfig& oldConfig,
                            long long term) -> StatusWith<ReplSetConfig> {
        // Even though memberIds should properly identify nodes across config changes, to be safe we
        // only want to do an automatic reconfig where the base config is the one that specified
        // this memberId.
        if (oldConfig.getConfigVersionAndTerm() != versionAndTerm) {
            return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                          str::stream()
                              << "Current config is no longer consistent with heartbeat "
                                 "data. Current config version: "
                              << oldConfig.getConfigVersionAndTerm().toString()
                              << ", heartbeat data config version: " << versionAndTerm.toString());
        }

        auto newConfig = oldConfig.getMutable();
        newConfig.setConfigVersion(newConfig.getConfigVersion() + 1);

        const auto hasNewlyAddedField =
            oldConfig.findMemberByID(memberId.getData())->isNewlyAdded();
        if (!hasNewlyAddedField) {
            return Status(ErrorCodes::NoSuchKey, "Old config no longer has 'newlyAdded' field");
        }

        newConfig.removeNewlyAddedFieldForMember(memberId);
        return ReplSetConfig(std::move(newConfig));
    };

    auto opCtx = cc().makeOperationContext();

    // Set info for currentOp to display if called while this is still running.
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        auto curOp = CurOp::get(opCtx.get());
        curOp->setLogicalOp_inlock(LogicalOp::opCommand);
        BSONObjBuilder bob;
        bob.append("replSetReconfig", "automatic");
        bob.append("memberId", memberId.getData());
        bob.append("configVersionAndTerm", versionAndTerm.toString());
        bob.append("info",
                   "An automatic reconfig. Used to remove a 'newlyAdded' config field for a "
                   "replica set member.");
        curOp->setOpDescription_inlock(bob.obj());
        curOp->setNS_inlock("local.system.replset");
        curOp->ensureStarted();
    }

    if (MONGO_unlikely(hangDuringAutomaticReconfig.shouldFail())) {
        LOGV2(4635700,
              "Failpoint 'hangDuringAutomaticReconfig' enabled. Blocking until it is disabled.");
        hangDuringAutomaticReconfig.pauseWhileSet();
    }

    auto status = doReplSetReconfig(opCtx.get(), getNewConfig, false /* force */);

    if (!status.isOK()) {
        LOGV2_DEBUG(4634503,
                    2,
                    "Failed to remove 'newlyAdded' config field",
                    "memberId"_attr = memberId.getData(),
                    "error"_attr = status);
        // It is safe to do nothing here as we will retry this on the next heartbeat, or we may
        // instead find out the reconfig already took place and is no longer necessary.
        return;
    }

    numAutoReconfigsForRemovalOfNewlyAddedFields.increment(1);

    // We intentionally do not wait for config commitment. If the config does not get committed, we
    // will try again on the next heartbeat.
    LOGV2(4634504, "Removed 'newlyAdded' config field", "memberId"_attr = memberId.getData());
}

Status ReplicationCoordinatorImpl::processReplSetInitiate(OperationContext* opCtx,
                                                          const BSONObj& configObj,
                                                          BSONObjBuilder* resultObj) {
    LOGV2(21356, "replSetInitiate admin command received from client");

    stdx::unique_lock<Latch> lk(_mutex);
    if (!isReplEnabled()) {
        return Status(ErrorCodes::NoReplicationEnabled, "server is not running with --replSet");
    }
    while (_rsConfigState == kConfigPreStart || _rsConfigState == kConfigStartingUp) {
        _rsConfigStateChange.wait(lk);
    }

    if (_rsConfigState != kConfigUninitialized) {
        resultObj->append("info", "try querying local.system.replset to see current configuration");
        return Status(ErrorCodes::AlreadyInitialized, "already initialized");
    }
    invariant(!_rsConfig.isInitialized());
    _setConfigState_inlock(kConfigInitiating);

    ScopeGuard configStateGuard = [&] {
        lockAndCall(&lk, [=] { _setConfigState_inlock(kConfigUninitialized); });
    };

    // When writing our first oplog entry below, disable advancement of the stable timestamp so that
    // we don't set it before setting our initial data timestamp. We will set it after we set our
    // initialDataTimestamp. This will ensure we trigger an initial stable checkpoint properly.
    if (!serverGlobalParams.enableMajorityReadConcern) {
        _shouldSetStableTimestamp = false;
    }

    lk.unlock();

    // Initiate FCV in local storage. This will propagate to other nodes via initial sync.
    FeatureCompatibilityVersion::setIfCleanStartup(opCtx, _storage);

    ReplSetConfig newConfig;
    try {
        newConfig = ReplSetConfig::parseForInitiate(configObj, OID::gen());
    } catch (const DBException& e) {
        Status status = e.toStatus();
        LOGV2_ERROR(21423,
                    "replSet initiate got {error} while parsing {config}",
                    "replSetInitiate error while parsing config",
                    "error"_attr = status,
                    "config"_attr = configObj);
        return Status(ErrorCodes::InvalidReplicaSetConfig, status.reason());
    }

    // The setname is not provided as a command line argument in serverless mode.
    if (!_settings.isServerless() && newConfig.getReplSetName() != _settings.ourSetName()) {
        static constexpr char errmsg[] =
            "Rejecting initiate with a set name that differs from command line set name";
        LOGV2_ERROR(21424,
                    errmsg,
                    "initiateSetName"_attr = newConfig.getReplSetName(),
                    "commandLineSetName"_attr = _settings.ourSetName());
        return Status(ErrorCodes::InvalidReplicaSetConfig,
                      str::stream()
                          << errmsg << ", initiate set name: " << newConfig.getReplSetName()
                          << ", command line set name: " << _settings.ourSetName());
    }

    StatusWith<int> myIndex =
        validateConfigForInitiate(_externalState.get(), newConfig, opCtx->getServiceContext());
    if (!myIndex.isOK()) {
        LOGV2_ERROR(21425,
                    "replSet initiate got {error} while validating {config}",
                    "replSetInitiate error while validating config",
                    "error"_attr = myIndex.getStatus(),
                    "config"_attr = configObj);
        return Status(ErrorCodes::InvalidReplicaSetConfig, myIndex.getStatus().reason());
    }

    LOGV2(21357,
          "replSetInitiate config object with {numMembers} members parses ok",
          "replSetInitiate config object parses ok",
          "numMembers"_attr = newConfig.getNumMembers());

    // In pv1, the TopologyCoordinator has not set the term yet. It will be set to kInitialTerm if
    // the initiate succeeds so we pass that here.
    auto status = checkQuorumForInitiate(
        _replExecutor.get(), newConfig, myIndex.getValue(), OpTime::kInitialTerm);

    if (!status.isOK()) {
        LOGV2_ERROR(21426,
                    "replSetInitiate failed; {error}",
                    "replSetInitiate failed",
                    "error"_attr = status);
        return status;
    }

    status = _externalState->initializeReplSetStorage(opCtx, newConfig.toBSON());
    if (!status.isOK()) {
        LOGV2_ERROR(21427,
                    "replSetInitiate failed to store config document or create the oplog; {error}",
                    "replSetInitiate failed to store config document or create the oplog",
                    "error"_attr = status);
        return status;
    }

    _replicationProcess->getConsistencyMarkers()->initializeMinValidDocument(opCtx);

    auto lastAppliedOpTimeAndWallTime = getMyLastAppliedOpTimeAndWallTime();

    // Since the JournalListener has not yet been set up, we must manually set our
    // durableOpTime.
    setMyLastDurableOpTimeAndWallTime(lastAppliedOpTimeAndWallTime);

    // Sets the initial data timestamp on the storage engine so it can assign a timestamp
    // to data on disk. We do this after writing the "initiating set" oplog entry.
    _storage->setInitialDataTimestamp(getServiceContext(),
                                      lastAppliedOpTimeAndWallTime.opTime.getTimestamp());

    // Set our stable timestamp for storage and re-enable stable timestamp advancement after we have
    // set our initial data timestamp.
    if (!serverGlobalParams.enableMajorityReadConcern) {
        stdx::unique_lock<Latch> lk(_mutex);
        _shouldSetStableTimestamp = true;
        _setStableTimestampForStorage(lk);
    }

    // In the EMRC=true case, we need to advance the commit point and take a stable checkpoint,
    // to make sure that we can recover if we happen to roll back our first entries after
    // replSetInitiate.
    if (serverGlobalParams.enableMajorityReadConcern) {
        LOGV2_INFO(5872101, "Taking a stable checkpoint for replSetInitiate");
        stdx::unique_lock<Latch> lk(_mutex);
        // Will call _setStableTimestampForStorage() on success.
        _advanceCommitPoint(
            lk, lastAppliedOpTimeAndWallTime, false /* fromSyncSource */, true /* forInitiate */);
        opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx,
                                                                 /*stableCheckpoint*/ true);
    }

    _finishReplSetInitiate(opCtx, newConfig, myIndex.getValue());
    // A configuration passed to replSetInitiate() with the current node as an arbiter
    // will fail validation with a "replSet initiate got ... while validating" reason.
    invariant(!newConfig.getMemberAt(myIndex.getValue()).isArbiter());
    _externalState->startThreads();
    _startDataReplication(opCtx);

    configStateGuard.dismiss();
    return Status::OK();
}

void ReplicationCoordinatorImpl::_finishReplSetInitiate(OperationContext* opCtx,
                                                        const ReplSetConfig& newConfig,
                                                        int myIndex) {
    stdx::unique_lock<Latch> lk(_mutex);
    invariant(_rsConfigState == kConfigInitiating);
    invariant(!_rsConfig.isInitialized());
    auto action = _setCurrentRSConfig(lk, opCtx, newConfig, myIndex);
    lk.unlock();
    _performPostMemberStateUpdateAction(action);
}

void ReplicationCoordinatorImpl::_setConfigState_inlock(ConfigState newState) {
    if (newState != _rsConfigState) {
        LOGV2(6015317,
              "Setting new configuration state",
              "newState"_attr = getConfigStateString(newState),
              "oldState"_attr = getConfigStateString(_rsConfigState));
        _rsConfigState = newState;
        _rsConfigStateChange.notify_all();
    }
}

std::string ReplicationCoordinatorImpl::getConfigStateString(ConfigState state) {
    switch (state) {
        case kConfigPreStart:
            return "ConfigPreStart";
        case kConfigStartingUp:
            return "ConfigStartingUp";
        case kConfigReplicationDisabled:
            return "ConfigReplicationDisabled";
        case kConfigUninitialized:
            return "ConfigUninitialized";
        case kConfigSteady:
            return "ConfigSteady";
        case kConfigInitiating:
            return "ConfigInitiating";
        case kConfigReconfiguring:
            return "ConfigReconfiguring";
        case kConfigHBReconfiguring:
            return "ConfigHBReconfiguring";
        default:
            MONGO_UNREACHABLE;
    }
}

void ReplicationCoordinatorImpl::_errorOnPromisesIfHorizonChanged(WithLock lk,
                                                                  OperationContext* opCtx,
                                                                  const ReplSetConfig& oldConfig,
                                                                  const ReplSetConfig& newConfig,
                                                                  int oldIndex,
                                                                  int newIndex) {
    if (newIndex < 0) {
        // When a node is removed, always return a hello response indicating the server has no
        // config set.
        return;
    }

    // We were previously removed but are now rejoining the replica set.
    if (_memberState.removed()) {
        // Reply with an error to hello requests received while the node had an invalid config.
        invariant(_horizonToTopologyChangePromiseMap.empty());

        for (const auto& [sni, promise] : _sniToValidConfigPromiseMap) {
            promise->setError({ErrorCodes::SplitHorizonChange,
                               "Received a reconfig that changed the horizon mappings."});
        }
        _sniToValidConfigPromiseMap.clear();
        HelloMetrics::get(opCtx)->resetNumAwaitingTopologyChanges();
    }

    if (oldIndex >= 0) {
        invariant(_sniToValidConfigPromiseMap.empty());

        const auto oldHorizonMappings = oldConfig.getMemberAt(oldIndex).getHorizonMappings();
        const auto newHorizonMappings = newConfig.getMemberAt(newIndex).getHorizonMappings();
        if (oldHorizonMappings != newHorizonMappings) {
            for (const auto& [horizon, promise] : _horizonToTopologyChangePromiseMap) {
                promise->setError({ErrorCodes::SplitHorizonChange,
                                   "Received a reconfig that changed the horizon mappings."});
            }
            _createHorizonTopologyChangePromiseMapping(lk);
            HelloMetrics::get(opCtx)->resetNumAwaitingTopologyChanges();
        }
    }
}

void ReplicationCoordinatorImpl::_fulfillTopologyChangePromise(WithLock lock) {
    _topCoord->incrementTopologyVersion();
    _cachedTopologyVersionCounter.store(_topCoord->getTopologyVersion().getCounter());
    const auto myState = _topCoord->getMemberState();
    const bool hasValidConfig = _rsConfig.isInitialized() && !myState.removed();
    // Create a hello response for each horizon the server is knowledgeable about.
    for (auto iter = _horizonToTopologyChangePromiseMap.begin();
         iter != _horizonToTopologyChangePromiseMap.end();
         iter++) {
        if (_inQuiesceMode) {
            iter->second->setError(
                Status(ShutdownInProgressQuiesceInfo(_calculateRemainingQuiesceTimeMillis()),
                       kQuiesceModeShutdownMessage));
        } else {
            StringData horizonString = iter->first;
            auto response = _makeHelloResponse(horizonString, lock, hasValidConfig);
            // Fulfill the promise and replace with a new one for future waiters.
            iter->second->emplaceValue(response);
            iter->second = std::make_shared<SharedPromise<std::shared_ptr<const HelloResponse>>>();
        }
    }
    if (_selfIndex >= 0 && !_sniToValidConfigPromiseMap.empty()) {
        // We are joining the replica set for the first time. Send back an error to hello
        // requests that are waiting on a horizon that does not exist in the new config. Otherwise,
        // reply with an updated hello response.
        const auto& reverseHostMappings =
            _rsConfig.getMemberAt(_selfIndex).getHorizonReverseHostMappings();
        for (const auto& [sni, promise] : _sniToValidConfigPromiseMap) {
            const auto iter = reverseHostMappings.find(sni);
            if (!sni.empty() && iter == end(reverseHostMappings)) {
                promise->setError({ErrorCodes::SplitHorizonChange,
                                   "The original request horizon parameter does not exist in the "
                                   "current replica set config"});
            } else {
                const auto horizon = sni.empty() ? SplitHorizon::kDefaultHorizon : iter->second;
                const auto response = _makeHelloResponse(horizon, lock, hasValidConfig);
                promise->emplaceValue(response);
            }
        }
        _sniToValidConfigPromiseMap.clear();
    }
    HelloMetrics::get(getGlobalServiceContext())->resetNumAwaitingTopologyChanges();

    if (_inQuiesceMode) {
        // No more hello requests will wait for a topology change, so clear _horizonToPromiseMap.
        _horizonToTopologyChangePromiseMap.clear();
    }
}

void ReplicationCoordinatorImpl::incrementTopologyVersion() {
    stdx::lock_guard lk(_mutex);
    _fulfillTopologyChangePromise(lk);
}

void ReplicationCoordinatorImpl::_updateWriteAbilityFromTopologyCoordinator(
    WithLock lk, OperationContext* opCtx) {
    bool canAcceptWrites = _topCoord->canAcceptWrites();
    _readWriteAbility->setCanAcceptNonLocalWrites(lk, opCtx, canAcceptWrites);
}

ReplicationCoordinatorImpl::PostMemberStateUpdateAction
ReplicationCoordinatorImpl::_updateMemberStateFromTopologyCoordinator(WithLock lk) {
    // We want to respond to any waiting hellos even if our current and target state are the
    // same as it is possible writes have been disabled during a stepDown but the primary has yet
    // to transition to SECONDARY state.  We do not do so when _waitingForRSTLAtStepDown is true
    // because in that case we have already said we cannot accept writes in the hello response
    // and explictly incremented the toplogy version.
    ON_BLOCK_EXIT([&] {
        if (_rsConfig.isInitialized() && !_waitingForRSTLAtStepDown) {
            _fulfillTopologyChangePromise(lk);
        }
    });

    const MemberState newState = _topCoord->getMemberState();

    if (newState == _memberState) {
        return kActionNone;
    }

    PostMemberStateUpdateAction result;
    if (_memberState.primary() || newState.removed() || newState.rollback()) {
        // Wake up any threads blocked in awaitReplication, close connections, etc.
        _replicationWaiterList.setErrorAll_inlock(
            {ErrorCodes::PrimarySteppedDown, "Primary stepped down while waiting for replication"});
        // Wake up the optime waiter that is waiting for primary catch-up to finish.
        _opTimeWaiterList.setErrorAll_inlock(
            {ErrorCodes::PrimarySteppedDown, "Primary stepped down while waiting for replication"});

        // _canAcceptNonLocalWrites should already be set.
        invariant(!_readWriteAbility->canAcceptNonLocalWrites(lk));

        serverGlobalParams.validateFeaturesAsPrimary.store(false);
        result = (newState.removed() || newState.rollback()) ? kActionRollbackOrRemoved
                                                             : kActionSteppedDown;
    } else {
        result = kActionFollowerModeStateChange;
    }

    // Exit catchup mode if we're in it and enable replication producer and applier on stepdown.
    if (_memberState.primary()) {
        if (_catchupState) {
            // _pendingTermUpdateDuringStepDown is set before stepping down due to hearing about a
            // higher term, so that we can remember the term we heard and update our term as part of
            // finishing stepdown. It is then unset toward the end of stepdown, after the function
            // we are in is called. Thus we must be stepping down due to seeing a higher term if and
            // only if _pendingTermUpdateDuringStepDown is set here.
            if (_pendingTermUpdateDuringStepDown) {
                _catchupState->abort_inlock(PrimaryCatchUpConclusionReason::kFailedWithNewTerm);
            } else {
                _catchupState->abort_inlock(PrimaryCatchUpConclusionReason::kFailedWithError);
            }
        }
        _applierState = ApplierState::Running;
        _externalState->startProducerIfStopped();
    }

    if (_memberState.secondary() && !newState.primary() && !newState.rollback()) {
        // Switching out of SECONDARY, but not to PRIMARY or ROLLBACK. Note that ROLLBACK case is
        // handled separately and requires RSTL lock held, see setFollowerModeRollback.
        _readWriteAbility->setCanServeNonLocalReads_UNSAFE(0U);
    } else if (!_memberState.primary() && newState.secondary()) {
        // Switching into SECONDARY, but not from PRIMARY.
        _readWriteAbility->setCanServeNonLocalReads_UNSAFE(1U);
    }

    if (newState.secondary() && result != kActionSteppedDown &&
        _topCoord->isElectableNodeInSingleNodeReplicaSet()) {
        // When transitioning from other follower states to SECONDARY, run for election on a
        // single-node replica set.
        result = kActionStartSingleNodeElection;
    }

    // If we are transitioning from secondary, cancel any scheduled takeovers.
    if (_memberState.secondary()) {
        _cancelCatchupTakeover_inlock();
        _cancelPriorityTakeover_inlock();
    }

    // Ensure replication is running if we are no longer REMOVED.
    if (_memberState.removed() && !newState.arbiter()) {
        LOGV2(5268000, "Scheduling a task to begin or continue replication");
        _scheduleWorkAt(_replExecutor->now(),
                        [=](const mongo::executor::TaskExecutor::CallbackArgs& cbData) {
                            _externalState->startThreads();
                            auto opCtx = cc().makeOperationContext();
                            _startDataReplication(opCtx.get());
                        });
    }

    LOGV2(21358,
          "transition to {newState} from {oldState}",
          "Replica set state transition",
          "newState"_attr = newState,
          "oldState"_attr = _memberState);

    // Initializes the featureCompatibilityVersion to the latest value, because arbiters do not
    // receive the replicated version. This is to avoid bugs like SERVER-32639.
    if (newState.arbiter()) {
        // (Generic FCV reference): This FCV check should exist across LTS binary versions.
        serverGlobalParams.mutableFeatureCompatibility.setVersion(
            multiversion::GenericFCV::kLatest);
        serverGlobalParams.featureCompatibility.logFCVWithContext("arbiter"_sd);
    }

    _memberState = newState;

    _cancelAndRescheduleElectionTimeout_inlock();

    // Notifies waiters blocked in waitForMemberState().
    // For testing only.
    _memberStateChange.notify_all();

    return result;
}

void ReplicationCoordinatorImpl::_performPostMemberStateUpdateAction(
    PostMemberStateUpdateAction action) {
    switch (action) {
        case kActionNone:
            break;
        case kActionFollowerModeStateChange:
            _onFollowerModeStateChange();
            break;
        case kActionRollbackOrRemoved:
            _externalState->closeConnections();
            [[fallthrough]];
        case kActionSteppedDown:
            _externalState->onStepDownHook();
            ReplicaSetAwareServiceRegistry::get(_service).onStepDown();
            break;
        case kActionStartSingleNodeElection:
            _startElectSelfIfEligibleV1(StartElectionReasonEnum::kElectionTimeout);
            break;
        default:
            LOGV2_FATAL(26010,
                        "Unknown post member state update action {action}",
                        "Unknown post member state update action",
                        "action"_attr = static_cast<int>(action));
    }
}

void ReplicationCoordinatorImpl::_postWonElectionUpdateMemberState(WithLock lk) {
    invariant(_topCoord->getTerm() != OpTime::kUninitializedTerm);
    _electionId = OID::fromTerm(_topCoord->getTerm());
    auto ts = VectorClockMutable::get(getServiceContext())->tickClusterTime(1).asTimestamp();
    _topCoord->processWinElection(_electionId, ts);
    const PostMemberStateUpdateAction nextAction = _updateMemberStateFromTopologyCoordinator(lk);

    invariant(nextAction == kActionFollowerModeStateChange,
              str::stream() << "nextAction == " << static_cast<int>(nextAction));
    invariant(_getMemberState_inlock().primary());
    // Clear the sync source.
    _onFollowerModeStateChange();

    // Notify all secondaries of the election win by cancelling all current heartbeats and sending
    // new heartbeat requests to all nodes. We must cancel and start instead of restarting scheduled
    // heartbeats because all heartbeats must be restarted upon election succeeding.
    _cancelHeartbeats_inlock();
    _startHeartbeats_inlock();

    invariant(!_catchupState);
    _catchupState = std::make_unique<CatchupState>(this);
    _catchupState->start_inlock();
}

void ReplicationCoordinatorImpl::_onFollowerModeStateChange() {
    _externalState->signalApplierToChooseNewSyncSource();
}

void ReplicationCoordinatorImpl::CatchupState::start_inlock() {
    LOGV2(21359, "Entering primary catch-up mode");

    // Reset the number of catchup operations performed before starting catchup.
    _numCatchUpOps = 0;

    // No catchup in single node replica set.
    if (_repl->_rsConfig.getNumMembers() == 1) {
        LOGV2(6015304, "Skipping primary catchup since we are the only node in the replica set.");
        abort_inlock(PrimaryCatchUpConclusionReason::kSkipped);
        return;
    }

    auto catchupTimeout = _repl->_rsConfig.getCatchUpTimeoutPeriod();

    // When catchUpTimeoutMillis is 0, we skip doing catchup entirely.
    if (catchupTimeout == ReplSetConfig::kCatchUpDisabled) {
        LOGV2(21360, "Skipping primary catchup since the catchup timeout is 0");
        abort_inlock(PrimaryCatchUpConclusionReason::kSkipped);
        return;
    }

    auto mutex = &_repl->_mutex;
    auto timeoutCB = [this, mutex](const CallbackArgs& cbData) {
        if (!cbData.status.isOK()) {
            return;
        }
        stdx::lock_guard<Latch> lk(*mutex);
        // Check whether the callback has been cancelled while holding mutex.
        if (cbData.myHandle.isCanceled()) {
            return;
        }
        LOGV2(21361, "Catchup timed out after becoming primary");
        abort_inlock(PrimaryCatchUpConclusionReason::kTimedOut);
    };

    // Deal with infinity and overflow - no timeout.
    if (catchupTimeout == ReplSetConfig::kInfiniteCatchUpTimeout ||
        Date_t::max() - _repl->_replExecutor->now() <= catchupTimeout) {
        return;
    }
    // Schedule timeout callback.
    auto timeoutDate = _repl->_replExecutor->now() + catchupTimeout;
    auto status = _repl->_replExecutor->scheduleWorkAt(timeoutDate, std::move(timeoutCB));
    if (!status.isOK()) {
        LOGV2(21362, "Failed to schedule catchup timeout work");
        abort_inlock(PrimaryCatchUpConclusionReason::kFailedWithError);
        return;
    }
    _timeoutCbh = status.getValue();
}

void ReplicationCoordinatorImpl::CatchupState::abort_inlock(PrimaryCatchUpConclusionReason reason) {
    invariant(_repl->_getMemberState_inlock().primary());

    ReplicationMetrics::get(getGlobalServiceContext())
        .incrementNumCatchUpsConcludedForReason(reason);

    LOGV2(21363, "Exited primary catch-up mode");
    // Clean up its own members.
    if (_timeoutCbh) {
        _repl->_replExecutor->cancel(_timeoutCbh);
    }
    if (reason != PrimaryCatchUpConclusionReason::kSucceeded && _waiter) {
        _repl->_opTimeWaiterList.remove_inlock(_waiter);
        _waiter.reset();
    }

    // Enter primary drain mode.
    _repl->_enterDrainMode_inlock();
    // Destroy the state itself.
    _repl->_catchupState.reset();
}

void ReplicationCoordinatorImpl::CatchupState::signalHeartbeatUpdate_inlock() {
    auto targetOpTime = _repl->_topCoord->latestKnownOpTimeSinceHeartbeatRestart();
    // Haven't collected all heartbeat responses.
    if (!targetOpTime) {
        LOGV2_DEBUG(
            6015305,
            1,
            "Not updating target optime for catchup, we haven't collected all heartbeat responses");
        return;
    }

    // We've caught up.
    const auto myLastApplied = _repl->_getMyLastAppliedOpTime_inlock();
    if (*targetOpTime <= myLastApplied) {
        LOGV2(21364,
              "Caught up to the latest optime known via heartbeats after becoming primary. Target "
              "optime: {targetOpTime}. My Last Applied: {myLastApplied}",
              "Caught up to the latest optime known via heartbeats after becoming primary",
              "targetOpTime"_attr = *targetOpTime,
              "myLastApplied"_attr = myLastApplied);
        // Report the number of ops applied during catchup in replSetGetStatus once the primary is
        // caught up.
        ReplicationMetrics::get(getGlobalServiceContext()).setNumCatchUpOps(_numCatchUpOps);
        abort_inlock(PrimaryCatchUpConclusionReason::kAlreadyCaughtUp);
        return;
    }

    // Reset the target optime if it has changed.
    if (_waiter && _targetOpTime == *targetOpTime) {
        return;
    }
    _targetOpTime = *targetOpTime;

    ReplicationMetrics::get(getGlobalServiceContext()).setTargetCatchupOpTime(_targetOpTime);

    LOGV2(21365,
          "Heartbeats updated catchup target optime to {targetOpTime}",
          "Heartbeats updated catchup target optime",
          "targetOpTime"_attr = _targetOpTime);
    LOGV2(21366, "Latest known optime per replica set member");
    auto opTimesPerMember = _repl->_topCoord->latestKnownOpTimeSinceHeartbeatRestartPerMember();
    for (auto&& pair : opTimesPerMember) {
        LOGV2(21367,
              "Member ID: {memberId}, latest known optime: {latestKnownOpTime}",
              "Latest known optime",
              "memberId"_attr = pair.first,
              "latestKnownOpTime"_attr = (pair.second ? (*pair.second).toString() : "unknown"));
    }

    if (_waiter) {
        _repl->_opTimeWaiterList.remove_inlock(_waiter);
        _waiter.reset();
    } else {
        // Only increment the 'numCatchUps' election metric the first time we add a waiter, so that
        // we only increment it once each time a primary has to catch up. If there is already an
        // existing waiter, then the node is catching up and has already been counted.
        ReplicationMetrics::get(getGlobalServiceContext()).incrementNumCatchUps();
    }

    auto targetOpTimeCB = [this](Status status) {
        // Double check the target time since stepdown may signal us too.
        const auto myLastApplied = _repl->_getMyLastAppliedOpTime_inlock();
        if (_targetOpTime <= myLastApplied) {
            LOGV2(21368,
                  "Caught up to the latest known optime successfully after becoming primary. "
                  "Target optime: {targetOpTime}. My Last Applied: {myLastApplied}",
                  "Caught up to the latest known optime successfully after becoming primary",
                  "targetOpTime"_attr = _targetOpTime,
                  "myLastApplied"_attr = myLastApplied);
            // Report the number of ops applied during catchup in replSetGetStatus once the primary
            // is caught up.
            ReplicationMetrics::get(getGlobalServiceContext()).setNumCatchUpOps(_numCatchUpOps);
            abort_inlock(PrimaryCatchUpConclusionReason::kSucceeded);
        }
    };
    auto pf = makePromiseFuture<void>();
    _waiter = std::make_shared<Waiter>(std::move(pf.promise));
    auto future = std::move(pf.future).onCompletion(targetOpTimeCB);
    _repl->_opTimeWaiterList.add_inlock(_targetOpTime, _waiter);
}

void ReplicationCoordinatorImpl::CatchupState::incrementNumCatchUpOps_inlock(long numOps) {
    _numCatchUpOps += numOps;
}

Status ReplicationCoordinatorImpl::abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_catchupState) {
        _catchupState->abort_inlock(reason);
        return Status::OK();
    }
    return Status(ErrorCodes::IllegalOperation, "The node is not in catch-up mode.");
}

void ReplicationCoordinatorImpl::incrementNumCatchUpOpsIfCatchingUp(long numOps) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_catchupState) {
        _catchupState->incrementNumCatchUpOps_inlock(numOps);
    }
}

void ReplicationCoordinatorImpl::signalDropPendingCollectionsRemovedFromStorage() {
    stdx::lock_guard<Latch> lock(_mutex);
    _wakeReadyWaiters(lock, _externalState->getEarliestDropPendingOpTime());
}

boost::optional<Timestamp> ReplicationCoordinatorImpl::getRecoveryTimestamp() {
    return _storage->getRecoveryTimestamp(getServiceContext());
}

void ReplicationCoordinatorImpl::_enterDrainMode_inlock() {
    _applierState = ApplierState::Draining;
    _externalState->stopProducer();
}

Future<void> ReplicationCoordinatorImpl::_drainForShardSplit() {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(!_finishedDrainingPromise.has_value());
    auto [promise, future] = makePromiseFuture<void>();
    _finishedDrainingPromise = std::move(promise);
    _applierState = ApplierState::DrainingForShardSplit;
    _externalState->stopProducer();
    return std::move(future);
}

ReplicationCoordinatorImpl::PostMemberStateUpdateAction
ReplicationCoordinatorImpl::_setCurrentRSConfig(WithLock lk,
                                                OperationContext* opCtx,
                                                const ReplSetConfig& newConfig,
                                                int myIndex) {
    invariant(newConfig.getProtocolVersion() == 1);
    invariant(_settings.usingReplSets());
    _cancelHeartbeats_inlock();
    _setConfigState_inlock(kConfigSteady);

    _topCoord->updateConfig(newConfig, myIndex, _replExecutor->now());

    // It is only necessary to check if an arbiter is running on a quarterly binary version when a
    // fresh node is added to the replica set as an arbiter and when an old secondary node is
    // removed and then re-added to the replica set as an arbiter. That's why we only need to warn
    // once per process as converting from secondary to arbiter normally requires a server shutdown.
    static std::once_flag checkArbiterOnQuarterlyBinaryVersion;
    std::call_once(checkArbiterOnQuarterlyBinaryVersion, [this] {
        // Warn if an arbiter is running on a quarterly binary version.
        if (_topCoord->getMemberState().arbiter() && !ServerGlobalParams::kIsLTSBinaryVersion) {
            LOGV2_WARNING_OPTIONS(
                4906901,
                {logv2::LogTag::kStartupWarnings},
                "** WARNING: Arbiters are not supported in quarterly binary versions");
        }
    });

    // updateConfig() can change terms, so update our term shadow to match.
    _termShadow.store(_topCoord->getTerm());

    const ReplSetConfig oldConfig = _rsConfig;
    _rsConfig = newConfig;
    _protVersion.store(_rsConfig.getProtocolVersion());

    if (!oldConfig.isInitialized()) {
        // We allow the IDWC to be set only once after initial configuration is loaded.
        _setImplicitDefaultWriteConcern(opCtx, lk);
        _validateDefaultWriteConcernOnShardStartup(lk);
    } else {
        // If 'enableDefaultWriteConcernUpdatesForInitiate' is enabled, we allow the IDWC to be
        // recalculated after a reconfig. However, this logic is only relevant for testing,
        // and should not be executed outside of our test infrastructure. This is needed due to an
        // optimization in our ReplSetTest jstest fixture that initiates replica sets with only the
        // primary, and then reconfigs the full membership set in. As a result, we must calculate
        // the final IDWC only after the last node has been added to the set.
        if (repl::enableDefaultWriteConcernUpdatesForInitiate.load()) {
            _setImplicitDefaultWriteConcern(opCtx, lk);
        }
    }

    // Warn if using the in-memory (ephemeral) storage engine with
    // writeConcernMajorityJournalDefault=true.
    StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    if (storageEngine && newConfig.getWriteConcernMajorityShouldJournal() &&
        (!oldConfig.isInitialized() || !oldConfig.getWriteConcernMajorityShouldJournal())) {
        if (storageEngine->isEphemeral()) {
            LOGV2_OPTIONS(21378, {logv2::LogTag::kStartupWarnings}, "");
            LOGV2_OPTIONS(21379,
                          {logv2::LogTag::kStartupWarnings},
                          "** WARNING: This replica set node is using in-memory (ephemeral) "
                          "storage with the");
            LOGV2_OPTIONS(
                21380,
                {logv2::LogTag::kStartupWarnings},
                "**          writeConcernMajorityJournalDefault option to the replica set config ");
            LOGV2_OPTIONS(
                21381,
                {logv2::LogTag::kStartupWarnings},
                "**          set to true. The writeConcernMajorityJournalDefault option to the ");
            LOGV2_OPTIONS(21382,
                          {logv2::LogTag::kStartupWarnings},
                          "**          replica set config must be set to false ");
            LOGV2_OPTIONS(21383,
                          {logv2::LogTag::kStartupWarnings},
                          "**          or w:majority write concerns will never complete.");
            LOGV2_OPTIONS(
                21384,
                {logv2::LogTag::kStartupWarnings},
                "**          In addition, this node's memory consumption may increase until all");
            LOGV2_OPTIONS(21385,
                          {logv2::LogTag::kStartupWarnings},
                          "**          available free RAM is exhausted.");
            LOGV2_OPTIONS(21386, {logv2::LogTag::kStartupWarnings}, "");
        }
    }

    // Check that getLastErrorDefaults has not been changed from the default settings of
    // { w: 1, wtimeout: 0 }.
    if (newConfig.containsCustomizedGetLastErrorDefaults()) {
        LOGV2_OPTIONS(21387, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(21388,
                      {logv2::LogTag::kStartupWarnings},
                      "** WARNING: Replica set config contains customized getLastErrorDefaults,");
        LOGV2_OPTIONS(21389,
                      {logv2::LogTag::kStartupWarnings},
                      "**          which have been deprecated and are now ignored. Use "
                      "setDefaultRWConcern instead to set a");
        LOGV2_OPTIONS(21390,
                      {logv2::LogTag::kStartupWarnings},
                      "**          cluster-wide default writeConcern.");
        LOGV2_OPTIONS(21391, {logv2::LogTag::kStartupWarnings}, "");
    }

    // Emit a warning at startup if there are IP addresses in the SplitHorizon field of the
    // replset config.
    std::vector<std::string> offendingConfigs;
    for (const MemberConfig& member : oldConfig.members()) {
        // Check that no horizon mappings contain IP addresses
        for (auto& mapping : member.getHorizonMappings()) {
            // Emit a startup warning for any SplitHorizon mapping that can be parsed as
            // a valid CIDR range, except for the default horizon.
            if (mapping.first != SplitHorizon::kDefaultHorizon &&
                CIDR::parse(mapping.second.host()).isOK()) {
                offendingConfigs.emplace_back(str::stream() << mapping.second.host() << ":"
                                                            << mapping.second.port());
            }
        }
    }
    if (!offendingConfigs.empty()) {
        LOGV2_WARNING_OPTIONS(4907900,
                              {logv2::LogTag::kStartupWarnings},
                              "Found split horizon configuration using IP "
                              "address(es), which is disallowed.",
                              "offendingConfigs"_attr = offendingConfigs);
    }

    // If the SplitHorizon has changed, reply to all waiting hellos with an error.
    _errorOnPromisesIfHorizonChanged(lk, opCtx, oldConfig, newConfig, _selfIndex, myIndex);

    LOGV2(21392,
          "New replica set config in use: {config}",
          "New replica set config in use",
          "config"_attr = _rsConfig.toBSON());
    _selfIndex = myIndex;
    if (_selfIndex >= 0) {
        LOGV2(21393,
              "This node is {hostAndPort} in the config",
              "Found self in config",
              "hostAndPort"_attr = _rsConfig.getMemberAt(_selfIndex).getHostAndPort());
    } else {
        LOGV2(21394, "This node is not a member of the config");
    }

    // Wake up writeConcern waiters that are no longer satisfiable due to the rsConfig change.
    _replicationWaiterList.setValueIf_inlock([this](const OpTime& opTime,
                                                    const SharedWaiterHandle& waiter) {
        invariant(waiter->writeConcern);
        // This throws if a waiter's writeConcern is no longer satisfiable, in which case
        // setValueIf_inlock will fulfill the waiter's promise with the error status.
        uassertStatusOK(_checkIfWriteConcernCanBeSatisfied_inlock(waiter->writeConcern.value()));
        // Return false meaning that the waiter is still satisfiable and thus can remain in the
        // waiter list.
        return false;
    });

    _cancelCatchupTakeover_inlock();
    _cancelPriorityTakeover_inlock();
    _cancelAndRescheduleElectionTimeout_inlock();

    PostMemberStateUpdateAction action = _updateMemberStateFromTopologyCoordinator(lk);
    if (_topCoord->isElectableNodeInSingleNodeReplicaSet()) {
        // If the new config describes an electable one-node replica set, we need to start an
        // election.
        action = PostMemberStateUpdateAction::kActionStartSingleNodeElection;
    }

    if (_selfIndex >= 0) {
        // Don't send heartbeats if we're not in the config, if we get re-added one of the
        // nodes in the set will contact us.
        _startHeartbeats_inlock();

        if (_horizonToTopologyChangePromiseMap.empty()) {
            // We should only create a new horizon-to-promise mapping for nodes that are members of
            // the config.
            _createHorizonTopologyChangePromiseMapping(lk);
        }
    } else {
        // Clear the horizon promise mappings of removed nodes so they can be recreated if the
        // node later rejoins the set.
        _horizonToTopologyChangePromiseMap.clear();

        // If we're still REMOVED, clear the seedList.
        _seedList.clear();
    }

    _updateLastCommittedOpTimeAndWallTime(lk);
    _wakeReadyWaiters(lk);

    return action;
}

void ReplicationCoordinatorImpl::_wakeReadyWaiters(WithLock lk, boost::optional<OpTime> opTime) {
    _replicationWaiterList.setValueIf_inlock(
        [this](const OpTime& opTime, const SharedWaiterHandle& waiter) {
            invariant(waiter->writeConcern);
            return _doneWaitingForReplication_inlock(opTime, waiter->writeConcern.value());
        },
        opTime);
}

Status ReplicationCoordinatorImpl::processReplSetUpdatePosition(const UpdatePositionArgs& updates) {
    stdx::unique_lock<Latch> lock(_mutex);
    Status status = Status::OK();
    bool gotValidUpdate = false;
    OpTime maxRemoteOpTime;
    for (UpdatePositionArgs::UpdateIterator update = updates.updatesBegin();
         update != updates.updatesEnd();
         ++update) {
        auto statusWithOpTime = _setLastOptimeForMember(lock, *update);
        if (!statusWithOpTime.isOK()) {
            status = statusWithOpTime.getStatus();
            break;
        }
        maxRemoteOpTime = std::max(maxRemoteOpTime, statusWithOpTime.getValue());
        gotValidUpdate = true;
    }
    _updateStateAfterRemoteOpTimeUpdates(lock, maxRemoteOpTime);

    if (gotValidUpdate) {
        // If we become primary after the unlock below, the forwardSecondaryProgress will do nothing
        // (slightly expensively).  If we become secondary after the unlock below, BackgroundSync
        // will take care of forwarding our progress by calling signalUpstreamUpdater() once we
        // select a new sync source.  So it's OK to depend on the stale value of wasPrimary here.
        bool wasPrimary = _getMemberState_inlock().primary();
        lock.unlock();
        // maxRemoteOpTime is null here if we got valid updates but no downstream node had
        // actually advanced any optime.
        if (!maxRemoteOpTime.isNull())
            _externalState->notifyOtherMemberDataChanged();
        if (!wasPrimary) {
            // Must do this outside _mutex
            _externalState->forwardSecondaryProgress();
        }
    }
    return status;
}

bool ReplicationCoordinatorImpl::buildsIndexes() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_selfIndex == -1) {
        return true;
    }
    const MemberConfig& self = _rsConfig.getMemberAt(_selfIndex);
    return self.shouldBuildIndexes();
}

std::vector<HostAndPort> ReplicationCoordinatorImpl::getHostsWrittenTo(const OpTime& op,
                                                                       bool durablyWritten) {
    stdx::lock_guard<Latch> lk(_mutex);
    return _topCoord->getHostsWrittenTo(op, durablyWritten);
}

Status ReplicationCoordinatorImpl::checkIfWriteConcernCanBeSatisfied(
    const WriteConcernOptions& writeConcern) const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _checkIfWriteConcernCanBeSatisfied_inlock(writeConcern);
}

Status ReplicationCoordinatorImpl::_checkIfWriteConcernCanBeSatisfied_inlock(
    const WriteConcernOptions& writeConcern) const {
    if (getReplicationMode() == modeNone) {
        return Status(ErrorCodes::NoReplicationEnabled,
                      "No replication enabled when checking if write concern can be satisfied");
    }

    invariant(getReplicationMode() == modeReplSet);
    return _rsConfig.checkIfWriteConcernCanBeSatisfied(writeConcern);
}

Status ReplicationCoordinatorImpl::checkIfCommitQuorumCanBeSatisfied(
    const CommitQuorumOptions& commitQuorum) const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _checkIfCommitQuorumCanBeSatisfied(lock, commitQuorum);
}

Status ReplicationCoordinatorImpl::_checkIfCommitQuorumCanBeSatisfied(
    WithLock, const CommitQuorumOptions& commitQuorum) const {
    if (getReplicationMode() == modeNone) {
        return Status(ErrorCodes::NoReplicationEnabled,
                      "No replication enabled when checking if commit quorum can be satisfied");
    }

    invariant(getReplicationMode() == modeReplSet);

    // We need to ensure that the 'commitQuorum' can be satisfied by all the members of this
    // replica set.
    return _topCoord->checkIfCommitQuorumCanBeSatisfied(commitQuorum);
}

WriteConcernOptions ReplicationCoordinatorImpl::getGetLastErrorDefault() {
    stdx::lock_guard<Latch> lock(_mutex);
    if (_rsConfig.isInitialized()) {
        return _rsConfig.getDefaultWriteConcern();
    }
    return WriteConcernOptions();
}

Status ReplicationCoordinatorImpl::checkReplEnabledForCommand(BSONObjBuilder* result) {
    if (!_settings.usingReplSets()) {
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            result->append("info", "configsvr");  // for shell prompt
        }
        return Status(ErrorCodes::NoReplicationEnabled, "not running with --replSet");
    }

    if (getMemberState().startup()) {
        result->append("info", "run rs.initiate(...) if not yet done for the set");
        return Status(ErrorCodes::NotYetInitialized, "no replset config has been received");
    }

    return Status::OK();
}

bool ReplicationCoordinatorImpl::isReplEnabled() const {
    return getReplicationMode() != modeNone;
}

ReadPreference ReplicationCoordinatorImpl::_getSyncSourceReadPreference(WithLock) const {
    // Always allow chaining while in catchup and drain mode.
    auto memberState = _getMemberState_inlock();
    ReadPreference readPreference = ReadPreference::Nearest;

    bool parsedSyncSourceFromInitialSync = false;
    // Handle special case of initial sync source read preference.
    // This sync source will be cleared when we go to secondary mode, because we will perform
    // a postMemberState action of kOnFollowerModeStateChange which calls chooseNewSyncSource().
    if (memberState.startup2() && _selfIndex != -1) {
        if (!initialSyncSourceReadPreference.empty()) {
            try {
                readPreference =
                    ReadPreference_parse(IDLParserContext("initialSyncSourceReadPreference"),
                                         initialSyncSourceReadPreference);
                parsedSyncSourceFromInitialSync = true;
            } catch (const DBException& e) {
                fassertFailedWithStatus(3873100, e.toStatus());
            }
        } else if (_rsConfig.getMemberAt(_selfIndex).getNumVotes() > 0) {
            // Voting nodes prefer to sync from the primary.  A voting node that is initial syncing
            // may have acknowledged writes which are part of the set's write majority; if it then
            // resyncs from a node which does not have those writes, and (before it replicates them
            // again) helps elect a new primary which also does not have those writes, the writes
            // may be lost.  By resyncing from the primary (if possible), which always has the
            // majority-commited writes, the probability of this scenario is reduced.
            readPreference = ReadPreference::PrimaryPreferred;
        }
    }
    if (!parsedSyncSourceFromInitialSync && !memberState.primary() &&
        !_rsConfig.isChainingAllowed() && !enableOverrideClusterChainingSetting.load()) {
        // If we are not the primary and chaining is disabled in the config (without overrides), we
        // should only be syncing from the primary.
        readPreference = ReadPreference::PrimaryOnly;
    }
    return readPreference;
}

HostAndPort ReplicationCoordinatorImpl::chooseNewSyncSource(const OpTime& lastOpTimeFetched) {
    stdx::lock_guard<Latch> lk(_mutex);

    HostAndPort oldSyncSource = _topCoord->getSyncSourceAddress();

    const auto readPreference = _getSyncSourceReadPreference(lk);

    HostAndPort newSyncSource =
        _topCoord->chooseNewSyncSource(_replExecutor->now(), lastOpTimeFetched, readPreference);
    auto primary = _topCoord->getCurrentPrimaryMember();
    // If read preference is SecondaryOnly, we should never choose the primary.
    invariant(readPreference != ReadPreference::SecondaryOnly || !primary ||
              primary->getHostAndPort() != newSyncSource);

    // If we lost our sync source, schedule new heartbeats immediately to update our knowledge
    // of other members's state, allowing us to make informed sync source decisions.
    if (newSyncSource.empty() && !oldSyncSource.empty() && _selfIndex >= 0 &&
        !_getMemberState_inlock().primary()) {
        _restartScheduledHeartbeats_inlock(_rsConfig.getReplSetName().toString());
    }

    return newSyncSource;
}

void ReplicationCoordinatorImpl::_undenylistSyncSource(
    const executor::TaskExecutor::CallbackArgs& cbData, const HostAndPort& host) {
    if (cbData.status == ErrorCodes::CallbackCanceled)
        return;

    stdx::lock_guard<Latch> lock(_mutex);
    _topCoord->undenylistSyncSource(host, _replExecutor->now());
}

void ReplicationCoordinatorImpl::denylistSyncSource(const HostAndPort& host, Date_t until) {
    stdx::lock_guard<Latch> lock(_mutex);
    _topCoord->denylistSyncSource(host, until);
    _scheduleWorkAt(until, [=](const executor::TaskExecutor::CallbackArgs& cbData) {
        _undenylistSyncSource(cbData, host);
    });
}

void ReplicationCoordinatorImpl::resetLastOpTimesFromOplog(OperationContext* opCtx) {
    auto lastOpTimeAndWallTimeStatus = _externalState->loadLastOpTimeAndWallTime(opCtx);
    OpTimeAndWallTime lastOpTimeAndWallTime = {OpTime(), Date_t()};
    if (!lastOpTimeAndWallTimeStatus.getStatus().isOK()) {
        LOGV2_WARNING(21412,
                      "Failed to load timestamp and/or wall clock time of most recently applied "
                      "operation; {error}",
                      "Failed to load timestamp and/or wall clock time of most recently applied "
                      "operation",
                      "error"_attr = lastOpTimeAndWallTimeStatus.getStatus());
    } else {
        lastOpTimeAndWallTime = lastOpTimeAndWallTimeStatus.getValue();
    }

    // Update the global timestamp before setting last applied opTime forward so the last applied
    // optime is never greater than the latest in-memory cluster time.
    _externalState->setGlobalTimestamp(opCtx->getServiceContext(),
                                       lastOpTimeAndWallTime.opTime.getTimestamp());

    stdx::unique_lock<Latch> lock(_mutex);
    bool isRollbackAllowed = true;
    _setMyLastAppliedOpTimeAndWallTime(lock, lastOpTimeAndWallTime, isRollbackAllowed);
    _setMyLastDurableOpTimeAndWallTime(lock, lastOpTimeAndWallTime, isRollbackAllowed);
    _reportUpstream_inlock(std::move(lock));
}

ChangeSyncSourceAction ReplicationCoordinatorImpl::shouldChangeSyncSource(
    const HostAndPort& currentSource,
    const rpc::ReplSetMetadata& replMetadata,
    const rpc::OplogQueryMetadata& oqMetadata,
    const OpTime& previousOpTimeFetched,
    const OpTime& lastOpTimeFetched) const {
    if (shouldDropSyncSourceAfterShardSplit(replMetadata.getReplicaSetId())) {
        // Drop the last batch of message following a change of replica set due to a shard split.
        LOGV2(6394902,
              "Choosing new sync source because we left the replica set due to a shard split.",
              "currentReplicaSetId"_attr = _rsConfig.getReplicaSetId(),
              "otherReplicaSetId"_attr = replMetadata.getReplicaSetId());
        return ChangeSyncSourceAction::kStopSyncingAndDropLastBatchIfPresent;
    }

    stdx::lock_guard<Latch> lock(_mutex);
    const auto now = _replExecutor->now();

    if (_topCoord->shouldChangeSyncSource(
            currentSource, replMetadata, oqMetadata, lastOpTimeFetched, now)) {
        return ChangeSyncSourceAction::kStopSyncingAndEnqueueLastBatch;
    }

    const auto readPreference = _getSyncSourceReadPreference(lock);
    if (_topCoord->shouldChangeSyncSourceDueToPingTime(
            currentSource, _getMemberState_inlock(), previousOpTimeFetched, now, readPreference)) {
        // We should drop the last batch if we find a significantly closer node. This is to
        // avoid advancing our 'lastFetched', which makes it more likely that we will be able to
        // choose the closer node as our sync source.
        return ChangeSyncSourceAction::kStopSyncingAndDropLastBatchIfPresent;
    }

    return ChangeSyncSourceAction::kContinueSyncing;
}

ChangeSyncSourceAction ReplicationCoordinatorImpl::shouldChangeSyncSourceOnError(
    const HostAndPort& currentSource, const OpTime& lastOpTimeFetched) const {
    stdx::lock_guard<Latch> lock(_mutex);
    const auto now = _replExecutor->now();

    if (_topCoord->shouldChangeSyncSourceOnError(currentSource, lastOpTimeFetched, now)) {
        return ChangeSyncSourceAction::kStopSyncingAndDropLastBatchIfPresent;
    }

    const auto readPreference = _getSyncSourceReadPreference(lock);
    if (_topCoord->shouldChangeSyncSourceDueToPingTime(
            currentSource, _getMemberState_inlock(), lastOpTimeFetched, now, readPreference)) {
        return ChangeSyncSourceAction::kStopSyncingAndDropLastBatchIfPresent;
    }

    return ChangeSyncSourceAction::kContinueSyncing;
}

void ReplicationCoordinatorImpl::_updateLastCommittedOpTimeAndWallTime(WithLock lk) {
    if (_topCoord->updateLastCommittedOpTimeAndWallTime()) {
        _setStableTimestampForStorage(lk);
    }
}

void ReplicationCoordinatorImpl::attemptToAdvanceStableTimestamp() {
    stdx::unique_lock<Latch> lk(_mutex);
    _setStableTimestampForStorage(lk);
}

OpTime ReplicationCoordinatorImpl::_recalculateStableOpTime(WithLock lk) {
    auto commitPoint = _topCoord->getLastCommittedOpTime();
    auto lastApplied = _topCoord->getMyLastAppliedOpTime();
    if (_currentCommittedSnapshot) {
        invariant(_currentCommittedSnapshot->getTimestamp() <= commitPoint.getTimestamp(),
                  str::stream() << "currentCommittedSnapshot: "
                                << _currentCommittedSnapshot->toString()
                                << " commitPoint: " << commitPoint.toString());
        invariant(*_currentCommittedSnapshot <= commitPoint,
                  str::stream() << "currentCommittedSnapshot: "
                                << _currentCommittedSnapshot->toString()
                                << " commitPoint: " << commitPoint.toString());
    }

    //
    // The stable timestamp must be a "consistent" timestamp with respect to the oplog. Intuitively,
    // it must be a timestamp at which the oplog history is "set in stone" i.e. no writes will
    // commit at earlier timestamps. More precisely, it must be a timestamp T such that future
    // writers only commit at times greater than T and readers only read at, or earlier than, T.  We
    // refer to this timestamp as the "no-overlap" point, since it is the timestamp that delineates
    // these non overlapping readers and writers. The calculation of this value differs on primary
    // and secondary nodes due to their distinct behaviors, as described below.
    //

    // On a primary node, oplog writes may commit out of timestamp order, which can lead to the
    // creation of oplog "holes". On a primary the all_durable timestamp tracks the newest timestamp
    // T such that no future transactions will commit behind T. Since all_durable is a timestamp,
    // however, without a term, we need to construct an optime with a proper term. If we are
    // primary, then the all_durable should always correspond to a timestamp at or newer than the
    // first write completed by this node as primary, since we write down a new oplog entry before
    // allowing writes as a new primary. Thus, it can be assigned the current term of this primary.
    OpTime allDurableOpTime = OpTime::max();
    if (_readWriteAbility->canAcceptNonLocalWrites(lk)) {
        allDurableOpTime =
            OpTime(_storage->getAllDurableTimestamp(getServiceContext()), _topCoord->getTerm());
    }

    // On a secondary, oplog entries are written in parallel, and so may be written out of timestamp
    // order. Because of this, the stable timestamp must not fall in the middle of a batch while it
    // is being applied. To prevent this we ensure the no-overlap point does not surpass the
    // lastApplied, which is only advanced at the end of secondary batch application.
    OpTime noOverlap = std::min(lastApplied, allDurableOpTime);

    // The stable optime must always be less than or equal to the no overlap point. When majority
    // reads are enabled, the stable optime must also not surpass the majority commit point. When
    // majority reads are disabled, the stable optime is not required to be majority committed.
    OpTime stableOpTime;
    auto maximumStableOpTime =
        serverGlobalParams.enableMajorityReadConcern ? commitPoint : lastApplied;

    // Make sure the stable optime does not surpass its maximum.
    stableOpTime = std::min(noOverlap, maximumStableOpTime);

    // Check that the selected stable optime does not exceed our maximum and that it does not
    // surpass the no-overlap point.
    invariant(stableOpTime.getTimestamp() <= maximumStableOpTime.getTimestamp(),
              str::stream() << "stableOpTime: " << stableOpTime.toString()
                            << " maximumStableOpTime: " << maximumStableOpTime.toString());
    invariant(stableOpTime <= maximumStableOpTime,
              str::stream() << "stableOpTime: " << stableOpTime.toString()
                            << " maximumStableOpTime: " << maximumStableOpTime.toString());
    invariant(stableOpTime.getTimestamp() <= noOverlap.getTimestamp(),
              str::stream() << "stableOpTime: " << stableOpTime.toString() << " noOverlap: "
                            << noOverlap.toString() << " lastApplied: " << lastApplied.toString()
                            << " allDurableOpTime: " << allDurableOpTime.toString());
    invariant(stableOpTime <= noOverlap,
              str::stream() << "stableOpTime: " << stableOpTime.toString() << " noOverlap: "
                            << noOverlap.toString() << " lastApplied: " << lastApplied.toString()
                            << " allDurableOpTime: " << allDurableOpTime.toString());

    return stableOpTime;
}

MONGO_FAIL_POINT_DEFINE(disableSnapshotting);

void ReplicationCoordinatorImpl::_setStableTimestampForStorage(WithLock lk) {
    if (!_shouldSetStableTimestamp) {
        LOGV2_DEBUG(21395, 2, "Not setting stable timestamp for storage");
        return;
    }

    // Don't update the stable optime if we are in initial sync. We advance the oldest timestamp
    // continually to the lastApplied optime during initial sync oplog application, so if we learned
    // about an earlier commit point during this period, we would risk setting the stable timestamp
    // behind the oldest timestamp, which is prohibited in the storage engine. Note that we don't
    // take stable checkpoints during initial sync, so the stable timestamp during this period
    // doesn't play a functionally important role anyway.
    auto memberState = _getMemberState_inlock();
    if (memberState.startup2()) {
        LOGV2_DEBUG(
            2139501, 2, "Not updating stable timestamp", "state"_attr = memberState.toString());
        return;
    }

    // Get the current stable optime.
    OpTime stableOpTime = _recalculateStableOpTime(lk);

    // Don't update the stable timestamp if it is earlier than the initial data timestamp.
    // Timestamps before the initialDataTimestamp are not consistent and so are not safe to use for
    // the stable timestamp or the committed snapshot, which is the timestamp used by majority
    // readers. This also prevents us from setting the stable timestamp behind the oldest timestamp
    // after leaving initial sync, since the initialDataTimestamp and oldest timestamp will be equal
    // after initial sync oplog application has completed.
    auto initialDataTimestamp = _service->getStorageEngine()->getInitialDataTimestamp();
    if (stableOpTime.getTimestamp() < initialDataTimestamp) {
        LOGV2_DEBUG(2139504,
                    2,
                    "Not updating stable timestamp since it is less than the initialDataTimestamp",
                    "stableTimestamp"_attr = stableOpTime.getTimestamp(),
                    "initialDataTimestamp"_attr = initialDataTimestamp);
        return;
    }

    if (stableOpTime.getTimestamp().isNull()) {
        LOGV2_DEBUG(2139502, 2, "Not updating stable timestamp to a null timestamp");
        return;
    }

    if (gTestingSnapshotBehaviorInIsolation) {
        return;
    }

    // Set the stable timestamp and update the committed snapshot.
    LOGV2_DEBUG(21396,
                2,
                "Setting replication's stable optime to {stableOpTime}",
                "Setting replication's stable optime",
                "stableOpTime"_attr = stableOpTime);

    // As arbiters aren't data bearing nodes, the all durable timestamp does not get advanced. To
    // advance the all durable timestamp when setting the stable timestamp we use 'force=true'.
    const bool force = _getMemberState_inlock().arbiter();

    // Update committed snapshot and wake up any threads waiting on read concern or
    // write concern.
    if (serverGlobalParams.enableMajorityReadConcern) {
        // When majority read concern is enabled, the committed snapshot is set to the new
        // stable optime. The wall time of the committed snapshot is not used for anything so we can
        // create a fake one.
        if (_updateCommittedSnapshot(lk, stableOpTime)) {
            // Update the stable timestamp for the storage engine.
            _storage->setStableTimestamp(getServiceContext(), stableOpTime.getTimestamp(), force);
        }
    } else {
        const auto lastCommittedOpTime = _topCoord->getLastCommittedOpTime();
        if (!lastCommittedOpTime.isNull()) {
            // When majority read concern is disabled, we set the stable timestamp to be less than
            // or equal to the all-durable timestamp. This makes sure that the committed snapshot is
            // not past the all-durable timestamp to guarantee we can always read our own majority
            // committed writes. This problem is specific to the case where we have a single node
            // replica set and the lastCommittedOpTime is set to be the lastApplied which can be
            // ahead of the all-durable.
            OpTime newCommittedSnapshot = std::min(lastCommittedOpTime, stableOpTime);
            // The wall clock time of the committed snapshot is not used for anything so we can
            // create a fake one.
            _updateCommittedSnapshot(lk, newCommittedSnapshot);
        }
        // Set the stable timestamp regardless of whether the majority commit point moved
        // forward. If we are in rollback state, however, do not alter the stable timestamp,
        // since it may be moved backwards explicitly by the rollback-via-refetch process.
        if (!MONGO_unlikely(disableSnapshotting.shouldFail()) && !_memberState.rollback()) {
            _storage->setStableTimestamp(getServiceContext(), stableOpTime.getTimestamp(), force);
        }
    }
}

void ReplicationCoordinatorImpl::finishRecoveryIfEligible(OperationContext* opCtx) {
    // Check to see if we can immediately return without taking any locks.
    if (isInPrimaryOrSecondaryState_UNSAFE()) {
        return;
    }

    // This needs to happen after the attempt so readers can be sure we've already tried.
    ON_BLOCK_EXIT([] { attemptsToBecomeSecondary.increment(); });

    // Need the RSTL in mode X to transition to SECONDARY
    ReplicationStateTransitionLockGuard transitionGuard(opCtx, MODE_X);

    // We can only transition to SECONDARY from RECOVERING state.
    MemberState state(getMemberState());
    if (!state.recovering()) {
        LOGV2_DEBUG(21397,
                    2,
                    "We cannot transition to SECONDARY state since we are not currently in "
                    "RECOVERING state. Current state: {currentState}",
                    "We cannot transition to SECONDARY state since we are not currently in "
                    "RECOVERING state",
                    "currentState"_attr = state.toString());
        return;
    }

    // Maintenance mode will force us to remain in RECOVERING state, no matter what.
    if (getMaintenanceMode()) {
        LOGV2_DEBUG(21398, 1, "We cannot transition to SECONDARY state while in maintenance mode");
        return;
    }

    // We can't go to SECONDARY state until we reach 'minValid', since the data may be in an
    // inconsistent state before this point. If our state is inconsistent, we need to disallow reads
    // from clients, which is why we stay in RECOVERING state.
    auto lastApplied = getMyLastAppliedOpTime();
    auto minValid = _replicationProcess->getConsistencyMarkers()->getMinValid(opCtx);
    if (lastApplied < minValid) {
        LOGV2_DEBUG(21399,
                    2,
                    "We cannot transition to SECONDARY state because our 'lastApplied' optime"
                    " is less than the 'minValid' optime. minValid optime: {minValid}, lastApplied "
                    "optime: {lastApplied}",
                    "We cannot transition to SECONDARY state because our 'lastApplied' optime"
                    " is less than the 'minValid' optime",
                    "minValid"_attr = minValid,
                    "lastApplied"_attr = lastApplied);
        return;
    }

    // Rolling back with eMRC false, we set initialDataTimestamp to max(local oplog top, source's
    // oplog top), then rollback via refetch. Data is inconsistent until lastApplied >=
    // initialDataTimestamp.
    auto initialTs = opCtx->getServiceContext()->getStorageEngine()->getInitialDataTimestamp();
    if (lastApplied.getTimestamp() < initialTs) {
        invariant(!serverGlobalParams.enableMajorityReadConcern);
        LOGV2_DEBUG(4851800,
                    2,
                    "We cannot transition to SECONDARY state because our 'lastApplied' optime is "
                    "less than the initial data timestamp and enableMajorityReadConcern = false",
                    "minValid"_attr = minValid,
                    "lastApplied"_attr = lastApplied,
                    "initialDataTimestamp"_attr = initialTs);
        return;
    }

    // Execute the transition to SECONDARY.
    auto status = setFollowerMode(MemberState::RS_SECONDARY);
    if (!status.isOK()) {
        LOGV2_WARNING(21413,
                      "Failed to transition into {targetState}. Current "
                      "state: {currentState} {error}",
                      "Failed to perform replica set state transition",
                      "targetState"_attr = MemberState(MemberState::RS_SECONDARY),
                      "currentState"_attr = getMemberState(),
                      "error"_attr = causedBy(status));
    }
}

void ReplicationCoordinatorImpl::advanceCommitPoint(
    const OpTimeAndWallTime& committedOpTimeAndWallTime, bool fromSyncSource) {
    stdx::unique_lock<Latch> lk(_mutex);
    _advanceCommitPoint(lk, committedOpTimeAndWallTime, fromSyncSource);
}

void ReplicationCoordinatorImpl::_advanceCommitPoint(
    WithLock lk,
    const OpTimeAndWallTime& committedOpTimeAndWallTime,
    bool fromSyncSource,
    bool forInitiate) {
    if (_topCoord->advanceLastCommittedOpTimeAndWallTime(
            committedOpTimeAndWallTime, fromSyncSource, forInitiate)) {
        if (_getMemberState_inlock().arbiter()) {
            // Arbiters do not store replicated data, so we consider their data trivially
            // consistent.
            _setMyLastAppliedOpTimeAndWallTime(lk, committedOpTimeAndWallTime, false);
        }

        _setStableTimestampForStorage(lk);
        // Even if we have no new snapshot, we need to notify waiters that the commit point moved.
        _externalState->notifyOplogMetadataWaiters(committedOpTimeAndWallTime.opTime);
    }
}

OpTime ReplicationCoordinatorImpl::getLastCommittedOpTime() const {
    stdx::unique_lock<Latch> lk(_mutex);
    return _topCoord->getLastCommittedOpTime();
}

OpTimeAndWallTime ReplicationCoordinatorImpl::getLastCommittedOpTimeAndWallTime() const {
    stdx::unique_lock<Latch> lk(_mutex);
    return _topCoord->getLastCommittedOpTimeAndWallTime();
}

Status ReplicationCoordinatorImpl::processReplSetRequestVotes(
    OperationContext* opCtx,
    const ReplSetRequestVotesArgs& args,
    ReplSetRequestVotesResponse* response) {

    auto termStatus = updateTerm(opCtx, args.getTerm());
    if (!termStatus.isOK() && termStatus.code() != ErrorCodes::StaleTerm)
        return termStatus;

    {
        stdx::lock_guard<Latch> lk(_mutex);

        // We should only enter terminal shutdown from global terminal exit.  In that case, rather
        // than voting in a term we don't plan to stay alive in, refuse to vote.
        if (_inTerminalShutdown) {
            return Status(ErrorCodes::ShutdownInProgress, "In the process of shutting down");
        }

        const int candidateIndex = args.getCandidateIndex();
        if (candidateIndex < 0 || candidateIndex >= _rsConfig.getNumMembers()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Invalid candidateIndex: " << candidateIndex
                                        << ". Must be between 0 and "
                                        << _rsConfig.getNumMembers() - 1 << " inclusive");
        }

        if (_selfIndex == -1) {
            return Status(ErrorCodes::InvalidReplicaSetConfig,
                          "Invalid replica set config, or this node is not a member");
        }

        _topCoord->processReplSetRequestVotes(args, response);

        if (!args.isADryRun()) {
            const bool votedForCandidate = response->getVoteGranted();
            const long long electionTerm = args.getTerm();
            const Date_t lastVoteDate = _replExecutor->now();
            const int electionCandidateMemberId =
                _rsConfig.getMemberAt(candidateIndex).getId().getData();
            const std::string voteReason = response->getReason();
            const OpTime lastAppliedOpTime = _topCoord->getMyLastAppliedOpTime();
            const OpTime maxAppliedOpTime = _topCoord->latestKnownOpTime();
            const double priorityAtElection = _rsConfig.getMemberAt(_selfIndex).getPriority();
            ReplicationMetrics::get(getServiceContext())
                .setElectionParticipantMetrics(votedForCandidate,
                                               electionTerm,
                                               lastVoteDate,
                                               electionCandidateMemberId,
                                               voteReason,
                                               lastAppliedOpTime,
                                               maxAppliedOpTime,
                                               priorityAtElection);
        }
    }

    // It's safe to store lastVote outside of _mutex. The topology coordinator grants only one
    // vote per term, and storeLocalLastVoteDocument does nothing unless lastVote has a higher term
    // than the previous lastVote, so threads racing to store votes from different terms will
    // eventually store the latest vote.
    if (!args.isADryRun() && response->getVoteGranted()) {
        LastVote lastVote{args.getTerm(), args.getCandidateIndex()};
        Status status = _externalState->storeLocalLastVoteDocument(opCtx, lastVote);
        if (!status.isOK()) {
            LOGV2_ERROR(21428,
                        "replSetRequestVotes failed to store LastVote document",
                        "error"_attr = status);
            return status;
        }
    }
    return Status::OK();
}

void ReplicationCoordinatorImpl::prepareReplMetadata(const BSONObj& metadataRequestObj,
                                                     const OpTime& lastOpTimeFromClient,
                                                     BSONObjBuilder* builder) const {

    bool hasReplSetMetadata = metadataRequestObj.hasField(rpc::kReplSetMetadataFieldName);
    bool hasOplogQueryMetadata = metadataRequestObj.hasField(rpc::kOplogQueryMetadataFieldName);
    // Don't take any locks if we do not need to.
    if (!hasReplSetMetadata && !hasOplogQueryMetadata) {
        return;
    }

    // Avoid retrieving Rollback ID if we do not need it for _prepareOplogQueryMetadata_inlock().
    int rbid = -1;
    if (hasOplogQueryMetadata) {
        rbid = _replicationProcess->getRollbackID();
        invariant(-1 != rbid);
    }

    boost::optional<rpc::ReplSetMetadata> replSetMetadata;
    boost::optional<rpc::OplogQueryMetadata> oplogQueryMetadata;
    {
        stdx::lock_guard<Latch> lk(_mutex);

        if (hasReplSetMetadata) {
            OpTime lastVisibleOpTime =
                std::max(lastOpTimeFromClient, _getCurrentCommittedSnapshotOpTime_inlock());
            replSetMetadata = _topCoord->prepareReplSetMetadata(lastVisibleOpTime);
        }

        if (hasOplogQueryMetadata) {
            oplogQueryMetadata = _topCoord->prepareOplogQueryMetadata(rbid);
        }
    }

    // Do BSON serialization outside lock.
    if (replSetMetadata)
        invariantStatusOK(replSetMetadata->writeToMetadata(builder));
    if (oplogQueryMetadata)
        invariantStatusOK(oplogQueryMetadata->writeToMetadata(builder));
}

bool ReplicationCoordinatorImpl::getWriteConcernMajorityShouldJournal() {
    stdx::unique_lock lock(_mutex);
    return getWriteConcernMajorityShouldJournal_inlock();
}

bool ReplicationCoordinatorImpl::getWriteConcernMajorityShouldJournal_inlock() const {
    return _rsConfig.getWriteConcernMajorityShouldJournal();
}

namespace {
// Fail point to block and optionally skip fetching config. Supported arguments:
//   versionAndTerm: [ v, t ]
void _handleBeforeFetchingConfig(const BSONObj& customArgs,
                                 ConfigVersionAndTerm versionAndTerm,
                                 bool* skipFetchingConfig) {
    if (customArgs.hasElement("versionAndTerm")) {
        const auto nested = customArgs["versionAndTerm"].embeddedObject();
        std::vector<BSONElement> elements;
        nested.elems(elements);
        invariant(elements.size() == 2);
        ConfigVersionAndTerm patternVersionAndTerm =
            ConfigVersionAndTerm(elements[0].numberInt(), elements[1].numberInt());
        if (patternVersionAndTerm == versionAndTerm) {
            LOGV2(5940905,
                  "Failpoint is activated to skip fetching config for version and term",
                  "versionAndTerm"_attr = versionAndTerm);
            *skipFetchingConfig = true;
        }
    }
}
}  // namespace

Status ReplicationCoordinatorImpl::processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                                      ReplSetHeartbeatResponse* response) {
    {
        stdx::lock_guard<Latch> lock(_mutex);
        if (_rsConfigState == kConfigPreStart || _rsConfigState == kConfigStartingUp) {
            return Status(ErrorCodes::NotYetInitialized,
                          "Received heartbeat while still initializing replication system");
        }
    }

    Status result(ErrorCodes::InternalError, "didn't set status in prepareHeartbeatResponse");
    stdx::lock_guard<Latch> lk(_mutex);

    std::string replSetName = [&]() {
        if (!_settings.isServerless()) {
            return _settings.ourSetName();
        } else {
            if (_rsConfig.isInitialized()) {
                return _rsConfig.getReplSetName().toString();
            }

            // In serverless mode before having an initialized config, simply use the replica set
            // name provided in the hearbeat request.
            return args.getSetName();
        }
    }();

    auto senderHost(args.getSenderHost());
    const Date_t now = _replExecutor->now();
    auto configChanged = _topCoord->prepareHeartbeatResponseV1(now, args, replSetName, response);
    result = configChanged.getStatus();
    if (configChanged.isOK() && configChanged.getValue()) {
        // If the latest heartbeat indicates that the remote node's config has changed, we want to
        // update it's member data as soon as possible. Send an immediate hearbeat, and update the
        // member data on processing its response.
        _scheduleHeartbeatToTarget_inlock(senderHost, now, replSetName);
    }

    if ((result.isOK() || result == ErrorCodes::InvalidReplicaSetConfig) && _selfIndex < 0) {
        // If this node does not belong to the configuration it knows about, send heartbeats
        // back to any node that sends us a heartbeat, in case one of those remote nodes has
        // a configuration that contains us.  Chances are excellent that it will, since that
        // is the only reason for a remote node to send this node a heartbeat request.
        if (!senderHost.empty() && _seedList.insert(senderHost).second) {
            LOGV2(21400,
                  "Scheduling heartbeat to fetch a new config from: {senderHost} since we are not "
                  "a member of our current config.",
                  "Scheduling heartbeat to fetch a new config since we are not "
                  "a member of our current config",
                  "senderHost"_attr = senderHost);

            _scheduleHeartbeatToTarget_inlock(senderHost, now, replSetName);
        }
    } else if (result.isOK() &&
               response->getConfigVersionAndTerm() < args.getConfigVersionAndTerm()) {
        logv2::DynamicAttributes attr;
        attr.add("configTerm", args.getConfigTerm());
        attr.add("configVersion", args.getConfigVersion());
        attr.add("senderHost", senderHost);

        // If we are currently in drain mode, we won't allow installing newer configs, so we don't
        // schedule a heartbeat to fetch one. We do allow force reconfigs to proceed even if we are
        // in drain mode.
        if (_memberState.primary() && !_readWriteAbility->canAcceptNonLocalWrites(lk) &&
            args.getConfigTerm() != OpTime::kUninitializedTerm) {
            LOGV2(4794901,
                  "Not scheduling a heartbeat to fetch a newer config since we are in PRIMARY "
                  "state but cannot accept writes yet.",
                  attr);
        }
        // Schedule a heartbeat to the sender to fetch the new config.
        // Only send this if the sender's config is newer.
        // We cannot cancel the enqueued heartbeat, but either this one or the enqueued heartbeat
        // will trigger reconfig, which cancels and reschedules all heartbeats.
        else if (args.hasSender()) {
            bool inTestSkipFetchingConfig = false;
            skipBeforeFetchingConfig.execute([&](const BSONObj& customArgs) {
                _handleBeforeFetchingConfig(
                    customArgs, args.getConfigVersionAndTerm(), &inTestSkipFetchingConfig);
            });

            if (!inTestSkipFetchingConfig) {
                LOGV2(21401, "Scheduling heartbeat to fetch a newer config", attr);
                _scheduleHeartbeatToTarget_inlock(senderHost, now, replSetName);
            }
        }
    } else if (result.isOK() && args.getPrimaryId() >= 0 &&
               (!response->hasPrimaryId() || response->getPrimaryId() != args.getPrimaryId())) {
        // If the sender thinks the primary is different from what we think and if the sender itself
        // is the primary, then we want to update our view of primary by immediately sending out a
        // new round of heartbeats, whose responses should inform us of the new primary. We only do
        // this if the term of the heartbeat is greater than or equal to our own, to prevent
        // updating our view to a stale primary.
        if (args.hasSender() && args.getSenderId() == args.getPrimaryId() &&
            args.getTerm() >= _topCoord->getTerm()) {
            std::string myPrimaryId =
                (response->hasPrimaryId() ? (str::stream() << response->getPrimaryId())
                                          : std::string("none"));
            LOGV2(2903000,
                  "Restarting heartbeats after learning of a new primary",
                  "myPrimaryId"_attr = myPrimaryId,
                  "senderAndPrimaryId"_attr = args.getPrimaryId(),
                  "senderTerm"_attr = args.getTerm());
            _restartScheduledHeartbeats_inlock(replSetName);
        }
    }
    return result;
}

long long ReplicationCoordinatorImpl::getTerm() const {
    // Note: no mutex acquisition here, as we are reading an Atomic variable.
    return _termShadow.load();
}

TopologyVersion ReplicationCoordinatorImpl::getTopologyVersion() const {
    return TopologyVersion(repl::instanceId, _cachedTopologyVersionCounter.load());
}

EventHandle ReplicationCoordinatorImpl::updateTerm_forTest(
    long long term, TopologyCoordinator::UpdateTermResult* updateResult) {
    stdx::lock_guard<Latch> lock(_mutex);

    EventHandle finishEvh;
    finishEvh = _updateTerm_inlock(term, updateResult);
    return finishEvh;
}

Status ReplicationCoordinatorImpl::updateTerm(OperationContext* opCtx, long long term) {
    // Term is only valid if we are replicating.
    if (getReplicationMode() != modeReplSet) {
        return {ErrorCodes::BadValue, "cannot supply 'term' without active replication"};
    }

    // Check we haven't acquired any lock, because potential stepdown needs global lock.
    dassert(!opCtx->lockState()->isLocked() || opCtx->lockState()->isNoop());

    // If the term is already up to date, we can skip the update and the mutex acquisition.
    if (!_needToUpdateTerm(term))
        return Status::OK();

    TopologyCoordinator::UpdateTermResult updateTermResult;
    EventHandle finishEvh;

    {
        stdx::lock_guard<Latch> lock(_mutex);
        finishEvh = _updateTerm_inlock(term, &updateTermResult);
    }

    // Wait for potential stepdown to finish.
    if (finishEvh.isValid()) {
        LOGV2(6015302, "Waiting for potential stepdown to complete before finishing term update");
        _replExecutor->waitForEvent(finishEvh);
    }
    if (updateTermResult == TopologyCoordinator::UpdateTermResult::kUpdatedTerm ||
        updateTermResult == TopologyCoordinator::UpdateTermResult::kTriggerStepDown) {
        return {ErrorCodes::StaleTerm, "Replication term of this node was stale; retry query"};
    }

    return Status::OK();
}

bool ReplicationCoordinatorImpl::_needToUpdateTerm(long long term) {
    return term > _termShadow.load();
}

EventHandle ReplicationCoordinatorImpl::_updateTerm_inlock(
    long long term, TopologyCoordinator::UpdateTermResult* updateTermResult) {

    auto now = _replExecutor->now();
    TopologyCoordinator::UpdateTermResult localUpdateTermResult = _topCoord->updateTerm(term, now);
    if (localUpdateTermResult == TopologyCoordinator::UpdateTermResult::kUpdatedTerm) {
        // When the node discovers a new term, the new term date metrics are now out-of-date, so we
        // clear them.
        ReplicationMetrics::get(getServiceContext()).clearParticipantNewTermDates();

        _termShadow.store(term);
        _cancelPriorityTakeover_inlock();
        _cancelAndRescheduleElectionTimeout_inlock();
    }

    if (updateTermResult) {
        *updateTermResult = localUpdateTermResult;
    }

    if (localUpdateTermResult == TopologyCoordinator::UpdateTermResult::kTriggerStepDown) {
        if (!_pendingTermUpdateDuringStepDown || *_pendingTermUpdateDuringStepDown < term) {
            _pendingTermUpdateDuringStepDown = term;
        }
        if (_topCoord->prepareForUnconditionalStepDown()) {
            LOGV2(21402,
                  "stepping down from primary, because a new term has begun: {term}",
                  "Stepping down from primary, because a new term has begun",
                  "term"_attr = term);
            ReplicationMetrics::get(getServiceContext()).incrementNumStepDownsCausedByHigherTerm();
            return _stepDownStart();
        } else {
            LOGV2_DEBUG(21403,
                        2,
                        "Updated term but not triggering stepdown because we are already in the "
                        "process of stepping down");
        }
    }
    return EventHandle();
}

void ReplicationCoordinatorImpl::waitUntilSnapshotCommitted(OperationContext* opCtx,
                                                            const Timestamp& untilSnapshot) {
    stdx::unique_lock<Latch> lock(_mutex);

    uassert(ErrorCodes::NotYetInitialized,
            "Cannot use snapshots until replica set is finished initializing.",
            _rsConfigState != kConfigUninitialized && _rsConfigState != kConfigInitiating);

    opCtx->waitForConditionOrInterrupt(_currentCommittedSnapshotCond, lock, [&] {
        return _currentCommittedSnapshot &&
            _currentCommittedSnapshot->getTimestamp() >= untilSnapshot;
    });
}

void ReplicationCoordinatorImpl::createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) {
    stdx::lock_guard<Latch> lk(_mutex);

    WriteConcernOptions writeConcern(WriteConcernOptions::kMajority,
                                     WriteConcernOptions::SyncMode::UNSET,
                                     // The timeout isn't used by _doneWaitingForReplication_inlock.
                                     WriteConcernOptions::kNoTimeout);
    writeConcern = _populateUnsetWriteConcernOptionsSyncMode(lk, writeConcern);

    auto setOpTimeCB = [this](Status status) {
        // Only setWMajorityWriteAvailabilityDate if the wait was successful.
        if (status.isOK()) {
            ReplicationMetrics::get(getServiceContext())
                .setWMajorityWriteAvailabilityDate(_replExecutor->now());
        }
    };

    if (_doneWaitingForReplication_inlock(opTime, writeConcern)) {
        // Runs callback and returns early if the writeConcern is immediately satisfied.
        setOpTimeCB(Status::OK());
        return;
    }

    auto pf = makePromiseFuture<void>();
    auto waiter = std::make_shared<Waiter>(std::move(pf.promise), writeConcern);
    auto future = std::move(pf.future).onCompletion(setOpTimeCB);
    _replicationWaiterList.add_inlock(opTime, waiter);
}

bool ReplicationCoordinatorImpl::_updateCommittedSnapshot(WithLock lk,
                                                          const OpTime& newCommittedSnapshot) {
    if (gTestingSnapshotBehaviorInIsolation) {
        return false;
    }

    // If we are in ROLLBACK state, do not set any new _currentCommittedSnapshot, as it will be
    // cleared at the end of rollback anyway.
    if (_memberState.rollback()) {
        LOGV2(21404, "Not updating committed snapshot because we are in rollback");
        return false;
    }
    invariant(!newCommittedSnapshot.isNull());

    // The new committed snapshot should be <= the current replication commit point.
    OpTime lastCommittedOpTime = _topCoord->getLastCommittedOpTime();
    invariant(newCommittedSnapshot.getTimestamp() <= lastCommittedOpTime.getTimestamp());
    invariant(newCommittedSnapshot <= lastCommittedOpTime);

    // The new committed snapshot should be >= the current snapshot.
    if (_currentCommittedSnapshot) {
        invariant(newCommittedSnapshot.getTimestamp() >= _currentCommittedSnapshot->getTimestamp());
        invariant(newCommittedSnapshot >= *_currentCommittedSnapshot);
    }
    if (MONGO_unlikely(disableSnapshotting.shouldFail()))
        return false;
    _currentCommittedSnapshot = newCommittedSnapshot;
    _currentCommittedSnapshotCond.notify_all();

    _externalState->updateCommittedSnapshot(newCommittedSnapshot);

    // Wake up any threads waiting for read concern or write concern.
    if (_externalState->snapshotsEnabled() && _currentCommittedSnapshot) {
        _wakeReadyWaiters(lk, _currentCommittedSnapshot);
    }
    return true;
}

void ReplicationCoordinatorImpl::clearCommittedSnapshot() {
    stdx::lock_guard<Latch> lock(_mutex);
    _clearCommittedSnapshot_inlock();
}

void ReplicationCoordinatorImpl::_clearCommittedSnapshot_inlock() {
    _currentCommittedSnapshot = boost::none;
    _externalState->clearCommittedSnapshot();
}

void ReplicationCoordinatorImpl::waitForElectionFinish_forTest() {
    EventHandle finishedEvent;
    {
        stdx::lock_guard lk(_mutex);
        if (_electionState) {
            finishedEvent = _electionState->getElectionFinishedEvent(lk);
        }
    }
    if (finishedEvent.isValid()) {
        _replExecutor->waitForEvent(finishedEvent);
    }
}

void ReplicationCoordinatorImpl::waitForElectionDryRunFinish_forTest() {
    EventHandle finishedEvent;
    if (_electionState) {
        stdx::lock_guard lk(_mutex);
        finishedEvent = _electionState->getElectionDryRunFinishedEvent(lk);
    }
    if (finishedEvent.isValid()) {
        _replExecutor->waitForEvent(finishedEvent);
    }
}

CallbackHandle ReplicationCoordinatorImpl::_scheduleWorkAt(Date_t when, CallbackFn work) {
    auto cbh =
        _replExecutor->scheduleWorkAt(when, [work = std::move(work)](const CallbackArgs& args) {
            if (args.status == ErrorCodes::CallbackCanceled) {
                return;
            }
            work(args);
        });
    if (cbh == ErrorCodes::ShutdownInProgress) {
        return {};
    }
    return fassert(28800, cbh);
}

EventHandle ReplicationCoordinatorImpl::_makeEvent() {
    auto eventResult = this->_replExecutor->makeEvent();
    if (eventResult.getStatus() == ErrorCodes::ShutdownInProgress) {
        return EventHandle();
    }
    fassert(28825, eventResult.getStatus());
    return eventResult.getValue();
}

WriteConcernOptions ReplicationCoordinatorImpl::populateUnsetWriteConcernOptionsSyncMode(
    WriteConcernOptions wc) {
    stdx::lock_guard<Latch> lock(_mutex);
    return _populateUnsetWriteConcernOptionsSyncMode(lock, wc);
}

WriteConcernOptions ReplicationCoordinatorImpl::_populateUnsetWriteConcernOptionsSyncMode(
    WithLock lk, WriteConcernOptions wc) {
    WriteConcernOptions writeConcern(wc);
    if (writeConcern.syncMode == WriteConcernOptions::SyncMode::UNSET) {
        if (writeConcern.isMajority() && getWriteConcernMajorityShouldJournal_inlock()) {
            writeConcern.syncMode = WriteConcernOptions::SyncMode::JOURNAL;
        } else {
            writeConcern.syncMode = WriteConcernOptions::SyncMode::NONE;
        }
    }
    return writeConcern;
}

CallbackFn ReplicationCoordinatorImpl::_wrapAsCallbackFn(const std::function<void()>& work) {
    return [work](const CallbackArgs& cbData) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }

        work();
    };
}

Status ReplicationCoordinatorImpl::stepUpIfEligible(bool skipDryRun) {

    auto reason = skipDryRun ? StartElectionReasonEnum::kStepUpRequestSkipDryRun
                             : StartElectionReasonEnum::kStepUpRequest;
    _startElectSelfIfEligibleV1(reason);

    EventHandle finishEvent;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        // A null _electionState indicates that the election has already completed.
        if (_electionState) {
            finishEvent = _electionState->getElectionFinishedEvent(lk);
        }
    }
    if (finishEvent.isValid()) {
        LOGV2(6015303, "Waiting for in-progress election to complete before finishing stepup");
        _replExecutor->waitForEvent(finishEvent);
    }
    {
        // Step up is considered successful only if we are currently a primary and we are not in the
        // process of stepping down. If we know we are going to step down, we should fail the
        // replSetStepUp command so caller can retry if necessary.
        stdx::lock_guard<Latch> lk(_mutex);
        if (!_getMemberState_inlock().primary())
            return Status(ErrorCodes::CommandFailed, "Election failed.");
        else if (_topCoord->isSteppingDown())
            return Status(ErrorCodes::CommandFailed, "Election failed due to concurrent stepdown.");
    }
    return Status::OK();
}

executor::TaskExecutor::EventHandle ReplicationCoordinatorImpl::_cancelElectionIfNeeded(
    WithLock lk) {
    if (_topCoord->getRole() != TopologyCoordinator::Role::kCandidate) {
        return {};
    }
    invariant(_electionState);
    _electionState->cancel(lk);
    return _electionState->getElectionFinishedEvent(lk);
}

int64_t ReplicationCoordinatorImpl::_nextRandomInt64_inlock(int64_t limit) {
    return _random.nextInt64(limit);
}

bool ReplicationCoordinatorImpl::setContainsArbiter() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig.containsArbiter();
}

void ReplicationCoordinatorImpl::ReadWriteAbility::setCanAcceptNonLocalWrites(
    WithLock lk, OperationContext* opCtx, bool canAcceptWrites) {
    // We must be holding the RSTL in mode X to change _canAcceptNonLocalWrites.
    invariant(opCtx);
    invariant(opCtx->lockState()->isRSTLExclusive());
    if (canAcceptWrites == canAcceptNonLocalWrites(lk)) {
        return;
    }
    _canAcceptNonLocalWrites.store(canAcceptWrites);
}

bool ReplicationCoordinatorImpl::ReadWriteAbility::canAcceptNonLocalWrites(WithLock) const {
    return _canAcceptNonLocalWrites.loadRelaxed();
}

bool ReplicationCoordinatorImpl::ReadWriteAbility::canAcceptNonLocalWrites_UNSAFE() const {
    return _canAcceptNonLocalWrites.loadRelaxed();
}

bool ReplicationCoordinatorImpl::ReadWriteAbility::canAcceptNonLocalWrites(
    OperationContext* opCtx) const {
    // We must be holding the RSTL.
    invariant(opCtx);
    invariant(opCtx->lockState()->isRSTLLocked());
    return _canAcceptNonLocalWrites.loadRelaxed();
}

bool ReplicationCoordinatorImpl::ReadWriteAbility::canServeNonLocalReads_UNSAFE() const {
    return _canServeNonLocalReads.loadRelaxed();
}

bool ReplicationCoordinatorImpl::ReadWriteAbility::canServeNonLocalReads(
    OperationContext* opCtx) const {
    // We must be holding the RSTL.
    invariant(opCtx);
    invariant(opCtx->lockState()->isRSTLLocked() || opCtx->isLockFreeReadsOp());
    return _canServeNonLocalReads.loadRelaxed();
}

void ReplicationCoordinatorImpl::ReadWriteAbility::setCanServeNonLocalReads(OperationContext* opCtx,
                                                                            unsigned int newVal) {
    // We must be holding the RSTL in mode X to change _canServeNonLocalReads.
    invariant(opCtx);
    invariant(opCtx->lockState()->isRSTLExclusive());
    _canServeNonLocalReads.store(newVal);
}

void ReplicationCoordinatorImpl::ReadWriteAbility::setCanServeNonLocalReads_UNSAFE(
    unsigned int newVal) {
    _canServeNonLocalReads.store(newVal);
}

void ReplicationCoordinatorImpl::recordIfCWWCIsSetOnConfigServerOnStartup(OperationContext* opCtx) {
    auto isCWWCSet = _externalState->isCWWCSetOnConfigShard(opCtx);
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _wasCWWCSetOnConfigServerOnStartup = isCWWCSet;
    }
}

void ReplicationCoordinatorImpl::_validateDefaultWriteConcernOnShardStartup(WithLock lk) const {
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        // Checking whether the shard is part of a sharded cluster or not by checking if CWWC
        // flag is set as we record it during sharding initialization phase, as on restarting a
        // shard node for upgrading or any other reason, sharding initialization happens before
        // config initialization.
        if (_wasCWWCSetOnConfigServerOnStartup && !_wasCWWCSetOnConfigServerOnStartup.value() &&
            !_rsConfig.isImplicitDefaultWriteConcernMajority()) {
            auto msg =
                "Cannot start shard because the implicit default write concern on this shard is "
                "set to {w : 1}, since the number of writable voting members is not strictly more "
                "than the voting majority. Change the shard configuration or set the cluster-wide "
                "write concern using setDefaultRWConcern command and try again.";
            fassert(5684400, {ErrorCodes::IllegalOperation, msg});
        }
    }
}

ReplicationCoordinatorImpl::WriteConcernTagChanges*
ReplicationCoordinatorImpl::getWriteConcernTagChanges() {
    return &_writeConcernTagChanges;
}

}  // namespace repl
}  // namespace mongo

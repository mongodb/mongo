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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#define LOGV2_FOR_ELECTION(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                             \
        ID, DLEVEL, {logv2::LogComponent::kReplicationElection}, MESSAGE, ##__VA_ARGS__)

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_coordinator_impl.h"

#include <algorithm>
#include <functional>
#include <limits>

#include "mongo/base/status.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/mongod_options_storage_gen.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/prepare_conflict_tracker.h"
#include "mongo/db/repl/check_quorum_for_config_change.h"
#include "mongo/db/repl/data_replicator_external_state_initial_sync.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/local_oplog_info.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config_checks.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_impl_gen.h"
#include "mongo/db/repl/replication_metrics.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/network_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/transport/ismaster_metrics.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(stepdownHangBeforePerformingPostMemberStateUpdateActions);
MONGO_FAIL_POINT_DEFINE(holdStableTimestampAtSpecificTimestamp);
MONGO_FAIL_POINT_DEFINE(stepdownHangBeforeRSTLEnqueue);
// Fail setMaintenanceMode with ErrorCodes::NotSecondary to simulate a concurrent election.
MONGO_FAIL_POINT_DEFINE(setMaintenanceModeFailsWithNotSecondary);
MONGO_FAIL_POINT_DEFINE(forceSyncSourceRetryWaitForInitialSync);
// Signals that an isMaster request has started waiting.
MONGO_FAIL_POINT_DEFINE(waitForIsMasterResponse);
// Will cause an isMaster request to hang as it starts waiting.
MONGO_FAIL_POINT_DEFINE(hangWhileWaitingForIsMasterResponse);
MONGO_FAIL_POINT_DEFINE(skipDurableTimestampUpdates);
// Will cause a reconfig to hang after completing the config quorum check.
MONGO_FAIL_POINT_DEFINE(omitConfigQuorumCheck);
// Will cause signal drain complete to hang after reconfig
MONGO_FAIL_POINT_DEFINE(hangAfterReconfigOnDrainComplete);

// Number of times we tried to go live as a secondary.
Counter64 attemptsToBecomeSecondary;
ServerStatusMetricField<Counter64> displayAttemptsToBecomeSecondary(
    "repl.apply.attemptsToBecomeSecondary", &attemptsToBecomeSecondary);

// Tracks the last state transition performed in this replca set.
std::string lastStateTransition;
ServerStatusMetricField<std::string> displayLastStateTransition(
    "repl.stateTransition.lastStateTransition", &lastStateTransition);

// Tracks the number of operations killed on state transition.
Counter64 userOpsKilled;
ServerStatusMetricField<Counter64> displayUserOpsKilled("repl.stateTransition.userOperationsKilled",
                                                        &userOpsKilled);

// Tracks the number of operations left running on state transition.
Counter64 userOpsRunning;
ServerStatusMetricField<Counter64> displayUserOpsRunning(
    "repl.stateTransition.userOperationsRunning", &userOpsRunning);

using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using CallbackFn = executor::TaskExecutor::CallbackFn;
using CallbackHandle = executor::TaskExecutor::CallbackHandle;
using EventHandle = executor::TaskExecutor::EventHandle;
using executor::NetworkInterface;
using NextAction = Fetcher::NextAction;

namespace {

const char kLocalDB[] = "local";
// Overrides _canAcceptLocalWrites for the decorated OperationContext.
const OperationContext::Decoration<bool> alwaysAllowNonLocalWrites =
    OperationContext::declareDecoration<bool>();

/**
 * Allows non-local writes despite _canAcceptNonLocalWrites being false on a single OperationContext
 * while in scope.
 *
 * Resets to original value when leaving scope so it is safe to nest.
 */
class AllowNonLocalWritesBlock {
    AllowNonLocalWritesBlock(const AllowNonLocalWritesBlock&) = delete;
    AllowNonLocalWritesBlock& operator=(const AllowNonLocalWritesBlock&) = delete;

public:
    AllowNonLocalWritesBlock(OperationContext* opCtx)
        : _opCtx(opCtx), _initialState(alwaysAllowNonLocalWrites(_opCtx)) {
        alwaysAllowNonLocalWrites(_opCtx) = true;
    }

    ~AllowNonLocalWritesBlock() {
        alwaysAllowNonLocalWrites(_opCtx) = _initialState;
    }

private:
    OperationContext* const _opCtx;
    const bool _initialState;
};

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

InitialSyncerOptions createInitialSyncerOptions(
    ReplicationCoordinator* replCoord, ReplicationCoordinatorExternalState* externalState) {
    InitialSyncerOptions options;
    options.getMyLastOptime = [replCoord]() { return replCoord->getMyLastAppliedOpTime(); };
    options.setMyLastOptime = [replCoord,
                               externalState](const OpTimeAndWallTime& opTimeAndWallTime,
                                              ReplicationCoordinator::DataConsistency consistency) {
        // Note that setting the last applied opTime forward also advances the global timestamp.
        replCoord->setMyLastAppliedOpTimeAndWallTimeForward(opTimeAndWallTime, consistency);
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
    if (!_handleElectionTimeoutCbh.isValid()) {
        return Date_t();
    }
    return _handleElectionTimeoutWhen;
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

OpTimeAndWallTime ReplicationCoordinatorImpl::getCurrentCommittedSnapshotOpTimeAndWallTime() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _getCurrentCommittedSnapshotOpTimeAndWallTime_inlock();
}

OpTime ReplicationCoordinatorImpl::_getCurrentCommittedSnapshotOpTime_inlock() const {
    if (_currentCommittedSnapshot) {
        return _currentCommittedSnapshot->opTime;
    }
    return OpTime();
}

OpTimeAndWallTime ReplicationCoordinatorImpl::_getCurrentCommittedSnapshotOpTimeAndWallTime_inlock()
    const {
    if (_currentCommittedSnapshot) {
        return _currentCommittedSnapshot.get();
    }
    return OpTimeAndWallTime();
}

void ReplicationCoordinatorImpl::appendDiagnosticBSON(mongo::BSONObjBuilder* bob) {
    BSONObjBuilder eBuilder(bob->subobjStart("executor"));
    _replExecutor->appendDiagnosticBSON(&eBuilder);
}

void ReplicationCoordinatorImpl::appendConnectionStats(executor::ConnectionPoolStats* stats) const {
    _replExecutor->appendConnectionStats(stats);
}

bool ReplicationCoordinatorImpl::_startLoadLocalConfig(OperationContext* opCtx) {
    // Create necessary replication collections to guarantee that if a checkpoint sees data after
    // initial sync has completed, it also sees these collections.
    fassert(50708, _replicationProcess->getConsistencyMarkers()->createInternalCollections(opCtx));

    // Ensure (update if needed) the in-memory count for the oplogTruncateAfterPoint collection
    // matches the collection contents.
    _replicationProcess->getConsistencyMarkers()->ensureFastCountOnOplogTruncateAfterPoint(opCtx);

    _replicationProcess->getConsistencyMarkers()->initializeMinValidDocument(opCtx);

    fassert(51240, _externalState->createLocalLastVoteCollection(opCtx));

    StatusWith<LastVote> lastVote = _externalState->loadLocalLastVoteDocument(opCtx);
    if (!lastVote.isOK()) {
        LOGV2_FATAL(21429,
                    "Error loading local voted for document at startup; {status}",
                    "status"_attr = lastVote.getStatus());
        fassertFailedNoTrace(40367);
    }
    if (lastVote.getValue().getTerm() == OpTime::kInitialTerm) {
        // This log line is checked in unit tests.
        LOGV2(21311, "Did not find local initialized voted for document at startup.");
    }
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _topCoord->loadLastVote(lastVote.getValue());
    }

    // Check that we have a local Rollback ID. If we do not have one, create one.
    auto status = _replicationProcess->refreshRollbackID(opCtx);
    if (!status.isOK()) {
        if (status == ErrorCodes::NamespaceNotFound) {
            LOGV2(21312, "Did not find local Rollback ID document at startup. Creating one.");
            auto initializingStatus = _replicationProcess->initializeRollbackID(opCtx);
            fassert(40424, initializingStatus);
        } else {
            LOGV2_FATAL(21430,
                        "Error loading local Rollback ID document at startup; {status}",
                        "status"_attr = status);
            fassertFailedNoTrace(40428);
        }
    }

    StatusWith<BSONObj> cfg = _externalState->loadLocalConfigDocument(opCtx);
    if (!cfg.isOK()) {
        LOGV2(21313,
              "Did not find local replica set configuration document at startup;  {status}",
              "status"_attr = cfg.getStatus());
        return true;
    }
    ReplSetConfig localConfig;
    status = localConfig.initialize(cfg.getValue());
    if (!status.isOK()) {
        if (status.code() == ErrorCodes::RepairedReplicaSetNode) {
            LOGV2_FATAL(
                21431,
                "This instance has been repaired and may contain modified replicated data that "
                "would not match other replica set members. To see your repaired data, start "
                "mongod without the --replSet option. When you are finished recovering your "
                "data and would like to perform a complete re-sync, please refer to the "
                "documentation here: "
                "https://docs.mongodb.com/manual/tutorial/resync-replica-set-member/");
            fassertFailedNoTrace(50923);
        }
        LOGV2_ERROR(21414,
                    "Locally stored replica set configuration does not parse; See "
                    "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config "
                    "for information on how to recover from this. Got \"{status}\" while parsing "
                    "{config}",
                    "status"_attr = status,
                    "config"_attr = cfg.getValue());
        fassertFailedNoTrace(28545);
    }

    // Read the last op from the oplog after cleaning up any partially applied batches.
    const auto stableTimestamp = boost::none;
    _replicationProcess->getReplicationRecovery()->recoverFromOplog(opCtx, stableTimestamp);
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

    return false;
}

void ReplicationCoordinatorImpl::_createHorizonTopologyChangePromiseMapping(WithLock) {
    auto horizonMappings = _rsConfig.getMemberAt(_selfIndex).getHorizonMappings();
    // Create a new horizon to promise mapping since it is possible for the horizons
    // to change after a replica set reconfig.
    _horizonToPromiseMap.clear();
    for (auto const& [horizon, hostAndPort] : horizonMappings) {
        _horizonToPromiseMap.emplace(
            horizon, std::make_shared<SharedPromise<std::shared_ptr<const IsMasterResponse>>>());
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
                    "Loading local replica set configuration failed due to {status}",
                    "status"_attr = cbData.status);
        return;
    }

    StatusWith<int> myIndex =
        validateConfigForStartUp(_externalState.get(), localConfig, getServiceContext());
    if (!myIndex.isOK()) {
        if (myIndex.getStatus() == ErrorCodes::NodeNotFound ||
            myIndex.getStatus() == ErrorCodes::InvalidReplicaSetConfig) {
            LOGV2_WARNING(21405,
                          "Locally stored replica set configuration does not have a valid entry "
                          "for the current node; waiting for reconfig or remote heartbeat; Got "
                          "\"{status}\" while validating {localConfig}",
                          "status"_attr = myIndex.getStatus(),
                          "localConfig"_attr = localConfig.toBSON());
            myIndex = StatusWith<int>(-1);
        } else {
            LOGV2_ERROR(21415,
                        "Locally stored replica set configuration is invalid; See "
                        "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config"
                        " for information on how to recover from this. Got \"{status}\" "
                        "while validating {localConfig}",
                        "status"_attr = myIndex.getStatus(),
                        "localConfig"_attr = localConfig.toBSON());
            fassertFailedNoTrace(28544);
        }
    }

    if (localConfig.getReplSetName() != _settings.ourSetName()) {
        LOGV2_WARNING(21406,
                      "Local replica set configuration document reports set name of "
                      "{localConfigSetName}, but command line reports "
                      "{settingsSetName}; waiting for reconfig or remote heartbeat",
                      "localConfigSetName"_attr = localConfig.getReplSetName(),
                      "settingsSetName"_attr = _settings.ourSetName());
        myIndex = StatusWith<int>(-1);
    }

    if (serverGlobalParams.enableMajorityReadConcern && localConfig.getNumMembers() == 3 &&
        localConfig.getNumDataBearingMembers() == 2) {
        LOGV2_OPTIONS(21315, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(
            21316,
            {logv2::LogTag::kStartupWarnings},
            "** WARNING: This replica set has a Primary-Secondary-Arbiter architecture, but "
            "readConcern:majority is enabled ");
        LOGV2_OPTIONS(
            21317,
            {logv2::LogTag::kStartupWarnings},
            "**          for this node. This is not a recommended configuration. Please see ");
        LOGV2_OPTIONS(21318,
                      {logv2::LogTag::kStartupWarnings},
                      "**          https://dochub.mongodb.org/core/psa-disable-rc-majority");
        LOGV2_OPTIONS(21319, {logv2::LogTag::kStartupWarnings}, "");
    }

    // Do not check optime, if this node is an arbiter.
    bool isArbiter =
        myIndex.getValue() != -1 && localConfig.getMemberAt(myIndex.getValue()).isArbiter();
    OpTimeAndWallTime lastOpTimeAndWallTime = OpTimeAndWallTime();
    if (!isArbiter) {
        if (!lastOpTimeAndWallTimeStatus.isOK()) {
            LOGV2_WARNING(
                21407,
                "Failed to load timestamp and/or wall clock time of most recently applied "
                "operation: {status}",
                "status"_attr = lastOpTimeAndWallTimeStatus.getStatus());
        } else {
            lastOpTimeAndWallTime = lastOpTimeAndWallTimeStatus.getValue();
        }
    } else {
        // The node is an arbiter hence will not need logical clock for external operations.
        LogicalClock::get(getServiceContext())->disable();
        if (auto validator = LogicalTimeValidator::get(getServiceContext())) {
            validator->stopKeyManager();
        }
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
    auto consistency = DataConsistency::Inconsistent;
    if (!lastOpTime.isNull()) {

        // If we have an oplog, it is still possible that our data is not in a consistent state. For
        // example, if we are starting up after a crash following a post-rollback RECOVERING state.
        // To detect this, we see if our last optime is >= the 'minValid' optime, which
        // should be persistent across node crashes.
        OpTime minValid = _replicationProcess->getConsistencyMarkers()->getMinValid(opCtx.get());
        consistency =
            (lastOpTime >= minValid) ? DataConsistency::Consistent : DataConsistency::Inconsistent;
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
        bool isRollbackAllowed = false;
        _setMyLastAppliedOpTimeAndWallTime(
            lock, lastOpTimeAndWallTime, isRollbackAllowed, consistency);
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
    LOGV2_DEBUG(21320, 1, "Current term is now {term}", "term"_attr = term);
    _performPostMemberStateUpdateAction(action);

    if (!isArbiter) {
        _externalState->startThreads(_settings);
        _startDataReplication(opCtx.get());
    }
}

void ReplicationCoordinatorImpl::_stopDataReplication(OperationContext* opCtx) {
    std::shared_ptr<InitialSyncer> initialSyncerCopy;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _initialSyncer.swap(initialSyncerCopy);
    }
    if (initialSyncerCopy) {
        LOGV2_DEBUG(
            21321,
            1,
            "ReplicationCoordinatorImpl::_stopDataReplication calling InitialSyncer::shutdown.");
        const auto status = initialSyncerCopy->shutdown();
        if (!status.isOK()) {
            LOGV2_WARNING(21408, "InitialSyncer shutdown failed: {status}", "status"_attr = status);
        }
        initialSyncerCopy.reset();
        // Do not return here, fall through.
    }
    LOGV2_DEBUG(21322,
                1,
                "ReplicationCoordinatorImpl::_stopDataReplication calling "
                "ReplCoordExtState::stopDataReplication.");
    _externalState->stopDataReplication(opCtx);
}

void ReplicationCoordinatorImpl::_startDataReplication(OperationContext* opCtx,
                                                       std::function<void()> startCompleted) {
    // Check to see if we need to do an initial sync.
    const auto lastOpTime = getMyLastAppliedOpTime();
    const auto needsInitialSync =
        lastOpTime.isNull() || _externalState->isInitialSyncFlagSet(opCtx);
    if (!needsInitialSync) {
        // Start steady replication, since we already have data.
        // ReplSetConfig has been installed, so it's either in STARTUP2 or REMOVED.
        auto memberState = getMemberState();
        invariant(memberState.startup2() || memberState.removed());
        invariant(setFollowerMode(MemberState::RS_RECOVERING));
        _externalState->startSteadyStateReplication(opCtx, this);
        return;
    }

    // Do initial sync.
    if (!_externalState->getTaskExecutor()) {
        LOGV2(21323, "not running initial sync during test.");
        return;
    }

    auto onCompletion = [this, startCompleted](const StatusWith<OpTimeAndWallTime>& opTimeStatus) {
        {
            stdx::lock_guard<Latch> lock(_mutex);
            if (opTimeStatus == ErrorCodes::CallbackCanceled) {
                LOGV2(21324,
                      "Initial Sync has been cancelled: {status}",
                      "status"_attr = opTimeStatus.getStatus());
                return;
            } else if (!opTimeStatus.isOK()) {
                if (_inShutdown) {
                    LOGV2(21325,
                          "Initial Sync failed during shutdown due to {status}",
                          "status"_attr = opTimeStatus.getStatus());
                    return;
                } else {
                    LOGV2_ERROR(21416,
                                "Initial sync failed, shutting down now. Restart the server "
                                "to attempt a new initial sync.");
                    fassertFailedWithStatusNoTrace(40088, opTimeStatus.getStatus());
                }
            }

            const auto lastApplied = opTimeStatus.getValue();
            _setMyLastAppliedOpTimeAndWallTime(
                lock, lastApplied, false, DataConsistency::Consistent);
        }

        // Clear maint. mode.
        while (getMaintenanceMode()) {
            setMaintenanceMode(false).transitional_ignore();
        }

        if (startCompleted) {
            startCompleted();
        }
        // Because initial sync completed, we can only be in STARTUP2, not REMOVED.
        // Transition from STARTUP2 to RECOVERING and start the producer and the applier.
        invariant(getMemberState().startup2());
        invariant(setFollowerMode(MemberState::RS_RECOVERING));
        auto opCtxHolder = cc().makeOperationContext();
        _externalState->startSteadyStateReplication(opCtxHolder.get(), this);
    };

    std::shared_ptr<InitialSyncer> initialSyncerCopy;
    try {
        {
            // Must take the lock to set _initialSyncer, but not call it.
            stdx::lock_guard<Latch> lock(_mutex);
            if (_inShutdown) {
                LOGV2(21326, "Initial Sync not starting because replication is shutting down.");
                return;
            }
            initialSyncerCopy = std::make_shared<InitialSyncer>(
                createInitialSyncerOptions(this, _externalState.get()),
                std::make_unique<DataReplicatorExternalStateInitialSync>(this,
                                                                         _externalState.get()),
                _externalState->getDbWorkThreadPool(),
                _storage,
                _replicationProcess,
                onCompletion);
            _initialSyncer = initialSyncerCopy;
        }
        // InitialSyncer::startup() must be called outside lock because it uses features (eg.
        // setting the initial sync flag) which depend on the ReplicationCoordinatorImpl.
        uassertStatusOK(initialSyncerCopy->startup(opCtx, numInitialSyncAttempts.load()));
    } catch (const DBException& e) {
        auto status = e.toStatus();
        LOGV2(21327, "Initial Sync failed to start: {status}", "status"_attr = status);
        if (ErrorCodes::CallbackCanceled == status || ErrorCodes::isShutdownError(status.code())) {
            return;
        }
        fassertFailedWithStatusNoTrace(40354, status);
    }
}

void ReplicationCoordinatorImpl::startup(OperationContext* opCtx) {
    if (!isReplEnabled()) {
        if (ReplSettings::shouldRecoverFromOplogAsStandalone()) {
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "Cannot set parameter 'recoverToOplogTimestamp' "
                                  << "when recovering from the oplog as a standalone",
                    recoverToOplogTimestamp.empty());
            _replicationProcess->getReplicationRecovery()->recoverFromOplogAsStandalone(opCtx);
        }

        if (storageGlobalParams.readOnly && !recoverToOplogTimestamp.empty()) {
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
            // Temporarily turn off read-only mode for this procedure as we'll have to do writes.
            storageGlobalParams.readOnly = false;
            ON_BLOCK_EXIT([&] { storageGlobalParams.readOnly = true; });
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

    bool doneLoadingConfig = _startLoadLocalConfig(opCtx);
    if (doneLoadingConfig) {
        // If we're not done loading the config, then the config state will be set by
        // _finishLoadLocalConfig.
        stdx::lock_guard<Latch> lk(_mutex);
        invariant(!_rsConfig.isInitialized());
        _setConfigState_inlock(kConfigUninitialized);
    }
}

void ReplicationCoordinatorImpl::enterTerminalShutdown() {
    stdx::lock_guard lk(_mutex);
    _inTerminalShutdown = true;
}

void ReplicationCoordinatorImpl::shutdown(OperationContext* opCtx) {
    // Shutdown must:
    // * prevent new threads from blocking in awaitReplication
    // * wake up all existing threads blocking in awaitReplication
    // * Shut down and join the execution resources it owns.

    if (!_settings.usingReplSets()) {
        return;
    }

    LOGV2(21328, "shutting down replication subsystems");

    // Used to shut down outside of the lock.
    std::shared_ptr<InitialSyncer> initialSyncerCopy;
    {
        stdx::unique_lock<Latch> lk(_mutex);
        fassert(28533, !_inShutdown);
        _inShutdown = true;
        if (_rsConfigState == kConfigPreStart) {
            LOGV2_WARNING(21409,
                          "ReplicationCoordinatorImpl::shutdown() called before "
                          "startup() finished.  Shutting down without cleaning up the "
                          "replication system");
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
            21329, 1, "ReplicationCoordinatorImpl::shutdown calling InitialSyncer::shutdown.");
        const auto status = initialSyncerCopy->shutdown();
        if (!status.isOK()) {
            LOGV2_WARNING(21410, "InitialSyncer shutdown failed: {status}", "status"_attr = status);
        }
        initialSyncerCopy->join();
        initialSyncerCopy.reset();
    }
    _externalState->shutdown(opCtx);
    _replExecutor->shutdown();
    _replExecutor->join();
}

void ReplicationCoordinatorImpl::markAsCleanShutdownIfPossible(OperationContext* opCtx) {
    _externalState->clearAppliedThroughIfCleanShutdown(opCtx);
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

Status ReplicationCoordinatorImpl::waitForMemberState(MemberState expectedState,
                                                      Milliseconds timeout) {
    if (timeout < Milliseconds(0)) {
        return Status(ErrorCodes::BadValue, "Timeout duration cannot be negative");
    }

    stdx::unique_lock<Latch> lk(_mutex);
    auto pred = [this, expectedState]() { return _memberState == expectedState; };
    if (!_memberStateChange.wait_for(lk, timeout.toSystemDuration(), pred)) {
        return Status(ErrorCodes::ExceededTimeLimit,
                      str::stream()
                          << "Timed out waiting for state to become " << expectedState.toString()
                          << ". Current state is " << _memberState.toString());
    }
    return Status::OK();
}

Seconds ReplicationCoordinatorImpl::getSlaveDelaySecs() const {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_rsConfig.isInitialized());
    if (_selfIndex == -1) {
        // We aren't currently in the set. Return 0 seconds so we can clear out the applier's
        // queue of work.
        return Seconds(0);
    }
    return _rsConfig.getMemberAt(_selfIndex).getSlaveDelay();
}

void ReplicationCoordinatorImpl::clearSyncSourceBlacklist() {
    stdx::lock_guard<Latch> lk(_mutex);
    _topCoord->clearSyncSourceBlacklist();
}

Status ReplicationCoordinatorImpl::setFollowerModeStrict(OperationContext* opCtx,
                                                         const MemberState& newState) {
    invariant(opCtx);
    invariant(opCtx->lockState()->isRSTLExclusive());
    return _setFollowerMode(opCtx, newState);
}

Status ReplicationCoordinatorImpl::setFollowerMode(const MemberState& newState) {
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

    if (auto electionFinishedEvent = _cancelElectionIfNeeded_inlock()) {
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

    const PostMemberStateUpdateAction action = _updateMemberStateFromTopologyCoordinator(lk, opCtx);
    lk.unlock();
    _performPostMemberStateUpdateAction(action);

    return Status::OK();
}

ReplicationCoordinator::ApplierState ReplicationCoordinatorImpl::getApplierState() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _applierState;
}

void ReplicationCoordinatorImpl::signalDrainComplete(OperationContext* opCtx,
                                                     long long termWhenBufferIsEmpty) {
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
        return;
    }
    lk.unlock();

    _externalState->onDrainComplete(opCtx);

    // Kill all user writes and user reads that encounter a prepare conflict. Also kills select
    // internal operations. Although secondaries cannot accept writes, a step up can kill writes
    // that were blocked behind the RSTL lock held by a step down attempt. These writes will be
    // killed with a retryable error code during step up.
    AutoGetRstlForStepUpStepDown arsu(
        this, opCtx, ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepUp);
    lk.lock();

    // Exit drain mode only if we're actually in draining mode, the apply buffer is empty in the
    // current term, and we're allowed to become the write master.
    if (_applierState != ApplierState::Draining ||
        !_topCoord->canCompleteTransitionToPrimary(termWhenBufferIsEmpty)) {
        return;
    }
    _applierState = ApplierState::Stopped;

    invariant(_getMemberState_inlock().primary());
    invariant(!_readWriteAbility->canAcceptNonLocalWrites(opCtx));

    {
        // If the config doesn't have a term, don't change it.
        auto needBumpConfigTerm = _rsConfig.getConfigTerm() != OpTime::kUninitializedTerm;
        lk.unlock();

        if (needBumpConfigTerm) {
            // We re-write the term but keep version the same. This conceptually a no-op
            // in the config consensus group, analogous to writing a new oplog entry
            // in Raft log state machine on step up.
            auto getNewConfig = [&](const ReplSetConfig& oldConfig, long long primaryTerm) {
                auto config = oldConfig;
                config.setConfigTerm(primaryTerm);
                return config;
            };
            LOGV2(4508103, "Increment the config term via reconfig.");
            auto reconfigStatus = doReplSetReconfig(opCtx, getNewConfig, true /* force */);
            if (!reconfigStatus.isOK()) {
                LOGV2(4508100,
                      "Automatic reconfig to increment the config term on stepup failed",
                      "status"_attr = reconfigStatus);
                // If the node stepped down after we released the lock, we can just return.
                if (ErrorCodes::isNotMasterError(reconfigStatus.code())) {
                    return;
                }
                // Writing this new config with a new term is somewhat "best effort", and if we get
                // preempted by a concurrent reconfig, that is fine since that new config will have
                // occurred after the node became primary and so the concurrent reconfig has updated
                // the term appropriately.
                if (reconfigStatus != ErrorCodes::ConfigurationInProgress) {
                    LOGV2_FATAL(4508101,
                                "Reconfig on stepup failed for unknown reasons.",
                                "status"_attr = reconfigStatus);
                    fassertFailedWithStatus(31477, reconfigStatus);
                }
            }
        }
        if (MONGO_unlikely(hangAfterReconfigOnDrainComplete.shouldFail())) {
            LOGV2(4508102, "Hanging due to hangAfterReconfigOnDrainComplete failpoint.");
            hangAfterReconfigOnDrainComplete.pauseWhileSet(opCtx);
        }

        AllowNonLocalWritesBlock writesAllowed(opCtx);
        OpTime firstOpTime = _externalState->onTransitionToPrimary(opCtx);
        lk.lock();

        auto status = _topCoord->completeTransitionToPrimary(firstOpTime);
        if (status.code() == ErrorCodes::PrimarySteppedDown) {
            LOGV2(21330, "Transition to primary failed{status}", "status"_attr = causedBy(status));
            return;
        }
        invariant(status);
    }

    // Must calculate the commit level again because firstOpTimeOfMyTerm wasn't set when we logged
    // our election in onTransitionToPrimary(), above.
    _updateLastCommittedOpTimeAndWallTime(lk);
    _wakeReadyWaiters(lk);

    // Update _canAcceptNonLocalWrites
    _updateMemberStateFromTopologyCoordinator(lk, opCtx);

    LOGV2_OPTIONS(21331,
                  {logv2::LogTag::kRS},
                  "transition to primary complete; database writes are now permitted");
    _drainFinishedCond.notify_all();
    _externalState->startNoopWriter(_getMyLastAppliedOpTime_inlock());
}

Status ReplicationCoordinatorImpl::waitForDrainFinish(Milliseconds timeout) {
    if (timeout < Milliseconds(0)) {
        return Status(ErrorCodes::BadValue, "Timeout duration cannot be negative");
    }

    stdx::unique_lock<Latch> lk(_mutex);
    auto pred = [this]() { return _applierState != ApplierState::Draining; };
    if (!_drainFinishedCond.wait_for(lk, timeout.toSystemDuration(), pred)) {
        return Status(ErrorCodes::ExceededTimeLimit,
                      "Timed out waiting to finish draining applier buffer");
    }

    return Status::OK();
}

void ReplicationCoordinatorImpl::signalUpstreamUpdater() {
    _externalState->forwardSlaveProgress();
}

void ReplicationCoordinatorImpl::setMyHeartbeatMessage(const std::string& msg) {
    stdx::unique_lock<Latch> lock(_mutex);
    _topCoord->setMyHeartbeatMessage(_replExecutor->now(), msg);
}

void ReplicationCoordinatorImpl::setMyLastAppliedOpTimeAndWallTimeForward(
    const OpTimeAndWallTime& opTimeAndWallTime, DataConsistency consistency) {
    // Update the global timestamp before setting the last applied opTime forward so the last
    // applied optime is never greater than the latest cluster time in the logical clock.
    const auto opTime = opTimeAndWallTime.opTime;
    _externalState->setGlobalTimestamp(getServiceContext(), opTime.getTimestamp());

    stdx::unique_lock<Latch> lock(_mutex);
    auto myLastAppliedOpTime = _getMyLastAppliedOpTime_inlock();
    if (opTime > myLastAppliedOpTime) {
        _setMyLastAppliedOpTimeAndWallTime(lock, opTimeAndWallTime, false, consistency);
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

        if (consistency == DataConsistency::Consistent &&
            _readWriteAbility->canAcceptNonLocalWrites(lock) && _rsConfig.getWriteMajority() == 1) {
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
    _setMyLastAppliedOpTimeAndWallTime(lock, opTimeAndWallTime, false, DataConsistency::Consistent);
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
    LOGV2_DEBUG(21332, 1, "resetting durable/applied optimes.");
    // Reset to uninitialized OpTime
    bool isRollbackAllowed = true;
    _setMyLastAppliedOpTimeAndWallTime(
        lk, OpTimeAndWallTime(), isRollbackAllowed, DataConsistency::Inconsistent);
    _setMyLastDurableOpTimeAndWallTime(lk, OpTimeAndWallTime(), isRollbackAllowed);
    _stableOpTimeCandidates.clear();
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

    _externalState->forwardSlaveProgress();  // Must do this outside _mutex
}

void ReplicationCoordinatorImpl::_setMyLastAppliedOpTimeAndWallTime(
    WithLock lk,
    const OpTimeAndWallTime& opTimeAndWallTime,
    bool isRollbackAllowed,
    DataConsistency consistency) {
    const auto opTime = opTimeAndWallTime.opTime;

    // The last applied opTime should never advance beyond the global timestamp (i.e. the latest
    // cluster time). Not enforced if the logical clock is disabled, e.g. for arbiters.
    dassert(!LogicalClock::get(getServiceContext())->isEnabled() ||
            _externalState->getGlobalTimestamp(getServiceContext()) >= opTime.getTimestamp());

    _topCoord->setMyLastAppliedOpTimeAndWallTime(
        opTimeAndWallTime, _replExecutor->now(), isRollbackAllowed);
    // If we are using applied times to calculate the commit level, update it now.
    if (!_rsConfig.getWriteConcernMajorityShouldJournal()) {
        _updateLastCommittedOpTimeAndWallTime(lk);
    }
    // No need to wake up replication waiters because there should not be any replication waiters
    // waiting on our own lastApplied.

    // Signal anyone waiting on optime changes.
    _opTimeWaiterList.setValueIf_inlock(
        [opTime](const OpTime& waitOpTime, const SharedWaiterHandle& waiter) {
            return waitOpTime <= opTime;
        },
        opTime);

    // Update the local snapshot before updating the stable timestamp on the storage engine. New
    // transactions reading from the local snapshot should start before the oldest timestamp is
    // advanced to avoid races.
    _externalState->updateLocalSnapshot(opTime);

    // Notify the oplog waiters after updating the local snapshot.
    signalOplogWaiters();

    if (opTime.isNull()) {
        return;
    }

    // Add the new applied optime to the list of stable optime candidates and then set the last
    // stable optime. Stable optimes are used to determine the last optime that it is safe to revert
    // the database to, in the event of a rollback via the 'recover to timestamp' method. If we are
    // setting our applied optime to a value that doesn't represent a consistent database state, we
    // should not add it to the set of stable optime candidates. For example, if we are in
    // RECOVERING after a rollback using the 'rollbackViaRefetch' algorithm, we will be inconsistent
    // until we reach the 'minValid' optime.
    if (consistency == DataConsistency::Consistent) {
        invariant(opTime.getTimestamp().getInc() > 0,
                  str::stream() << "Impossible optime received: " << opTime.toString());
        _stableOpTimeCandidates.insert(opTimeAndWallTime);
        // If we are lagged behind the commit optime, set a new stable timestamp here. When majority
        // read concern is disabled, the stable timestamp is set to lastApplied.
        if (opTime <= _topCoord->getLastCommittedOpTime() ||
            !serverGlobalParams.enableMajorityReadConcern) {
            _setStableTimestampForStorage(lk);
        }
    } else if (_getMemberState_inlock().startup2()) {
        // The oplog application phase of initial sync starts timestamping writes, causing
        // WiredTiger to pin this data in memory. Advancing the oldest timestamp in step with the
        // last applied optime here will permit WiredTiger to evict this data as it sees fit.
        _service->getStorageEngine()->setOldestTimestamp(opTime.getTimestamp());
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

OpTimeAndWallTime ReplicationCoordinatorImpl::getMyLastAppliedOpTimeAndWallTime() const {
    stdx::lock_guard<Latch> lock(_mutex);
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

Status ReplicationCoordinatorImpl::_waitUntilMajorityOpTime(mongo::OperationContext* opCtx,
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
                "targetOpTime"_attr = targetOpTime,
                "currentCommittedSnapshotOpTime"_attr =
                    _getCurrentCommittedSnapshotOpTime_inlock());

    LOGV2_DEBUG(21335,
                3,
                "waitUntilOpTime: waiting for a new snapshot until {deadline}",
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
    const bool isMajorityCommittedRead =
        readConcern.getLevel() == ReadConcernLevel::kMajorityReadConcern &&
        !readConcern.isSpeculativeMajority() && !opCtx->inMultiDocumentTransaction();

    if (isMajorityCommittedRead) {
        return _waitUntilMajorityOpTime(opCtx, targetOpTime, deadline);
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
        return _waitUntilMajorityOpTime(opCtx, targetOpTime);
    } else {
        return _waitUntilOpTime(opCtx, targetOpTime);
    }
}

Status ReplicationCoordinatorImpl::awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) {
    // Using an uninitialized term means that this optime will be compared to other optimes only by
    // its timestamp. This allows us to wait only on the timestamp of the commit point surpassing
    // this timestamp, without worrying about terms.
    OpTime waitOpTime(ts, OpTime::kUninitializedTerm);
    return _waitUntilMajorityOpTime(opCtx, waitOpTime);
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
    long long configVersion;
    const auto status = _setLastOptime(lock, update, &configVersion);
    return status;
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
    long long configVersion;
    const auto status = _setLastOptime(lock, update, &configVersion);
    return status;
}

Status ReplicationCoordinatorImpl::_setLastOptime(WithLock lk,
                                                  const UpdatePositionArgs::UpdateInfo& args,
                                                  long long* configVersion) {
    auto result = _topCoord->setLastOptime(args, _replExecutor->now(), configVersion);
    if (!result.isOK())
        return result.getStatus();
    const bool advancedOpTime = result.getValue();
    // Only update committed optime if the remote optimes increased.
    if (advancedOpTime) {
        _updateLastCommittedOpTimeAndWallTime(lk);
        // Wait up replication waiters on optime changes.
        _wakeReadyWaiters(lk, std::max(args.appliedOpTime, args.durableOpTime));
    }

    _cancelAndRescheduleLivenessUpdate_inlock(args.memberId);
    return Status::OK();
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
    } else if (commitQuorum.mode == CommitQuorumOptions::kAll) {
        patternName = ReplSetConfig::kAllWriteConcernModeName;
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
    if (writeConcern.wMode.empty()) {
        return _topCoord->haveNumNodesReachedOpTime(
            opTime, writeConcern.wNumNodes, useDurableOpTime);
    }
    StringData patternName;
    if (writeConcern.wMode == WriteConcernOptions::kMajority) {
        if (_externalState->snapshotsEnabled() && !gTestingSnapshotBehaviorInIsolation) {
            // Make sure we have a valid "committed" snapshot up to the needed optime.
            if (!_currentCommittedSnapshot) {
                return false;
            }

            // Wait for the "current" snapshot to advance to/past the opTime.
            const auto haveSnapshot = _currentCommittedSnapshot->opTime >= opTime;
            if (!haveSnapshot) {
                LOGV2_DEBUG(21337,
                            1,
                            "Required snapshot optime: {opTime} is not yet part of the current "
                            "'committed' snapshot: {currentCommittedSnapshotOpTime}",
                            "opTime"_attr = opTime,
                            "currentCommittedSnapshotOpTime"_attr =
                                _currentCommittedSnapshot->opTime);
                return false;
            }

            // Fallthrough to wait for "majority" write concern.
        }

        // Wait for all drop pending collections with drop optime before and at 'opTime' to be
        // removed from storage.
        if (auto dropOpTime = _externalState->getEarliestDropPendingOpTime()) {
            if (*dropOpTime <= opTime) {
                LOGV2_DEBUG(21338,
                            1,
                            "Unable to satisfy the requested majority write concern at "
                            "'committed' optime {opTime}. There are still drop pending collections "
                            "(earliest drop optime: {dropOpTime}) that have to be removed from "
                            "storage before we can "
                            "satisfy the write concern {writeConcern}",
                            "opTime"_attr = opTime,
                            "dropOpTime"_attr = *dropOpTime,
                            "writeConcern"_attr = writeConcern.toBSON());
                return false;
            }
        }

        // Continue and wait for replication to the majority (of voters).
        // *** Needed for J:True, writeConcernMajorityShouldJournal:False (appliedOpTime snapshot).
        patternName = ReplSetConfig::kMajorityWriteConcernModeName;
    } else {
        patternName = writeConcern.wMode;
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
    Timer timer;
    WriteConcernOptions fixedWriteConcern = populateUnsetWriteConcernOptionsSyncMode(writeConcern);

    // We should never wait for replication if we are holding any locks, because this can
    // potentially block for long time while doing network activity.
    invariant(!opCtx->lockState()->isLocked());

    auto interruptStatus = opCtx->checkForInterruptNoAssert();
    if (!interruptStatus.isOK()) {
        return {interruptStatus, duration_cast<Milliseconds>(timer.elapsed())};
    }

    const auto wTimeoutDate = [&]() -> const Date_t {
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

    if (getTestCommandsEnabled() && !status.isOK()) {
        stdx::lock_guard lock(_mutex);
        LOGV2(21339,
              "Replication failed for write concern: {writeConcern}, waiting for optime: {opTime}, "
              "opID: {opID}, progress: {progress}",
              "writeConcern"_attr = writeConcern.toBSON(),
              "opTime"_attr = opTime,
              "opID"_attr = opCtx->getOpID(),
              "progress"_attr = _getReplicationProgress(lock));
    }
    return {std::move(status), duration_cast<Milliseconds>(timer.elapsed())};
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
    bob.append("lastStateTransition", lastStateTransition);
    bob.appendNumber("userOpsKilled", userOpsKilled.get());
    bob.appendNumber("userOpsRunning", userOpsRunning.get());

    LOGV2(21340, "State transition ops metrics: {metrics}", "metrics"_attr = bob.obj());
}

std::shared_ptr<IsMasterResponse> ReplicationCoordinatorImpl::_makeIsMasterResponse(
    const StringData horizonString, WithLock lock) const {
    auto response = std::make_shared<IsMasterResponse>();
    invariant(getSettings().usingReplSets());
    _topCoord->fillIsMasterForReplSet(response, horizonString);

    OpTime lastOpTime = _getMyLastAppliedOpTime_inlock();
    response->setLastWrite(lastOpTime, lastOpTime.getTimestamp().getSecs());
    if (_currentCommittedSnapshot) {
        response->setLastMajorityWrite(_currentCommittedSnapshot->opTime,
                                       _currentCommittedSnapshot->opTime.getTimestamp().getSecs());
    }

    if (response->isMaster() && !_readWriteAbility->canAcceptNonLocalWrites(lock)) {
        // Report that we are secondary to ismaster callers until drain completes.
        response->setIsMaster(false);
        response->setIsSecondary(true);
    }

    if (_inShutdown) {
        response->setIsMaster(false);
        response->setIsSecondary(false);
    }
    return response;
}

SharedSemiFuture<ReplicationCoordinatorImpl::SharedIsMasterResponse>
ReplicationCoordinatorImpl::_getIsMasterResponseFuture(
    WithLock lk,
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion) const {

    const MemberState myState = _topCoord->getMemberState();
    if (!_rsConfig.isInitialized() || myState.removed()) {
        // It is possible the SplitHorizon mappings have not been initialized yet for a member
        // config. We also clear the horizon mappings for nodes that are no longer part of the
        // config.
        auto response = std::make_shared<IsMasterResponse>();
        response->setTopologyVersion(_topCoord->getTopologyVersion());
        response->markAsNoConfig();
        return SharedSemiFuture<SharedIsMasterResponse>(
            SharedIsMasterResponse(std::move(response)));
    }

    const auto& self = _rsConfig.getMemberAt(_selfIndex);
    // determineHorizon falls back to kDefaultHorizon if the server does not know of the given
    // horizon.
    const StringData horizonString = self.determineHorizon(horizonParams);
    if (!clientTopologyVersion) {
        // The client is not using awaitable isMaster so we respond immediately.
        return SharedSemiFuture<SharedIsMasterResponse>(
            SharedIsMasterResponse(_makeIsMasterResponse(horizonString, lk)));
    }

    // Each awaitable isMaster will wait on their specific horizon. We always expect horizonString
    // to exist in _horizonToPromiseMap.
    auto horizonIter = _horizonToPromiseMap.find(horizonString);
    invariant(horizonIter != end(_horizonToPromiseMap));
    SharedSemiFuture<std::shared_ptr<const IsMasterResponse>> future =
        horizonIter->second->getFuture();

    const TopologyVersion topologyVersion = _topCoord->getTopologyVersion();
    if (clientTopologyVersion->getProcessId() != topologyVersion.getProcessId()) {
        // Getting a different process id indicates that the server has restarted so we return
        // immediately with the updated process id.
        return SharedSemiFuture<SharedIsMasterResponse>(
            SharedIsMasterResponse(_makeIsMasterResponse(horizonString, lk)));
    }

    auto prevCounter = clientTopologyVersion->getCounter();
    auto topologyVersionCounter = topologyVersion.getCounter();
    uassert(31382,
            str::stream() << "Received a topology version with counter: " << prevCounter
                          << " which is greater than the server topology version counter: "
                          << topologyVersionCounter,
            prevCounter <= topologyVersionCounter);

    if (prevCounter < topologyVersionCounter) {
        // The received isMaster command contains a stale topology version so we respond
        // immediately with a more current topology version.
        return SharedSemiFuture<SharedIsMasterResponse>(
            SharedIsMasterResponse(_makeIsMasterResponse(horizonString, lk)));
    }

    return future;
}

SharedSemiFuture<ReplicationCoordinatorImpl::SharedIsMasterResponse>
ReplicationCoordinatorImpl::getIsMasterResponseFuture(
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion) const {
    stdx::lock_guard lk(_mutex);
    return _getIsMasterResponseFuture(lk, horizonParams, clientTopologyVersion);
}

std::shared_ptr<const IsMasterResponse> ReplicationCoordinatorImpl::awaitIsMasterResponse(
    OperationContext* opCtx,
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion,
    boost::optional<Date_t> deadline) const {
    stdx::unique_lock lk(_mutex);

    auto future = _getIsMasterResponseFuture(lk, horizonParams, clientTopologyVersion);
    if (future.isReady()) {
        return future.get();
    }

    // If clientTopologyVersion is not none, deadline must also be not none.
    invariant(deadline);
    const auto myState = _topCoord->getMemberState();
    invariant(_rsConfig.isInitialized() && !myState.removed());
    const auto& self = _rsConfig.getMemberAt(_selfIndex);
    const StringData horizonString = self.determineHorizon(horizonParams);
    const TopologyVersion topologyVersion = _topCoord->getTopologyVersion();
    lk.unlock();

    IsMasterMetrics::get(opCtx)->incrementNumAwaitingTopologyChanges();

    if (MONGO_unlikely(waitForIsMasterResponse.shouldFail())) {
        // Used in tests that wait for this failpoint to be entered before triggering a topology
        // change.
        LOGV2(31464, "waitForIsMasterResponse failpoint enabled.");
    }
    if (MONGO_unlikely(hangWhileWaitingForIsMasterResponse.shouldFail())) {
        LOGV2(21341, "Hanging due to hangWhileWaitingForIsMasterResponse failpoint.");
        hangWhileWaitingForIsMasterResponse.pauseWhileSet(opCtx);
    }

    // Wait for a topology change with timeout set to deadline.
    LOGV2_DEBUG(21342,
                1,
                "Waiting for an isMaster response from a topology change or until deadline: "
                "{deadline}. Current TopologyVersion counter is {topologyVersionCounter}",
                "deadline"_attr = deadline.get(),
                "topologyVersionCounter"_attr = topologyVersion.getCounter());
    auto statusWithIsMaster =
        futureGetNoThrowWithDeadline(opCtx, future, deadline.get(), opCtx->getTimeoutError());
    auto status = statusWithIsMaster.getStatus();

    if (status == ErrorCodes::ExceededTimeLimit) {
        // Return an IsMasterResponse with the current topology version on timeout when waiting for
        // a topology change.
        stdx::lock_guard lk(_mutex);
        IsMasterMetrics::get(opCtx)->decrementNumAwaitingTopologyChanges();
        return _makeIsMasterResponse(horizonString, lk);
    }

    // A topology change has happened so we return an IsMasterResponse with the updated
    // topology version.
    uassertStatusOK(status);
    return statusWithIsMaster.getValue();
}

OpTime ReplicationCoordinatorImpl::getLatestWriteOpTime(OperationContext* opCtx) const {
    ShouldNotConflictWithSecondaryBatchApplicationBlock noPBWMBlock(opCtx->lockState());
    Lock::GlobalLock globalLock(opCtx, MODE_IS);
    // Check if the node is primary after acquiring global IS lock.
    uassert(ErrorCodes::NotMaster,
            "Not primary so can't get latest write optime",
            canAcceptNonLocalWrites());
    auto oplog = LocalOplogInfo::get(opCtx)->getCollection();
    uassert(ErrorCodes::NamespaceNotFound, "oplog collection does not exist.", oplog);
    auto latestOplogTimestamp =
        uassertStatusOK(oplog->getRecordStore()->getLatestOplogTimestamp(opCtx));
    return OpTime(latestOplogTimestamp, getTerm());
}

HostAndPort ReplicationCoordinatorImpl::getCurrentPrimaryHostAndPort() const {
    stdx::lock_guard<Latch> lock(_mutex);
    auto primary = _topCoord->getCurrentPrimaryMember();
    return primary ? primary->getHostAndPort() : HostAndPort();
}

void ReplicationCoordinatorImpl::cancelCbkHandle(CallbackHandle activeHandle) {
    _replExecutor->cancel(activeHandle);
}

BSONObj ReplicationCoordinatorImpl::_runCmdOnSelfOnAlternativeClient(OperationContext* opCtx,
                                                                     const std::string& dbName,
                                                                     const BSONObj& cmdObj) {

    auto client = opCtx->getServiceContext()->makeClient("DBDirectClientCmd");
    // We want the command's opCtx that gets executed via DBDirectClient to be interruptible
    // so that we don't block state transitions. Callers of this function might run opCtx
    // in an uninterruptible mode. To be on safer side, run the command in AlternativeClientRegion,
    // to make sure that the command's opCtx is interruptible.
    AlternativeClientRegion acr(client);
    auto uniqueNewOpCtx = cc().makeOperationContext();
    {
        stdx::lock_guard<Client> lk(cc());
        cc().setSystemOperationKillable(lk);
    }

    DBDirectClient dbClient(uniqueNewOpCtx.get());
    const auto commandResponse = dbClient.runCommand(OpMsgRequest::fromDBAndBody(dbName, cmdObj));

    return commandResponse->getCommandReply();
}

BSONObj ReplicationCoordinatorImpl::runCmdOnPrimaryAndAwaitResponse(
    OperationContext* opCtx,
    const std::string& dbName,
    const BSONObj& cmdObj,
    OnRemoteCmdScheduledFn onRemoteCmdScheduled,
    OnRemoteCmdCompleteFn onRemoteCmdComplete) {
    // About to make network and DBDirectClient (recursive) calls, so we should not hold any locks.
    invariant(!opCtx->lockState()->isLocked());

    const auto myHostAndPort = getMyHostAndPort();
    const auto primaryHostAndPort = getCurrentPrimaryHostAndPort();

    if (myHostAndPort.empty()) {
        // Possibly because either rsconfig is uninitialized or the node got removed from config.
        uassertStatusOK(Status{ErrorCodes::NodeNotFound, "Address unknown."});
    }

    if (primaryHostAndPort.empty()) {
        uassertStatusOK(Status{ErrorCodes::NoConfigMaster, "Primary is unknown/down."});
    }

    auto iAmPrimary = (myHostAndPort == primaryHostAndPort) ? true : false;

    if (iAmPrimary) {
        // Run command using DBDirectClient to avoid tcp connection.
        return _runCmdOnSelfOnAlternativeClient(opCtx, dbName, cmdObj);
    }

    // Node is not primary, so we will run the remote command via AsyncDBClient. To use
    // AsyncDBClient, we will be using repl task executor.
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

    onRemoteCmdScheduled(cbkHandle);

    // Wait for the response in an interruptible mode.
    _replExecutor->wait(cbkHandle, opCtx);

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
        if (client->isFromSystemConnection() && !client->shouldKillSystemOperation(lk)) {
            continue;
        }

        OperationContext* toKill = client->getOperationContext();

        // Don't kill step up/step down thread.
        if (toKill && !toKill->isKillPending() && toKill->getOpID() != rstlOpCtx->getOpID()) {
            auto locker = toKill->lockState();
            if (locker->wasGlobalLockTakenInModeConflictingWithWrites() ||
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

    // Enqueues RSTL in X mode.
    _rstlLock.emplace(_opCtx, MODE_X, ReplicationStateTransitionLockGuard::EnqueueOnly());

    ON_BLOCK_EXIT([&] { _stopAndWaitForKillOpThread(); });
    _startKillOpThread();

    // Wait for RSTL to be acquired.
    _rstlLock->waitForLockUntil(deadline);
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
    uassert(ErrorCodes::NotMaster, "not primary so can't step down", getMemberState().primary());

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &stepdownHangBeforeRSTLEnqueue, opCtx, "stepdownHangBeforeRSTLEnqueue");

    // Using 'force' sets the default for the wait time to zero, which means the stepdown will
    // fail if it does not acquire the lock immediately. In such a scenario, we use the
    // stepDownUntil deadline instead.
    auto deadline = force ? stepDownUntil : waitUntil;
    AutoGetRstlForStepUpStepDown arsd(
        this, opCtx, ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown, deadline);

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
    auto action = _updateMemberStateFromTopologyCoordinator(lk, opCtx);
    invariant(action == PostMemberStateUpdateAction::kActionNone);
    invariant(!_readWriteAbility->canAcceptNonLocalWrites(lk));

    // Make sure that we leave _canAcceptNonLocalWrites in the proper state.
    auto updateMemberState = [&] {
        invariant(lk.owns_lock());
        invariant(opCtx->lockState()->isRSTLExclusive());

        auto action = _updateMemberStateFromTopologyCoordinator(lk, opCtx);
        lk.unlock();

        if (MONGO_unlikely(stepdownHangBeforePerformingPostMemberStateUpdateActions.shouldFail())) {
            LOGV2(21345,
                  "stepping down from primary - "
                  "stepdownHangBeforePerformingPostMemberStateUpdateActions fail point enabled. "
                  "Blocking until fail point is disabled.");
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
    auto onExitGuard = makeGuard([&] {
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
        LOGV2(21346, "Could not find node to hand off election to.");
        return;
    }

    auto target = _rsConfig.getMemberAt(candidateIndex).getHostAndPort();
    executor::RemoteCommandRequest request(
        target, "admin", BSON("replSetStepUp" << 1 << "skipDryRun" << true), nullptr);
    LOGV2(21347, "Handing off election to {target}", "target"_attr = target);

    auto callbackHandleSW = _replExecutor->scheduleRemoteCommand(
        request, [target](const executor::TaskExecutor::RemoteCommandCallbackArgs& callbackData) {
            auto status = callbackData.response.status;

            if (status.isOK()) {
                LOGV2_DEBUG(21348,
                            1,
                            "replSetStepUp request to {target} succeeded with response -- "
                            "{response}",
                            "target"_attr = target,
                            "response"_attr = callbackData.response.data);
            } else {
                LOGV2(21349,
                      "replSetStepUp request to {target} failed due to {status}",
                      "target"_attr = target,
                      "status"_attr = status);
            }
        });

    auto callbackHandleStatus = callbackHandleSW.getStatus();
    if (!callbackHandleStatus.isOK()) {
        LOGV2_ERROR(21417,
                    "Failed to schedule ReplSetStepUp request to {target} for election handoff: "
                    "{status}",
                    "target"_attr = target,
                    "status"_attr = callbackHandleStatus);
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

bool ReplicationCoordinatorImpl::isMasterForReportingPurposes() {
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
    invariant(opCtx->lockState()->isRSTLLocked());
    return canAcceptWritesForDatabase_UNSAFE(opCtx, dbName);
}

bool ReplicationCoordinatorImpl::canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                                                   StringData dbName) {
    // _canAcceptNonLocalWrites is always true for standalone nodes, and adjusted based on
    // primary+drain state in replica sets.
    //
    // Stand-alone nodes and drained replica set primaries can always accept writes.  Writes are
    // always permitted to the "local" database.
    if (_readWriteAbility->canAcceptNonLocalWrites_UNSAFE() || alwaysAllowNonLocalWrites(*opCtx)) {
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
            if (auto ns = CollectionCatalog::get(opCtx).lookupNSSByUUID(opCtx, *uuid)) {
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
    if (_readWriteAbility->canAcceptNonLocalWrites_UNSAFE() || alwaysAllowNonLocalWrites(*opCtx)) {
        return true;
    }

    if (auto ns = nsOrUUID.nss()) {
        if (!ns->isOplog()) {
            return true;
        }
    } else if (auto oplogCollection = LocalOplogInfo::get(opCtx)->getCollection()) {
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
                                                         bool slaveOk) {
    invariant(opCtx->lockState()->isRSTLLocked());
    return checkCanServeReadsFor_UNSAFE(opCtx, ns, slaveOk);
}

Status ReplicationCoordinatorImpl::checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                                const NamespaceString& ns,
                                                                bool slaveOk) {
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
            return Status{ErrorCodes::NotMasterOrSecondary,
                          "Oplog collection reads are not allowed while in the rollback or "
                          "startup state."};
        }
    }

    if (canAcceptWritesFor_UNSAFE(opCtx, ns)) {
        return Status::OK();
    }

    if (opCtx->inMultiDocumentTransaction()) {
        if (!_readWriteAbility->canAcceptNonLocalWrites_UNSAFE()) {
            return Status(ErrorCodes::NotMaster,
                          "Multi-document transactions are only allowed on replica set primaries.");
        }
    }

    if (slaveOk) {
        if (isPrimaryOrSecondary) {
            return Status::OK();
        }
        return Status(ErrorCodes::NotMasterOrSecondary,
                      "not master or secondary; cannot currently read from this replSet member");
    }
    return Status(ErrorCodes::NotMasterNoSlaveOk, "not master and slaveOk=false");
}

bool ReplicationCoordinatorImpl::isInPrimaryOrSecondaryState(OperationContext* opCtx) const {
    return _readWriteAbility->canServeNonLocalReads(opCtx);
}

bool ReplicationCoordinatorImpl::isInPrimaryOrSecondaryState_UNSAFE() const {
    return _readWriteAbility->canServeNonLocalReads_UNSAFE();
}

bool ReplicationCoordinatorImpl::shouldRelaxIndexConstraints(OperationContext* opCtx,
                                                             const NamespaceString& ns) {
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

Status ReplicationCoordinatorImpl::resyncData(OperationContext* opCtx, bool waitUntilCompleted) {
    _stopDataReplication(opCtx);
    auto finishedEvent = uassertStatusOK(_replExecutor->makeEvent());
    std::function<void()> f;
    if (waitUntilCompleted)
        f = [&finishedEvent, this]() { _replExecutor->signalEvent(finishedEvent); };

    {
        stdx::lock_guard<Latch> lk(_mutex);
        _resetMyLastOpTimes(lk);
    }
    // unlock before calling _startDataReplication().
    _startDataReplication(opCtx, f);
    if (waitUntilCompleted) {
        _replExecutor->waitForEvent(finishedEvent);
    }
    return Status::OK();
}

StatusWith<BSONObj> ReplicationCoordinatorImpl::prepareReplSetUpdatePositionCommand() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _topCoord->prepareReplSetUpdatePositionCommand(
        _getCurrentCommittedSnapshotOpTime_inlock());
}

Status ReplicationCoordinatorImpl::processReplSetGetStatus(
    BSONObjBuilder* response, ReplSetGetStatusResponseStyle responseStyle) {

    BSONObj initialSyncProgress;
    if (responseStyle == ReplSetGetStatusResponseStyle::kInitialSync) {
        std::shared_ptr<InitialSyncer> initialSyncerCopy;
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

    stdx::lock_guard<Latch> lk(_mutex);
    Status result(ErrorCodes::InternalError, "didn't set status in prepareStatusResponse");
    _topCoord->prepareStatusResponse(
        TopologyCoordinator::ReplSetStatusArgs{
            _replExecutor->now(),
            static_cast<unsigned>(time(nullptr) - serverGlobalParams.started),
            _getCurrentCommittedSnapshotOpTimeAndWallTime_inlock(),
            initialSyncProgress,
            electionCandidateMetrics,
            electionParticipantMetrics,
            _storage->getLastStableRecoveryTimestamp(_service),
            _externalState->tooStale()},
        response,
        &result);
    return result;
}

void ReplicationCoordinatorImpl::appendSlaveInfoData(BSONObjBuilder* result) {
    stdx::lock_guard<Latch> lock(_mutex);
    _topCoord->fillMemberData(result);
}

ReplSetConfig ReplicationCoordinatorImpl::getConfig() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _rsConfig;
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
                                                         bool commitmentStatus) {
    stdx::lock_guard<Latch> lock(_mutex);
    result->append("config", _rsConfig.toBSON());

    if (commitmentStatus) {
        uassert(ErrorCodes::NotMaster,
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

    {
        stdx::lock_guard<Latch> lock(_mutex);
        evh = _processReplSetMetadata_inlock(replMetadata);
    }

    if (evh) {
        _replExecutor->waitForEvent(evh);
    }
}

void ReplicationCoordinatorImpl::cancelAndRescheduleElectionTimeout() {
    stdx::lock_guard<Latch> lock(_mutex);
    _cancelAndRescheduleElectionTimeout_inlock();
}

EventHandle ReplicationCoordinatorImpl::_processReplSetMetadata_inlock(
    const rpc::ReplSetMetadata& replMetadata) {
    // If we're in FCV 4.4, allow metadata updates between config versions.
    if (!serverGlobalParams.featureCompatibility.isVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) ||
        !enableSafeReplicaSetReconfig) {
        if (replMetadata.getConfigVersion() != _rsConfig.getConfigVersion()) {
            return EventHandle();
        }
    }
    return _updateTerm_inlock(replMetadata.getTerm());
}

bool ReplicationCoordinatorImpl::getMaintenanceMode() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _topCoord->getMaintenanceCount() > 0;
}

Status ReplicationCoordinatorImpl::setMaintenanceMode(bool activate) {
    if (getReplicationMode() != modeReplSet) {
        return Status(ErrorCodes::NoReplicationEnabled,
                      "can only set maintenance mode on replica set members");
    }

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
        LOGV2_OPTIONS(21350,
                      {logv2::LogTag::kRS},
                      "going into maintenance mode with {curMaintenanceCalls} other maintenance "
                      "mode tasks in progress",
                      "curMaintenanceCalls"_attr = curMaintenanceCalls);
        _topCoord->adjustMaintenanceCountBy(1);
    } else if (curMaintenanceCalls > 0) {
        invariant(_topCoord->getRole() == TopologyCoordinator::Role::kFollower);

        _topCoord->adjustMaintenanceCountBy(-1);

        LOGV2_OPTIONS(21351,
                      {logv2::LogTag::kRS},
                      "leaving maintenance mode ({curMaintenanceCalls} other maintenance mode "
                      "tasks ongoing)",
                      "curMaintenanceCalls"_attr = curMaintenanceCalls - 1);
    } else {
        LOGV2_WARNING(21411, "Attempted to leave maintenance mode but it is not currently active");
        return Status(ErrorCodes::OperationFailed, "already out of maintenance mode");
    }

    const PostMemberStateUpdateAction action =
        _updateMemberStateFromTopologyCoordinator(lk, nullptr);
    lk.unlock();
    _performPostMemberStateUpdateAction(action);
    return Status::OK();
}

Status ReplicationCoordinatorImpl::processReplSetSyncFrom(OperationContext* opCtx,
                                                          const HostAndPort& target,
                                                          BSONObjBuilder* resultObj) {
    Status result(ErrorCodes::InternalError, "didn't set status in prepareSyncFromResponse");
    std::shared_ptr<InitialSyncer> initialSyncerCopy;
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
          "newConfig"_attr = args.newConfigObj);

    auto getNewConfig = [&](const ReplSetConfig& oldConfig,
                            long long currentTerm) -> StatusWith<ReplSetConfig> {
        ReplSetConfig newConfig;

        // Only explicitly set configTerm for reconfig to this node's term if we're in FCV 4.4.
        // Otherwise, use -1.
        auto useSafeReconfig = serverGlobalParams.featureCompatibility.isVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44);
        useSafeReconfig = useSafeReconfig && enableSafeReplicaSetReconfig;
        auto term = (!args.force && useSafeReconfig) ? currentTerm : OpTime::kUninitializedTerm;

        // When initializing a new config through the replSetReconfig command, ignore the term
        // field passed in through its args. Instead, use this node's term.
        const Status status =
            newConfig.initialize(args.newConfigObj, term, oldConfig.getReplicaSetId());

        if (!status.isOK()) {
            LOGV2_ERROR(21418,
                        "replSetReconfig got {status} while parsing {newConfigObj}",
                        "status"_attr = status,
                        "newConfigObj"_attr = args.newConfigObj);
            return Status(ErrorCodes::InvalidReplicaSetConfig, status.reason());
        }

        if (newConfig.getReplSetName() != _settings.ourSetName()) {
            str::stream errmsg;
            errmsg << "Attempting to reconfigure a replica set with name "
                   << newConfig.getReplSetName() << ", but command line reports "
                   << _settings.ourSetName() << "; rejecting";
            LOGV2_ERROR(
                21419, "{std_string_errmsg}", "std_string_errmsg"_attr = std::string(errmsg));
            return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
        }

        if (enableAutomaticReconfig) {
            bool addedNewlyAddedField = false;

            // Set the 'newlyAdded' field to true for all new voting nodes.
            for (int i = 0; i < newConfig.getNumMembers(); i++) {
                const auto newMem = newConfig.getMemberAt(i);

                // If this is a safe reconfig, the 'newlyAdded' flag should never already be set for
                // this member. If it is set, throw an error.
                if (!args.force && newMem.isNewlyAdded()) {
                    str::stream errmsg;
                    errmsg << "Cannot provide " << MemberConfig::kNewlyAddedFieldName
                           << " field to member config during safe reconfig.";
                    LOGV2_ERROR(
                        4634900,
                        "Initializing 'newlyAdded' field to member has failed with bad status.",
                        "errmsg"_attr = std::string(errmsg));
                    return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
                }

                const int newMemId = newMem.getId().getData();
                const auto oldMem = oldConfig.findMemberByID(newMemId);

                const bool isNewVotingMember = (oldMem == nullptr && newMem.isVoter());
                const bool isCurrentlyNewlyAdded = (oldMem != nullptr && oldMem->isNewlyAdded());

                // Append the 'newlyAdded' field if the node:
                // 1) Is a new, voting node
                // 2) Already has a 'newlyAdded' field in the old config
                if (isNewVotingMember || isCurrentlyNewlyAdded) {
                    newConfig.setNewlyAddedFieldForMemberAtIndex(i, true);
                    addedNewlyAddedField = true;
                }
            }

            if (addedNewlyAddedField) {
                LOGV2(4634400,
                      "Appended the 'newlyAdded' field to a node in the new config. Nodes with the "
                      "'newlyAdded' field will be considered to have 'votes:0'. Upon transition to "
                      "SECONDARY, this field will be automatically removed.",
                      "newConfigObj"_attr = newConfig.toBSON(),
                      "userProvidedConfig"_attr = args.newConfigObj,
                      "oldConfig"_attr = oldConfig.toBSON());
            }
        }

        // Increase the config version for force reconfig.
        if (args.force) {
            auto version = std::max(oldConfig.getConfigVersion(), newConfig.getConfigVersion());
            version += 10'000 + SecureRandom().nextInt32(100'000);
            newConfig.setConfigVersion(version);
        }
        return newConfig;
    };

    return doReplSetReconfig(opCtx, getNewConfig, args.force);
}

Status ReplicationCoordinatorImpl::doReplSetReconfig(OperationContext* opCtx,
                                                     GetNewConfigFn getNewConfig,
                                                     bool force) {
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
                false);  // should be unreachable due to !_settings.usingReplSets() check above
        case kConfigInitiating:
        case kConfigReconfiguring:
        case kConfigHBReconfiguring:
            return Status(ErrorCodes::ConfigurationInProgress,
                          "Cannot run replSetReconfig because the node is currently updating "
                          "its configuration");
        default:
            LOGV2_FATAL(21432,
                        "Unexpected _rsConfigState {_rsConfigState}",
                        "_rsConfigState"_attr = int(_rsConfigState));
            fassertFailed(18914);
    }

    invariant(_rsConfig.isInitialized());

    if (!force && !_getMemberState_inlock().primary()) {
        return Status(ErrorCodes::NotMaster,
                      str::stream()
                          << "replSetReconfig should only be run on PRIMARY, but my state is "
                          << _getMemberState_inlock().toString()
                          << "; use the \"force\" argument to override");
    }
    auto topCoordTerm = _topCoord->getTerm();

    auto configWriteConcern = _getConfigReplicationWriteConcern();
    // Construct a fake OpTime that can be accepted but isn't used.
    OpTime fakeOpTime(Timestamp(1, 1), topCoordTerm);

    if (serverGlobalParams.featureCompatibility.isVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) &&
        !force && enableSafeReplicaSetReconfig) {
        if (!_doneWaitingForReplication_inlock(fakeOpTime, configWriteConcern)) {
            return Status(ErrorCodes::ConfigurationInProgress,
                          str::stream()
                              << "Cannot run replSetReconfig because the current config: "
                              << _rsConfig.getConfigVersionAndTerm().toString() << " is not "
                              << "majority committed.");
        }

        // Make sure that the latest committed optime from the previous config is committed in the
        // current config. If this is the initial reconfig, then we don't need to check this
        // condition, since there were no prior configs. Also, for force reconfigs we bypass this
        // safety check condition. In any FCV < 4.4 we also bypass it to preserve client facing
        // behavior in mixed version sets.
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
                  "configOplogCommitmentOpTime"_attr = configOplogCommitmentOpTime);
            return Status(ErrorCodes::ConfigurationInProgress,
                          str::stream() << "Last committed optime from previous config ("
                                        << configOplogCommitmentOpTime.toString()
                                        << ") is not committed in the current config.");
        }
    }

    _setConfigState_inlock(kConfigReconfiguring);
    auto configStateGuard =
        makeGuard([&] { lockAndCall(&lk, [=] { _setConfigState_inlock(kConfigSteady); }); });

    ReplSetConfig oldConfig = _rsConfig;
    lk.unlock();

    // Call the callback to get the new config given the old one.
    auto newConfigStatus = getNewConfig(oldConfig, topCoordTerm);
    Status status = newConfigStatus.getStatus();
    if (!status.isOK())
        return status;
    ReplSetConfig newConfig = newConfigStatus.getValue();

    BSONObj oldConfigObj = oldConfig.toBSON();
    BSONObj newConfigObj = newConfig.toBSON();
    audit::logReplSetReconfig(opCtx->getClient(), &oldConfigObj, &newConfigObj);

    StatusWith<int> myIndex = validateConfigForReconfig(
        _externalState.get(), oldConfig, newConfig, opCtx->getServiceContext(), force);
    if (!myIndex.isOK()) {
        LOGV2_ERROR(21420,
                    "replSetReconfig got {status} while validating {newConfigObj}",
                    "status"_attr = myIndex.getStatus(),
                    "newConfigObj"_attr = newConfigObj);
        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      myIndex.getStatus().reason());
    }

    LOGV2(21353,
          "replSetReconfig config object with {numMembers} members parses ok",
          "numMembers"_attr = newConfig.getNumMembers());

    if (!force && !MONGO_unlikely(omitConfigQuorumCheck.shouldFail())) {
        status = checkQuorumForReconfig(
            _replExecutor.get(), newConfig, myIndex.getValue(), _topCoord->getTerm());
        if (!status.isOK()) {
            LOGV2_ERROR(21421, "replSetReconfig failed; {status}", "status"_attr = status);
            return status;
        }
    }

    LOGV2(51814, "Persisting new config to disk.");
    status = _externalState->storeLocalConfigDocument(opCtx, newConfig.toBSON());
    if (!status.isOK()) {
        LOGV2_ERROR(21422,
                    "replSetReconfig failed to store config document; {status}",
                    "status"_attr = status);
        return status;
    }

    configStateGuard.dismiss();
    _finishReplSetReconfig(opCtx, newConfig, force, myIndex.getValue());

    if (!force &&
        serverGlobalParams.featureCompatibility.isVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) &&
        enableSafeReplicaSetReconfig) {
        // Wait for the config document to be replicated to a majority of nodes in the new
        // config.
        StatusAndDuration configAwaitStatus =
            awaitReplication(opCtx, fakeOpTime, configWriteConcern);
        uassertStatusOK(configAwaitStatus.status);

        // Now that the new config has been persisted and installed in memory, wait for the latest
        // committed optime in the previous config to be committed in the newly installed config.
        // For force reconfigs we don't need to check this safety condition, and in any FCV < 4.4 we
        // also bypass this to preserve client facing behavior in mixed version sets. Note that even
        // if we have just left a force config via a non-force reconfig, we still want to wait for
        // this oplog commitment check, since a subsequent safe reconfig will check it as a
        // precondition.
        lk.lock();
        auto configOplogCommitmentOpTime = _topCoord->getConfigOplogCommitmentOpTime();
        auto oplogWriteConcern = _getOplogCommitmentWriteConcern(lk);
        lk.unlock();

        LOGV2(51815,
              "Waiting for the last committed optime in the previous config "
              "({configOplogCommitmentOpTime}) to be committed in the current config.",
              "configOplogCommitmentOpTime"_attr = configOplogCommitmentOpTime);
        StatusAndDuration oplogAwaitStatus =
            awaitReplication(opCtx, configOplogCommitmentOpTime, oplogWriteConcern);
        if (!oplogAwaitStatus.status.isOK()) {
            uasserted(oplogAwaitStatus.status.code(),
                      str::stream() << "Last committed optime in the previous config ("
                                    << configOplogCommitmentOpTime.toString()
                                    << ") did not become committed in the current config.");
        }

        LOGV2(4508701,
              "Committed new replica set config",
              "newConfigVersion"_attr = newConfig.getConfigVersion(),
              "newConfigTerm"_attr = newConfig.getConfigTerm(),
              "configWaitDuration"_attr = configAwaitStatus.duration,
              "oplogWaitDuration"_attr = oplogAwaitStatus.duration,
              "configOplogCommitmentOpTime"_attr = configOplogCommitmentOpTime);
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
        electionFinishedEvent = _cancelElectionIfNeeded_inlock();
    }

    // If there is an election in-progress, there can be at most one. No new election can happen as
    // we have already set our ReplicationCoordinatorImpl::_rsConfigState state to
    // "kConfigReconfiguring" which prevents new elections from happening.
    if (electionFinishedEvent) {
        LOGV2_DEBUG(21354,
                    2,
                    "Waiting for election to complete before finishing reconfig to config with "
                    "{configVersionAndTerm}",
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
            LOGV2(21355, "stepping down from primary, because we received a new config");
            // We need to release the mutex before yielding locks for prepared transactions, which
            // might check out sessions, to avoid deadlocks with checked-out sessions accessing
            // this mutex.
            lk.unlock();

            yieldLocksForPreparedTransactions(opCtx);

            lk.lock();

            // Clear the node's election candidate metrics since it is no longer primary.
            ReplicationMetrics::get(opCtx).clearElectionCandidateMetrics();
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

    // On a reconfig we drop all snapshots so we don't mistakenly read from the wrong one.
    // For example, if we change the meaning of the "committed" snapshot from applied -> durable.
    //
    // If the new config has the same content but different version and term, skip it, since
    // the quorum condition is still the same.
    auto newConfigCopy = newConfig;
    newConfigCopy.setConfigTerm(oldConfig.getConfigTerm());
    newConfigCopy.setConfigVersion(oldConfig.getConfigVersion());
    if (SimpleBSONObjComparator::kInstance.evaluate(oldConfig.toBSON() != newConfigCopy.toBSON())) {
        _dropAllSnapshots_inlock();
    }

    lk.unlock();
    _performPostMemberStateUpdateAction(action);

    // Inform the index builds coordinator of the replica set reconfig.
    IndexBuildsCoordinator::get(opCtx)->onReplicaSetReconfig();
}

Status ReplicationCoordinatorImpl::processReplSetInitiate(OperationContext* opCtx,
                                                          const BSONObj& configObj,
                                                          BSONObjBuilder* resultObj) {
    LOGV2(21356, "replSetInitiate admin command received from client");

    const auto replEnabled = _settings.usingReplSets();
    stdx::unique_lock<Latch> lk(_mutex);
    if (!replEnabled) {
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

    auto configStateGuard =
        makeGuard([&] { lockAndCall(&lk, [=] { _setConfigState_inlock(kConfigUninitialized); }); });

    // When writing our first oplog entry below, disable advancement of the stable timestamp so that
    // we don't set it before setting our initial data timestamp. We will set it after we set our
    // initialDataTimestamp. This will ensure we trigger an initial stable checkpoint properly.
    if (!serverGlobalParams.enableMajorityReadConcern) {
        _shouldSetStableTimestamp = false;
    }

    lk.unlock();

    ReplSetConfig newConfig;
    Status status = newConfig.initializeForInitiate(configObj);
    if (!status.isOK()) {
        LOGV2_ERROR(21423,
                    "replSet initiate got {status} while parsing {configObj}",
                    "status"_attr = status,
                    "configObj"_attr = configObj);
        return Status(ErrorCodes::InvalidReplicaSetConfig, status.reason());
    }
    if (newConfig.getReplSetName() != _settings.ourSetName()) {
        str::stream errmsg;
        errmsg << "Attempting to initiate a replica set with name " << newConfig.getReplSetName()
               << ", but command line reports " << _settings.ourSetName() << "; rejecting";
        LOGV2_ERROR(21424, "{errmsg}", "errmsg"_attr = std::string(errmsg));
        return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
    }

    StatusWith<int> myIndex =
        validateConfigForInitiate(_externalState.get(), newConfig, opCtx->getServiceContext());
    if (!myIndex.isOK()) {
        LOGV2_ERROR(21425,
                    "replSet initiate got {status} while validating {configObj}",
                    "status"_attr = myIndex.getStatus(),
                    "configObj"_attr = configObj);
        return Status(ErrorCodes::InvalidReplicaSetConfig, myIndex.getStatus().reason());
    }

    LOGV2(21357,
          "replSetInitiate config object with {numMembers} members parses ok",
          "numMembers"_attr = newConfig.getNumMembers());

    // In pv1, the TopologyCoordinator has not set the term yet. It will be set to kInitialTerm if
    // the initiate succeeds so we pass that here.
    status = checkQuorumForInitiate(
        _replExecutor.get(), newConfig, myIndex.getValue(), OpTime::kInitialTerm);

    if (!status.isOK()) {
        LOGV2_ERROR(21426, "replSetInitiate failed; {status}", "status"_attr = status);
        return status;
    }

    status = _externalState->initializeReplSetStorage(opCtx, newConfig.toBSON());
    if (!status.isOK()) {
        LOGV2_ERROR(21427,
                    "replSetInitiate failed to store config document or create the oplog; {status}",
                    "status"_attr = status);
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

    _finishReplSetInitiate(opCtx, newConfig, myIndex.getValue());

    // A configuration passed to replSetInitiate() with the current node as an arbiter
    // will fail validation with a "replSet initiate got ... while validating" reason.
    invariant(!newConfig.getMemberAt(myIndex.getValue()).isArbiter());
    _externalState->startThreads(_settings);
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
        _rsConfigState = newState;
        _rsConfigStateChange.notify_all();
    }
}

bool ReplicationCoordinatorImpl::_haveHorizonsChanged(const ReplSetConfig& oldConfig,
                                                      const ReplSetConfig& newConfig,
                                                      int oldIndex,
                                                      int newIndex) {
    if (oldIndex < 0 || newIndex < 0) {
        // It's possible for index to be -1 if we are performing a reconfig via heartbeat.
        return false;
    }
    const auto oldHorizonMappings = oldConfig.getMemberAt(oldIndex).getHorizonMappings();
    const auto newHorizonMappings = newConfig.getMemberAt(newIndex).getHorizonMappings();
    return oldHorizonMappings != newHorizonMappings;
}

void ReplicationCoordinatorImpl::_fulfillTopologyChangePromise(OperationContext* opCtx,
                                                               WithLock lock) {
    _topCoord->incrementTopologyVersion();
    _cachedTopologyVersionCounter.store(_topCoord->getTopologyVersion().getCounter());
    // Create an isMaster response for each horizon the server is knowledgeable about.
    for (auto iter = _horizonToPromiseMap.begin(); iter != _horizonToPromiseMap.end(); iter++) {
        auto response = _makeIsMasterResponse(iter->first, lock);
        // Fulfill the promise and replace with a new one for future waiters.
        iter->second->emplaceValue(response);
        iter->second = std::make_shared<SharedPromise<std::shared_ptr<const IsMasterResponse>>>();
    }
}

void ReplicationCoordinatorImpl::incrementTopologyVersion(OperationContext* opCtx) {
    stdx::lock_guard lk(_mutex);
    _fulfillTopologyChangePromise(opCtx, lk);
}

ReplicationCoordinatorImpl::PostMemberStateUpdateAction
ReplicationCoordinatorImpl::_updateMemberStateFromTopologyCoordinator(WithLock lk,
                                                                      OperationContext* opCtx) {
    {
        // We have to do this check even if our current and target state are the same as we might
        // have just failed a stepdown attempt and thus are staying in PRIMARY state but restoring
        // our ability to accept writes.
        bool canAcceptWrites = _topCoord->canAcceptWrites();
        _readWriteAbility->setCanAcceptNonLocalWrites(lk, opCtx, canAcceptWrites);
    }

    // We want to respond to any waiting isMasters even if our current and target state are the
    // same as it is possible writes have been disabled during a stepDown but the primary has yet
    // to transition to SECONDARY state.
    ON_BLOCK_EXIT([&] {
        if (_rsConfig.isInitialized()) {
            _fulfillTopologyChangePromise(opCtx, lk);
            // Use the global ServiceContext here in case the current opCtx is null.
            IsMasterMetrics::get(getGlobalServiceContext())->resetNumAwaitingTopologyChanges();
        }
    });

    const MemberState newState = _topCoord->getMemberState();

    if (newState == _memberState) {
        if (_topCoord->getRole() == TopologyCoordinator::Role::kCandidate) {
            invariant(_rsConfig.getNumMembers() == 1 && _selfIndex == 0 &&
                      _rsConfig.getMemberAt(0).isElectable());
            // Start election in protocol version 1
            return kActionStartSingleNodeElection;
        }
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

        // _canAcceptNonLocalWrites should already be set above.
        invariant(!_readWriteAbility->canAcceptNonLocalWrites(lk));

        serverGlobalParams.validateFeaturesAsMaster.store(false);
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

    if (_memberState.secondary() && newState.rollback()) {
        // If we are switching out of SECONDARY and to ROLLBACK, we must make sure that we hold the
        // RSTL in mode X to prevent readers that have the RSTL in intent mode from reading.
        _readWriteAbility->setCanServeNonLocalReads(opCtx, 0U);
    } else if (_memberState.secondary() && !newState.primary()) {
        // Switching out of SECONDARY, but not to PRIMARY or ROLLBACK.
        _readWriteAbility->setCanServeNonLocalReads_UNSAFE(0U);
    } else if (!_memberState.primary() && newState.secondary()) {
        // Switching into SECONDARY, but not from PRIMARY.
        _readWriteAbility->setCanServeNonLocalReads_UNSAFE(1U);
    }

    if (newState.secondary() && _topCoord->getRole() == TopologyCoordinator::Role::kCandidate) {
        // When transitioning to SECONDARY, the only way for _topCoord to report the candidate
        // role is if the configuration represents a single-node replica set.  In that case, the
        // overriding requirement is to elect this singleton node primary.
        invariant(_rsConfig.getNumMembers() == 1 && _selfIndex == 0 &&
                  _rsConfig.getMemberAt(0).isElectable());
        // Start election in protocol version 1
        result = kActionStartSingleNodeElection;
    }

    if (newState.rollback()) {
        // When we start rollback, we need to drop all snapshots since we may need to create
        // out-of-order snapshots. This would be necessary even if the SnapshotName was completely
        // monotonically increasing because we don't necessarily have a snapshot of every write.
        // If we didn't drop all snapshots on rollback it could lead to the following situation:
        //
        //  |--------|-------------|-------------|
        //  | OpTime | HasSnapshot | Committed   |
        //  |--------|-------------|-------------|
        //  | (0, 1) | *           | *           |
        //  | (0, 2) | *           | ROLLED BACK |
        //  | (1, 2) |             | *           |
        //  |--------|-------------|-------------|
        //
        // When we try to make (1,2) the commit point, we'd find (0,2) as the newest snapshot
        // before the commit point, but it would be invalid to mark it as the committed snapshot
        // since it was never committed.
        _dropAllSnapshots_inlock();
    }

    // Upon transitioning out of ROLLBACK, we must clear any stable optime candidates that may have
    // been rolled back.
    if (_memberState.rollback()) {
        // Our 'lastApplied' optime at this point should be the rollback common point. We should
        // remove any stable optime candidates greater than the common point.
        auto lastApplied = _getMyLastAppliedOpTimeAndWallTime_inlock();
        // The upper bound will give us the first optime T such that T > lastApplied.
        auto deletePoint = _stableOpTimeCandidates.upper_bound(lastApplied);
        _stableOpTimeCandidates.erase(deletePoint, _stableOpTimeCandidates.end());

        // Ensure that no snapshots were created while we were in rollback.
        invariant(!_currentCommittedSnapshot);
    }

    // If we are transitioning from secondary, cancel any scheduled takeovers.
    if (_memberState.secondary()) {
        _cancelCatchupTakeover_inlock();
        _cancelPriorityTakeover_inlock();
    }

    LOGV2_OPTIONS(21358,
                  {logv2::LogTag::kRS},
                  "transition to {newState} from {memberState}",
                  "newState"_attr = newState,
                  "memberState"_attr = _memberState);
    // Initializes the featureCompatibilityVersion to the latest value, because arbiters do not
    // receive the replicated version. This is to avoid bugs like SERVER-32639.
    if (newState.arbiter()) {
        serverGlobalParams.featureCompatibility.setVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44);
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
        /* FALLTHROUGH */
        case kActionSteppedDown:
            // This code must be safe to run on node rollback and node removal!
            _externalState->shardingOnStepDownHook();
            _externalState->stopNoopWriter();
            _externalState->clearOplogVisibilityStateForStepDown();
            break;
        case kActionStartSingleNodeElection:
            // In protocol version 1, single node replset will run an election instead of
            // kActionWinElection as in protocol version 0.
            _startElectSelfV1(StartElectionReasonEnum::kElectionTimeout);
            break;
        default:
            LOGV2_FATAL(21433,
                        "Unknown post member state update action {action}",
                        "action"_attr = static_cast<int>(action));
            fassertFailed(26010);
    }
}

void ReplicationCoordinatorImpl::_postWonElectionUpdateMemberState(WithLock lk) {
    invariant(_topCoord->getTerm() != OpTime::kUninitializedTerm);
    _electionId = OID::fromTerm(_topCoord->getTerm());
    auto ts = LogicalClock::get(getServiceContext())->reserveTicks(1).asTimestamp();
    _topCoord->processWinElection(_electionId, ts);
    const PostMemberStateUpdateAction nextAction =
        _updateMemberStateFromTopologyCoordinator(lk, nullptr);

    invariant(nextAction == kActionFollowerModeStateChange,
              str::stream() << "nextAction == " << static_cast<int>(nextAction));
    invariant(_getMemberState_inlock().primary());
    // Clear the sync source.
    _onFollowerModeStateChange();
    // Notify all secondaries of the election win.
    _restartHeartbeats_inlock();
    invariant(!_catchupState);
    _catchupState = std::make_unique<CatchupState>(this);
    _catchupState->start_inlock();
}

void ReplicationCoordinatorImpl::_onFollowerModeStateChange() {
    _externalState->signalApplierToChooseNewSyncSource();
}

void ReplicationCoordinatorImpl::CatchupState::start_inlock() {
    LOGV2(21359, "Entering primary catch-up mode.");

    // Reset the number of catchup operations performed before starting catchup.
    _numCatchUpOps = 0;

    // No catchup in single node replica set.
    if (_repl->_rsConfig.getNumMembers() == 1) {
        abort_inlock(PrimaryCatchUpConclusionReason::kSkipped);
        return;
    }

    auto catchupTimeout = _repl->_rsConfig.getCatchUpTimeoutPeriod();

    // When catchUpTimeoutMillis is 0, we skip doing catchup entirely.
    if (catchupTimeout == ReplSetConfig::kCatchUpDisabled) {
        LOGV2(21360, "Skipping primary catchup since the catchup timeout is 0.");
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
        LOGV2(21361, "Catchup timed out after becoming primary.");
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
        LOGV2(21362, "Failed to schedule catchup timeout work.");
        abort_inlock(PrimaryCatchUpConclusionReason::kFailedWithError);
        return;
    }
    _timeoutCbh = status.getValue();
}

void ReplicationCoordinatorImpl::CatchupState::abort_inlock(PrimaryCatchUpConclusionReason reason) {
    invariant(_repl->_getMemberState_inlock().primary());

    ReplicationMetrics::get(getGlobalServiceContext())
        .incrementNumCatchUpsConcludedForReason(reason);

    LOGV2(21363, "Exited primary catch-up mode.");
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
        return;
    }

    // We've caught up.
    const auto myLastApplied = _repl->_getMyLastAppliedOpTime_inlock();
    if (*targetOpTime <= myLastApplied) {
        LOGV2(21364,
              "Caught up to the latest optime known via heartbeats after becoming primary. Target "
              "optime: {targetOpTime}. My Last Applied: {myLastApplied}",
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
          "targetOpTime"_attr = _targetOpTime);
    LOGV2(21366, "Latest known optime per replica set member:");
    auto opTimesPerMember = _repl->_topCoord->latestKnownOpTimeSinceHeartbeatRestartPerMember();
    for (auto&& pair : opTimesPerMember) {
        LOGV2(21367,
              "Member ID: {memberId}, latest known optime: {latestKnownOpTime}",
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

    // updateConfig() can change terms, so update our term shadow to match.
    _termShadow.store(_topCoord->getTerm());

    const ReplSetConfig oldConfig = _rsConfig;
    _rsConfig = newConfig;
    _protVersion.store(_rsConfig.getProtocolVersion());

    // Warn if running --nojournal and writeConcernMajorityJournalDefault = true
    StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    if (storageEngine && !storageEngine->isDurable() &&
        (newConfig.getWriteConcernMajorityShouldJournal() &&
         (!oldConfig.isInitialized() || !oldConfig.getWriteConcernMajorityShouldJournal()))) {
        LOGV2_OPTIONS(21369, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(
            21370,
            {logv2::LogTag::kStartupWarnings},
            "** WARNING: This replica set node is running without journaling enabled but the ");
        LOGV2_OPTIONS(
            21371,
            {logv2::LogTag::kStartupWarnings},
            "**          writeConcernMajorityJournalDefault option to the replica set config ");
        LOGV2_OPTIONS(21372,
                      {logv2::LogTag::kStartupWarnings},
                      "**          is set to true. The writeConcernMajorityJournalDefault ");
        LOGV2_OPTIONS(21373,
                      {logv2::LogTag::kStartupWarnings},
                      "**          option to the replica set config must be set to false ");
        LOGV2_OPTIONS(21374,
                      {logv2::LogTag::kStartupWarnings},
                      "**          or w:majority write concerns will never complete.");
        LOGV2_OPTIONS(
            21375,
            {logv2::LogTag::kStartupWarnings},
            "**          In addition, this node's memory consumption may increase until all");
        LOGV2_OPTIONS(21376,
                      {logv2::LogTag::kStartupWarnings},
                      "**          available free RAM is exhausted.");
        LOGV2_OPTIONS(21377, {logv2::LogTag::kStartupWarnings}, "");
    }

    // Warn if using the in-memory (ephemeral) storage engine with
    // writeConcernMajorityJournalDefault = true
    if (storageEngine && storageEngine->isEphemeral() &&
        (newConfig.getWriteConcernMajorityShouldJournal() &&
         (!oldConfig.isInitialized() || !oldConfig.getWriteConcernMajorityShouldJournal()))) {
        LOGV2_OPTIONS(21378, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(
            21379,
            {logv2::LogTag::kStartupWarnings},
            "** WARNING: This replica set node is using in-memory (ephemeral) storage with the");
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

    // Since the ReplSetConfig always has a WriteConcernOptions, the only way to know if it has been
    // customized is if it's different to the implicit defaults of { w: 1, wtimeout: 0 }.
    if (const auto& wc = newConfig.getDefaultWriteConcern();
        !(wc.wNumNodes == 1 && wc.wTimeout == 0)) {
        LOGV2_OPTIONS(21387, {logv2::LogTag::kStartupWarnings}, "");
        LOGV2_OPTIONS(21388,
                      {logv2::LogTag::kStartupWarnings},
                      "** WARNING: Replica set config contains customized getLastErrorDefaults,");
        LOGV2_OPTIONS(21389,
                      {logv2::LogTag::kStartupWarnings},
                      "**          which are deprecated. Use setDefaultRWConcern instead to set a");
        LOGV2_OPTIONS(21390,
                      {logv2::LogTag::kStartupWarnings},
                      "**          cluster-wide default writeConcern.");
        LOGV2_OPTIONS(21391, {logv2::LogTag::kStartupWarnings}, "");
    }

    const bool horizonsChanged = _haveHorizonsChanged(oldConfig, newConfig, _selfIndex, myIndex);

    LOGV2_OPTIONS(21392,
                  {logv2::LogTag::kRS},
                  "New replica set config in use: {rsConfig}",
                  "rsConfig"_attr = _rsConfig.toBSON());
    _selfIndex = myIndex;
    if (_selfIndex >= 0) {
        LOGV2(21393,
              "This node is {hostAndPort} in the config",
              "hostAndPort"_attr = _rsConfig.getMemberAt(_selfIndex).getHostAndPort());
    } else {
        LOGV2(21394, "This node is not a member of the config");
    }

    if (horizonsChanged) {
        for (auto iter = _horizonToPromiseMap.begin(); iter != _horizonToPromiseMap.end(); iter++) {
            iter->second->setError({ErrorCodes::SplitHorizonChange,
                                    "Received a reconfig that changed the horizon parameters."});
            IsMasterMetrics::get(opCtx)->resetNumAwaitingTopologyChanges();
        }
        if (_selfIndex >= 0) {
            // Only create a new horizon promise mapping if the node exists in the new config.
            _createHorizonTopologyChangePromiseMapping(lk);
        }
    }

    // Wake up writeConcern waiters that are no longer satisfiable due to the rsConfig change.
    _replicationWaiterList.setValueIf_inlock(
        [this](const OpTime& opTime, const SharedWaiterHandle& waiter) {
            invariant(waiter->writeConcern);
            // This throws if a waiter's writeConcern is no longer satisfiable, in which case
            // setValueIf_inlock will fulfill the waiter's promise with the error status.
            uassertStatusOK(_checkIfWriteConcernCanBeSatisfied_inlock(waiter->writeConcern.get()));
            // Return false meaning that the waiter is still satisfiable and thus can remain in the
            // waiter list.
            return false;
        });

    _cancelCatchupTakeover_inlock();
    _cancelPriorityTakeover_inlock();
    _cancelAndRescheduleElectionTimeout_inlock();

    const PostMemberStateUpdateAction action = _updateMemberStateFromTopologyCoordinator(lk, opCtx);
    if (_selfIndex >= 0) {
        // Don't send heartbeats if we're not in the config, if we get re-added one of the
        // nodes in the set will contact us.
        _startHeartbeats_inlock();

        if (_horizonToPromiseMap.empty()) {
            // We should only create a new horizon-to-promise mapping for nodes that are members of
            // the config.
            _createHorizonTopologyChangePromiseMapping(lk);
        }
    } else {
        // Clear the horizon promise mappings of removed nodes so they can be recreated if the node
        // later rejoins the set.
        _horizonToPromiseMap.clear();
    }

    _updateLastCommittedOpTimeAndWallTime(lk);
    _wakeReadyWaiters(lk);

    return action;
}

void ReplicationCoordinatorImpl::_wakeReadyWaiters(WithLock lk, boost::optional<OpTime> opTime) {
    _replicationWaiterList.setValueIf_inlock(
        [this](const OpTime& opTime, const SharedWaiterHandle& waiter) {
            invariant(waiter->writeConcern);
            return _doneWaitingForReplication_inlock(opTime, waiter->writeConcern.get());
        },
        opTime);
}

Status ReplicationCoordinatorImpl::processReplSetUpdatePosition(const UpdatePositionArgs& updates,
                                                                long long* configVersion) {
    stdx::unique_lock<Latch> lock(_mutex);
    Status status = Status::OK();
    bool somethingChanged = false;
    for (UpdatePositionArgs::UpdateIterator update = updates.updatesBegin();
         update != updates.updatesEnd();
         ++update) {
        status = _setLastOptime(lock, *update, configVersion);
        if (!status.isOK()) {
            break;
        }
        somethingChanged = true;
    }

    if (somethingChanged && !_getMemberState_inlock().primary()) {
        lock.unlock();
        // Must do this outside _mutex
        _externalState->forwardSlaveProgress();
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

std::vector<HostAndPort> ReplicationCoordinatorImpl::getOtherNodesInReplSet() const {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_settings.usingReplSets());

    std::vector<HostAndPort> nodes;
    if (_selfIndex == -1) {
        return nodes;
    }

    for (int i = 0; i < _rsConfig.getNumMembers(); ++i) {
        if (i == _selfIndex)
            continue;

        nodes.push_back(_rsConfig.getMemberAt(i).getHostAndPort());
    }
    return nodes;
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
    bool commitQuorumCanBeSatisfied = _topCoord->checkIfCommitQuorumCanBeSatisfied(commitQuorum);
    if (!commitQuorumCanBeSatisfied) {
        return Status(ErrorCodes::UnsatisfiableCommitQuorum,
                      str::stream() << "Commit quorum cannot be satisfied with the current replica "
                                    << "set configuration");
    }
    return Status::OK();
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

HostAndPort ReplicationCoordinatorImpl::chooseNewSyncSource(const OpTime& lastOpTimeFetched) {
    stdx::lock_guard<Latch> lk(_mutex);

    HostAndPort oldSyncSource = _topCoord->getSyncSourceAddress();
    // Always allow chaining while in catchup and drain mode.
    auto chainingPreference = _getMemberState_inlock().primary()
        ? TopologyCoordinator::ChainingPreference::kAllowChaining
        : TopologyCoordinator::ChainingPreference::kUseConfiguration;
    HostAndPort newSyncSource =
        _topCoord->chooseNewSyncSource(_replExecutor->now(), lastOpTimeFetched, chainingPreference);

    // If we lost our sync source, schedule new heartbeats immediately to update our knowledge
    // of other members's state, allowing us to make informed sync source decisions.
    if (newSyncSource.empty() && !oldSyncSource.empty() && _selfIndex >= 0 &&
        !_getMemberState_inlock().primary()) {
        _restartHeartbeats_inlock();
    }

    return newSyncSource;
}

void ReplicationCoordinatorImpl::_unblacklistSyncSource(
    const executor::TaskExecutor::CallbackArgs& cbData, const HostAndPort& host) {
    if (cbData.status == ErrorCodes::CallbackCanceled)
        return;

    stdx::lock_guard<Latch> lock(_mutex);
    _topCoord->unblacklistSyncSource(host, _replExecutor->now());
}

void ReplicationCoordinatorImpl::blacklistSyncSource(const HostAndPort& host, Date_t until) {
    stdx::lock_guard<Latch> lock(_mutex);
    _topCoord->blacklistSyncSource(host, until);
    _scheduleWorkAt(until, [=](const executor::TaskExecutor::CallbackArgs& cbData) {
        _unblacklistSyncSource(cbData, host);
    });
}

void ReplicationCoordinatorImpl::resetLastOpTimesFromOplog(OperationContext* opCtx,
                                                           DataConsistency consistency) {
    auto lastOpTimeAndWallTimeStatus = _externalState->loadLastOpTimeAndWallTime(opCtx);
    OpTimeAndWallTime lastOpTimeAndWallTime = {OpTime(), Date_t()};
    if (!lastOpTimeAndWallTimeStatus.getStatus().isOK()) {
        LOGV2_WARNING(21412,
                      "Failed to load timestamp and/or wall clock time of most recently applied "
                      "operation; {status}",
                      "status"_attr = lastOpTimeAndWallTimeStatus.getStatus());
    } else {
        lastOpTimeAndWallTime = lastOpTimeAndWallTimeStatus.getValue();
    }

    // Update the global timestamp before setting last applied opTime forward so the last applied
    // optime is never greater than the latest in-memory cluster time.
    _externalState->setGlobalTimestamp(opCtx->getServiceContext(),
                                       lastOpTimeAndWallTime.opTime.getTimestamp());

    stdx::unique_lock<Latch> lock(_mutex);
    bool isRollbackAllowed = true;
    _setMyLastAppliedOpTimeAndWallTime(lock, lastOpTimeAndWallTime, isRollbackAllowed, consistency);
    _setMyLastDurableOpTimeAndWallTime(lock, lastOpTimeAndWallTime, isRollbackAllowed);
    _reportUpstream_inlock(std::move(lock));
}

bool ReplicationCoordinatorImpl::shouldChangeSyncSource(
    const HostAndPort& currentSource,
    const rpc::ReplSetMetadata& replMetadata,
    boost::optional<rpc::OplogQueryMetadata> oqMetadata) {
    stdx::lock_guard<Latch> lock(_mutex);
    return _topCoord->shouldChangeSyncSource(
        currentSource, replMetadata, oqMetadata, _replExecutor->now());
}

void ReplicationCoordinatorImpl::_updateLastCommittedOpTimeAndWallTime(WithLock lk) {
    if (_topCoord->updateLastCommittedOpTimeAndWallTime()) {
        _setStableTimestampForStorage(lk);
    }
}

boost::optional<OpTimeAndWallTime> ReplicationCoordinatorImpl::_chooseStableOpTimeFromCandidates(
    WithLock lk,
    const std::set<OpTimeAndWallTime>& candidates,
    OpTimeAndWallTime maximumStableOpTime) {

    // No optime candidates.
    if (candidates.empty()) {
        return boost::none;
    }

    auto maximumStableTimestamp = maximumStableOpTime.opTime.getTimestamp();
    if (_readWriteAbility->canAcceptNonLocalWrites(lk) && _storage->supportsDocLocking(_service)) {
        // If the storage engine supports document level locking, then it is possible for oplog
        // writes to commit out of order. In that case, we don't want to set the stable timestamp
        // ahead of the all_durable timestamp. This is not a problem for oplog application
        // because we only set lastApplied between batches when the all_durable timestamp cannot
        // be behind. During oplog application the all_durable timestamp can jump around since
        // we first write oplog entries to the oplog and then go back and apply them.
        //
        // We must construct an upper bound for the stable optime candidates such that the upper
        // bound is at most 'maximumStableOpTime' and any candidate with a timestamp higher than the
        // all_durable is greater than the upper bound. If the timestamp of 'maximumStableOpTime'
        // is <= the all_durable, then we use 'maximumStableOpTime'. Otherwise, we construct an
        // optime using the all_durable and the term of 'maximumStableOpTime'. We must argue that
        // there are no stable optime candidates with a timestamp greater than the all_durable and
        // a term less than that of 'maximumStableOpTime'. Suppose there were. The
        // 'maximumStableOpTime' is either the commit point or the lastApplied, so the all_durable
        // can only be behind 'maximumStableOpTime' on a primary. If there is a candidate with a
        // higher timestamp than the all_durable but a lower term than 'maximumStableOpTime', then
        // the all_durable corresponds to a write in an earlier term than the current one. But
        // this is not possible on a primary, since on step-up, the primary storage commits a 'new
        // primary' oplog entry in the new term before accepting any new writes, so the all
        // durable must be in the current term.
        maximumStableTimestamp = std::min(_storage->getAllDurableTimestamp(_service),
                                          maximumStableOpTime.opTime.getTimestamp());
    }

    holdStableTimestampAtSpecificTimestamp.execute([&](const BSONObj& dataObj) {
        const auto holdStableTimestamp = dataObj["timestamp"].timestamp();
        maximumStableTimestamp = std::min(maximumStableTimestamp, holdStableTimestamp);
    });

    maximumStableOpTime = {OpTime(maximumStableTimestamp, maximumStableOpTime.opTime.getTerm()),
                           maximumStableOpTime.wallTime};

    // Find the greatest optime candidate that is less than or equal to 'maximumStableOpTime'. To do
    // this we first find the upper bound of 'maximumStableOpTime', which points to the smallest
    // element in 'candidates' that is greater than 'maximumStableOpTime'. We then step back one
    // element, which should give us the largest element in 'candidates' that is less than or equal
    // to the 'maximumStableOpTime'.
    auto upperBoundIter = candidates.upper_bound(maximumStableOpTime);

    // All optime candidates are greater than the commit point.
    if (upperBoundIter == candidates.begin()) {
        return boost::none;
    }
    // There is a valid stable optime.
    else {
        auto stableOpTime = *std::prev(upperBoundIter);
        invariant(stableOpTime.opTime.getTimestamp() <= maximumStableTimestamp);
        return stableOpTime;
    }
}

void ReplicationCoordinatorImpl::_cleanupStableOpTimeCandidates(
    std::set<OpTimeAndWallTime>* candidates, OpTimeAndWallTime stableOpTime) {
    // Discard optime candidates earlier than the current stable optime, since we don't need
    // them anymore. To do this, we find the lower bound of the 'stableOpTime' which is the first
    // element that is greater than or equal to the 'stableOpTime'. Then we discard everything up
    // to but not including this lower bound i.e. 'deletePoint'.
    auto deletePoint = candidates->lower_bound(stableOpTime);

    // Delete the entire range of unneeded optimes.
    candidates->erase(candidates->begin(), deletePoint);
}

boost::optional<OpTimeAndWallTime>
ReplicationCoordinatorImpl::chooseStableOpTimeFromCandidates_forTest(
    const std::set<OpTimeAndWallTime>& candidates, const OpTimeAndWallTime& maximumStableOpTime) {
    stdx::lock_guard<Latch> lk(_mutex);
    return _chooseStableOpTimeFromCandidates(lk, candidates, maximumStableOpTime);
}
void ReplicationCoordinatorImpl::cleanupStableOpTimeCandidates_forTest(
    std::set<OpTimeAndWallTime>* candidates, OpTimeAndWallTime stableOpTime) {
    _cleanupStableOpTimeCandidates(candidates, stableOpTime);
}

std::set<OpTimeAndWallTime> ReplicationCoordinatorImpl::getStableOpTimeCandidates_forTest() {
    stdx::unique_lock<Latch> lk(_mutex);
    return _stableOpTimeCandidates;
}

void ReplicationCoordinatorImpl::attemptToAdvanceStableTimestamp() {
    stdx::unique_lock<Latch> lk(_mutex);
    _setStableTimestampForStorage(lk);
}

boost::optional<OpTimeAndWallTime> ReplicationCoordinatorImpl::_recalculateStableOpTime(
    WithLock lk) {
    auto commitPoint = _topCoord->getLastCommittedOpTimeAndWallTime();
    if (_currentCommittedSnapshot) {
        auto snapshotOpTime = _currentCommittedSnapshot->opTime;
        invariant(snapshotOpTime.getTimestamp() <= commitPoint.opTime.getTimestamp());
        invariant(snapshotOpTime <= commitPoint.opTime);
    }

    // When majority read concern is disabled, the stable opTime is set to the lastApplied, rather
    // than the commit point.
    auto maximumStableOpTime = serverGlobalParams.enableMajorityReadConcern
        ? commitPoint
        : _topCoord->getMyLastAppliedOpTimeAndWallTime();

    // Compute the current stable optime.
    auto stableOpTime =
        _chooseStableOpTimeFromCandidates(lk, _stableOpTimeCandidates, maximumStableOpTime);
    if (stableOpTime) {
        // Check that the selected stable optime does not exceed our maximum.
        invariant(stableOpTime.get().opTime.getTimestamp() <=
                  maximumStableOpTime.opTime.getTimestamp());
        invariant(stableOpTime.get().opTime <= maximumStableOpTime.opTime);
    }

    return stableOpTime;
}

MONGO_FAIL_POINT_DEFINE(disableSnapshotting);

void ReplicationCoordinatorImpl::_setStableTimestampForStorage(WithLock lk) {
    if (!_shouldSetStableTimestamp) {
        LOGV2_DEBUG(21395, 2, "Not setting stable timestamp for storage.");
        return;
    }
    // Get the current stable optime.
    auto stableOpTime = _recalculateStableOpTime(lk);

    // If there is a valid stable optime, set it for the storage engine, and then remove any
    // old, unneeded stable optime candidates.
    if (stableOpTime) {
        LOGV2_DEBUG(21396,
                    2,
                    "Setting replication's stable optime to {stableOpTime}",
                    "stableOpTime"_attr = stableOpTime.value());

        if (!gTestingSnapshotBehaviorInIsolation) {
            // Update committed snapshot and wake up any threads waiting on read concern or
            // write concern.
            if (serverGlobalParams.enableMajorityReadConcern) {
                // When majority read concern is enabled, the committed snapshot is set to the new
                // stable optime.
                if (_updateCommittedSnapshot(lk, stableOpTime.value())) {
                    // Update the stable timestamp for the storage engine.
                    _storage->setStableTimestamp(getServiceContext(),
                                                 stableOpTime->opTime.getTimestamp());
                }
            } else {
                const auto lastCommittedOpTime = _topCoord->getLastCommittedOpTimeAndWallTime();
                if (!lastCommittedOpTime.opTime.isNull()) {
                    // When majority read concern is disabled, we set the stable timestamp to
                    // be less than or equal to the all committed timestamp. This makes sure that
                    // the committed snapshot is not past the all committed timestamp to guarantee
                    // we can always read our own majority committed writes. This problem is
                    // specific to the case where we have a single node replica set and the
                    // lastCommittedOpTime is set to be the lastApplied which can be ahead of the
                    // allCommitted.
                    auto newCommittedSnapshot = std::min(lastCommittedOpTime, *stableOpTime);
                    _updateCommittedSnapshot(lk, newCommittedSnapshot);
                }
                // Set the stable timestamp regardless of whether the majority commit point moved
                // forward. If we are in rollback state, however, do not alter the stable timestamp,
                // since it may be moved backwards explicitly by the rollback-via-refetch process.
                if (!MONGO_unlikely(disableSnapshotting.shouldFail()) && !_memberState.rollback()) {
                    _storage->setStableTimestamp(getServiceContext(),
                                                 stableOpTime->opTime.getTimestamp());
                }
            }
        }
        _cleanupStableOpTimeCandidates(&_stableOpTimeCandidates, stableOpTime.get());
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
                    "RECOVERING state. Current state: {state}",
                    "state"_attr = state.toString());
        return;
    }

    // Maintenance mode will force us to remain in RECOVERING state, no matter what.
    if (getMaintenanceMode()) {
        LOGV2_DEBUG(21398, 1, "We cannot transition to SECONDARY state while in maintenance mode.");
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
                    "minValid"_attr = minValid,
                    "lastApplied"_attr = lastApplied);
        return;
    }

    // Execute the transition to SECONDARY.
    auto status = setFollowerMode(MemberState::RS_SECONDARY);
    if (!status.isOK()) {
        LOGV2_WARNING(21413,
                      "Failed to transition into {MemberState_MemberState_RS_SECONDARY}. Current "
                      "state: {state}{status}",
                      "MemberState_MemberState_RS_SECONDARY"_attr =
                          MemberState(MemberState::RS_SECONDARY),
                      "state"_attr = getMemberState(),
                      "status"_attr = causedBy(status));
    }
}

void ReplicationCoordinatorImpl::advanceCommitPoint(
    const OpTimeAndWallTime& committedOpTimeAndWallTime, bool fromSyncSource) {
    stdx::unique_lock<Latch> lk(_mutex);
    _advanceCommitPoint(lk, committedOpTimeAndWallTime, fromSyncSource);
}

void ReplicationCoordinatorImpl::_advanceCommitPoint(
    WithLock lk, const OpTimeAndWallTime& committedOpTimeAndWallTime, bool fromSyncSource) {
    if (_topCoord->advanceLastCommittedOpTimeAndWallTime(committedOpTimeAndWallTime,
                                                         fromSyncSource)) {
        if (_getMemberState_inlock().arbiter()) {
            // Arbiters do not store replicated data, so we consider their data trivially
            // consistent.
            _setMyLastAppliedOpTimeAndWallTime(
                lk, committedOpTimeAndWallTime, false, DataConsistency::Consistent);
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

        _topCoord->processReplSetRequestVotes(args, response);
    }

    if (!args.isADryRun()) {
        const int candidateIndex = args.getCandidateIndex();
        LastVote lastVote{args.getTerm(), candidateIndex};

        const bool votedForCandidate = response->getVoteGranted();

        if (votedForCandidate) {
            Status status = _externalState->storeLocalLastVoteDocument(opCtx, lastVote);
            if (!status.isOK()) {
                LOGV2_ERROR(21428,
                            "replSetRequestVotes failed to store LastVote document; {status}",
                            "status"_attr = status);
                return status;
            }
        }

        // If the vote was not granted to the candidate, we still want to track metrics around the
        // node's participation in the election.
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

    stdx::lock_guard<Latch> lk(_mutex);

    if (hasReplSetMetadata) {
        _prepareReplSetMetadata_inlock(lastOpTimeFromClient, builder);
    }

    if (hasOplogQueryMetadata) {
        _prepareOplogQueryMetadata_inlock(rbid, builder);
    }
}

void ReplicationCoordinatorImpl::_prepareReplSetMetadata_inlock(const OpTime& lastOpTimeFromClient,
                                                                BSONObjBuilder* builder) const {
    OpTime lastVisibleOpTime =
        std::max(lastOpTimeFromClient, _getCurrentCommittedSnapshotOpTime_inlock());
    auto metadata = _topCoord->prepareReplSetMetadata(lastVisibleOpTime);
    metadata.writeToMetadata(builder).transitional_ignore();
}

void ReplicationCoordinatorImpl::_prepareOplogQueryMetadata_inlock(int rbid,
                                                                   BSONObjBuilder* builder) const {
    _topCoord->prepareOplogQueryMetadata(rbid).writeToMetadata(builder).transitional_ignore();
}

bool ReplicationCoordinatorImpl::getWriteConcernMajorityShouldJournal() {
    stdx::unique_lock lock(_mutex);
    return getWriteConcernMajorityShouldJournal_inlock();
}

bool ReplicationCoordinatorImpl::getWriteConcernMajorityShouldJournal_inlock() const {
    return _rsConfig.getWriteConcernMajorityShouldJournal();
}

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

    auto senderHost(args.getSenderHost());
    const Date_t now = _replExecutor->now();
    result = _topCoord->prepareHeartbeatResponseV1(now, args, _settings.ourSetName(), response);

    if ((result.isOK() || result == ErrorCodes::InvalidReplicaSetConfig) && _selfIndex < 0) {
        // If this node does not belong to the configuration it knows about, send heartbeats
        // back to any node that sends us a heartbeat, in case one of those remote nodes has
        // a configuration that contains us.  Chances are excellent that it will, since that
        // is the only reason for a remote node to send this node a heartbeat request.
        if (!senderHost.empty() && _seedList.insert(senderHost).second) {
            LOGV2(21400,
                  "Scheduling heartbeat to fetch a new config from: {senderHost} since we are not "
                  "a member of our current config.",
                  "senderHost"_attr = senderHost);
            _scheduleHeartbeatToTarget_inlock(senderHost, -1, now);
        }
    } else if (result.isOK() &&
               response->getConfigVersionAndTerm() < args.getConfigVersionAndTerm()) {
        // Schedule a heartbeat to the sender to fetch the new config.
        // Only send this if the sender's config is newer.
        // We cannot cancel the enqueued heartbeat, but either this one or the enqueued heartbeat
        // will trigger reconfig, which cancels and reschedules all heartbeats.
        if (args.hasSender()) {
            LOGV2(21401,
                  "Scheduling heartbeat to fetch a newer config with term {configTerm} and "
                  "version {configVersion} from member: {senderHost}",
                  "configTerm"_attr = args.getConfigTerm(),
                  "configVersion"_attr = args.getConfigVersion(),
                  "senderHost"_attr = senderHost);
            int senderIndex = _rsConfig.findMemberIndexByHostAndPort(senderHost);
            _scheduleHeartbeatToTarget_inlock(senderHost, senderIndex, now);
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
    TopologyCoordinator::UpdateTermResult updateTermResult;
    EventHandle finishEvh;

    {
        stdx::lock_guard<Latch> lock(_mutex);
        finishEvh = _updateTerm_inlock(term, &updateTermResult);
    }

    // Wait for potential stepdown to finish.
    if (finishEvh.isValid()) {
        _replExecutor->waitForEvent(finishEvh);
    }
    if (updateTermResult == TopologyCoordinator::UpdateTermResult::kUpdatedTerm ||
        updateTermResult == TopologyCoordinator::UpdateTermResult::kTriggerStepDown) {
        return {ErrorCodes::StaleTerm, "Replication term of this node was stale; retry query"};
    }

    return Status::OK();
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
            _currentCommittedSnapshot->opTime.getTimestamp() >= untilSnapshot;
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

bool ReplicationCoordinatorImpl::_updateCommittedSnapshot(
    WithLock lk, const OpTimeAndWallTime& newCommittedSnapshot) {
    if (gTestingSnapshotBehaviorInIsolation) {
        return false;
    }

    // If we are in ROLLBACK state, do not set any new _currentCommittedSnapshot, as it will be
    // cleared at the end of rollback anyway.
    if (_memberState.rollback()) {
        LOGV2(21404, "Not updating committed snapshot because we are in rollback");
        return false;
    }
    invariant(!newCommittedSnapshot.opTime.isNull());

    // The new committed snapshot should be <= the current replication commit point.
    OpTime lastCommittedOpTime = _topCoord->getLastCommittedOpTime();
    invariant(newCommittedSnapshot.opTime.getTimestamp() <= lastCommittedOpTime.getTimestamp());
    invariant(newCommittedSnapshot.opTime <= lastCommittedOpTime);

    // The new committed snapshot should be >= the current snapshot.
    if (_currentCommittedSnapshot) {
        invariant(newCommittedSnapshot.opTime.getTimestamp() >=
                  _currentCommittedSnapshot->opTime.getTimestamp());
        invariant(newCommittedSnapshot.opTime >= _currentCommittedSnapshot->opTime);
    }
    if (MONGO_unlikely(disableSnapshotting.shouldFail()))
        return false;
    _currentCommittedSnapshot = newCommittedSnapshot;
    _currentCommittedSnapshotCond.notify_all();

    _externalState->updateCommittedSnapshot(newCommittedSnapshot.opTime);

    // Wake up any threads waiting for read concern or write concern.
    if (_externalState->snapshotsEnabled() && _currentCommittedSnapshot) {
        _wakeReadyWaiters(lk, _currentCommittedSnapshot->opTime);
    }
    return true;
}

void ReplicationCoordinatorImpl::dropAllSnapshots() {
    stdx::lock_guard<Latch> lock(_mutex);
    _dropAllSnapshots_inlock();
}

void ReplicationCoordinatorImpl::_dropAllSnapshots_inlock() {
    _currentCommittedSnapshot = boost::none;
    _externalState->dropAllSnapshots();
}

void ReplicationCoordinatorImpl::waitForElectionFinish_forTest() {
    if (_electionFinishedEvent.isValid()) {
        _replExecutor->waitForEvent(_electionFinishedEvent);
    }
}

void ReplicationCoordinatorImpl::waitForElectionDryRunFinish_forTest() {
    if (_electionDryRunFinishedEvent.isValid()) {
        _replExecutor->waitForEvent(_electionDryRunFinishedEvent);
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
        if (writeConcern.wMode == WriteConcernOptions::kMajority &&
            getWriteConcernMajorityShouldJournal_inlock()) {
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
        finishEvent = _electionFinishedEvent;
    }
    if (finishEvent.isValid()) {
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

executor::TaskExecutor::EventHandle ReplicationCoordinatorImpl::_cancelElectionIfNeeded_inlock() {
    if (_topCoord->getRole() != TopologyCoordinator::Role::kCandidate) {
        return {};
    }
    invariant(_voteRequester);
    _voteRequester->cancel();
    return _electionFinishedEvent;
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
    if (canAcceptWrites == canAcceptNonLocalWrites(lk)) {
        return;
    }

    // We must be holding the RSTL in mode X to change _canAcceptNonLocalWrites.
    invariant(opCtx);
    if (opCtx->lockState()->isRSTLExclusive()) {
        _canAcceptNonLocalWrites.store(canAcceptWrites);
    }
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
    invariant(opCtx->lockState()->isRSTLLocked());
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

}  // namespace repl
}  // namespace mongo

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_coordinator_impl.h"

#include <algorithm>
#include <limits>

#include "mongo/base/status.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/check_quorum_for_config_change.h"
#include "mongo/db/repl/data_replicator_external_state_initial_sync.h"
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config_checks.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_set_html_summary.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/network_interface.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(stepdownHangBeforePerformingPostMemberStateUpdateActions);
MONGO_FAIL_POINT_DEFINE(transitionToPrimaryHangBeforeTakingGlobalExclusiveLock);

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

MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncAttempts, int, 10);

// Number of seconds between noop writer writes.
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(periodicNoopIntervalSecs, int, 10);

MONGO_INITIALIZER(periodicNoopIntervalSecs)(InitializerContext*) {
    if (periodicNoopIntervalSecs <= 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Periodic noop interval must be greater than 0 seconds: "
                                    << periodicNoopIntervalSecs);
    } else if (periodicNoopIntervalSecs > 10) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Periodic noop interval must be less than or equal to 10 seconds: "
                          << periodicNoopIntervalSecs);
    }
    return Status::OK();
}

/**
 * Allows non-local writes despite _canAcceptNonlocalWrites being false on a single OperationContext
 * while in scope.
 *
 * Resets to original value when leaving scope so it is safe to nest.
 */
class AllowNonLocalWritesBlock {
    MONGO_DISALLOW_COPYING(AllowNonLocalWritesBlock);

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

void lockAndCall(stdx::unique_lock<stdx::mutex>* lk, const stdx::function<void()>& fn) {
    if (!lk->owns_lock()) {
        lk->lock();
    }
    fn();
}

/**
 * Implements the force-reconfig behavior of incrementing config version by a large random
 * number.
 */
BSONObj incrementConfigVersionByRandom(BSONObj config) {
    BSONObjBuilder builder;
    for (BSONObjIterator iter(config); iter.more(); iter.next()) {
        BSONElement elem = *iter;
        if (elem.fieldNameStringData() == ReplSetConfig::kVersionFieldName && elem.isNumber()) {
            std::unique_ptr<SecureRandom> generator(SecureRandom::create());
            const int random = std::abs(static_cast<int>(generator->nextInt64()) % 100000);
            builder.appendIntOrLL(ReplSetConfig::kVersionFieldName,
                                  elem.numberLong() + 10000 + random);
        } else {
            builder.append(elem);
        }
    }
    return builder.obj();
}

// This is a special flag that allows for testing of snapshot behavior by skipping the replication
// related checks and isolating the storage/query side of snapshotting.
// SERVER-31304 rename this parameter to something more appropriate.
bool testingSnapshotBehaviorInIsolation = false;
ExportedServerParameter<bool, ServerParameterType::kStartupOnly> TestingSnapshotBehaviorInIsolation(
    ServerParameterSet::getGlobal(),
    "testingSnapshotBehaviorInIsolation",
    &testingSnapshotBehaviorInIsolation);

}  // namespace

ReplicationCoordinatorImpl::Waiter::Waiter(OpTime _opTime, const WriteConcernOptions* _writeConcern)
    : opTime(std::move(_opTime)), writeConcern(_writeConcern) {}

BSONObj ReplicationCoordinatorImpl::Waiter::toBSON() const {
    BSONObjBuilder bob;
    bob.append("opTime", opTime.toBSON());
    if (writeConcern) {
        bob.append("writeConcern", writeConcern->toBSON());
    }
    return bob.obj();
};

std::string ReplicationCoordinatorImpl::Waiter::toString() const {
    return toBSON().toString();
};


ReplicationCoordinatorImpl::ThreadWaiter::ThreadWaiter(OpTime _opTime,
                                                       const WriteConcernOptions* _writeConcern,
                                                       stdx::condition_variable* _condVar)
    : Waiter(_opTime, _writeConcern), condVar(_condVar) {}

void ReplicationCoordinatorImpl::ThreadWaiter::notify_inlock() {
    invariant(condVar);
    condVar->notify_all();
}

ReplicationCoordinatorImpl::CallbackWaiter::CallbackWaiter(OpTime _opTime,
                                                           FinishFunc _finishCallback)
    : Waiter(_opTime, nullptr), finishCallback(std::move(_finishCallback)) {}

void ReplicationCoordinatorImpl::CallbackWaiter::notify_inlock() {
    invariant(finishCallback);
    finishCallback();
}


class ReplicationCoordinatorImpl::WaiterGuard {
public:
    /**
     * Constructor takes the list of waiters and enqueues itself on the list, removing itself
     * in the destructor.
     *
     * Usually waiters will be signaled and removed when their criteria are satisfied, but
     * wait_until() with timeout may signal waiters earlier and this guard will remove the waiter
     * properly.
     *
     * _list is guarded by ReplicationCoordinatorImpl::_mutex, thus it is illegal to construct one
     * of these without holding _mutex
     */
    WaiterGuard(WaiterList* list, Waiter* waiter) : _list(list), _waiter(waiter) {
        list->add_inlock(_waiter);
    }

    ~WaiterGuard() {
        _list->remove_inlock(_waiter);
    }

private:
    WaiterList* _list;
    Waiter* _waiter;
};

void ReplicationCoordinatorImpl::WaiterList::add_inlock(WaiterType waiter) {
    _list.push_back(waiter);
}

void ReplicationCoordinatorImpl::WaiterList::signalAndRemoveIf_inlock(
    stdx::function<bool(WaiterType)> func) {
    // Only advance iterator when the element doesn't match.
    for (auto it = _list.begin(); it != _list.end();) {
        if (!func(*it)) {
            ++it;
            continue;
        }

        WaiterType waiter = std::move(*it);
        if (it == std::prev(_list.end())) {
            // Iterator will be invalid after erasing the last element, so set it to the
            // next one (i.e. end()).
            it = _list.erase(it);
        } else {
            // Iterator is still valid after pop_back().
            std::swap(*it, _list.back());
            _list.pop_back();
        }

        // It's important to call notify() after the waiter has been removed from the list
        // since notify() might remove the waiter itself.
        waiter->notify_inlock();
    }
}

void ReplicationCoordinatorImpl::WaiterList::signalAndRemoveAll_inlock() {
    std::vector<WaiterType> list = std::move(_list);
    // Call notify() after removing the waiters from the list.
    for (auto& waiter : list) {
        waiter->notify_inlock();
    }
}

bool ReplicationCoordinatorImpl::WaiterList::remove_inlock(WaiterType waiter) {
    auto it = std::find(_list.begin(), _list.end(), waiter);
    if (it == _list.end()) {
        return false;
    }
    std::swap(*it, _list.back());
    _list.pop_back();
    return true;
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
    options.setMyLastOptime = [replCoord, externalState](
        const OpTime& opTime, ReplicationCoordinator::DataConsistency consistency) {
        replCoord->setMyLastAppliedOpTimeForward(opTime, consistency);
        externalState->setGlobalTimestamp(replCoord->getServiceContext(), opTime.getTimestamp());
    };
    options.resetOptimes = [replCoord]() { replCoord->resetMyLastOpTimes(); };
    options.syncSourceSelector = replCoord;
    options.oplogFetcherMaxFetcherRestarts = externalState->getOplogFetcherMaxFetcherRestarts();
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
      _canAcceptNonLocalWrites(!settings.usingReplSets()),
      _canServeNonLocalReads(0U),
      _replicationProcess(replicationProcess),
      _storage(storage),
      _random(prngSeed) {

    _termShadow.store(OpTime::kUninitializedTerm);

    invariant(_service);

    if (!isReplEnabled()) {
        return;
    }

    _externalState->setupNoopWriter(Seconds(periodicNoopIntervalSecs));
}

ReplicationCoordinatorImpl::~ReplicationCoordinatorImpl() = default;

void ReplicationCoordinatorImpl::waitForStartUpComplete_forTest() {
    _waitForStartUpComplete();
}

void ReplicationCoordinatorImpl::_waitForStartUpComplete() {
    CallbackHandle handle;
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _rsConfig;
}

Date_t ReplicationCoordinatorImpl::getElectionTimeout_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (!_handleElectionTimeoutCbh.isValid()) {
        return Date_t();
    }
    return _handleElectionTimeoutWhen;
}

Milliseconds ReplicationCoordinatorImpl::getRandomizedElectionOffset_forTest() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _getRandomizedElectionOffset_inlock();
}

boost::optional<Date_t> ReplicationCoordinatorImpl::getPriorityTakeover_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (!_priorityTakeoverCbh.isValid()) {
        return boost::none;
    }
    return _priorityTakeoverWhen;
}

boost::optional<Date_t> ReplicationCoordinatorImpl::getCatchupTakeover_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _getCurrentCommittedSnapshotOpTime_inlock();
}

OpTime ReplicationCoordinatorImpl::_getCurrentCommittedSnapshotOpTime_inlock() const {
    if (_currentCommittedSnapshot) {
        return _currentCommittedSnapshot.get();
    }
    return OpTime();
}

LogicalTime ReplicationCoordinatorImpl::_getCurrentCommittedLogicalTime_inlock() const {
    return LogicalTime(_getCurrentCommittedSnapshotOpTime_inlock().getTimestamp());
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

    _replicationProcess->getConsistencyMarkers()->initializeMinValidDocument(opCtx);

    StatusWith<LastVote> lastVote = _externalState->loadLocalLastVoteDocument(opCtx);
    if (!lastVote.isOK()) {
        if (lastVote.getStatus() == ErrorCodes::NoMatchingDocument) {
            log() << "Did not find local voted for document at startup.";
        } else {
            severe() << "Error loading local voted for document at startup; "
                     << lastVote.getStatus();
            fassertFailedNoTrace(40367);
        }
    } else {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _topCoord->loadLastVote(lastVote.getValue());
    }

    // Check that we have a local Rollback ID. If we do not have one, create one.
    auto status = _replicationProcess->refreshRollbackID(opCtx);
    if (!status.isOK()) {
        if (status == ErrorCodes::NamespaceNotFound) {
            log() << "Did not find local Rollback ID document at startup. Creating one.";
            auto initializingStatus = _replicationProcess->initializeRollbackID(opCtx);
            fassert(40424, initializingStatus);
        } else {
            severe() << "Error loading local Rollback ID document at startup; " << status;
            fassertFailedNoTrace(40428);
        }
    }

    StatusWith<BSONObj> cfg = _externalState->loadLocalConfigDocument(opCtx);
    if (!cfg.isOK()) {
        log() << "Did not find local replica set configuration document at startup;  "
              << cfg.getStatus();
        return true;
    }
    ReplSetConfig localConfig;
    status = localConfig.initialize(cfg.getValue());
    if (!status.isOK()) {
        error() << "Locally stored replica set configuration does not parse; See "
                   "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config "
                   "for information on how to recover from this. Got \""
                << status << "\" while parsing " << cfg.getValue();
        fassertFailedNoTrace(28545);
    }

    // Read the last op from the oplog after cleaning up any partially applied batches.
    const auto stableTimestamp = boost::none;
    _replicationProcess->getReplicationRecovery()->recoverFromOplog(opCtx, stableTimestamp);
    auto lastOpTimeStatus = _externalState->loadLastOpTime(opCtx);

    // Use a callback here, because _finishLoadLocalConfig calls isself() which requires
    // that the server's networking layer be up and running and accepting connections, which
    // doesn't happen until startReplication finishes.
    auto handle =
        _replExecutor->scheduleWork([=](const executor::TaskExecutor::CallbackArgs& args) {
            _finishLoadLocalConfig(args, localConfig, lastOpTimeStatus, lastVote);
        });
    if (handle == ErrorCodes::ShutdownInProgress) {
        handle = CallbackHandle{};
    }
    fassert(40446, handle);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _finishLoadLocalConfigCbh = std::move(handle.getValue());

    return false;
}

void ReplicationCoordinatorImpl::_finishLoadLocalConfig(
    const executor::TaskExecutor::CallbackArgs& cbData,
    const ReplSetConfig& localConfig,
    const StatusWith<OpTime>& lastOpTimeStatus,
    const StatusWith<LastVote>& lastVoteStatus) {
    if (!cbData.status.isOK()) {
        LOG(1) << "Loading local replica set configuration failed due to " << cbData.status;
        return;
    }

    StatusWith<int> myIndex =
        validateConfigForStartUp(_externalState.get(), localConfig, getServiceContext());
    if (!myIndex.isOK()) {
        if (myIndex.getStatus() == ErrorCodes::NodeNotFound ||
            myIndex.getStatus() == ErrorCodes::DuplicateKey) {
            warning() << "Locally stored replica set configuration does not have a valid entry "
                         "for the current node; waiting for reconfig or remote heartbeat; Got \""
                      << myIndex.getStatus() << "\" while validating " << localConfig.toBSON();
            myIndex = StatusWith<int>(-1);
        } else {
            error() << "Locally stored replica set configuration is invalid; See "
                       "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config"
                       " for information on how to recover from this. Got \""
                    << myIndex.getStatus() << "\" while validating " << localConfig.toBSON();
            fassertFailedNoTrace(28544);
        }
    }

    if (localConfig.getReplSetName() != _settings.ourSetName()) {
        warning() << "Local replica set configuration document reports set name of "
                  << localConfig.getReplSetName() << ", but command line reports "
                  << _settings.ourSetName() << "; waitng for reconfig or remote heartbeat";
        myIndex = StatusWith<int>(-1);
    }

    // Do not check optime, if this node is an arbiter.
    bool isArbiter =
        myIndex.getValue() != -1 && localConfig.getMemberAt(myIndex.getValue()).isArbiter();
    OpTime lastOpTime;
    if (!isArbiter) {
        if (!lastOpTimeStatus.isOK()) {
            warning() << "Failed to load timestamp of most recently applied operation: "
                      << lastOpTimeStatus.getStatus();
        } else {
            lastOpTime = lastOpTimeStatus.getValue();
        }
    } else {
        // The node is an arbiter hence will not need logical clock for external operations.
        LogicalClock::get(getServiceContext())->disable();
        if (auto validator = LogicalTimeValidator::get(getServiceContext())) {
            validator->stopKeyManager();
        }
    }

    long long term = OpTime::kUninitializedTerm;
    if (localConfig.getProtocolVersion() == 1) {
        // Restore the current term according to the terms of last oplog entry and last vote.
        // The initial term of OpTime() is 0.
        term = lastOpTime.getTerm();
        if (lastVoteStatus.isOK()) {
            long long lastVoteTerm = lastVoteStatus.getValue().getTerm();
            if (term < lastVoteTerm) {
                term = lastVoteTerm;
            }
        }
    }

    auto opCtx = cc().makeOperationContext();
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    invariant(_rsConfigState == kConfigStartingUp);
    const PostMemberStateUpdateAction action =
        _setCurrentRSConfig_inlock(opCtx.get(), localConfig, myIndex.getValue());

    // Set our last applied and durable optimes to the top of the oplog, if we have one.
    if (!lastOpTime.isNull()) {
        bool isRollbackAllowed = false;

        // If we have an oplog, it is still possible that our data is not in a consistent state. For
        // example, if we are starting up after a crash following a post-rollback RECOVERING state.
        // To detect this, we see if our last optime is >= the 'minValid' optime, which
        // should be persistent across node crashes.
        OpTime minValid = _replicationProcess->getConsistencyMarkers()->getMinValid(opCtx.get());
        auto consistency =
            (lastOpTime >= minValid) ? DataConsistency::Consistent : DataConsistency::Inconsistent;

        _setMyLastAppliedOpTime_inlock(lastOpTime, isRollbackAllowed, consistency);
        _setMyLastDurableOpTime_inlock(lastOpTime, isRollbackAllowed);
        _reportUpstream_inlock(std::move(lock));  // unlocks _mutex.
    } else {
        lock.unlock();
    }

    _externalState->setGlobalTimestamp(getServiceContext(), lastOpTime.getTimestamp());
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        // Step down is impossible, so we don't need to wait for the returned event.
        _updateTerm_inlock(term);
    }
    LOG(1) << "Current term is now " << term;
    _performPostMemberStateUpdateAction(action);

    if (!isArbiter) {
        _externalState->startThreads(_settings);
        _startDataReplication(opCtx.get());
    }
}

void ReplicationCoordinatorImpl::_stopDataReplication(OperationContext* opCtx) {
    std::shared_ptr<InitialSyncer> initialSyncerCopy;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _initialSyncer.swap(initialSyncerCopy);
    }
    if (initialSyncerCopy) {
        LOG(1)
            << "ReplicationCoordinatorImpl::_stopDataReplication calling InitialSyncer::shutdown.";
        const auto status = initialSyncerCopy->shutdown();
        if (!status.isOK()) {
            warning() << "InitialSyncer shutdown failed: " << status;
        }
        initialSyncerCopy.reset();
        // Do not return here, fall through.
    }
    LOG(1) << "ReplicationCoordinatorImpl::_stopDataReplication calling "
              "ReplCoordExtState::stopDataReplication.";
    _externalState->stopDataReplication(opCtx);
}

void ReplicationCoordinatorImpl::_startDataReplication(OperationContext* opCtx,
                                                       stdx::function<void()> startCompleted) {
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
        log() << "not running initial sync during test.";
        return;
    }

    auto onCompletion = [this, startCompleted](const StatusWith<OpTimeWithHash>& status) {
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            if (status == ErrorCodes::CallbackCanceled) {
                log() << "Initial Sync has been cancelled: " << status.getStatus();
                return;
            } else if (!status.isOK()) {
                if (_inShutdown) {
                    log() << "Initial Sync failed during shutdown due to " << status.getStatus();
                    return;
                } else {
                    error() << "Initial sync failed, shutting down now. Restart the server "
                               "to attempt a new initial sync.";
                    fassertFailedWithStatusNoTrace(40088, status.getStatus());
                }
            }

            const auto lastApplied = status.getValue();
            _setMyLastAppliedOpTime_inlock(lastApplied.opTime, false, DataConsistency::Consistent);
        }

        // Clear maint. mode.
        while (getMaintenanceMode()) {
            setMaintenanceMode(false).transitional_ignore();
        }

        if (startCompleted) {
            startCompleted();
        }
        // Repair local db (to compact it).
        auto opCtxHolder = cc().makeOperationContext();
        uassertStatusOK(_externalState->runRepairOnLocalDB(opCtxHolder.get()));
        // Because initial sync completed, we can only be in STARTUP2, not REMOVED.
        // Transition from STARTUP2 to RECOVERING and start the producer and the applier.
        invariant(getMemberState().startup2());
        invariant(setFollowerMode(MemberState::RS_RECOVERING));
        _externalState->startSteadyStateReplication(opCtxHolder.get(), this);
    };

    std::shared_ptr<InitialSyncer> initialSyncerCopy;
    try {
        {
            // Must take the lock to set _initialSyncer, but not call it.
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            initialSyncerCopy = std::make_shared<InitialSyncer>(
                createInitialSyncerOptions(this, _externalState.get()),
                stdx::make_unique<DataReplicatorExternalStateInitialSync>(this,
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
    } catch (...) {
        auto status = exceptionToStatus();
        log() << "Initial Sync failed to start: " << status;
        if (ErrorCodes::CallbackCanceled == status || ErrorCodes::isShutdownError(status.code())) {
            return;
        }
        fassertFailedWithStatusNoTrace(40354, status);
    }
}

void ReplicationCoordinatorImpl::startup(OperationContext* opCtx) {
    if (!isReplEnabled()) {
        if (ReplSettings::shouldRecoverFromOplogAsStandalone()) {
            if (!_storage->supportsRecoverToStableTimestamp(opCtx->getServiceContext())) {
                severe() << "Cannot use 'recoverFromOplogAsStandalone' with a storage engine that "
                            "does not support recover to stable timestamp.";
                fassertFailedNoTrace(50805);
            }
            auto recoveryTS = _storage->getRecoveryTimestamp(opCtx->getServiceContext());
            if (!recoveryTS || recoveryTS->isNull()) {
                severe()
                    << "Cannot use 'recoverFromOplogAsStandalone' without a stable checkpoint.";
                fassertFailedNoTrace(50806);
            }

            // We pass in "none" for the stable timestamp so that recoverFromOplog asks storage
            // for the recoveryTimestamp just like on replica set recovery.
            const auto stableTimestamp = boost::none;
            _replicationProcess->getReplicationRecovery()->recoverFromOplog(opCtx, stableTimestamp);
            warning() << "Setting mongod to readOnly mode as a result of specifying "
                         "'recoverFromOplogAsStandalone'.";
            storageGlobalParams.readOnly = true;
        }

        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _setConfigState_inlock(kConfigReplicationDisabled);
        return;
    }
    invariant(_settings.usingReplSets());
    invariant(!ReplSettings::shouldRecoverFromOplogAsStandalone());

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        fassert(18822, !_inShutdown);
        _setConfigState_inlock(kConfigStartingUp);
        _topCoord->setStorageEngineSupportsReadCommitted(
            _externalState->isReadCommittedSupportedByStorageEngine(opCtx));
    }

    _replExecutor->startup();

    bool doneLoadingConfig = _startLoadLocalConfig(opCtx);
    if (doneLoadingConfig) {
        // If we're not done loading the config, then the config state will be set by
        // _finishLoadLocalConfig.
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        invariant(!_rsConfig.isInitialized());
        _setConfigState_inlock(kConfigUninitialized);
    }
}

void ReplicationCoordinatorImpl::shutdown(OperationContext* opCtx) {
    // Shutdown must:
    // * prevent new threads from blocking in awaitReplication
    // * wake up all existing threads blocking in awaitReplication
    // * Shut down and join the execution resources it owns.

    if (!_settings.usingReplSets()) {
        return;
    }

    log() << "shutting down replication subsystems";

    // Used to shut down outside of the lock.
    std::shared_ptr<InitialSyncer> initialSyncerCopy;
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        fassert(28533, !_inShutdown);
        _inShutdown = true;
        if (_rsConfigState == kConfigPreStart) {
            warning() << "ReplicationCoordinatorImpl::shutdown() called before "
                         "startup() finished.  Shutting down without cleaning up the "
                         "replication system";
            return;
        }
        if (_rsConfigState == kConfigStartingUp) {
            // Wait until we are finished starting up, so that we can cleanly shut everything down.
            lk.unlock();
            _waitForStartUpComplete();
            lk.lock();
            fassert(18823, _rsConfigState != kConfigStartingUp);
        }
        _replicationWaiterList.signalAndRemoveAll_inlock();
        _opTimeWaiterList.signalAndRemoveAll_inlock();
        _currentCommittedSnapshotCond.notify_all();
        _initialSyncer.swap(initialSyncerCopy);
        _stepDownWaiters.notify_all();
    }


    // joining the replication executor is blocking so it must be run outside of the mutex
    if (initialSyncerCopy) {
        LOG(1) << "ReplicationCoordinatorImpl::shutdown calling InitialSyncer::shutdown.";
        const auto status = initialSyncerCopy->shutdown();
        if (!status.isOK()) {
            warning() << "InitialSyncer shutdown failed: " << status;
        }
        initialSyncerCopy->join();
        initialSyncerCopy.reset();
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _getMemberState_inlock();
}

MemberState ReplicationCoordinatorImpl::_getMemberState_inlock() const {
    return _memberState;
}

Status ReplicationCoordinatorImpl::waitForMemberState(MemberState expectedState,
                                                      Milliseconds timeout) {
    if (timeout < Milliseconds(0)) {
        return Status(ErrorCodes::BadValue, "Timeout duration cannot be negative");
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto pred = [this, expectedState]() { return _memberState == expectedState; };
    if (!_memberStateChange.wait_for(lk, timeout.toSystemDuration(), pred)) {
        return Status(ErrorCodes::ExceededTimeLimit,
                      str::stream() << "Timed out waiting for state to become "
                                    << expectedState.toString()
                                    << ". Current state is "
                                    << _memberState.toString());
    }
    return Status::OK();
}

Seconds ReplicationCoordinatorImpl::getSlaveDelaySecs() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_rsConfig.isInitialized());
    if (_selfIndex == -1) {
        // We aren't currently in the set. Return 0 seconds so we can clear out the applier's
        // queue of work.
        return Seconds(0);
    }
    return _rsConfig.getMemberAt(_selfIndex).getSlaveDelay();
}

void ReplicationCoordinatorImpl::clearSyncSourceBlacklist() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _topCoord->clearSyncSourceBlacklist();
}

Status ReplicationCoordinatorImpl::setFollowerMode(const MemberState& newState) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
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

    const PostMemberStateUpdateAction action =
        _updateMemberStateFromTopologyCoordinator_inlock(nullptr);
    lk.unlock();
    _performPostMemberStateUpdateAction(action);

    return Status::OK();
}

ReplicationCoordinator::ApplierState ReplicationCoordinatorImpl::getApplierState() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _applierState;
}

void ReplicationCoordinatorImpl::signalDrainComplete(OperationContext* opCtx,
                                                     long long termWhenBufferIsEmpty) {
    // This logic is a little complicated in order to avoid acquiring the global exclusive lock
    // unnecessarily.  This is important because the applier may call signalDrainComplete()
    // whenever it wants, not only when the ReplicationCoordinator is expecting it.
    //
    // The steps are:
    // 1.) Check to see if we're waiting for this signal.  If not, return early.
    // 2.) Otherwise, release the mutex while acquiring the global exclusive lock,
    //     since that might take a while (NB there's a deadlock cycle otherwise, too).
    // 3.) Re-check to see if we've somehow left drain mode.  If we have not, clear
    //     producer and applier's states, set the flag allowing non-local database writes and
    //     drop the mutex.  At this point, no writes can occur from other threads, due to the
    //     global exclusive lock.
    // 4.) Drop all temp collections, and log the drops to the oplog.
    // 5.) Log transition to primary in the oplog and set that OpTime as the floor for what we will
    //     consider to be committed.
    // 6.) Drop the global exclusive lock.
    //
    // Because replicatable writes are forbidden while in drain mode, and we don't exit drain
    // mode until we have the global exclusive lock, which forbids all other threads from making
    // writes, we know that from the time that _canAcceptNonLocalWrites is set until
    // this method returns, no external writes will be processed.  This is important so that a new
    // temp collection isn't introduced on the new primary before we drop all the temp collections.

    // When we go to drop all temp collections, we must replicate the drops.
    invariant(opCtx->writesAreReplicated());

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_applierState != ApplierState::Draining) {
        return;
    }
    lk.unlock();

    _externalState->onDrainComplete(opCtx);

    if (MONGO_FAIL_POINT(transitionToPrimaryHangBeforeTakingGlobalExclusiveLock)) {
        log() << "transition to primary - "
                 "transitionToPrimaryHangBeforeTakingGlobalExclusiveLock fail point enabled. "
                 "Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(transitionToPrimaryHangBeforeTakingGlobalExclusiveLock)) {
            mongo::sleepsecs(1);
            {
                stdx::lock_guard<stdx::mutex> lock(_mutex);
                if (_inShutdown) {
                    break;
                }
            }
        }
    }

    Lock::GlobalWrite globalWriteLock(opCtx);
    lk.lock();

    // Exit drain mode only if we're actually in draining mode, the apply buffer is empty in the
    // current term, and we're allowed to become the write master.
    if (_applierState != ApplierState::Draining ||
        !_topCoord->canCompleteTransitionToPrimary(termWhenBufferIsEmpty)) {
        return;
    }
    _applierState = ApplierState::Stopped;

    invariant(_getMemberState_inlock().primary());
    invariant(!_canAcceptNonLocalWrites);

    {
        lk.unlock();
        AllowNonLocalWritesBlock writesAllowed(opCtx);
        OpTime firstOpTime = _externalState->onTransitionToPrimary(opCtx, isV1ElectionProtocol());
        lk.lock();

        auto status = _topCoord->completeTransitionToPrimary(firstOpTime);
        if (status.code() == ErrorCodes::PrimarySteppedDown) {
            log() << "Transition to primary failed" << causedBy(status);
            return;
        }
        invariant(status);
    }

    // Must calculate the commit level again because firstOpTimeOfMyTerm wasn't set when we logged
    // our election in onTransitionToPrimary(), above.
    _updateLastCommittedOpTime_inlock();

    // Update _canAcceptNonLocalWrites
    _updateMemberStateFromTopologyCoordinator_inlock(opCtx);

    log() << "transition to primary complete; database writes are now permitted" << rsLog;
    _drainFinishedCond.notify_all();
    _externalState->startNoopWriter(_getMyLastAppliedOpTime_inlock());
}

Status ReplicationCoordinatorImpl::waitForDrainFinish(Milliseconds timeout) {
    if (timeout < Milliseconds(0)) {
        return Status(ErrorCodes::BadValue, "Timeout duration cannot be negative");
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);
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
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _topCoord->setMyHeartbeatMessage(_replExecutor->now(), msg);
}

void ReplicationCoordinatorImpl::setMyLastAppliedOpTimeForward(const OpTime& opTime,
                                                               DataConsistency consistency) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    if (opTime > _getMyLastAppliedOpTime_inlock()) {
        _setMyLastAppliedOpTime_inlock(opTime, false, consistency);
        _reportUpstream_inlock(std::move(lock));
    } else if (consistency == DataConsistency::Consistent && _canAcceptNonLocalWrites &&
               _rsConfig.getWriteMajority() == 1) {
        // Single vote primaries may have a lagged stable timestamp due to paring back the stable
        // timestamp to the all committed timestamp.
        _setStableTimestampForStorage_inlock();
    }
}

void ReplicationCoordinatorImpl::setMyLastDurableOpTimeForward(const OpTime& opTime) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    if (opTime > _getMyLastDurableOpTime_inlock()) {
        _setMyLastDurableOpTime_inlock(opTime, false);
        _reportUpstream_inlock(std::move(lock));
    }
}

void ReplicationCoordinatorImpl::setMyLastAppliedOpTime(const OpTime& opTime) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    // The optime passed to this function is required to represent a consistent database state.
    _setMyLastAppliedOpTime_inlock(opTime, false, DataConsistency::Consistent);
    _reportUpstream_inlock(std::move(lock));
}

void ReplicationCoordinatorImpl::setMyLastDurableOpTime(const OpTime& opTime) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _setMyLastDurableOpTime_inlock(opTime, false);
    _reportUpstream_inlock(std::move(lock));
}

void ReplicationCoordinatorImpl::resetMyLastOpTimes() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _resetMyLastOpTimes_inlock();
    _reportUpstream_inlock(std::move(lock));
}

void ReplicationCoordinatorImpl::_resetMyLastOpTimes_inlock() {
    LOG(1) << "resetting durable/applied optimes.";
    // Reset to uninitialized OpTime
    bool isRollbackAllowed = true;
    _setMyLastAppliedOpTime_inlock(OpTime(), isRollbackAllowed, DataConsistency::Inconsistent);
    _setMyLastDurableOpTime_inlock(OpTime(), isRollbackAllowed);
    _stableOpTimeCandidates.clear();
}

void ReplicationCoordinatorImpl::_reportUpstream_inlock(stdx::unique_lock<stdx::mutex> lock) {
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

void ReplicationCoordinatorImpl::_setMyLastAppliedOpTime_inlock(const OpTime& opTime,
                                                                bool isRollbackAllowed,
                                                                DataConsistency consistency) {
    _topCoord->setMyLastAppliedOpTime(opTime, _replExecutor->now(), isRollbackAllowed);
    // If we are using applied times to calculate the commit level, update it now.
    if (!_rsConfig.getWriteConcernMajorityShouldJournal()) {
        _updateLastCommittedOpTime_inlock();
    }

    // Signal anyone waiting on optime changes.
    _opTimeWaiterList.signalAndRemoveIf_inlock(
        [opTime](Waiter* waiter) { return waiter->opTime <= opTime; });

    if (opTime.isNull()) {
        return;
    }

    // Update the local snapshot before updating the stable timestamp on the storage engine. New
    // transactions reading from the local snapshot should start before the oldest timestamp is
    // advanced to avoid races.
    _externalState->updateLocalSnapshot(opTime);

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
        _stableOpTimeCandidates.insert(opTime);
        // If we are lagged behind the commit optime, set a new stable timestamp here.
        if (opTime <= _topCoord->getLastCommittedOpTime()) {
            _setStableTimestampForStorage_inlock();
        }
    } else if (_getMemberState_inlock().startup2()) {
        // The oplog application phase of initial sync starts timestamping writes, causing
        // WiredTiger to pin this data in memory. Advancing the oldest timestamp in step with the
        // last applied optime here will permit WiredTiger to evict this data as it sees fit.
        _service->getStorageEngine()->setOldestTimestamp(opTime.getTimestamp());
    }
}

void ReplicationCoordinatorImpl::_setMyLastDurableOpTime_inlock(const OpTime& opTime,
                                                                bool isRollbackAllowed) {
    _topCoord->setMyLastDurableOpTime(opTime, _replExecutor->now(), isRollbackAllowed);
    // If we are using durable times to calculate the commit level, update it now.
    if (_rsConfig.getWriteConcernMajorityShouldJournal()) {
        _updateLastCommittedOpTime_inlock();
    }
}

OpTime ReplicationCoordinatorImpl::getMyLastAppliedOpTime() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _getMyLastAppliedOpTime_inlock();
}

OpTime ReplicationCoordinatorImpl::getMyLastDurableOpTime() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
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

    if (readConcern.getLevel() == ReadConcernLevel::kMajorityReadConcern &&
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
                                                    bool isMajorityCommittedRead,
                                                    OpTime targetOpTime,
                                                    boost::optional<Date_t> deadline) {
    if (!isMajorityCommittedRead) {
        // This assumes the read concern is "local" level.
        // We need to wait for all committed writes to be visible, even in the oplog (which uses
        // special visibility rules).
        _externalState->waitForAllEarlierOplogWritesToBeVisible(opCtx);
    }

    stdx::unique_lock<stdx::mutex> lock(_mutex);

    if (isMajorityCommittedRead && !_externalState->snapshotsEnabled()) {
        return {ErrorCodes::CommandNotSupported,
                "Current storage engine does not support majority committed reads"};
    }

    auto getCurrentOpTime = [this, isMajorityCommittedRead]() {
        return isMajorityCommittedRead ? _getCurrentCommittedSnapshotOpTime_inlock()
                                       : _getMyLastAppliedOpTime_inlock();
    };

    if (isMajorityCommittedRead && targetOpTime > getCurrentOpTime()) {
        LOG(1) << "waitUntilOpTime: waiting for optime:" << targetOpTime
               << " to be in a snapshot -- current snapshot: " << getCurrentOpTime();
    }

    while (targetOpTime > getCurrentOpTime()) {
        if (_inShutdown) {
            return {ErrorCodes::ShutdownInProgress, "Shutdown in progress"};
        }

        // If we are doing a majority committed read we only need to wait for a new snapshot.
        if (isMajorityCommittedRead) {
            LOG(3) << "waitUntilOpTime: waiting for a new snapshot until " << opCtx->getDeadline();

            auto waitStatus =
                opCtx->waitForConditionOrInterruptNoAssert(_currentCommittedSnapshotCond, lock);
            if (!waitStatus.isOK()) {
                return waitStatus;
            }
            LOG(3) << "Got notified of new snapshot: " << _currentCommittedSnapshot->toString();
            continue;
        }

        // We just need to wait for the opTime to catch up to what we need (not majority RC).
        stdx::condition_variable condVar;
        ThreadWaiter waiter(targetOpTime, nullptr, &condVar);
        WaiterGuard guard(&_opTimeWaiterList, &waiter);

        LOG(3) << "waitUntilOpTime: OpID " << opCtx->getOpID() << " is waiting for OpTime "
               << waiter << " until " << opCtx->getDeadline();

        auto waitStatus = Status::OK();
        if (deadline) {
            auto waitUntilStatus =
                opCtx->waitForConditionOrInterruptNoAssertUntil(condVar, lock, *deadline);
            if (!waitUntilStatus.isOK()) {
                waitStatus = waitUntilStatus.getStatus();
            }
            // If deadline is set no need to wait until the targetTime time is reached.
            return waitStatus;
        } else {
            waitStatus = opCtx->waitForConditionOrInterruptNoAssert(condVar, lock);
        }

        if (!waitStatus.isOK()) {
            return waitStatus;
        }
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

    // TODO SERVER-34620: Re-enable speculative behavior when "atClusterTime" is specified.
    auto session = OperationContextSession::get(opCtx);
    const bool speculative = session && session->inSnapshotReadOrMultiDocumentTransaction() &&
        !readConcern.getArgsAtClusterTime();

    const bool isMajorityCommittedRead =
        (readConcern.getLevel() == ReadConcernLevel::kMajorityReadConcern ||
         readConcern.getLevel() == ReadConcernLevel::kSnapshotReadConcern) &&
        !speculative;

    return _waitUntilOpTime(opCtx, isMajorityCommittedRead, targetOpTime, deadline);
}

// TODO: remove when SERVER-29729 is done
Status ReplicationCoordinatorImpl::_waitUntilOpTimeForReadDeprecated(
    OperationContext* opCtx, const ReadConcernArgs& readConcern) {
    const bool isMajorityCommittedRead =
        readConcern.getLevel() == ReadConcernLevel::kMajorityReadConcern ||
        readConcern.getLevel() == ReadConcernLevel::kSnapshotReadConcern;

    const auto targetOpTime = readConcern.getArgsOpTime().value_or(OpTime());
    return _waitUntilOpTime(opCtx, isMajorityCommittedRead, targetOpTime);
}

OpTime ReplicationCoordinatorImpl::_getMyLastAppliedOpTime_inlock() const {
    return _topCoord->getMyLastAppliedOpTime();
}

OpTime ReplicationCoordinatorImpl::_getMyLastDurableOpTime_inlock() const {
    return _topCoord->getMyLastDurableOpTime();
}

Status ReplicationCoordinatorImpl::setLastDurableOptime_forTest(long long cfgVer,
                                                                long long memberId,
                                                                const OpTime& opTime) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(getReplicationMode() == modeReplSet);

    const UpdatePositionArgs::UpdateInfo update(OpTime(), opTime, cfgVer, memberId);
    long long configVersion;
    const auto status = _setLastOptime_inlock(update, &configVersion);
    _updateLastCommittedOpTime_inlock();
    return status;
}

Status ReplicationCoordinatorImpl::setLastAppliedOptime_forTest(long long cfgVer,
                                                                long long memberId,
                                                                const OpTime& opTime) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(getReplicationMode() == modeReplSet);

    const UpdatePositionArgs::UpdateInfo update(opTime, OpTime(), cfgVer, memberId);
    long long configVersion;
    const auto status = _setLastOptime_inlock(update, &configVersion);
    _updateLastCommittedOpTime_inlock();
    return status;
}

Status ReplicationCoordinatorImpl::_setLastOptime_inlock(const UpdatePositionArgs::UpdateInfo& args,
                                                         long long* configVersion) {
    auto result = _topCoord->setLastOptime(args, _replExecutor->now(), configVersion);
    if (!result.isOK())
        return result.getStatus();
    const bool advancedOpTime = result.getValue();
    // Only update committed optime if the remote optimes increased.
    if (advancedOpTime) {
        _updateLastCommittedOpTime_inlock();
    }

    _cancelAndRescheduleLivenessUpdate_inlock(args.memberId);
    return Status::OK();
}

bool ReplicationCoordinatorImpl::_doneWaitingForReplication_inlock(
    const OpTime& opTime, const WriteConcernOptions& writeConcern) {
    // The syncMode cannot be unset.
    invariant(writeConcern.syncMode != WriteConcernOptions::SyncMode::UNSET);
    Status status = _checkIfWriteConcernCanBeSatisfied_inlock(writeConcern);
    if (!status.isOK()) {
        return true;
    }

    const bool useDurableOpTime = writeConcern.syncMode == WriteConcernOptions::SyncMode::JOURNAL;

    if (writeConcern.wMode.empty()) {
        return _topCoord->haveNumNodesReachedOpTime(
            opTime, writeConcern.wNumNodes, useDurableOpTime);
    }

    StringData patternName;
    if (writeConcern.wMode == WriteConcernOptions::kMajority) {
        if (_externalState->snapshotsEnabled() && !testingSnapshotBehaviorInIsolation) {
            // Make sure we have a valid "committed" snapshot up to the needed optime.
            if (!_currentCommittedSnapshot) {
                return false;
            }

            // Wait for the "current" snapshot to advance to/past the opTime.
            const auto haveSnapshot = _currentCommittedSnapshot >= opTime;
            if (!haveSnapshot) {
                LOG(1) << "Required snapshot optime: " << opTime << " is not yet part of the "
                       << "current 'committed' snapshot: " << *_currentCommittedSnapshot;
                return false;
            }

            // Fallthrough to wait for "majority" write concern.
        }

        // Wait for all drop pending collections with drop optime before and at 'opTime' to be
        // removed from storage.
        if (auto dropOpTime = _externalState->getEarliestDropPendingOpTime()) {
            if (*dropOpTime <= opTime) {
                LOG(1) << "Unable to satisfy the requested majority write concern at "
                          "'committed' optime "
                       << opTime
                       << ". There are still drop pending collections (earliest drop optime: "
                       << *dropOpTime << ") that have to be removed from storage before we can "
                                         "satisfy the write concern "
                       << writeConcern.toBSON();
                return false;
            }
        }

        // Continue and wait for replication to the majority (of voters).
        // *** Needed for J:True, writeConcernMajorityShouldJournal:False (appliedOpTime snapshot).
        patternName = ReplSetConfig::kMajorityWriteConcernModeName;
    } else {
        patternName = writeConcern.wMode;
    }

    StatusWith<ReplSetTagPattern> tagPattern = _rsConfig.findCustomWriteMode(patternName);
    if (!tagPattern.isOK()) {
        return true;
    }
    return _topCoord->haveTaggedNodesReachedOpTime(opTime, tagPattern.getValue(), useDurableOpTime);
}

ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorImpl::awaitReplication(
    OperationContext* opCtx, const OpTime& opTime, const WriteConcernOptions& writeConcern) {
    Timer timer;
    WriteConcernOptions fixedWriteConcern = populateUnsetWriteConcernOptionsSyncMode(writeConcern);
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    auto status = _awaitReplication_inlock(&lock, opCtx, opTime, fixedWriteConcern);
    return {std::move(status), duration_cast<Milliseconds>(timer.elapsed())};
}

Status ReplicationCoordinatorImpl::_awaitReplication_inlock(
    stdx::unique_lock<stdx::mutex>* lock,
    OperationContext* opCtx,
    const OpTime& opTime,
    const WriteConcernOptions& writeConcern) {

    // We should never wait for replication if we are holding any locks, because this can
    // potentially block for long time while doing network activity.
    if (opCtx->lockState()->isLocked()) {
        return {ErrorCodes::IllegalOperation,
                "Waiting for replication not allowed while holding a lock"};
    }

    const Mode replMode = getReplicationMode();
    if (replMode == modeNone) {
        // no replication check needed (validated above)
        return Status::OK();
    }

    if (opTime.isNull()) {
        // If waiting for the empty optime, always say it's been replicated.
        return Status::OK();
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
        return stepdownStatus;
    }

    auto interruptStatus = opCtx->checkForInterruptNoAssert();
    if (!interruptStatus.isOK()) {
        return interruptStatus;
    }

    if (writeConcern.wMode.empty()) {
        if (writeConcern.wNumNodes < 1) {
            return Status::OK();
        } else if (writeConcern.wNumNodes == 1 && _getMyLastAppliedOpTime_inlock() >= opTime) {
            return Status::OK();
        }
    }

    auto clockSource = opCtx->getServiceContext()->getFastClockSource();
    const auto wTimeoutDate = [&]() -> const Date_t {
        if (writeConcern.wDeadline != Date_t::max()) {
            return writeConcern.wDeadline;
        }
        if (writeConcern.wTimeout == WriteConcernOptions::kNoTimeout) {
            return Date_t::max();
        }
        return clockSource->now() + clockSource->getPrecision() +
            Milliseconds{writeConcern.wTimeout};
    }();

    // Must hold _mutex before constructing waitInfo as it will modify _replicationWaiterList
    stdx::condition_variable condVar;
    ThreadWaiter waiter(opTime, &writeConcern, &condVar);
    WaiterGuard guard(&_replicationWaiterList, &waiter);
    while (!_doneWaitingForReplication_inlock(opTime, writeConcern)) {

        if (_inShutdown) {
            return {ErrorCodes::ShutdownInProgress, "Replication is being shut down"};
        }

        auto status = opCtx->waitForConditionOrInterruptNoAssertUntil(condVar, *lock, wTimeoutDate);
        if (!status.isOK()) {
            return status.getStatus();
        }

        if (status.getValue() == stdx::cv_status::timeout) {
            if (getTestCommandsEnabled()) {
                // log state of replica set on timeout to help with diagnosis.
                BSONObjBuilder progress;
                _topCoord->fillMemberData(&progress);
                log() << "Replication for failed WC: " << writeConcern.toBSON()
                      << ", waitInfo: " << waiter << ", opID: " << opCtx->getOpID()
                      << ", progress: " << progress.done();
            }
            return {ErrorCodes::WriteConcernFailed, "waiting for replication timed out"};
        }

        stepdownStatus = checkForStepDown();
        if (!stepdownStatus.isOK()) {
            return stepdownStatus;
        }
    }

    return _checkIfWriteConcernCanBeSatisfied_inlock(writeConcern);
}

void ReplicationCoordinatorImpl::waitForStepDownAttempt_forTest() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (!_topCoord->isSteppingDown()) {
        _stepDownWaiters.wait(lk);
    }
}

Status ReplicationCoordinatorImpl::stepDown(OperationContext* opCtx,
                                            const bool force,
                                            const Milliseconds& waitTime,
                                            const Milliseconds& stepdownTime) {

    const Date_t startTime = _replExecutor->now();
    const Date_t stepDownUntil = startTime + stepdownTime;
    const Date_t waitUntil = startTime + waitTime;

    if (!getMemberState().primary()) {
        // Note this check is inherently racy - it's always possible for the node to
        // stepdown from some other path before we acquire the global exclusive lock.  This check
        // is just to try to save us from acquiring the global X lock unnecessarily.
        return {ErrorCodes::NotMaster, "not primary so can't step down"};
    }

    auto globalLock = stdx::make_unique<Lock::GlobalLock>(opCtx,
                                                          MODE_X,
                                                          stepDownUntil,
                                                          Lock::InterruptBehavior::kThrow,
                                                          Lock::GlobalLock::EnqueueOnly());

    // We've requested the global exclusive lock which will stop new operations from coming in,
    // but existing operations could take a long time to finish, so kill all user operations
    // to help us get the global lock faster.
    _externalState->killAllUserOperations(opCtx);

    globalLock->waitForLockUntil(stepDownUntil);
    if (!globalLock->isLocked()) {
        return {ErrorCodes::ExceededTimeLimit,
                "Could not acquire the global shared lock within the amount of time "
                "specified that we should step down for"};
    }

    // TODO SERVER-34395: Remove this method and kill cursors as part of killAllUserOperations call
    // when the CursorManager no longer requires collection locks to kill cursors.
    _externalState->killAllTransactionCursors(opCtx);

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto status = opCtx->checkForInterruptNoAssert();
    if (!status.isOK()) {
        return status;
    }

    const long long termAtStart = _topCoord->getTerm();

    status = _topCoord->prepareForStepDownAttempt();
    if (!status.isOK()) {
        // This will cause us to fail if we're already in the process of stepping down.
        // It is also possible to get here even if we're done stepping down via another path,
        // and this will also elicit a failure from this call.
        return status;
    }

    // Wake up threads blocked in waitForStepDownAttempt_forTest.
    _stepDownWaiters.notify_all();

    // Update _canAcceptNonLocalWrites from the TopologyCoordinator now that we're in the middle
    // of a stepdown attempt.  This will prevent us from accepting writes so that if our stepdown
    // attempt fails later we can release the global lock and go to sleep to allow secondaries to
    // catch up without allowing new writes in.
    auto action = _updateMemberStateFromTopologyCoordinator_inlock(opCtx);
    invariant(action == PostMemberStateUpdateAction::kActionNone);
    invariant(!_canAcceptNonLocalWrites);

    // Make sure that we leave _canAcceptNonLocalWrites in the proper state.
    auto updateMemberState = [&] {
        invariant(lk.owns_lock());
        invariant(opCtx->lockState()->isW());

        auto action = _updateMemberStateFromTopologyCoordinator_inlock(opCtx);
        lk.unlock();

        if (MONGO_FAIL_POINT(stepdownHangBeforePerformingPostMemberStateUpdateActions)) {
            log() << "stepping down from primary - "
                     "stepdownHangBeforePerformingPostMemberStateUpdateActions fail point enabled. "
                     "Blocking until fail point is disabled.";
            while (MONGO_FAIL_POINT(stepdownHangBeforePerformingPostMemberStateUpdateActions)) {
                mongo::sleepsecs(1);
                {
                    stdx::lock_guard<stdx::mutex> lock(_mutex);
                    if (_inShutdown) {
                        break;
                    }
                }
            }
        }

        _performPostMemberStateUpdateAction(action);
    };
    ScopeGuard onExitGuard = MakeGuard([&] {
        _topCoord->abortAttemptedStepDownIfNeeded();
        updateMemberState();
    });

    try {

        bool firstTime = true;
        while (!_topCoord->attemptStepDown(
            termAtStart, _replExecutor->now(), waitUntil, stepDownUntil, force)) {

            // The stepdown attempt failed.

            if (firstTime) {
                // We send out a fresh round of heartbeats because stepping down successfully
                // without {force: true} is dependent on timely heartbeat data.
                _restartHeartbeats_inlock();
                firstTime = false;
            }

            // Now release the global lock to allow secondaries to read the oplog, then wait until
            // enough secondaries are caught up for us to finish stepdown.
            globalLock.reset();
            invariant(!opCtx->lockState()->isLocked());

            // Make sure we re-acquire the global lock before returning so that we're always holding
            // the global lock when the onExitGuard set up earlier runs.
            ON_BLOCK_EXIT([&] {
                // Need to release _mutex before re-acquiring the global lock to preserve lock
                // acquisition order rules.
                lk.unlock();

                // Need to re-acquire the global lock before re-attempting stepdown.
                // We use no timeout here even though that means the lock acquisition could take
                // longer than the stepdown window.  If that happens, the call to _tryToStepDown
                // immediately after will error.  Since we'll need the global lock no matter what to
                // clean up a failed stepdown attempt, we might as well spend whatever time we need
                // to acquire it now.  For the same reason, we also disable lock acquisition
                // interruption, to guarantee that we get the lock eventually.
                UninterruptibleLockGuard noInterrupt(opCtx->lockState());
                globalLock.reset(new Lock::GlobalLock(opCtx, MODE_X));
                invariant(globalLock->isLocked());
                lk.lock();
            });

            // We ignore the case where waitForConditionOrInterruptUntil returns
            // stdx::cv_status::timeout because in that case coming back around the loop and calling
            // attemptStepDown again will cause attemptStepDown to return ExceededTimeLimit with
            // the proper error message.
            opCtx->waitForConditionOrInterruptUntil(
                _stepDownWaiters, lk, std::min(stepDownUntil, waitUntil));
        }
    } catch (const DBException& e) {
        return e.toStatus();
    }

    // Stepdown success!
    onExitGuard.Dismiss();
    updateMemberState();
    // Schedule work to (potentially) step back up once the stepdown period has ended.
    _scheduleWorkAt(stepDownUntil, [=](const executor::TaskExecutor::CallbackArgs& cbData) {
        _handleTimePassing(cbData);
    });
    return Status::OK();
}

void ReplicationCoordinatorImpl::_signalStepDownWaiterIfReady_inlock() {
    if (_topCoord->isSafeToStepDown()) {
        _stepDownWaiters.notify_all();
    }
}

void ReplicationCoordinatorImpl::_handleTimePassing(
    const executor::TaskExecutor::CallbackArgs& cbData) {
    if (!cbData.status.isOK()) {
        return;
    }

    // For election protocol v1, call _startElectSelfIfEligibleV1 to avoid race
    // against other elections caused by events like election timeout, replSetStepUp etc.
    if (isV1ElectionProtocol()) {
        _startElectSelfIfEligibleV1(
            TopologyCoordinator::StartElectionReason::kSingleNodeStepDownTimeout);
        return;
    }

    bool wonSingleNodeElection = [this]() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _topCoord->becomeCandidateIfStepdownPeriodOverAndSingleNodeSet(_replExecutor->now());
    }();

    if (wonSingleNodeElection) {
        _performPostMemberStateUpdateAction(kActionWinElection);
    }
}

bool ReplicationCoordinatorImpl::isMasterForReportingPurposes() {
    if (!_settings.usingReplSets()) {
        return true;
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(getReplicationMode() == modeReplSet);
    return _getMemberState_inlock().primary();
}

bool ReplicationCoordinatorImpl::canAcceptWritesForDatabase(OperationContext* opCtx,
                                                            StringData dbName) {
    // The answer isn't meaningful unless we hold the global lock.
    invariant(opCtx->lockState()->isLocked());
    return canAcceptWritesForDatabase_UNSAFE(opCtx, dbName);
}

bool ReplicationCoordinatorImpl::canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                                                   StringData dbName) {
    // _canAcceptNonLocalWrites is always true for standalone nodes, and adjusted based on
    // primary+drain state in replica sets.
    //
    // Stand-alone nodes and drained replica set primaries can always accept writes.  Writes are
    // always permitted to the "local" database.
    if (_canAcceptNonLocalWrites || alwaysAllowNonLocalWrites(*opCtx)) {
        return true;
    }
    if (dbName == kLocalDB) {
        return true;
    }
    return false;
}

bool ReplicationCoordinatorImpl::canAcceptWritesFor(OperationContext* opCtx,
                                                    const NamespaceString& ns) {
    invariant(opCtx->lockState()->isLocked());
    return canAcceptWritesFor_UNSAFE(opCtx, ns);
}

bool ReplicationCoordinatorImpl::canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                                           const NamespaceString& ns) {
    StringData dbName = ns.db();
    bool canWriteToDB = canAcceptWritesForDatabase_UNSAFE(opCtx, dbName);

    if (!canWriteToDB && !ns.isSystemDotProfile()) {
        return false;
    }

    // Even if we think we can write to the database we need to make sure we're not trying
    // to write to the oplog in ROLLBACK.
    // If we can accept non local writes (ie we're PRIMARY) then we must not be in ROLLBACK.
    // This check is redundant of the check of _memberState below, but since this can be checked
    // without locking, we do it as an optimization.
    if (_canAcceptNonLocalWrites || alwaysAllowNonLocalWrites(*opCtx)) {
        return true;
    }

    if (!ns.isOplog()) {
        return true;
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (_memberState.rollback()) {
        return false;
    }
    return true;
}

Status ReplicationCoordinatorImpl::checkCanServeReadsFor(OperationContext* opCtx,
                                                         const NamespaceString& ns,
                                                         bool slaveOk) {
    invariant(opCtx->lockState()->isLocked());
    return checkCanServeReadsFor_UNSAFE(opCtx, ns, slaveOk);
}

Status ReplicationCoordinatorImpl::checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                                const NamespaceString& ns,
                                                                bool slaveOk) {
    auto client = opCtx->getClient();
    bool isPrimaryOrSecondary = _canServeNonLocalReads.loadRelaxed();

    // Always allow reads from the direct client, no matter what.
    if (client->isInDirectClient()) {
        return Status::OK();
    }

    // Oplog reads are not allowed during STARTUP state, but we make an exception for internal
    // reads. Internal reads are required for cleaning up unfinished apply batches.
    if (!isPrimaryOrSecondary && getReplicationMode() == modeReplSet && ns.isOplog()) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
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

    auto session = OperationContextSession::get(opCtx);
    if (session && session->inMultiDocumentTransaction()) {
        if (!_canAcceptNonLocalWrites && !getTestCommandsEnabled()) {
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

bool ReplicationCoordinatorImpl::isInPrimaryOrSecondaryState() const {
    return _canServeNonLocalReads.loadRelaxed();
}

bool ReplicationCoordinatorImpl::shouldRelaxIndexConstraints(OperationContext* opCtx,
                                                             const NamespaceString& ns) {
    return !canAcceptWritesFor(opCtx, ns);
}

OID ReplicationCoordinatorImpl::getElectionId() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _electionId;
}

int ReplicationCoordinatorImpl::getMyId() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _getMyId_inlock();
}

int ReplicationCoordinatorImpl::_getMyId_inlock() const {
    const MemberConfig& self = _rsConfig.getMemberAt(_selfIndex);
    return self.getId();
}

Status ReplicationCoordinatorImpl::resyncData(OperationContext* opCtx, bool waitUntilCompleted) {
    _stopDataReplication(opCtx);
    auto finishedEvent = uassertStatusOK(_replExecutor->makeEvent());
    stdx::function<void()> f;
    if (waitUntilCompleted)
        f = [&finishedEvent, this]() { _replExecutor->signalEvent(finishedEvent); };

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _resetMyLastOpTimes_inlock();
    }
    // unlock before calling _startDataReplication().
    _startDataReplication(opCtx, f);
    if (waitUntilCompleted) {
        _replExecutor->waitForEvent(finishedEvent);
    }
    return Status::OK();
}

StatusWith<BSONObj> ReplicationCoordinatorImpl::prepareReplSetUpdatePositionCommand() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _topCoord->prepareReplSetUpdatePositionCommand(
        _getCurrentCommittedSnapshotOpTime_inlock());
}

Status ReplicationCoordinatorImpl::processReplSetGetStatus(
    BSONObjBuilder* response, ReplSetGetStatusResponseStyle responseStyle) {

    BSONObj initialSyncProgress;
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (responseStyle == ReplSetGetStatusResponseStyle::kInitialSync) {
        if (_initialSyncer) {
            initialSyncProgress = _initialSyncer->getInitialSyncProgress();
        }
    }


    Status result(ErrorCodes::InternalError, "didn't set status in prepareStatusResponse");
    _topCoord->prepareStatusResponse(
        TopologyCoordinator::ReplSetStatusArgs{
            _replExecutor->now(),
            static_cast<unsigned>(time(0) - serverGlobalParams.started),
            _getCurrentCommittedSnapshotOpTime_inlock(),
            initialSyncProgress,
            _storage->getLastStableCheckpointTimestamp(_service)},
        response,
        &result);
    return result;
}

void ReplicationCoordinatorImpl::fillIsMasterForReplSet(IsMasterResponse* response) {
    invariant(getSettings().usingReplSets());

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _topCoord->fillIsMasterForReplSet(response);

    OpTime lastOpTime = _getMyLastAppliedOpTime_inlock();
    response->setLastWrite(lastOpTime, lastOpTime.getTimestamp().getSecs());
    if (_currentCommittedSnapshot) {
        response->setLastMajorityWrite(_currentCommittedSnapshot.get(),
                                       _currentCommittedSnapshot->getTimestamp().getSecs());
    }

    if (response->isMaster() && !_canAcceptNonLocalWrites) {
        // Report that we are secondary to ismaster callers until drain completes.
        response->setIsMaster(false);
        response->setIsSecondary(true);
    }
}

void ReplicationCoordinatorImpl::appendSlaveInfoData(BSONObjBuilder* result) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _topCoord->fillMemberData(result);
}

ReplSetConfig ReplicationCoordinatorImpl::getConfig() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _rsConfig;
}

void ReplicationCoordinatorImpl::processReplSetGetConfig(BSONObjBuilder* result) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    result->append("config", _rsConfig.toBSON());
}

void ReplicationCoordinatorImpl::processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) {
    EventHandle evh;

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        evh = _processReplSetMetadata_inlock(replMetadata);
    }

    if (evh) {
        _replExecutor->waitForEvent(evh);
    }
}

void ReplicationCoordinatorImpl::cancelAndRescheduleElectionTimeout() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _cancelAndRescheduleElectionTimeout_inlock();
}

EventHandle ReplicationCoordinatorImpl::_processReplSetMetadata_inlock(
    const rpc::ReplSetMetadata& replMetadata) {
    if (replMetadata.getConfigVersion() != _rsConfig.getConfigVersion()) {
        return EventHandle();
    }
    return _updateTerm_inlock(replMetadata.getTerm());
}

bool ReplicationCoordinatorImpl::getMaintenanceMode() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _topCoord->getMaintenanceCount() > 0;
}

Status ReplicationCoordinatorImpl::setMaintenanceMode(bool activate) {
    if (getReplicationMode() != modeReplSet) {
        return Status(ErrorCodes::NoReplicationEnabled,
                      "can only set maintenance mode on replica set members");
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_topCoord->getRole() == TopologyCoordinator::Role::kCandidate) {
        return Status(ErrorCodes::NotSecondary, "currently running for election");
    }

    if (_getMemberState_inlock().primary()) {
        return Status(ErrorCodes::NotSecondary, "primaries can't modify maintenance mode");
    }

    int curMaintenanceCalls = _topCoord->getMaintenanceCount();
    if (activate) {
        log() << "going into maintenance mode with " << curMaintenanceCalls
              << " other maintenance mode tasks in progress" << rsLog;
        _topCoord->adjustMaintenanceCountBy(1);
    } else if (curMaintenanceCalls > 0) {
        invariant(_topCoord->getRole() == TopologyCoordinator::Role::kFollower);

        _topCoord->adjustMaintenanceCountBy(-1);

        log() << "leaving maintenance mode (" << curMaintenanceCalls - 1
              << " other maintenance mode tasks ongoing)" << rsLog;
    } else {
        warning() << "Attempted to leave maintenance mode but it is not currently active";
        return Status(ErrorCodes::OperationFailed, "already out of maintenance mode");
    }

    const PostMemberStateUpdateAction action =
        _updateMemberStateFromTopologyCoordinator_inlock(nullptr);
    lk.unlock();
    _performPostMemberStateUpdateAction(action);
    return Status::OK();
}

Status ReplicationCoordinatorImpl::processReplSetSyncFrom(OperationContext* opCtx,
                                                          const HostAndPort& target,
                                                          BSONObjBuilder* resultObj) {
    Status result(ErrorCodes::InternalError, "didn't set status in prepareSyncFromResponse");
    auto doResync = false;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _topCoord->prepareSyncFromResponse(target, resultObj, &result);
        // If we are in the middle of an initial sync, do a resync.
        doResync = result.isOK() && _initialSyncer && _initialSyncer->isActive();
    }

    if (doResync) {
        return resyncData(opCtx, false);
    }

    return result;
}

Status ReplicationCoordinatorImpl::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
    auto result = [=]() {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _topCoord->prepareFreezeResponse(_replExecutor->now(), secs, resultObj);
    }();
    if (!result.isOK()) {
        return result.getStatus();
    }

    if (TopologyCoordinator::PrepareFreezeResponseResult::kElectSelf == result.getValue()) {
        // If we just unfroze and ended our stepdown period and we are a one node replica set,
        // the topology coordinator will have gone into the candidate role to signal that we
        // need to elect ourself.
        _performPostMemberStateUpdateAction(kActionWinElection);
    }

    return Status::OK();
}

Status ReplicationCoordinatorImpl::processHeartbeat(const ReplSetHeartbeatArgs& args,
                                                    ReplSetHeartbeatResponse* response) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_rsConfigState == kConfigPreStart || _rsConfigState == kConfigStartingUp) {
            return Status(ErrorCodes::NotYetInitialized,
                          "Received heartbeat while still initializing replication system");
        }
    }

    auto senderHost(args.getSenderHost());

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    const Date_t now = _replExecutor->now();
    Status result =
        _topCoord->prepareHeartbeatResponse(now, args, _settings.ourSetName(), response);
    if ((result.isOK() || result == ErrorCodes::InvalidReplicaSetConfig) && _selfIndex < 0) {
        // If this node does not belong to the configuration it knows about, send heartbeats
        // back to any node that sends us a heartbeat, in case one of those remote nodes has
        // a configuration that contains us.  Chances are excellent that it will, since that
        // is the only reason for a remote node to send this node a heartbeat request.
        if (!senderHost.empty() && _seedList.insert(senderHost).second) {
            _scheduleHeartbeatToTarget_inlock(senderHost, -1, now);
        }
    } else if (result.isOK() && response->getConfigVersion() < args.getConfigVersion()) {
        // Schedule a heartbeat to the sender to fetch the new config.
        // We cannot cancel the enqueued heartbeat, but either this one or the enqueued heartbeat
        // will trigger reconfig, which cancels and reschedules all heartbeats.

        if (args.hasSenderHost()) {
            int senderIndex = _rsConfig.findMemberIndexByHostAndPort(senderHost);
            _scheduleHeartbeatToTarget_inlock(senderHost, senderIndex, now);
        }
    }
    return result;
}

Status ReplicationCoordinatorImpl::processReplSetReconfig(OperationContext* opCtx,
                                                          const ReplSetReconfigArgs& args,
                                                          BSONObjBuilder* resultObj) {
    log() << "replSetReconfig admin command received from client; new config: "
          << args.newConfigObj;

    stdx::unique_lock<stdx::mutex> lk(_mutex);

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
            severe() << "Unexpected _rsConfigState " << int(_rsConfigState);
            fassertFailed(18914);
    }

    invariant(_rsConfig.isInitialized());

    if (!args.force && !_getMemberState_inlock().primary()) {
        return Status(ErrorCodes::NotMaster,
                      str::stream()
                          << "replSetReconfig should only be run on PRIMARY, but my state is "
                          << _getMemberState_inlock().toString()
                          << "; use the \"force\" argument to override");
    }

    _setConfigState_inlock(kConfigReconfiguring);
    ScopeGuard configStateGuard =
        MakeGuard(lockAndCall, &lk, [=] { _setConfigState_inlock(kConfigSteady); });

    ReplSetConfig oldConfig = _rsConfig;
    lk.unlock();

    ReplSetConfig newConfig;
    BSONObj newConfigObj = args.newConfigObj;
    if (args.force) {
        newConfigObj = incrementConfigVersionByRandom(newConfigObj);
    }

    Status status = newConfig.initialize(newConfigObj, oldConfig.getReplicaSetId());
    if (!status.isOK()) {
        error() << "replSetReconfig got " << status << " while parsing " << newConfigObj;
        return Status(ErrorCodes::InvalidReplicaSetConfig, status.reason());
        ;
    }
    if (newConfig.getReplSetName() != _settings.ourSetName()) {
        str::stream errmsg;
        errmsg << "Attempting to reconfigure a replica set with name " << newConfig.getReplSetName()
               << ", but command line reports " << _settings.ourSetName() << "; rejecting";
        error() << std::string(errmsg);
        return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
    }

    StatusWith<int> myIndex = validateConfigForReconfig(
        _externalState.get(), oldConfig, newConfig, opCtx->getServiceContext(), args.force);
    if (!myIndex.isOK()) {
        error() << "replSetReconfig got " << myIndex.getStatus() << " while validating "
                << newConfigObj;
        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      myIndex.getStatus().reason());
    }

    log() << "replSetReconfig config object with " << newConfig.getNumMembers()
          << " members parses ok";

    if (!args.force) {
        status = checkQuorumForReconfig(
            _replExecutor.get(), newConfig, myIndex.getValue(), _topCoord->getTerm());
        if (!status.isOK()) {
            error() << "replSetReconfig failed; " << status;
            return status;
        }
    }

    status = _externalState->storeLocalConfigDocument(opCtx, newConfig.toBSON());
    if (!status.isOK()) {
        error() << "replSetReconfig failed to store config document; " << status;
        return status;
    }

    auto reconfigFinished = uassertStatusOK(_replExecutor->makeEvent());
    uassertStatusOK(_replExecutor->scheduleWork([ =, f = args.force, v = myIndex.getValue() ](
        const executor::TaskExecutor::CallbackArgs& cbData) {
        _finishReplSetReconfig(cbData, newConfig, f, v, reconfigFinished);
    }));
    configStateGuard.Dismiss();
    _replExecutor->waitForEvent(reconfigFinished);
    return Status::OK();
}

void ReplicationCoordinatorImpl::_finishReplSetReconfig(
    const executor::TaskExecutor::CallbackArgs& cbData,
    const ReplSetConfig& newConfig,
    const bool isForceReconfig,
    int myIndex,
    const executor::TaskExecutor::EventHandle& finishedEvent) {

    if (cbData.status == ErrorCodes::CallbackCanceled) {
        return;
    }
    auto opCtx = cc().makeOperationContext();
    boost::optional<Lock::GlobalWrite> globalExclusiveLock;
    if (isForceReconfig) {
        // Since it's a force reconfig, the primary node may not be electable after the
        // configuration change.  In case we are that primary node, finish the reconfig under the
        // global lock, so that the step down occurs safely.
        globalExclusiveLock.emplace(opCtx.get());
    }
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    invariant(_rsConfigState == kConfigReconfiguring);
    invariant(_rsConfig.isInitialized());

    // Do not conduct an election during a reconfig, as the node may not be electable post-reconfig.
    if (auto electionFinishedEvent = _cancelElectionIfNeeded_inlock()) {
        // Wait for the election to complete and the node's Role to be set to follower.
        _replExecutor
            ->onEvent(electionFinishedEvent,
                      [=](const executor::TaskExecutor::CallbackArgs& cbData) {
                          _finishReplSetReconfig(
                              cbData, newConfig, isForceReconfig, myIndex, finishedEvent);
                      })
            .status_with_transitional_ignore();
        return;
    }

    const ReplSetConfig oldConfig = _rsConfig;
    const PostMemberStateUpdateAction action =
        _setCurrentRSConfig_inlock(opCtx.get(), newConfig, myIndex);

    // On a reconfig we drop all snapshots so we don't mistakenely read from the wrong one.
    // For example, if we change the meaning of the "committed" snapshot from applied -> durable.
    _dropAllSnapshots_inlock();

    lk.unlock();
    _resetElectionInfoOnProtocolVersionUpgrade(opCtx.get(), oldConfig, newConfig);
    _performPostMemberStateUpdateAction(action);
    _replExecutor->signalEvent(finishedEvent);
}

Status ReplicationCoordinatorImpl::processReplSetInitiate(OperationContext* opCtx,
                                                          const BSONObj& configObj,
                                                          BSONObjBuilder* resultObj) {
    log() << "replSetInitiate admin command received from client";

    const auto replEnabled = _settings.usingReplSets();
    stdx::unique_lock<stdx::mutex> lk(_mutex);
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

    ScopeGuard configStateGuard =
        MakeGuard(lockAndCall, &lk, [=] { _setConfigState_inlock(kConfigUninitialized); });
    lk.unlock();

    ReplSetConfig newConfig;
    Status status = newConfig.initializeForInitiate(configObj);
    if (!status.isOK()) {
        error() << "replSet initiate got " << status << " while parsing " << configObj;
        return Status(ErrorCodes::InvalidReplicaSetConfig, status.reason());
    }
    if (newConfig.getReplSetName() != _settings.ourSetName()) {
        str::stream errmsg;
        errmsg << "Attempting to initiate a replica set with name " << newConfig.getReplSetName()
               << ", but command line reports " << _settings.ourSetName() << "; rejecting";
        error() << std::string(errmsg);
        return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
    }

    StatusWith<int> myIndex =
        validateConfigForInitiate(_externalState.get(), newConfig, opCtx->getServiceContext());
    if (!myIndex.isOK()) {
        error() << "replSet initiate got " << myIndex.getStatus() << " while validating "
                << configObj;
        return Status(ErrorCodes::InvalidReplicaSetConfig, myIndex.getStatus().reason());
    }

    log() << "replSetInitiate config object with " << newConfig.getNumMembers()
          << " members parses ok";

    // In pv1, the TopologyCoordinator has not set the term yet. It will be set to kInitialTerm if
    // the initiate succeeds so we pass that here.
    status = checkQuorumForInitiate(
        _replExecutor.get(),
        newConfig,
        myIndex.getValue(),
        newConfig.getProtocolVersion() == 1 ? OpTime::kInitialTerm : OpTime::kUninitializedTerm);

    if (!status.isOK()) {
        error() << "replSetInitiate failed; " << status;
        return status;
    }

    status = _externalState->initializeReplSetStorage(opCtx, newConfig.toBSON());
    if (!status.isOK()) {
        error() << "replSetInitiate failed to store config document or create the oplog; "
                << status;
        return status;
    }

    _replicationProcess->getConsistencyMarkers()->initializeMinValidDocument(opCtx);

    auto lastAppliedOpTime = getMyLastAppliedOpTime();

    // Since the JournalListener has not yet been set up, we must manually set our
    // durableOpTime.
    setMyLastDurableOpTime(lastAppliedOpTime);

    // Sets the initial data timestamp on the storage engine so it can assign a timestamp
    // to data on disk. We do this after writing the "initiating set" oplog entry.
    _storage->setInitialDataTimestamp(getServiceContext(), lastAppliedOpTime.getTimestamp());

    _finishReplSetInitiate(opCtx, newConfig, myIndex.getValue());

    // A configuration passed to replSetInitiate() with the current node as an arbiter
    // will fail validation with a "replSet initiate got ... while validating" reason.
    invariant(!newConfig.getMemberAt(myIndex.getValue()).isArbiter());
    _externalState->startThreads(_settings);
    _startDataReplication(opCtx);

    configStateGuard.Dismiss();
    return Status::OK();
}

void ReplicationCoordinatorImpl::_finishReplSetInitiate(OperationContext* opCtx,
                                                        const ReplSetConfig& newConfig,
                                                        int myIndex) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_rsConfigState == kConfigInitiating);
    invariant(!_rsConfig.isInitialized());
    auto action = _setCurrentRSConfig_inlock(opCtx, newConfig, myIndex);
    lk.unlock();
    _performPostMemberStateUpdateAction(action);
}

void ReplicationCoordinatorImpl::_setConfigState_inlock(ConfigState newState) {
    if (newState != _rsConfigState) {
        _rsConfigState = newState;
        _rsConfigStateChange.notify_all();
    }
}

ReplicationCoordinatorImpl::PostMemberStateUpdateAction
ReplicationCoordinatorImpl::_updateMemberStateFromTopologyCoordinator_inlock(
    OperationContext* opCtx) {
    {
        // We have to do this check even if our current and target state are the same as we might
        // have just failed a stepdown attempt and thus are staying in PRIMARY state but restoring
        // our ability to accept writes.
        bool canAcceptWrites = _topCoord->canAcceptWrites();
        if (canAcceptWrites != _canAcceptNonLocalWrites) {
            // We must be holding the global X lock to change _canAcceptNonLocalWrites.
            invariant(opCtx);
            invariant(opCtx->lockState()->isW());
        }
        _canAcceptNonLocalWrites = canAcceptWrites;
    }


    const MemberState newState = _topCoord->getMemberState();
    if (newState == _memberState) {
        if (_topCoord->getRole() == TopologyCoordinator::Role::kCandidate) {
            invariant(_rsConfig.getNumMembers() == 1 && _selfIndex == 0 &&
                      _rsConfig.getMemberAt(0).isElectable());
            if (isV1ElectionProtocol()) {
                // Start election in protocol version 1
                return kActionStartSingleNodeElection;
            }
            return kActionWinElection;
        }
        return kActionNone;
    }

    PostMemberStateUpdateAction result;
    if (_memberState.primary() || newState.removed() || newState.rollback()) {
        // Wake up any threads blocked in awaitReplication, close connections, etc.
        _replicationWaiterList.signalAndRemoveAll_inlock();
        // Wake up the optime waiter that is waiting for primary catch-up to finish.
        _opTimeWaiterList.signalAndRemoveAll_inlock();
        // If there are any pending stepdown command requests wake them up.
        _stepDownWaiters.notify_all();

        // _canAcceptNonLocalWrites should already be set above.
        invariant(!_canAcceptNonLocalWrites);

        serverGlobalParams.validateFeaturesAsMaster.store(false);
        result = kActionCloseAllConnections;
    } else {
        result = kActionFollowerModeStateChange;
    }

    // Exit catchup mode if we're in it and enable replication producer and applier on stepdown.
    if (_memberState.primary()) {
        if (_catchupState) {
            _catchupState->abort_inlock();
        }
        _applierState = ApplierState::Running;
        _externalState->startProducerIfStopped();
    }

    if (_memberState.secondary() && !newState.primary()) {
        // Switching out of SECONDARY, but not to PRIMARY.
        _canServeNonLocalReads.store(0U);
    } else if (!_memberState.primary() && newState.secondary()) {
        // Switching into SECONDARY, but not from PRIMARY.
        _canServeNonLocalReads.store(1U);
    }

    if (newState.secondary() && _topCoord->getRole() == TopologyCoordinator::Role::kCandidate) {
        // When transitioning to SECONDARY, the only way for _topCoord to report the candidate
        // role is if the configuration represents a single-node replica set.  In that case, the
        // overriding requirement is to elect this singleton node primary.
        invariant(_rsConfig.getNumMembers() == 1 && _selfIndex == 0 &&
                  _rsConfig.getMemberAt(0).isElectable());
        if (isV1ElectionProtocol()) {
            // Start election in protocol version 1
            result = kActionStartSingleNodeElection;
        } else {
            result = kActionWinElection;
        }
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
        //
        // TODO SERVER-19209 We also need to clear snapshots before a resync.
        _dropAllSnapshots_inlock();
    }

    // Upon transitioning out of ROLLBACK, we must clear any stable optime candidates that may have
    // been rolled back.
    if (_memberState.rollback()) {
        // Our 'lastApplied' optime at this point should be the rollback common point. We should
        // remove any stable optime candidates greater than the common point.
        auto lastApplied = _getMyLastAppliedOpTime_inlock();
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

    log() << "transition to " << newState << " from " << _memberState << rsLog;
    // Initializes the featureCompatibilityVersion to the latest value, because arbiters do not
    // receive the replicated version. This is to avoid bugs like SERVER-32639.
    if (newState.arbiter()) {
        serverGlobalParams.featureCompatibility.setVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);
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
            // In follower mode, or sub-mode so ensure replication is active
            _externalState->signalApplierToChooseNewSyncSource();
            break;
        case kActionCloseAllConnections:
            _externalState->closeConnections();
            _externalState->shardingOnStepDownHook();
            _externalState->stopNoopWriter();
            break;
        case kActionWinElection: {
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            if (isV1ElectionProtocol()) {
                invariant(_topCoord->getTerm() != OpTime::kUninitializedTerm);
                _electionId = OID::fromTerm(_topCoord->getTerm());
            } else {
                _electionId = OID::gen();
            }

            auto ts = LogicalClock::get(getServiceContext())->reserveTicks(1).asTimestamp();
            _topCoord->processWinElection(_electionId, ts);
            const PostMemberStateUpdateAction nextAction =
                _updateMemberStateFromTopologyCoordinator_inlock(nullptr);
            invariant(nextAction != kActionWinElection);
            lk.unlock();
            _performPostMemberStateUpdateAction(nextAction);
            lk.lock();
            if (!_getMemberState_inlock().primary()) {
                break;
            }
            // Notify all secondaries of the election win.
            _restartHeartbeats_inlock();
            if (isV1ElectionProtocol()) {
                invariant(!_catchupState);
                _catchupState = stdx::make_unique<CatchupState>(this);
                _catchupState->start_inlock();
            } else {
                _enterDrainMode_inlock();
            }
            break;
        }
        case kActionStartSingleNodeElection:
            // In protocol version 1, single node replset will run an election instead of
            // kActionWinElection as in protocol version 0.
            _startElectSelfV1(TopologyCoordinator::StartElectionReason::kElectionTimeout);
            break;
        default:
            severe() << "Unknown post member state update action " << static_cast<int>(action);
            fassertFailed(26010);
    }
}

void ReplicationCoordinatorImpl::CatchupState::start_inlock() {
    log() << "Entering primary catch-up mode.";

    // No catchup in single node replica set.
    if (_repl->_rsConfig.getNumMembers() == 1) {
        abort_inlock();
        return;
    }

    auto catchupTimeout = _repl->_rsConfig.getCatchUpTimeoutPeriod();

    // When catchUpTimeoutMillis is 0, we skip doing catchup entirely.
    if (catchupTimeout == ReplSetConfig::kCatchUpDisabled) {
        log() << "Skipping primary catchup since the catchup timeout is 0.";
        abort_inlock();
        return;
    }

    auto mutex = &_repl->_mutex;
    auto timeoutCB = [this, mutex](const CallbackArgs& cbData) {
        if (!cbData.status.isOK()) {
            return;
        }
        stdx::lock_guard<stdx::mutex> lk(*mutex);
        // Check whether the callback has been cancelled while holding mutex.
        if (cbData.myHandle.isCanceled()) {
            return;
        }
        log() << "Catchup timed out after becoming primary.";
        abort_inlock();
    };

    // Deal with infinity and overflow - no timeout.
    if (catchupTimeout == ReplSetConfig::kInfiniteCatchUpTimeout ||
        Date_t::max() - _repl->_replExecutor->now() <= catchupTimeout) {
        return;
    }
    // Schedule timeout callback.
    auto timeoutDate = _repl->_replExecutor->now() + catchupTimeout;
    auto status = _repl->_replExecutor->scheduleWorkAt(timeoutDate, timeoutCB);
    if (!status.isOK()) {
        log() << "Failed to schedule catchup timeout work.";
        abort_inlock();
        return;
    }
    _timeoutCbh = status.getValue();
}

void ReplicationCoordinatorImpl::CatchupState::abort_inlock() {
    invariant(_repl->_getMemberState_inlock().primary());

    log() << "Exited primary catch-up mode.";
    // Clean up its own members.
    if (_timeoutCbh) {
        _repl->_replExecutor->cancel(_timeoutCbh);
    }
    if (_waiter) {
        _repl->_opTimeWaiterList.remove_inlock(_waiter.get());
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
    if (*targetOpTime <= _repl->_getMyLastAppliedOpTime_inlock()) {
        log() << "Caught up to the latest optime known via heartbeats after becoming primary.";
        abort_inlock();
        return;
    }

    // Reset the target optime if it has changed.
    if (_waiter && _waiter->opTime == *targetOpTime) {
        return;
    }

    log() << "Heartbeats updated catchup target optime to " << *targetOpTime;
    if (_waiter) {
        _repl->_opTimeWaiterList.remove_inlock(_waiter.get());
    }
    auto targetOpTimeCB = [this, targetOpTime]() {
        // Double check the target time since stepdown may signal us too.
        if (*targetOpTime <= _repl->_getMyLastAppliedOpTime_inlock()) {
            log() << "Caught up to the latest known optime successfully after becoming primary.";
            abort_inlock();
        }
    };
    _waiter = stdx::make_unique<CallbackWaiter>(*targetOpTime, targetOpTimeCB);
    _repl->_opTimeWaiterList.add_inlock(_waiter.get());
}

Status ReplicationCoordinatorImpl::abortCatchupIfNeeded() {
    if (!isV1ElectionProtocol()) {
        return Status(ErrorCodes::CommandNotSupported,
                      "Primary catch-up is only supported by Protocol Version 1");
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_catchupState) {
        _catchupState->abort_inlock();
        return Status::OK();
    }
    return Status(ErrorCodes::IllegalOperation, "The node is not in catch-up mode.");
}

void ReplicationCoordinatorImpl::signalDropPendingCollectionsRemovedFromStorage() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _wakeReadyWaiters_inlock();
}

void ReplicationCoordinatorImpl::_enterDrainMode_inlock() {
    _applierState = ApplierState::Draining;
    _externalState->stopProducer();
}

Status ReplicationCoordinatorImpl::processReplSetFresh(const ReplSetFreshArgs& args,
                                                       BSONObjBuilder* resultObj) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    Status result(ErrorCodes::InternalError, "didn't set status in prepareFreshResponse");
    _topCoord->prepareFreshResponse(args, _replExecutor->now(), resultObj, &result);
    return result;
}

Status ReplicationCoordinatorImpl::processReplSetElect(const ReplSetElectArgs& args,
                                                       BSONObjBuilder* responseObj) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    Status result = Status(ErrorCodes::InternalError, "status not set by callback");
    _topCoord->prepareElectResponse(args, _replExecutor->now(), responseObj, &result);
    return result;
}

ReplicationCoordinatorImpl::PostMemberStateUpdateAction
ReplicationCoordinatorImpl::_setCurrentRSConfig_inlock(OperationContext* opCtx,
                                                       const ReplSetConfig& newConfig,
                                                       int myIndex) {
    invariant(_settings.usingReplSets());
    _cancelHeartbeats_inlock();
    _setConfigState_inlock(kConfigSteady);

    _topCoord->updateConfig(newConfig, myIndex, _replExecutor->now());

    // updateConfig() can change terms, so update our term shadow to match.
    _termShadow.store(_topCoord->getTerm());

    const ReplSetConfig oldConfig = _rsConfig;
    _rsConfig = newConfig;
    _protVersion.store(_rsConfig.getProtocolVersion());

    // Warn if this config has protocol version 0
    if (newConfig.getProtocolVersion() == 0 &&
        (!oldConfig.isInitialized() || oldConfig.getProtocolVersion() == 1)) {
        log() << startupWarningsLog;
        log() << "** WARNING: This replica set was configured with protocol version 0."
              << startupWarningsLog;
        log() << "**          This protocol version is deprecated and subject to be removed "
              << startupWarningsLog;
        log() << "**          in a future version." << startupWarningsLog;
    }

    // Warn if running --nojournal and writeConcernMajorityJournalDefault = false
    StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    if (storageEngine && !storageEngine->isDurable() &&
        (newConfig.getWriteConcernMajorityShouldJournal() &&
         (!oldConfig.isInitialized() || !oldConfig.getWriteConcernMajorityShouldJournal()))) {
        log() << startupWarningsLog;
        log() << "** WARNING: This replica set is running without journaling enabled but the "
              << startupWarningsLog;
        log() << "**          writeConcernMajorityJournalDefault option to the replica set config "
              << startupWarningsLog;
        log() << "**          is set to true. The writeConcernMajorityJournalDefault "
              << startupWarningsLog;
        log() << "**          option to the replica set config must be set to false "
              << startupWarningsLog;
        log() << "**          or w:majority write concerns will never complete."
              << startupWarningsLog;
        log() << startupWarningsLog;
    }

    log() << "New replica set config in use: " << _rsConfig.toBSON() << rsLog;
    _selfIndex = myIndex;
    if (_selfIndex >= 0) {
        log() << "This node is " << _rsConfig.getMemberAt(_selfIndex).getHostAndPort()
              << " in the config";
    } else {
        log() << "This node is not a member of the config";
    }

    _cancelCatchupTakeover_inlock();
    _cancelPriorityTakeover_inlock();
    _cancelAndRescheduleElectionTimeout_inlock();

    const PostMemberStateUpdateAction action =
        _updateMemberStateFromTopologyCoordinator_inlock(opCtx);
    if (_selfIndex >= 0) {
        // Don't send heartbeats if we're not in the config, if we get re-added one of the
        // nodes in the set will contact us.
        _startHeartbeats_inlock();
    }
    _updateLastCommittedOpTime_inlock();

    // Set election id if we're primary.
    if (oldConfig.isInitialized() && _memberState.primary()) {
        if (oldConfig.getProtocolVersion() > newConfig.getProtocolVersion()) {
            // Downgrade
            invariant(newConfig.getProtocolVersion() == 0);
            _electionId = OID::gen();
            auto ts = LogicalClock::get(getServiceContext())->reserveTicks(1).asTimestamp();
            _topCoord->setElectionInfo(_electionId, ts);
        } else if (oldConfig.getProtocolVersion() < newConfig.getProtocolVersion()) {
            // Upgrade
            invariant(newConfig.getProtocolVersion() == 1);
            invariant(_topCoord->getTerm() != OpTime::kUninitializedTerm);
            _electionId = OID::fromTerm(_topCoord->getTerm());
            auto ts = LogicalClock::get(getServiceContext())->reserveTicks(1).asTimestamp();
            _topCoord->setElectionInfo(_electionId, ts);
        }
    }

    return action;
}

void ReplicationCoordinatorImpl::_wakeReadyWaiters_inlock() {
    _replicationWaiterList.signalAndRemoveIf_inlock([this](Waiter* waiter) {
        return _doneWaitingForReplication_inlock(waiter->opTime, *waiter->writeConcern);
    });
}

Status ReplicationCoordinatorImpl::processReplSetUpdatePosition(const UpdatePositionArgs& updates,
                                                                long long* configVersion) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    Status status = Status::OK();
    bool somethingChanged = false;
    for (UpdatePositionArgs::UpdateIterator update = updates.updatesBegin();
         update != updates.updatesEnd();
         ++update) {
        status = _setLastOptime_inlock(*update, configVersion);
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_selfIndex == -1) {
        return true;
    }
    const MemberConfig& self = _rsConfig.getMemberAt(_selfIndex);
    return self.shouldBuildIndexes();
}

std::vector<HostAndPort> ReplicationCoordinatorImpl::getHostsWrittenTo(const OpTime& op,
                                                                       bool durablyWritten) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _topCoord->getHostsWrittenTo(op, durablyWritten);
}

std::vector<HostAndPort> ReplicationCoordinatorImpl::getOtherNodesInReplSet() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
    stdx::lock_guard<stdx::mutex> lock(_mutex);
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

WriteConcernOptions ReplicationCoordinatorImpl::getGetLastErrorDefault() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);

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

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _topCoord->unblacklistSyncSource(host, _replExecutor->now());
}

void ReplicationCoordinatorImpl::blacklistSyncSource(const HostAndPort& host, Date_t until) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _topCoord->blacklistSyncSource(host, until);
    _scheduleWorkAt(until, [=](const executor::TaskExecutor::CallbackArgs& cbData) {
        _unblacklistSyncSource(cbData, host);
    });
}

void ReplicationCoordinatorImpl::resetLastOpTimesFromOplog(OperationContext* opCtx,
                                                           DataConsistency consistency) {
    StatusWith<OpTime> lastOpTimeStatus = _externalState->loadLastOpTime(opCtx);
    OpTime lastOpTime;
    if (!lastOpTimeStatus.isOK()) {
        warning() << "Failed to load timestamp of most recently applied operation; "
                  << lastOpTimeStatus.getStatus();
    } else {
        lastOpTime = lastOpTimeStatus.getValue();
    }

    stdx::unique_lock<stdx::mutex> lock(_mutex);
    bool isRollbackAllowed = true;
    _setMyLastAppliedOpTime_inlock(lastOpTime, isRollbackAllowed, consistency);
    _setMyLastDurableOpTime_inlock(lastOpTime, isRollbackAllowed);
    _reportUpstream_inlock(std::move(lock));
    // Unlocked below.

    _externalState->setGlobalTimestamp(opCtx->getServiceContext(), lastOpTime.getTimestamp());
}

bool ReplicationCoordinatorImpl::shouldChangeSyncSource(
    const HostAndPort& currentSource,
    const rpc::ReplSetMetadata& replMetadata,
    boost::optional<rpc::OplogQueryMetadata> oqMetadata) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _topCoord->shouldChangeSyncSource(
        currentSource, replMetadata, oqMetadata, _replExecutor->now());
}

void ReplicationCoordinatorImpl::_updateLastCommittedOpTime_inlock() {
    if (_topCoord->updateLastCommittedOpTime()) {
        _setStableTimestampForStorage_inlock();
    }
    // Wake up any threads waiting for replication that now have their replication
    // check satisfied.  We must do this regardless of whether we updated the lastCommittedOpTime,
    // as lastCommittedOpTime may be based on durable optimes whereas some waiters may be
    // waiting on applied (but not necessarily durable) optimes.
    _wakeReadyWaiters_inlock();
}

boost::optional<OpTime> ReplicationCoordinatorImpl::_calculateStableOpTime_inlock(
    const std::set<OpTime>& candidates, const OpTime& commitPoint) {

    // No optime candidates.
    if (candidates.empty()) {
        return boost::none;
    }

    auto maximumStableTimestamp = commitPoint.getTimestamp();
    if (_canAcceptNonLocalWrites && _storage->supportsDocLocking(_service)) {
        // If the storage engine supports document level locking, then it is possible for oplog
        // writes to commit out of order. In that case, we don't want to set the stable timestamp
        // ahead of the all committed timestamp. This is not a problem for oplog application
        // because we only set lastApplied between batches when the all committed timestamp cannot
        // be behind. During oplog application the all committed timestamp can jump around since
        // we first write oplog entries to the oplog and then go back and apply them.
        //
        // If the all committed timestamp is less than the commit point, then we are guaranteed that
        // there are no stable timestamp candidates with a greater timestamp than the all committed
        // timestamp and a lower term than the commit point. Thus we can consider the all committed
        // timestamp to have the same term as the commit point. When a primary enters a new term, it
        // first storage-commits a 'new primary' oplog entry in the new term before accepting any
        // new writes. This will ensure that the all committed timestamp is in the new term before
        // any writes in the new term are replication committed.
        maximumStableTimestamp =
            std::min(_storage->getAllCommittedTimestamp(_service), commitPoint.getTimestamp());
    }
    const auto maximumStableOpTime = OpTime(maximumStableTimestamp, commitPoint.getTerm());

    // Find the greatest optime candidate that is less than or equal to the commit point.
    // To do this we first find the upper bound of 'commitPoint', which points to the smallest
    // element in 'candidates' that is greater than 'commitPoint'. We then step back one element,
    // which should give us the largest element in 'candidates' that is less than or equal to the
    // 'commitPoint'.
    auto upperBoundIter = candidates.upper_bound(maximumStableOpTime);

    // All optime candidates are greater than the commit point.
    if (upperBoundIter == candidates.begin()) {
        return boost::none;
    }
    // There is a valid stable optime.
    else {
        return *std::prev(upperBoundIter);
    }
}

void ReplicationCoordinatorImpl::_cleanupStableOpTimeCandidates(std::set<OpTime>* candidates,
                                                                OpTime stableOpTime) {
    // Discard optime candidates earlier than the current stable optime, since we don't need
    // them anymore. To do this, we find the lower bound of the 'stableOpTime' which is the first
    // element that is greater than or equal to the 'stableOpTime'. Then we discard everything up
    // to but not including this lower bound i.e. 'deletePoint'.
    auto deletePoint = candidates->lower_bound(stableOpTime);

    // Delete the entire range of unneeded optimes.
    candidates->erase(candidates->begin(), deletePoint);
}

boost::optional<OpTime> ReplicationCoordinatorImpl::calculateStableOpTime_forTest(
    const std::set<OpTime>& candidates, const OpTime& commitPoint) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _calculateStableOpTime_inlock(candidates, commitPoint);
}
void ReplicationCoordinatorImpl::cleanupStableOpTimeCandidates_forTest(std::set<OpTime>* candidates,
                                                                       OpTime stableOpTime) {
    _cleanupStableOpTimeCandidates(candidates, stableOpTime);
}

std::set<OpTime> ReplicationCoordinatorImpl::getStableOpTimeCandidates_forTest() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stableOpTimeCandidates;
}

boost::optional<OpTime> ReplicationCoordinatorImpl::getStableOpTime_forTest() {
    return _getStableOpTime_inlock();
}

boost::optional<OpTime> ReplicationCoordinatorImpl::_getStableOpTime_inlock() {
    auto commitPoint = _topCoord->getLastCommittedOpTime();
    if (_currentCommittedSnapshot) {
        auto snapshotOpTime = *_currentCommittedSnapshot;
        invariant(snapshotOpTime.getTimestamp() <= commitPoint.getTimestamp());
        invariant(snapshotOpTime <= commitPoint);
    }

    // Compute the current stable optime.
    auto stableOpTime = _calculateStableOpTime_inlock(_stableOpTimeCandidates, commitPoint);
    if (stableOpTime) {
        // By definition, the stable optime should never be greater than the commit point.
        invariant(stableOpTime->getTimestamp() <= commitPoint.getTimestamp());
        invariant(*stableOpTime <= commitPoint);
    }

    return stableOpTime;
}

void ReplicationCoordinatorImpl::_setStableTimestampForStorage_inlock() {

    // Get the current stable optime.
    auto stableOpTime = _getStableOpTime_inlock();

    // If there is a valid stable optime, set it for the storage engine, and then remove any
    // old, unneeded stable optime candidates.
    if (stableOpTime) {
        LOG(2) << "Setting replication's stable optime to " << stableOpTime.value();

        if (!testingSnapshotBehaviorInIsolation) {
            // Update committed snapshot and wake up any threads waiting on read concern or
            // write concern.
            if (_updateCommittedSnapshot_inlock(stableOpTime.get())) {
                // Update the stable timestamp for the storage engine.
                _storage->setStableTimestamp(getServiceContext(), stableOpTime->getTimestamp());
            }
        }
        _cleanupStableOpTimeCandidates(&_stableOpTimeCandidates, stableOpTime.get());
    }
}

void ReplicationCoordinatorImpl::advanceCommitPoint(const OpTime& committedOpTime) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _advanceCommitPoint_inlock(committedOpTime);
}

void ReplicationCoordinatorImpl::_advanceCommitPoint_inlock(const OpTime& committedOpTime) {
    if (_topCoord->advanceLastCommittedOpTime(committedOpTime)) {
        if (_getMemberState_inlock().arbiter()) {
            // Arbiters do not store replicated data, so we consider their data trivially
            // consistent.
            _setMyLastAppliedOpTime_inlock(committedOpTime, false, DataConsistency::Consistent);
        }

        _setStableTimestampForStorage_inlock();
        // Even if we have no new snapshot, we need to notify waiters that the commit point moved.
        _externalState->notifyOplogMetadataWaiters(committedOpTime);
    }
}

OpTime ReplicationCoordinatorImpl::getLastCommittedOpTime() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _topCoord->getLastCommittedOpTime();
}

Status ReplicationCoordinatorImpl::processReplSetRequestVotes(
    OperationContext* opCtx,
    const ReplSetRequestVotesArgs& args,
    ReplSetRequestVotesResponse* response) {
    if (!isV1ElectionProtocol()) {
        return {ErrorCodes::BadValue, "not using election protocol v1"};
    }

    auto termStatus = updateTerm(opCtx, args.getTerm());
    if (!termStatus.isOK() && termStatus.code() != ErrorCodes::StaleTerm)
        return termStatus;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _topCoord->processReplSetRequestVotes(args, response);
    }

    if (!args.isADryRun() && response->getVoteGranted()) {
        LastVote lastVote{args.getTerm(), args.getCandidateIndex()};

        Status status = _externalState->storeLocalLastVoteDocument(opCtx, lastVote);
        if (!status.isOK()) {
            error() << "replSetRequestVotes failed to store LastVote document; " << status;
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

    stdx::lock_guard<stdx::mutex> lk(_mutex);

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

bool ReplicationCoordinatorImpl::isV1ElectionProtocol() const {
    return _protVersion.load() == 1;
}

bool ReplicationCoordinatorImpl::getWriteConcernMajorityShouldJournal() {
    return getConfig().getWriteConcernMajorityShouldJournal();
}

bool ReplicationCoordinatorImpl::getWriteConcernMajorityShouldJournal_inlock() const {
    return _rsConfig.getWriteConcernMajorityShouldJournal();
}

Status ReplicationCoordinatorImpl::processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                                      ReplSetHeartbeatResponse* response) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_rsConfigState == kConfigPreStart || _rsConfigState == kConfigStartingUp) {
            return Status(ErrorCodes::NotYetInitialized,
                          "Received heartbeat while still initializing replication system");
        }
    }

    Status result(ErrorCodes::InternalError, "didn't set status in prepareHeartbeatResponse");
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    auto senderHost(args.getSenderHost());
    const Date_t now = _replExecutor->now();
    result = _topCoord->prepareHeartbeatResponseV1(now, args, _settings.ourSetName(), response);

    if ((result.isOK() || result == ErrorCodes::InvalidReplicaSetConfig) && _selfIndex < 0) {
        // If this node does not belong to the configuration it knows about, send heartbeats
        // back to any node that sends us a heartbeat, in case one of those remote nodes has
        // a configuration that contains us.  Chances are excellent that it will, since that
        // is the only reason for a remote node to send this node a heartbeat request.
        if (!senderHost.empty() && _seedList.insert(senderHost).second) {
            _scheduleHeartbeatToTarget_inlock(senderHost, -1, now);
        }
    } else if (result.isOK() && response->getConfigVersion() < args.getConfigVersion()) {
        // Schedule a heartbeat to the sender to fetch the new config.
        // We cannot cancel the enqueued heartbeat, but either this one or the enqueued heartbeat
        // will trigger reconfig, which cancels and reschedules all heartbeats.
        if (args.hasSender()) {
            int senderIndex = _rsConfig.findMemberIndexByHostAndPort(senderHost);
            _scheduleHeartbeatToTarget_inlock(senderHost, senderIndex, now);
        }
    }
    return result;
}

void ReplicationCoordinatorImpl::summarizeAsHtml(ReplSetHtmlSummary* output) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    output->setSelfUptime(time(0) - serverGlobalParams.started);
    output->setNow(_replExecutor->now());

    _topCoord->summarizeAsHtml(output);
}

long long ReplicationCoordinatorImpl::getTerm() {
    // Note: no mutex acquisition here, as we are reading an Atomic variable.
    return _termShadow.load();
}

EventHandle ReplicationCoordinatorImpl::updateTerm_forTest(
    long long term, TopologyCoordinator::UpdateTermResult* updateResult) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    EventHandle finishEvh;
    finishEvh = _updateTerm_inlock(term, updateResult);
    return finishEvh;
}

Status ReplicationCoordinatorImpl::updateTerm(OperationContext* opCtx, long long term) {
    // Term is only valid if we are replicating.
    if (getReplicationMode() != modeReplSet) {
        return {ErrorCodes::BadValue, "cannot supply 'term' without active replication"};
    }

    if (!isV1ElectionProtocol()) {
        // Do not update if not in V1 protocol.
        return Status::OK();
    }

    // Check we haven't acquired any lock, because potential stepdown needs global lock.
    dassert(!opCtx->lockState()->isLocked() || opCtx->lockState()->isNoop());
    TopologyCoordinator::UpdateTermResult updateTermResult;
    EventHandle finishEvh;

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
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
    if (!isV1ElectionProtocol()) {
        LOG(3) << "Cannot update term in election protocol version 0";
        return EventHandle();
    }

    auto now = _replExecutor->now();
    TopologyCoordinator::UpdateTermResult localUpdateTermResult = _topCoord->updateTerm(term, now);
    {
        if (localUpdateTermResult == TopologyCoordinator::UpdateTermResult::kUpdatedTerm) {
            _termShadow.store(term);
            _cancelPriorityTakeover_inlock();
            _cancelAndRescheduleElectionTimeout_inlock();
        }
    }

    if (updateTermResult) {
        *updateTermResult = localUpdateTermResult;
    }

    if (localUpdateTermResult == TopologyCoordinator::UpdateTermResult::kTriggerStepDown) {
        if (!_pendingTermUpdateDuringStepDown || *_pendingTermUpdateDuringStepDown < term) {
            _pendingTermUpdateDuringStepDown = term;
        }
        if (_topCoord->prepareForUnconditionalStepDown()) {
            log() << "stepping down from primary, because a new term has begun: " << term;
            return _stepDownStart();
        } else {
            LOG(2) << "Updated term but not triggering stepdown because we are already in the "
                      "process of stepping down";
        }
    }
    return EventHandle();
}

void ReplicationCoordinatorImpl::waitUntilSnapshotCommitted(OperationContext* opCtx,
                                                            const Timestamp& untilSnapshot) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    uassert(ErrorCodes::NotYetInitialized,
            "Cannot use snapshots until replica set is finished initializing.",
            _rsConfigState != kConfigUninitialized && _rsConfigState != kConfigInitiating);
    while (!_currentCommittedSnapshot ||
           _currentCommittedSnapshot->getTimestamp() < untilSnapshot) {
        opCtx->waitForConditionOrInterrupt(_currentCommittedSnapshotCond, lock);
    }
}

size_t ReplicationCoordinatorImpl::getNumUncommittedSnapshots() {
    return _uncommittedSnapshotsSize.load();
}

MONGO_FAIL_POINT_DEFINE(disableSnapshotting);

bool ReplicationCoordinatorImpl::_updateCommittedSnapshot_inlock(
    const OpTime& newCommittedSnapshot) {
    if (testingSnapshotBehaviorInIsolation) {
        return false;
    }

    // If we are in ROLLBACK state, do not set any new _currentCommittedSnapshot, as it will be
    // cleared at the end of rollback anyway.
    if (_memberState.rollback()) {
        log() << "Not updating committed snapshot because we are in rollback";
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
        invariant(newCommittedSnapshot >= _currentCommittedSnapshot);
    }
    if (MONGO_FAIL_POINT(disableSnapshotting))
        return false;
    _currentCommittedSnapshot = newCommittedSnapshot;
    _currentCommittedSnapshotCond.notify_all();

    _externalState->updateCommittedSnapshot(newCommittedSnapshot);

    // Wake up any threads waiting for read concern or write concern.
    _wakeReadyWaiters_inlock();
    return true;
}

void ReplicationCoordinatorImpl::dropAllSnapshots() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
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

void ReplicationCoordinatorImpl::_resetElectionInfoOnProtocolVersionUpgrade(
    OperationContext* opCtx, const ReplSetConfig& oldConfig, const ReplSetConfig& newConfig) {

    // On protocol version upgrade, reset last vote as if I just learned the term 0 from other
    // nodes.
    if (!oldConfig.isInitialized() ||
        oldConfig.getProtocolVersion() >= newConfig.getProtocolVersion()) {
        return;
    }
    invariant(newConfig.getProtocolVersion() == 1);

    const LastVote lastVote{OpTime::kInitialTerm, -1};
    fassert(40445, _externalState->storeLocalLastVoteDocument(opCtx, lastVote));
}

CallbackHandle ReplicationCoordinatorImpl::_scheduleWorkAt(Date_t when, const CallbackFn& work) {
    auto cbh = _replExecutor->scheduleWorkAt(when, [work](const CallbackArgs& args) {
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

    WriteConcernOptions writeConcern(wc);
    if (writeConcern.syncMode == WriteConcernOptions::SyncMode::UNSET) {
        if (writeConcern.wMode == WriteConcernOptions::kMajority &&
            getWriteConcernMajorityShouldJournal()) {
            writeConcern.syncMode = WriteConcernOptions::SyncMode::JOURNAL;
        } else {
            writeConcern.syncMode = WriteConcernOptions::SyncMode::NONE;
        }
    }
    return writeConcern;
}

CallbackFn ReplicationCoordinatorImpl::_wrapAsCallbackFn(const stdx::function<void()>& work) {
    return [work](const CallbackArgs& cbData) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }

        work();
    };
}

Status ReplicationCoordinatorImpl::stepUpIfEligible() {
    if (!isV1ElectionProtocol()) {
        return Status(ErrorCodes::CommandNotSupported,
                      "Step-up command is only supported by Protocol Version 1");
    }

    _startElectSelfIfEligibleV1(TopologyCoordinator::StartElectionReason::kStepUpRequest);
    EventHandle finishEvent;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        finishEvent = _electionFinishedEvent;
    }
    if (finishEvent.isValid()) {
        _replExecutor->waitForEvent(finishEvent);
    }
    auto state = getMemberState();
    if (state.primary()) {
        return Status::OK();
    }
    return Status(ErrorCodes::CommandFailed, "Election failed.");
}

ReplSettings::IndexPrefetchConfig ReplicationCoordinatorImpl::getIndexPrefetchConfig() const {
    stdx::lock_guard<stdx::mutex> lock(_indexPrefetchMutex);
    return _indexPrefetchConfig;
}

void ReplicationCoordinatorImpl::setIndexPrefetchConfig(
    const ReplSettings::IndexPrefetchConfig cfg) {
    stdx::lock_guard<stdx::mutex> lock(_indexPrefetchMutex);
    _indexPrefetchConfig = cfg;
}

executor::TaskExecutor::EventHandle ReplicationCoordinatorImpl::_cancelElectionIfNeeded_inlock() {
    if (_topCoord->getRole() != TopologyCoordinator::Role::kCandidate) {
        return {};
    }
    if (isV1ElectionProtocol()) {
        invariant(_voteRequester);
        _voteRequester->cancel();
    } else {
        invariant(_freshnessChecker);
        _freshnessChecker->cancel();
        if (_electCmdRunner) {
            _electCmdRunner->cancel();
        }
    }
    return _electionFinishedEvent;
}

int64_t ReplicationCoordinatorImpl::_nextRandomInt64_inlock(int64_t limit) {
    return _random.nextInt64(limit);
}

}  // namespace repl
}  // namespace mongo

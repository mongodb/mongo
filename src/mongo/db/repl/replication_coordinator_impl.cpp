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
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/check_quorum_for_config_change.h"
#include "mongo/db/repl/data_replicator_external_state_initial_sync.h"
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/db/repl/handshake_args.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/old_update_position_args.h"
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
#include "mongo/db/storage/mmap_v1/dur.h"
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

MONGO_FP_DECLARE(transitionToPrimaryHangBeforeTakingGlobalExclusiveLock);

using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using CallbackFn = executor::TaskExecutor::CallbackFn;
using CallbackHandle = executor::TaskExecutor::CallbackHandle;
using EventHandle = executor::TaskExecutor::EventHandle;
using executor::NetworkInterface;
using NextAction = Fetcher::NextAction;

namespace {

const char kLocalDB[] = "local";

MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncAttempts, int, 10);

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

const Seconds kNoopWriterPeriod(10);
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
    if (settings.isMaster() || settings.isSlave()) {
        return ReplicationCoordinator::modeMasterSlave;
    }
    return ReplicationCoordinator::modeNone;
}

InitialSyncerOptions createInitialSyncerOptions(
    ReplicationCoordinator* replCoord, ReplicationCoordinatorExternalState* externalState) {
    InitialSyncerOptions options;
    options.getMyLastOptime = [replCoord]() { return replCoord->getMyLastAppliedOpTime(); };
    options.setMyLastOptime = [replCoord, externalState](const OpTime& opTime) {
        replCoord->setMyLastAppliedOpTime(opTime);
        externalState->setGlobalTimestamp(replCoord->getServiceContext(), opTime.getTimestamp());
    };
    options.resetOptimes = [replCoord]() { replCoord->resetMyLastOpTimes(); };
    options.getSlaveDelay = [replCoord]() { return replCoord->getSlaveDelaySecs(); };
    options.syncSourceSelector = replCoord;
    options.replBatchLimitBytes = dur::UncommittedBytesLimit;
    options.oplogFetcherMaxFetcherRestarts = externalState->getOplogFetcherMaxFetcherRestarts();
    return options;
}
}  // namespace

std::string ReplicationCoordinatorImpl::SnapshotInfo::toString() const {
    BSONObjBuilder bob;
    bob.append("optime", opTime.toBSON());
    bob.append("name-id", name.toString());
    return bob.obj().toString();
}

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
      _canAcceptNonLocalWrites(!(settings.usingReplSets() || settings.isSlave())),
      _canServeNonLocalReads(0U),
      _replicationProcess(replicationProcess),
      _storage(storage),
      _random(prngSeed) {

    invariant(_service);

    if (!isReplEnabled()) {
        return;
    }

    _externalState->setupNoopWriter(kNoopWriterPeriod);
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
        return _currentCommittedSnapshot->opTime;
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
    auto rbid = _replicationProcess->getRollbackID(opCtx);
    if (!rbid.isOK()) {
        if (rbid.getStatus() == ErrorCodes::NamespaceNotFound) {
            log() << "Did not find local Rollback ID document at startup. Creating one.";
            auto initializingStatus = _replicationProcess->initializeRollbackID(opCtx);
            fassertStatusOK(40424, initializingStatus);
        } else {
            severe() << "Error loading local Rollback ID document at startup; " << rbid.getStatus();
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
    Status status = localConfig.initialize(cfg.getValue());
    if (!status.isOK()) {
        error() << "Locally stored replica set configuration does not parse; See "
                   "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config "
                   "for information on how to recover from this. Got \""
                << status << "\" while parsing " << cfg.getValue();
        fassertFailedNoTrace(28545);
    }

    // Read the last op from the oplog after cleaning up any partially applied batches.
    _externalState->cleanUpLastApplyBatch(opCtx);
    auto lastOpTimeStatus = _externalState->loadLastOpTime(opCtx);

    // Use a callback here, because _finishLoadLocalConfig calls isself() which requires
    // that the server's networking layer be up and running and accepting connections, which
    // doesn't happen until startReplication finishes.
    auto handle =
        _replExecutor->scheduleWork(stdx::bind(&ReplicationCoordinatorImpl::_finishLoadLocalConfig,
                                               this,
                                               stdx::placeholders::_1,
                                               localConfig,
                                               lastOpTimeStatus,
                                               lastVote));
    if (handle == ErrorCodes::ShutdownInProgress) {
        handle = CallbackHandle{};
    }
    fassertStatusOK(40446, handle);
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

    stdx::unique_lock<stdx::mutex> lock(_mutex);
    invariant(_rsConfigState == kConfigStartingUp);
    const PostMemberStateUpdateAction action =
        _setCurrentRSConfig_inlock(localConfig, myIndex.getValue());
    if (!lastOpTime.isNull()) {
        _setMyLastAppliedOpTime_inlock(lastOpTime, false);
        _setMyLastDurableOpTime_inlock(lastOpTime, false);
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
        auto opCtx = cc().makeOperationContext();
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
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (!_inShutdown) {
            // Start steady replication, since we already have data.
            _externalState->startSteadyStateReplication(opCtx, this);
        }
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
            _setMyLastAppliedOpTime_inlock(lastApplied.opTime, false);
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
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _setConfigState_inlock(kConfigReplicationDisabled);
        return;
    }

    {
        OID rid = _externalState->ensureMe(opCtx);

        stdx::lock_guard<stdx::mutex> lk(_mutex);
        fassert(18822, !_inShutdown);
        _setConfigState_inlock(kConfigStartingUp);
        _myRID = rid;
        _topCoord->getMyMemberData()->setRid(rid);
    }

    if (!_settings.usingReplSets()) {
        // Must be Master/Slave
        invariant(_settings.isMaster() || _settings.isSlave());
        _externalState->startMasterSlave(opCtx);
        return;
    }

    _replExecutor->startup();

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _topCoord->setStorageEngineSupportsReadCommitted(
            _externalState->isReadCommittedSupportedByStorageEngine(opCtx));
    }

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
        _signalStepDownWaiter_inlock();
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
    if (_topCoord->getRole() == TopologyCoordinator::Role::leader) {
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

    const PostMemberStateUpdateAction action = _updateMemberStateFromTopologyCoordinator_inlock();
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

    // Exit drain mode when the buffer is empty in the current term and we're in Draining mode.
    if (_applierState != ApplierState::Draining || termWhenBufferIsEmpty != _topCoord->getTerm()) {
        return;
    }
    _applierState = ApplierState::Stopped;
    _drainFinishedCond.notify_all();

    invariant(_getMemberState_inlock().primary());
    invariant(!_canAcceptNonLocalWrites);
    _canAcceptNonLocalWrites = true;

    lk.unlock();
    OpTime firstOpTime = _externalState->onTransitionToPrimary(opCtx, isV1ElectionProtocol());
    lk.lock();
    _topCoord->setFirstOpTimeOfMyTerm(firstOpTime);

    // Must calculate the commit level again because firstOpTimeOfMyTerm wasn't set when we logged
    // our election in onTransitionToPrimary(), above.
    _updateLastCommittedOpTime_inlock();

    log() << "transition to primary complete; database writes are now permitted" << rsLog;
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

Status ReplicationCoordinatorImpl::setLastOptimeForSlave(const OID& rid, const Timestamp& ts) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    massert(28576,
            "Received an old style replication progress update, which is only used for Master/"
            "Slave replication now, but this node is not using Master/Slave replication. "
            "This is likely caused by an old (pre-2.6) member syncing from this node.",
            getReplicationMode() == modeMasterSlave);

    // term == -1 for master-slave
    OpTime opTime(ts, OpTime::kUninitializedTerm);
    MemberData* memberData = _topCoord->findMemberDataByRid(rid);
    if (memberData) {
        memberData->advanceLastAppliedOpTime(opTime, _replExecutor->now());
    } else {
        auto* memberData = _topCoord->addSlaveMemberData(rid);
        memberData->setLastAppliedOpTime(opTime, _replExecutor->now());
    }
    _updateLastCommittedOpTime_inlock();
    return Status::OK();
}

void ReplicationCoordinatorImpl::setMyHeartbeatMessage(const std::string& msg) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _topCoord->setMyHeartbeatMessage(_replExecutor->now(), msg);
}

void ReplicationCoordinatorImpl::setMyLastAppliedOpTimeForward(const OpTime& opTime) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    if (opTime > _getMyLastAppliedOpTime_inlock()) {
        const bool allowRollback = false;
        _setMyLastAppliedOpTime_inlock(opTime, allowRollback);
        _reportUpstream_inlock(std::move(lock));
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
    _setMyLastAppliedOpTime_inlock(opTime, false);
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
    _setMyLastAppliedOpTime_inlock(OpTime(), true);
    _setMyLastDurableOpTime_inlock(OpTime(), true);
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
                                                                bool isRollbackAllowed) {
    auto* myMemberData = _topCoord->getMyMemberData();
    invariant(isRollbackAllowed || myMemberData->getLastAppliedOpTime() <= opTime);
    myMemberData->setLastAppliedOpTime(opTime, _replExecutor->now());
    _updateLastCommittedOpTime_inlock();
    _opTimeWaiterList.signalAndRemoveIf_inlock(
        [opTime](Waiter* waiter) { return waiter->opTime <= opTime; });
}

void ReplicationCoordinatorImpl::_setMyLastDurableOpTime_inlock(const OpTime& opTime,
                                                                bool isRollbackAllowed) {
    auto* myMemberData = _topCoord->getMyMemberData();
    invariant(isRollbackAllowed || myMemberData->getLastDurableOpTime() <= opTime);
    myMemberData->setLastDurableOpTime(opTime, _replExecutor->now());
    _updateLastCommittedOpTime_inlock();
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
    // We should never wait for replication if we are holding any locks, because this can
    // potentially block for long time while doing network activity.
    if (opCtx->lockState()->isLocked()) {
        return {ErrorCodes::IllegalOperation,
                "Waiting for replication not allowed while holding a lock"};
    }

    const bool isMajorityReadConcern =
        readConcern.getLevel() == ReadConcernLevel::kMajorityReadConcern;

    if (readConcern.getArgsClusterTime() &&
        readConcern.getLevel() != ReadConcernLevel::kMajorityReadConcern &&
        readConcern.getLevel() != ReadConcernLevel::kLocalReadConcern) {
        return {ErrorCodes::BadValue,
                "Only readConcern level 'majority' or 'local' is allowed when specifying "
                "afterClusterTime"};
    }

    if (isMajorityReadConcern && !getSettings().isMajorityReadConcernEnabled()) {
        // This is an opt-in feature. Fail if the user didn't opt-in.
        return {ErrorCodes::ReadConcernMajorityNotEnabled,
                "Majority read concern requested, but server was not started with "
                "--enableMajorityReadConcern."};
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
    if (!readConcern.getArgsClusterTime() && !readConcern.getArgsOpTime()) {
        return Status::OK();
    }

    if (getReplicationMode() != repl::ReplicationCoordinator::modeReplSet) {
        // For master/slave and standalone nodes, readAfterOpTime is not supported, so we return an
        // error. However, we consider all writes "committed" and can treat MajorityReadConcern as
        // LocalReadConcern, which is immediately satisfied since there is no OpTime to wait for.
        return {ErrorCodes::NotAReplicaSet,
                "node needs to be a replica set member to use read concern"};
    }

    if (readConcern.getArgsClusterTime()) {
        return _waitUntilClusterTimeForRead(opCtx, readConcern);
    } else {
        return _waitUntilOpTimeForReadDeprecated(opCtx, readConcern);
    }
}

Status ReplicationCoordinatorImpl::_waitUntilOpTime(OperationContext* opCtx,
                                                    bool isMajorityReadConcern,
                                                    OpTime targetOpTime) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    if (isMajorityReadConcern && !_externalState->snapshotsEnabled()) {
        return {ErrorCodes::CommandNotSupported,
                "Current storage engine does not support majority readConcerns"};
    }

    auto getCurrentOpTime = [this, isMajorityReadConcern]() {
        return isMajorityReadConcern ? _getCurrentCommittedSnapshotOpTime_inlock()
                                     : _getMyLastAppliedOpTime_inlock();
    };

    if (isMajorityReadConcern && targetOpTime > getCurrentOpTime()) {
        LOG(1) << "waitUntilOpTime: waiting for optime:" << targetOpTime
               << " to be in a snapshot -- current snapshot: " << getCurrentOpTime();
    }

    while (targetOpTime > getCurrentOpTime()) {
        if (_inShutdown) {
            return {ErrorCodes::ShutdownInProgress, "Shutdown in progress"};
        }

        // If we are doing a majority read concern we only need to wait for a new snapshot.
        if (isMajorityReadConcern) {
            // Wait for a snapshot that meets our needs (< targetOpTime).
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

        auto waitStatus = opCtx->waitForConditionOrInterruptNoAssert(condVar, lock);
        if (!waitStatus.isOK()) {
            return waitStatus;
        }
    }

    return Status::OK();
}

Status ReplicationCoordinatorImpl::_waitUntilClusterTimeForRead(
    OperationContext* opCtx, const ReadConcernArgs& readConcern) {
    auto clusterTime = *readConcern.getArgsClusterTime();
    invariant(clusterTime != LogicalTime::kUninitialized);

    // convert clusterTime to opTime so it can be used by the _opTimeWaiterList for wait on
    // readConcern level local.
    auto targetOpTime = OpTime(clusterTime.asTimestamp(), OpTime::kUninitializedTerm);
    invariant(!readConcern.getArgsOpTime());

    const bool isMajorityReadConcern =
        readConcern.getLevel() == ReadConcernLevel::kMajorityReadConcern;

    return _waitUntilOpTime(opCtx, isMajorityReadConcern, targetOpTime);
}

// TODO: remove when SERVER-29729 is done
Status ReplicationCoordinatorImpl::_waitUntilOpTimeForReadDeprecated(
    OperationContext* opCtx, const ReadConcernArgs& readConcern) {
    const bool isMajorityReadConcern =
        readConcern.getLevel() == ReadConcernLevel::kMajorityReadConcern;

    const auto targetOpTime = readConcern.getArgsOpTime().value_or(OpTime());
    return _waitUntilOpTime(opCtx, isMajorityReadConcern, targetOpTime);
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

Status ReplicationCoordinatorImpl::_setLastOptime_inlock(
    const OldUpdatePositionArgs::UpdateInfo& args, long long* configVersion) {
    if (_selfIndex == -1) {
        // Ignore updates when we're in state REMOVED
        return Status(ErrorCodes::NotMasterOrSecondary,
                      "Received replSetUpdatePosition command but we are in state REMOVED");
    }
    invariant(getReplicationMode() == modeReplSet);

    if (args.memberId < 0) {
        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << args.memberId
            << " which is negative and therefore invalid";
        LOG(1) << errmsg;
        return Status(ErrorCodes::NodeNotFound, errmsg);
    }

    if (args.memberId == _rsConfig.getMemberAt(_selfIndex).getId()) {
        // Do not let remote nodes tell us what our optime is.
        return Status::OK();
    }

    LOG(2) << "received notification that node with memberID " << args.memberId
           << " in config with version " << args.cfgver
           << " has durably reached optime: " << args.ts;

    if (args.cfgver != _rsConfig.getConfigVersion()) {
        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << args.memberId
            << " whose config version of " << args.cfgver << " doesn't match our config version of "
            << _rsConfig.getConfigVersion();
        LOG(1) << errmsg;
        *configVersion = _rsConfig.getConfigVersion();
        return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
    }

    auto* memberData = _topCoord->findMemberDataByMemberId(args.memberId);
    if (!memberData) {
        invariant(!_rsConfig.findMemberByID(args.memberId));

        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << args.memberId
            << " which doesn't exist in our config";
        LOG(1) << errmsg;
        return Status(ErrorCodes::NodeNotFound, errmsg);
    }

    invariant(args.memberId == memberData->getMemberId());

    LOG(3) << "Node with memberID " << args.memberId << " has durably applied operations through "
           << memberData->getLastDurableOpTime() << " and has applied operations through "
           << memberData->getLastAppliedOpTime()
           << "; updating to new durable operation with timestamp " << args.ts;

    auto now(_replExecutor->now());
    bool advancedOpTime = memberData->advanceLastAppliedOpTime(args.ts, now);
    advancedOpTime = memberData->advanceLastDurableOpTime(args.ts, now) || advancedOpTime;

    // Only update committed optime if the remote optimes increased.
    if (advancedOpTime) {
        _updateLastCommittedOpTime_inlock();
    }

    _cancelAndRescheduleLivenessUpdate_inlock(args.memberId);
    return Status::OK();
}

Status ReplicationCoordinatorImpl::_setLastOptime_inlock(const UpdatePositionArgs::UpdateInfo& args,
                                                         long long* configVersion) {
    if (_selfIndex == -1) {
        // Ignore updates when we're in state REMOVED.
        return Status(ErrorCodes::NotMasterOrSecondary,
                      "Received replSetUpdatePosition command but we are in state REMOVED");
    }
    invariant(getReplicationMode() == modeReplSet);

    if (args.memberId < 0) {
        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << args.memberId
            << " which is negative and therefore invalid";
        LOG(1) << errmsg;
        return Status(ErrorCodes::NodeNotFound, errmsg);
    }

    if (args.memberId == _rsConfig.getMemberAt(_selfIndex).getId()) {
        // Do not let remote nodes tell us what our optime is.
        return Status::OK();
    }

    LOG(2) << "received notification that node with memberID " << args.memberId
           << " in config with version " << args.cfgver
           << " has reached optime: " << args.appliedOpTime
           << " and is durable through: " << args.durableOpTime;

    if (args.cfgver != _rsConfig.getConfigVersion()) {
        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << args.memberId
            << " whose config version of " << args.cfgver << " doesn't match our config version of "
            << _rsConfig.getConfigVersion();
        LOG(1) << errmsg;
        *configVersion = _rsConfig.getConfigVersion();
        return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
    }

    auto* memberData = _topCoord->findMemberDataByMemberId(args.memberId);
    if (!memberData) {
        invariant(!_rsConfig.findMemberByID(args.memberId));

        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << args.memberId
            << " which doesn't exist in our config";
        LOG(1) << errmsg;
        return Status(ErrorCodes::NodeNotFound, errmsg);
    }

    invariant(args.memberId == memberData->getMemberId());

    LOG(3) << "Node with memberID " << args.memberId << " currently has optime "
           << memberData->getLastAppliedOpTime() << " durable through "
           << memberData->getLastDurableOpTime() << "; updating to optime " << args.appliedOpTime
           << " and durable through " << args.durableOpTime;


    auto now(_replExecutor->now());
    bool advancedOpTime = memberData->advanceLastAppliedOpTime(args.appliedOpTime, now);
    advancedOpTime =
        memberData->advanceLastDurableOpTime(args.durableOpTime, now) || advancedOpTime;

    // Only update committed optime if the remote optimes increased.
    if (advancedOpTime) {
        _updateLastCommittedOpTime_inlock();
    }

    _cancelAndRescheduleLivenessUpdate_inlock(args.memberId);
    return Status::OK();
}

bool ReplicationCoordinatorImpl::_doneWaitingForReplication_inlock(
    const OpTime& opTime, SnapshotName minSnapshot, const WriteConcernOptions& writeConcern) {
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
        if (_externalState->snapshotsEnabled()) {
            // Make sure we have a valid "committed" snapshot up to the needed optime.
            if (!_currentCommittedSnapshot) {
                return false;
            }

            // Wait for the "current" snapshot to advance to/past the opTime.
            const auto haveSnapshot = (_currentCommittedSnapshot->opTime >= opTime &&
                                       _currentCommittedSnapshot->name >= minSnapshot);
            if (!haveSnapshot) {
                LOG(1) << "Required snapshot optime: " << opTime << " is not yet part of the "
                       << "current 'committed' snapshot: " << *_currentCommittedSnapshot;
                return false;
            }

            // Fallthrough to wait for "majority" write concern.
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
    auto status =
        _awaitReplication_inlock(&lock, opCtx, opTime, SnapshotName::min(), fixedWriteConcern);
    return {std::move(status), duration_cast<Milliseconds>(timer.elapsed())};
}

ReplicationCoordinator::StatusAndDuration
ReplicationCoordinatorImpl::awaitReplicationOfLastOpForClient(
    OperationContext* opCtx, const WriteConcernOptions& writeConcern) {
    Timer timer;
    WriteConcernOptions fixedWriteConcern = populateUnsetWriteConcernOptionsSyncMode(writeConcern);
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    const auto& clientInfo = ReplClientInfo::forClient(opCtx->getClient());
    auto status = _awaitReplication_inlock(
        &lock, opCtx, clientInfo.getLastOp(), clientInfo.getLastSnapshot(), fixedWriteConcern);
    return {std::move(status), duration_cast<Milliseconds>(timer.elapsed())};
}

Status ReplicationCoordinatorImpl::_awaitReplication_inlock(
    stdx::unique_lock<stdx::mutex>* lock,
    OperationContext* opCtx,
    const OpTime& opTime,
    SnapshotName minSnapshot,
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

    if (replMode == modeMasterSlave && writeConcern.wMode == WriteConcernOptions::kMajority) {
        // with master/slave, majority is equivalent to w=1
        return Status::OK();
    }

    if (opTime.isNull() && minSnapshot == SnapshotName::min()) {
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

        if (_topCoord->isStepDownPending()) {
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
    while (!_doneWaitingForReplication_inlock(opTime, minSnapshot, writeConcern)) {

        if (_inShutdown) {
            return {ErrorCodes::ShutdownInProgress, "Replication is being shut down"};
        }

        auto status = opCtx->waitForConditionOrInterruptNoAssertUntil(condVar, *lock, wTimeoutDate);
        if (!status.isOK()) {
            return status.getStatus();
        }

        if (status.getValue() == stdx::cv_status::timeout) {
            if (Command::testCommandsEnabled) {
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

Status ReplicationCoordinatorImpl::stepDown(OperationContext* opCtx,
                                            const bool force,
                                            const Milliseconds& waitTime,
                                            const Milliseconds& stepdownTime) {

    const Date_t startTime = _replExecutor->now();
    const Date_t stepDownUntil = startTime + stepdownTime;
    const Date_t waitUntil = startTime + waitTime;

    if (!getMemberState().primary()) {
        // Note this check is inherently racy - it's always possible for the node to
        // stepdown from some other path before we acquire the global shared lock, but
        // that's okay because we are resiliant to that happening in _stepDownContinue.
        return {ErrorCodes::NotMaster, "not primary so can't step down"};
    }

    Lock::GlobalLock globalReadLock(
        opCtx, MODE_S, durationCount<Milliseconds>(stepdownTime), Lock::GlobalLock::EnqueueOnly());

    // We've requested the global shared lock which will stop new writes from coming in,
    // but existing writes could take a long time to finish, so kill all user operations
    // to help us get the global lock faster.
    _externalState->killAllUserOperations(opCtx);

    globalReadLock.waitForLock(durationCount<Milliseconds>(stepdownTime));

    if (!globalReadLock.isLocked()) {
        return {ErrorCodes::ExceededTimeLimit,
                "Could not acquire the global shared lock within the amount of time "
                "specified that we should step down for"};
    }

    PostMemberStateUpdateAction action = kActionNone;
    try {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        opCtx->checkForInterrupt();
        if (!_tryToStepDown_inlock(waitUntil, stepDownUntil, force)) {
            // We send out a fresh round of heartbeats because stepping down successfully
            // without {force: true} is dependent on timely heartbeat data.
            _restartHeartbeats_inlock();
            do {
                opCtx->waitForConditionOrInterruptUntil(
                    _stepDownWaiters, lk, std::min(stepDownUntil, waitUntil));
            } while (!_tryToStepDown_inlock(waitUntil, stepDownUntil, force));
        }
        action = _updateMemberStateFromTopologyCoordinator_inlock();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    _performPostMemberStateUpdateAction(action);

    // Schedule work to (potentially) step back up once the stepdown period has ended.
    _scheduleWorkAt(
        stepDownUntil,
        stdx::bind(&ReplicationCoordinatorImpl::_handleTimePassing, this, stdx::placeholders::_1));

    return Status::OK();
}

void ReplicationCoordinatorImpl::_signalStepDownWaiter_inlock() {
    _stepDownWaiters.notify_all();
}

bool ReplicationCoordinatorImpl::_tryToStepDown_inlock(const Date_t waitUntil,
                                                       const Date_t stepDownUntil,
                                                       const bool force) {
    if (_topCoord->getRole() != TopologyCoordinator::Role::leader) {
        uasserted(ErrorCodes::NotMaster,
                  "Already stepped down from primary while processing step down request");
    }
    const Date_t now = _replExecutor->now();
    if (now >= stepDownUntil) {
        uasserted(ErrorCodes::ExceededTimeLimit,
                  "By the time we were ready to step down, we were already past the "
                  "time we were supposed to step down until");
    }

    const bool forceNow = force && (now >= waitUntil);
    OpTime lastApplied = _getMyLastAppliedOpTime_inlock();

    if (forceNow) {
        return _topCoord->stepDown(stepDownUntil, forceNow);
    }

    auto tagStatus = _rsConfig.findCustomWriteMode(ReplSetConfig::kMajorityWriteConcernModeName);
    invariant(tagStatus.isOK());

    // Check if a majority of nodes have reached the last applied optime
    // and there exist an electable node that has my last applied optime.
    if (_topCoord->haveTaggedNodesReachedOpTime(lastApplied, tagStatus.getValue(), false) &&
        _topCoord->stepDown(stepDownUntil, forceNow)) {
        return true;
    }

    // Stepdown attempt failed.

    // Check waitUntil after at least one stepdown attempt, so that stepdown could succeed even if
    // secondaryCatchUpPeriodSecs == 0.
    if (now >= waitUntil) {
        uasserted(ErrorCodes::ExceededTimeLimit,
                  str::stream() << "No electable secondaries caught up as of "
                                << dateToISOStringLocal(now)
                                << "Please use the replSetStepDown command with the argument "
                                << "{force: true} to force node to step down.");
    }
    return false;
}

void ReplicationCoordinatorImpl::_handleTimePassing(
    const executor::TaskExecutor::CallbackArgs& cbData) {
    if (!cbData.status.isOK()) {
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
    if (_settings.usingReplSets()) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (getReplicationMode() == modeReplSet && _getMemberState_inlock().primary()) {
            return true;
        }
        return false;
    }

    if (!_settings.isSlave())
        return true;


    // TODO(dannenberg) replAllDead is bad and should be removed when master slave is removed
    if (replAllDead) {
        return false;
    }

    if (_settings.isMaster()) {
        // if running with --master --slave, allow.
        return true;
    }

    return false;
}

bool ReplicationCoordinatorImpl::canAcceptWritesForDatabase(OperationContext* opCtx,
                                                            StringData dbName) {
    // The answer isn't meaningful unless we hold the global lock.
    invariant(opCtx->lockState()->isLocked());
    return canAcceptWritesForDatabase_UNSAFE(opCtx, dbName);
}

bool ReplicationCoordinatorImpl::canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                                                   StringData dbName) {
    // _canAcceptNonLocalWrites is always true for standalone nodes, always false for nodes
    // started with --slave, and adjusted based on primary+drain state in replica sets.
    //
    // That is, stand-alone nodes, non-slave nodes and drained replica set primaries can always
    // accept writes.  Similarly, writes are always permitted to the "local" database.  Finally,
    // in the event that a node is started with --slave and --master, we allow writes unless the
    // master/slave system has set the replAllDead flag.
    if (_canAcceptNonLocalWrites) {
        return true;
    }
    if (dbName == kLocalDB) {
        return true;
    }
    return !replAllDead && _settings.isMaster();
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
    if (!canWriteToDB) {
        return false;
    }

    // Even if we think we can write to the database we need to make sure we're not trying
    // to write to the oplog in ROLLBACK.
    // If we can accept non local writes (ie we're PRIMARY) then we must not be in ROLLBACK.
    // This check is redundant of the check of _memberState below, but since this can be checked
    // without locking, we do it as an optimization.
    if (_canAcceptNonLocalWrites) {
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

    if (client->isInDirectClient()) {
        return Status::OK();
    }
    if (canAcceptWritesFor_UNSAFE(opCtx, ns)) {
        return Status::OK();
    }
    if (getReplicationMode() == modeMasterSlave) {
        return Status::OK();
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

OID ReplicationCoordinatorImpl::getMyRID() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _getMyRID_inlock();
}

OID ReplicationCoordinatorImpl::_getMyRID_inlock() const {
    return _myRID;
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

StatusWith<BSONObj> ReplicationCoordinatorImpl::prepareReplSetUpdatePositionCommand(
    ReplicationCoordinator::ReplSetUpdatePositionCommandStyle commandStyle) const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _topCoord->prepareReplSetUpdatePositionCommand(
        commandStyle, _getCurrentCommittedSnapshotOpTime_inlock());
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
            initialSyncProgress},
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
        OpTime majorityOpTime = _currentCommittedSnapshot->opTime;
        response->setLastMajorityWrite(majorityOpTime, majorityOpTime.getTimestamp().getSecs());
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
    if (_topCoord->getRole() == TopologyCoordinator::Role::candidate) {
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
        invariant(_topCoord->getRole() == TopologyCoordinator::Role::follower);

        _topCoord->adjustMaintenanceCountBy(-1);

        log() << "leaving maintenance mode (" << curMaintenanceCalls - 1
              << " other maintenance mode tasks ongoing)" << rsLog;
    } else {
        warning() << "Attempted to leave maintenance mode but it is not currently active";
        return Status(ErrorCodes::OperationFailed, "already out of maintenance mode");
    }

    const PostMemberStateUpdateAction action = _updateMemberStateFromTopologyCoordinator_inlock();
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
    ScopeGuard configStateGuard = MakeGuard(
        lockAndCall,
        &lk,
        stdx::bind(&ReplicationCoordinatorImpl::_setConfigState_inlock, this, kConfigSteady));

    ReplSetConfig oldConfig = _rsConfig;
    lk.unlock();

    ReplSetConfig newConfig;
    BSONObj newConfigObj = args.newConfigObj;
    if (args.force) {
        newConfigObj = incrementConfigVersionByRandom(newConfigObj);
    }

    Status status = newConfig.initialize(
        newConfigObj, oldConfig.getProtocolVersion() == 1, oldConfig.getReplicaSetId());
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
        status = checkQuorumForReconfig(_replExecutor.get(), newConfig, myIndex.getValue());
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
    uassertStatusOK(
        _replExecutor->scheduleWork(stdx::bind(&ReplicationCoordinatorImpl::_finishReplSetReconfig,
                                               this,
                                               stdx::placeholders::_1,
                                               newConfig,
                                               args.force,
                                               myIndex.getValue(),
                                               reconfigFinished)));
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
                      stdx::bind(&ReplicationCoordinatorImpl::_finishReplSetReconfig,
                                 this,
                                 stdx::placeholders::_1,
                                 newConfig,
                                 isForceReconfig,
                                 myIndex,
                                 finishedEvent))
            .status_with_transitional_ignore();
        return;
    }

    const ReplSetConfig oldConfig = _rsConfig;
    const PostMemberStateUpdateAction action = _setCurrentRSConfig_inlock(newConfig, myIndex);

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

    ScopeGuard configStateGuard = MakeGuard(
        lockAndCall,
        &lk,
        stdx::bind(
            &ReplicationCoordinatorImpl::_setConfigState_inlock, this, kConfigUninitialized));
    lk.unlock();

    ReplSetConfig newConfig;
    Status status = newConfig.initializeForInitiate(configObj, true);
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

    status = checkQuorumForInitiate(_replExecutor.get(), newConfig, myIndex.getValue());

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

    auto lastAppliedOpTime = getMyLastAppliedOpTime();

    // Since the JournalListener has not yet been set up, we must manually set our
    // durableOpTime.
    setMyLastDurableOpTime(lastAppliedOpTime);

    // Sets the initial data timestamp on the storage engine so it can assign a timestamp
    // to data on disk. We do this after writing the "initiating set" oplog entry.
    auto initialDataTS = SnapshotName(lastAppliedOpTime.getTimestamp().asULL());
    _storage->setInitialDataTimestamp(opCtx, initialDataTS);

    _finishReplSetInitiate(newConfig, myIndex.getValue());

    // A configuration passed to replSetInitiate() with the current node as an arbiter
    // will fail validation with a "replSet initiate got ... while validating" reason.
    invariant(!newConfig.getMemberAt(myIndex.getValue()).isArbiter());
    _externalState->startThreads(_settings);
    _startDataReplication(opCtx);

    configStateGuard.Dismiss();
    return Status::OK();
}

void ReplicationCoordinatorImpl::_finishReplSetInitiate(const ReplSetConfig& newConfig,
                                                        int myIndex) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_rsConfigState == kConfigInitiating);
    invariant(!_rsConfig.isInitialized());
    auto action = _setCurrentRSConfig_inlock(newConfig, myIndex);
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
ReplicationCoordinatorImpl::_updateMemberStateFromTopologyCoordinator_inlock() {
    const MemberState newState = _topCoord->getMemberState();
    if (newState == _memberState) {
        if (_topCoord->getRole() == TopologyCoordinator::Role::candidate) {
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
        _canAcceptNonLocalWrites = false;
        serverGlobalParams.featureCompatibility.validateFeaturesAsMaster.store(false);
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

    if (newState.secondary() && _topCoord->getRole() == TopologyCoordinator::Role::candidate) {
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

    if (newState.readable() && !_memberState.readable()) {
        // When we transition to a readable state from a non-readable one, force the SnapshotThread
        // to take a snapshot, if it is running. This is because it never takes snapshots when not
        // in readable states.
        _externalState->forceSnapshotCreation();
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

    if (_memberState.rollback()) {
        // Ensure that no snapshots were created while we were in rollback.
        invariant(!_currentCommittedSnapshot);
        invariant(_uncommittedSnapshots.empty());
    }

    // If we are transitioning from secondary, cancel any scheduled takeovers.
    if (_memberState.secondary()) {
        _cancelCatchupTakeover_inlock();
        _cancelPriorityTakeover_inlock();
    }

    _memberState = newState;
    log() << "transition to " << newState.toString() << rsLog;

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
                _updateMemberStateFromTopologyCoordinator_inlock();
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
            _startElectSelfV1();
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
    if (catchupTimeout == Milliseconds::zero()) {
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
ReplicationCoordinatorImpl::_setCurrentRSConfig_inlock(const ReplSetConfig& newConfig,
                                                       int myIndex) {
    invariant(_settings.usingReplSets());
    _cancelHeartbeats_inlock();
    _setConfigState_inlock(kConfigSteady);

    _topCoord->updateConfig(newConfig, myIndex, _replExecutor->now());
    const ReplSetConfig oldConfig = _rsConfig;
    _rsConfig = newConfig;
    _protVersion.store(_rsConfig.getProtocolVersion());
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

    const PostMemberStateUpdateAction action = _updateMemberStateFromTopologyCoordinator_inlock();
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
        return _doneWaitingForReplication_inlock(
            waiter->opTime, SnapshotName::min(), *waiter->writeConcern);
    });
}

Status ReplicationCoordinatorImpl::processReplSetUpdatePosition(
    const OldUpdatePositionArgs& updates, long long* configVersion) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    Status status = Status::OK();
    bool somethingChanged = false;
    for (OldUpdatePositionArgs::UpdateIterator update = updates.updatesBegin();
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

Status ReplicationCoordinatorImpl::processHandshake(OperationContext* opCtx,
                                                    const HandshakeArgs& handshake) {
    LOG(2) << "Received handshake " << handshake.toBSON();

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (getReplicationMode() != modeMasterSlave) {
        return Status(ErrorCodes::IllegalOperation,
                      "The handshake command is only used for master/slave replication");
    }

    auto* memberData = _topCoord->findMemberDataByRid(handshake.getRid());
    if (memberData) {
        return Status::OK();  // nothing to do
    }

    memberData = _topCoord->addSlaveMemberData(handshake.getRid());
    memberData->setHostAndPort(_externalState->getClientHostAndPort(opCtx));

    return Status::OK();
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
    /* skip self in master-slave mode because our own HostAndPort is unknown */
    const bool skipSelf = getReplicationMode() == modeMasterSlave;
    return _topCoord->getHostsWrittenTo(op, durablyWritten, skipSelf);
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

    if (getReplicationMode() == modeMasterSlave) {
        if (!writeConcern.wMode.empty()) {
            return Status(ErrorCodes::UnknownReplWriteConcern,
                          "Cannot use named write concern modes in master-slave");
        }
        // No way to know how many slaves there are, so assume any numeric mode is possible.
        return Status::OK();
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
    _scheduleWorkAt(until,
                    stdx::bind(&ReplicationCoordinatorImpl::_unblacklistSyncSource,
                               this,
                               stdx::placeholders::_1,
                               host));
}

void ReplicationCoordinatorImpl::resetLastOpTimesFromOplog(OperationContext* opCtx) {
    StatusWith<OpTime> lastOpTimeStatus = _externalState->loadLastOpTime(opCtx);
    OpTime lastOpTime;
    if (!lastOpTimeStatus.isOK()) {
        warning() << "Failed to load timestamp of most recently applied operation; "
                  << lastOpTimeStatus.getStatus();
    } else {
        lastOpTime = lastOpTimeStatus.getValue();
    }

    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _setMyLastAppliedOpTime_inlock(lastOpTime, true);
    _setMyLastDurableOpTime_inlock(lastOpTime, true);
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
        _updateCommitPoint_inlock();
    }
    // Wake up any threads waiting for replication that now have their replication
    // check satisfied.  We must do this regardless of whether we updated the lastCommittedOpTime,
    // as lastCommittedOpTime may be based on durable optimes whereas some waiters may be
    // waiting on applied (but not necessarily durable) optimes.
    _wakeReadyWaiters_inlock();
}

void ReplicationCoordinatorImpl::advanceCommitPoint(const OpTime& committedOpTime) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _advanceCommitPoint_inlock(committedOpTime);
}

void ReplicationCoordinatorImpl::_advanceCommitPoint_inlock(const OpTime& committedOpTime) {
    if (_topCoord->advanceLastCommittedOpTime(committedOpTime)) {
        if (_getMemberState_inlock().arbiter()) {
            _setMyLastAppliedOpTime_inlock(committedOpTime, false);
        }

        _updateCommitPoint_inlock();
    }
}

void ReplicationCoordinatorImpl::_updateCommitPoint_inlock() {
    auto committedOpTime = _topCoord->getLastCommittedOpTime();
    _externalState->notifyOplogMetadataWaiters(committedOpTime);

    auto maxSnapshotForOpTime = SnapshotInfo{committedOpTime, SnapshotName::max()};

    if (!_uncommittedSnapshots.empty() && _uncommittedSnapshots.front() <= maxSnapshotForOpTime) {
        // At least one uncommitted snapshot is ready to be blessed as committed.

        // Seek to the first entry > the commit point. Previous element must be <=.
        const auto onePastCommitPoint = std::upper_bound(
            _uncommittedSnapshots.begin(), _uncommittedSnapshots.end(), maxSnapshotForOpTime);
        const auto newSnapshot = *std::prev(onePastCommitPoint);

        // Forget about all snapshots <= the new commit point.
        _uncommittedSnapshots.erase(_uncommittedSnapshots.begin(), onePastCommitPoint);
        _uncommittedSnapshotsSize.store(_uncommittedSnapshots.size());

        // Update committed snapshot and wake up any threads waiting on read concern or
        // write concern.
        //
        // This function is only called on secondaries, so only threads waiting for
        // committed snapshot need to be woken up.
        _updateCommittedSnapshot_inlock(newSnapshot);
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

void ReplicationCoordinatorImpl::prepareReplMetadata(OperationContext* opCtx,
                                                     const BSONObj& metadataRequestObj,
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
        rbid = fassertStatusOK(40427, _replicationProcess->getRollbackID(opCtx));
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
    } else if (result.isOK()) {
        // Update liveness for sending node.
        auto* memberData = _topCoord->findMemberDataByMemberId(args.getSenderId());
        if (!memberData) {
            return result;
        }
        memberData->updateLiveness(_replExecutor->now());
    }
    return result;
}

void ReplicationCoordinatorImpl::summarizeAsHtml(ReplSetHtmlSummary* output) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    // TODO(dannenberg) consider putting both optimes into the htmlsummary.
    output->setSelfOptime(_getMyLastAppliedOpTime_inlock());
    output->setSelfUptime(time(0) - serverGlobalParams.started);
    output->setNow(_replExecutor->now());

    _topCoord->summarizeAsHtml(output);
}

long long ReplicationCoordinatorImpl::getTerm() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _topCoord->getTerm();
}

EventHandle ReplicationCoordinatorImpl::updateTerm_forTest(
    long long term, TopologyCoordinator::UpdateTermResult* updateResult) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    EventHandle finishEvh;
    finishEvh = _updateTerm_inlock(term, updateResult);
    if (!finishEvh) {
        auto finishEvhStatus = _replExecutor->makeEvent();
        invariantOK(finishEvhStatus.getStatus());
        finishEvh = finishEvhStatus.getValue();
        _replExecutor->signalEvent(finishEvh);
    }
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
    dassert(!opCtx->lockState()->isLocked());
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
            _cancelPriorityTakeover_inlock();
            _cancelAndRescheduleElectionTimeout_inlock();
        }
    }

    if (updateTermResult) {
        *updateTermResult = localUpdateTermResult;
    }

    if (localUpdateTermResult == TopologyCoordinator::UpdateTermResult::kTriggerStepDown) {
        log() << "stepping down from primary, because a new term has begun: " << term;
        _topCoord->prepareForStepDown();
        return _stepDownStart();
    }
    return EventHandle();
}

SnapshotName ReplicationCoordinatorImpl::reserveSnapshotName(OperationContext* opCtx) {
    auto reservedName = SnapshotName(_snapshotNameGenerator.addAndFetch(1));
    dassert(reservedName > SnapshotName::min());
    dassert(reservedName < SnapshotName::max());
    if (opCtx) {
        ReplClientInfo::forClient(opCtx->getClient()).setLastSnapshot(reservedName);
    }
    return reservedName;
}

void ReplicationCoordinatorImpl::forceSnapshotCreation() {
    _externalState->forceSnapshotCreation();
}

void ReplicationCoordinatorImpl::waitUntilSnapshotCommitted(OperationContext* opCtx,
                                                            const SnapshotName& untilSnapshot) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    while (!_currentCommittedSnapshot || _currentCommittedSnapshot->name < untilSnapshot) {
        opCtx->waitForConditionOrInterrupt(_currentCommittedSnapshotCond, lock);
    }
}

size_t ReplicationCoordinatorImpl::getNumUncommittedSnapshots() {
    return _uncommittedSnapshotsSize.load();
}

void ReplicationCoordinatorImpl::createSnapshot(OperationContext* opCtx,
                                                OpTime timeOfSnapshot,
                                                SnapshotName name) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _externalState->createSnapshot(opCtx, name);
    auto snapshotInfo = SnapshotInfo{timeOfSnapshot, name};

    if (timeOfSnapshot <= _topCoord->getLastCommittedOpTime()) {
        // This snapshot is ready to be marked as committed.
        invariant(_uncommittedSnapshots.empty());
        _updateCommittedSnapshot_inlock(snapshotInfo);
        return;
    }

    if (!_uncommittedSnapshots.empty()) {
        invariant(snapshotInfo > _uncommittedSnapshots.back());
        // The name must independently be newer.
        invariant(snapshotInfo.name > _uncommittedSnapshots.back().name);
        // Technically, we could delete older snapshots from the same optime since we will only ever
        // want the newest. However, multiple snapshots on the same optime will be very rare so it
        // isn't worth the effort and potential bugs that would introduce.
    }
    _uncommittedSnapshots.push_back(snapshotInfo);
    _uncommittedSnapshotsSize.store(_uncommittedSnapshots.size());
}

void ReplicationCoordinatorImpl::_updateCommittedSnapshot_inlock(
    SnapshotInfo newCommittedSnapshot) {
    invariant(!newCommittedSnapshot.opTime.isNull());
    invariant(newCommittedSnapshot.opTime <= _topCoord->getLastCommittedOpTime());
    if (_currentCommittedSnapshot) {
        invariant(newCommittedSnapshot.opTime >= _currentCommittedSnapshot->opTime);
        invariant(newCommittedSnapshot.name > _currentCommittedSnapshot->name);
    }
    if (!_uncommittedSnapshots.empty())
        invariant(newCommittedSnapshot < _uncommittedSnapshots.front());

    _currentCommittedSnapshot = newCommittedSnapshot;
    _currentCommittedSnapshotCond.notify_all();

    _externalState->updateCommittedSnapshot(newCommittedSnapshot.name);

    // Wake up any threads waiting for read concern or write concern.
    _wakeReadyWaiters_inlock();
}

void ReplicationCoordinatorImpl::dropAllSnapshots() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _dropAllSnapshots_inlock();
}

void ReplicationCoordinatorImpl::_dropAllSnapshots_inlock() {
    _uncommittedSnapshots.clear();
    _uncommittedSnapshotsSize.store(_uncommittedSnapshots.size());
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
    return fassertStatusOK(28800, cbh);
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

    _startElectSelfIfEligibleV1(StartElectionV1Reason::kStepUpRequest);
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
    if (_topCoord->getRole() != TopologyCoordinator::Role::candidate) {
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

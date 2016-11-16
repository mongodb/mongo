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
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/global_timestamp.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/check_quorum_for_config_change.h"
#include "mongo/db/repl/data_replicator_external_state_impl.h"
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/db/repl/freshness_scanner.h"
#include "mongo/db/repl/handshake_args.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/old_update_position_args.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_set_html_summary.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replica_set_config_checks.h"
#include "mongo/db/repl/replication_executor.h"
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
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {

using CallbackFn = executor::TaskExecutor::CallbackFn;
using CallbackHandle = executor::TaskExecutor::CallbackHandle;
using CBHandle = ReplicationExecutor::CallbackHandle;
using CBHStatus = StatusWith<CBHandle>;
using EventHandle = executor::TaskExecutor::EventHandle;
using executor::NetworkInterface;
using LockGuard = stdx::lock_guard<stdx::mutex>;
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
        if (elem.fieldNameStringData() == ReplicaSetConfig::kVersionFieldName && elem.isNumber()) {
            std::unique_ptr<SecureRandom> generator(SecureRandom::create());
            const int random = std::abs(static_cast<int>(generator->nextInt64()) % 100000);
            builder.appendIntOrLL(ReplicaSetConfig::kVersionFieldName,
                                  elem.numberLong() + 10000 + random);
        } else {
            builder.append(elem);
        }
    }
    return builder.obj();
}

const Seconds kNoopWriterPeriod(10);
}  // namespace

BSONObj ReplicationCoordinatorImpl::SlaveInfo::toBSON() const {
    BSONObjBuilder bo;
    bo.append("id", memberId);
    bo.append("rid", rid);
    bo.append("host", hostAndPort.toString());
    bo.append("lastDurableOpTime", lastDurableOpTime.toBSON());
    bo.append("lastAppliedOpTime", lastAppliedOpTime.toBSON());
    if (self)
        bo.append("self", true);
    if (down)
        bo.append("down", true);
    bo.append("lastUpdated", lastUpdate);
    return bo.obj();
}

std::string ReplicationCoordinatorImpl::SlaveInfo::toString() const {
    return toBSON().toString();
}

struct ReplicationCoordinatorImpl::WaiterInfo {

    using FinishFunc = stdx::function<void()>;

    WaiterInfo(unsigned int _opID,
               const OpTime _opTime,
               const WriteConcernOptions* _writeConcern,
               stdx::condition_variable* _condVar)
        : opID(_opID), opTime(_opTime), writeConcern(_writeConcern), condVar(_condVar) {}

    // When waiter is signaled, finishCallback will be called while holding replCoord _mutex
    // since WaiterLists are protected by _mutex.
    WaiterInfo(const OpTime _opTime, FinishFunc _finishCallback)
        : opTime(_opTime), finishCallback(_finishCallback) {}

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        bob.append("opId", opID);
        bob.append("opTime", opTime.toBSON());
        if (writeConcern) {
            bob.append("writeConcern", writeConcern->toBSON());
        }
        return bob.obj();
    };

    std::string toString() const {
        return toBSON().toString();
    };

    // It is invalid to call notify() unless holding ReplicationCoordinatorImpl::_mutex.
    void notify() {
        if (condVar) {
            condVar->notify_all();
        }
        if (finishCallback) {
            finishCallback();
        }
    }

    const unsigned int opID = 0;
    const OpTime opTime;
    const WriteConcernOptions* writeConcern = nullptr;
    stdx::condition_variable* condVar = nullptr;
    // The callback that will be called when this waiter is notified.
    FinishFunc finishCallback = nullptr;
};

struct ReplicationCoordinatorImpl::WaiterInfoGuard {
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
    WaiterInfoGuard(WaiterList* list,
                    unsigned int opID,
                    const OpTime opTime,
                    const WriteConcernOptions* writeConcern,
                    stdx::condition_variable* condVar)
        : waiter(opID, opTime, writeConcern, condVar), _list(list) {
        list->add_inlock(&waiter);
    }

    ~WaiterInfoGuard() {
        _list->remove_inlock(&waiter);
    }

    WaiterInfo waiter;

private:
    WaiterList* _list;
};

void ReplicationCoordinatorImpl::WaiterList::add_inlock(WaiterType waiter) {
    _list.push_back(waiter);
}

void ReplicationCoordinatorImpl::WaiterList::signalAndRemoveIf_inlock(
    stdx::function<bool(WaiterType)> func) {
    std::vector<WaiterType>::iterator it = _list.end();
    while (true) {
        it = std::find_if(_list.begin(), _list.end(), func);
        if (it == _list.end()) {
            break;
        }
        (*it)->notify();
        std::swap(*it, _list.back());
        _list.pop_back();
    }
}

void ReplicationCoordinatorImpl::WaiterList::signalAndRemoveAll_inlock() {
    for (auto& waiter : _list) {
        waiter->notify();
    }
    _list.clear();
}

bool ReplicationCoordinatorImpl::WaiterList::remove_inlock(WaiterType waiter) {
    auto it = std::find(_list.begin(), _list.end(), waiter);
    if (it != _list.end()) {
        std::swap(*it, _list.back());
        _list.pop_back();
        return true;
    }
    return false;
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

DataReplicatorOptions createDataReplicatorOptions(
    ReplicationCoordinator* replCoord, ReplicationCoordinatorExternalState* externalState) {
    DataReplicatorOptions options;
    options.getMyLastOptime = [replCoord]() { return replCoord->getMyLastAppliedOpTime(); };
    options.setMyLastOptime = [replCoord, externalState](const OpTime& opTime) {
        replCoord->setMyLastAppliedOpTime(opTime);
        externalState->setGlobalTimestamp(opTime.getTimestamp());
    };
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
    const ReplSettings& settings,
    ReplicationCoordinatorExternalState* externalState,
    TopologyCoordinator* topCoord,
    StorageInterface* storage,
    int64_t prngSeed,
    NetworkInterface* network,
    ReplicationExecutor* replExec,
    stdx::function<bool()>* isDurableStorageEngineFn)
    : _settings(settings),
      _replMode(getReplicationModeFromSettings(settings)),
      _topCoord(topCoord),
      _replExecutorIfOwned(replExec ? nullptr : new ReplicationExecutor(network, prngSeed)),
      _replExecutor(replExec ? *replExec : *_replExecutorIfOwned),
      _externalState(externalState),
      _inShutdown(false),
      _memberState(MemberState::RS_STARTUP),
      _isWaitingForDrainToComplete(false),
      _rsConfigState(kConfigPreStart),
      _selfIndex(-1),
      _sleptLastElection(false),
      _canAcceptNonLocalWrites(!(settings.usingReplSets() || settings.isSlave())),
      _canServeNonLocalReads(0U),
      _storage(storage),
      _isDurableStorageEngine(isDurableStorageEngineFn ? *isDurableStorageEngineFn : []() -> bool {
          return getGlobalServiceContext()->getGlobalStorageEngine()->isDurable();
      }) {
    if (!isReplEnabled()) {
        return;
    }

    std::unique_ptr<SecureRandom> rbidGenerator(SecureRandom::create());
    _rbid = static_cast<int>(rbidGenerator->nextInt64());
    if (_rbid < 0) {
        // Ensure _rbid is always positive
        _rbid = -_rbid;
    }

    // Make sure there is always an entry in _slaveInfo for ourself.
    SlaveInfo selfInfo;
    selfInfo.self = true;
    _slaveInfo.push_back(selfInfo);

    _externalState->setupNoopWriter(kNoopWriterPeriod);
}

ReplicationCoordinatorImpl::ReplicationCoordinatorImpl(
    const ReplSettings& settings,
    ReplicationCoordinatorExternalState* externalState,
    NetworkInterface* network,
    TopologyCoordinator* topCoord,
    StorageInterface* storage,
    int64_t prngSeed)
    : ReplicationCoordinatorImpl(
          settings, externalState, topCoord, storage, prngSeed, network, nullptr, nullptr) {}

ReplicationCoordinatorImpl::ReplicationCoordinatorImpl(
    const ReplSettings& settings,
    ReplicationCoordinatorExternalState* externalState,
    TopologyCoordinator* topCoord,
    StorageInterface* storage,
    ReplicationExecutor* replExec,
    int64_t prngSeed,
    stdx::function<bool()>* isDurableStorageEngineFn)
    : ReplicationCoordinatorImpl(settings,
                                 externalState,
                                 topCoord,
                                 storage,
                                 prngSeed,
                                 nullptr,
                                 replExec,
                                 isDurableStorageEngineFn) {}

ReplicationCoordinatorImpl::~ReplicationCoordinatorImpl() {}

void ReplicationCoordinatorImpl::waitForStartUpComplete() {
    CallbackHandle handle;
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        while (_rsConfigState == kConfigPreStart || _rsConfigState == kConfigStartingUp) {
            _rsConfigStateChange.wait(lk);
        }
        handle = _finishLoadLocalConfigCbh;
    }
    if (handle.isValid()) {
        _replExecutor.wait(handle);
    }
}

ReplicaSetConfig ReplicationCoordinatorImpl::getReplicaSetConfig_forTest() {
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

Date_t ReplicationCoordinatorImpl::getPriorityTakeover_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (!_priorityTakeoverCbh.isValid()) {
        return Date_t();
    }
    return _priorityTakeoverWhen;
}

OpTime ReplicationCoordinatorImpl::getCurrentCommittedSnapshotOpTime() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_currentCommittedSnapshot) {
        return _currentCommittedSnapshot->opTime;
    }
    return OpTime();
}

void ReplicationCoordinatorImpl::appendConnectionStats(executor::ConnectionPoolStats* stats) const {
    _replExecutor.appendConnectionStats(stats);
}

bool ReplicationCoordinatorImpl::_startLoadLocalConfig(OperationContext* txn) {
    StatusWith<LastVote> lastVote = _externalState->loadLocalLastVoteDocument(txn);
    if (!lastVote.isOK()) {
        log() << "Did not find local voted for document at startup;  " << lastVote.getStatus();
    } else {
        LockGuard topoLock(_topoMutex);
        _topCoord->loadLastVote(lastVote.getValue());
    }

    StatusWith<BSONObj> cfg = _externalState->loadLocalConfigDocument(txn);
    if (!cfg.isOK()) {
        log() << "Did not find local replica set configuration document at startup;  "
              << cfg.getStatus();
        return true;
    }
    ReplicaSetConfig localConfig;
    Status status = localConfig.initialize(cfg.getValue());
    if (!status.isOK()) {
        error() << "Locally stored replica set configuration does not parse; See "
                   "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config "
                   "for information on how to recover from this. Got \""
                << status << "\" while parsing " << cfg.getValue();
        fassertFailedNoTrace(28545);
    }

    // Read the last op from the oplog after cleaning up any partially applied batches.
    _externalState->cleanUpLastApplyBatch(txn);
    auto lastOpTimeStatus = _externalState->loadLastOpTime(txn);

    // Use a callback here, because _finishLoadLocalConfig calls isself() which requires
    // that the server's networking layer be up and running and accepting connections, which
    // doesn't happen until startReplication finishes.
    auto handle = _scheduleDBWork(stdx::bind(&ReplicationCoordinatorImpl::_finishLoadLocalConfig,
                                             this,
                                             stdx::placeholders::_1,
                                             localConfig,
                                             lastOpTimeStatus,
                                             lastVote));
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _finishLoadLocalConfigCbh = handle;
    }

    return false;
}

void ReplicationCoordinatorImpl::_finishLoadLocalConfig(
    const ReplicationExecutor::CallbackArgs& cbData,
    const ReplicaSetConfig& localConfig,
    const StatusWith<OpTime>& lastOpTimeStatus,
    const StatusWith<LastVote>& lastVoteStatus) {
    if (!cbData.status.isOK()) {
        LOG(1) << "Loading local replica set configuration failed due to " << cbData.status;
        return;
    }

    LockGuard topoLock(_topoMutex);

    StatusWith<int> myIndex = validateConfigForStartUp(
        _externalState.get(), _rsConfig, localConfig, getGlobalServiceContext());
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
        _reportUpstream_inlock(std::move(lock));
        // Unlocked below.
    } else {
        lock.unlock();
    }

    _externalState->setGlobalTimestamp(lastOpTime.getTimestamp());
    // Step down is impossible, so we don't need to wait for the returned event.
    _updateTerm_incallback(term);
    LOG(1) << "Current term is now " << term;
    _performPostMemberStateUpdateAction(action);
    if (!isArbiter) {
        _externalState->startThreads(_settings);
        invariant(cbData.txn);
        _startDataReplication(cbData.txn);
    }
}

void ReplicationCoordinatorImpl::_stopDataReplication(OperationContext* txn) {
    std::shared_ptr<DataReplicator> drCopy;
    {
        LockGuard lk(_mutex);
        _dr.swap(drCopy);
    }
    if (drCopy && drCopy->getState() == DataReplicatorState::InitialSync) {
        LOG(1)
            << "ReplicationCoordinatorImpl::_stopDataReplication calling DataReplicator::shutdown.";
        const auto status = drCopy->shutdown();
        if (!status.isOK()) {
            warning() << "DataReplicator shutdown failed: " << status;
        }
        drCopy.reset();
        // Do not return here, fall through.
    }
    LOG(1) << "ReplicationCoordinatorImpl::_stopDataReplication calling "
              "ReplCoordExtState::stopDataReplication.";
    _externalState->stopDataReplication(txn);
}

void ReplicationCoordinatorImpl::_startDataReplication(OperationContext* txn,
                                                       stdx::function<void()> startCompleted) {
    // Check to see if we need to do an initial sync.
    const auto lastOpTime = getMyLastAppliedOpTime();
    const auto needsInitialSync = lastOpTime.isNull() || _externalState->isInitialSyncFlagSet(txn);
    if (!needsInitialSync) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (!_inShutdown) {
            // Start steady replication, since we already have data.
            _externalState->startSteadyStateReplication(txn, this);
        }
        return;
    }

    // Do initial sync.
    if (_externalState->shouldUseDataReplicatorInitialSync()) {
        _externalState->runOnInitialSyncThread([this, startCompleted](OperationContext* txn) {
            std::shared_ptr<DataReplicator> drCopy;
            UniqueLock lk(_mutex);  // Must take the lock to set _dr, but not call it.
            drCopy = std::make_shared<DataReplicator>(
                createDataReplicatorOptions(this, _externalState.get()),
                stdx::make_unique<DataReplicatorExternalStateImpl>(this, _externalState.get()),
                _storage);
            _dr = drCopy;
            lk.unlock();

            const auto status = drCopy->doInitialSync(txn, numInitialSyncAttempts);
            // If it is interrupted by resync, we do not need to cleanup the DataReplicator.
            if (status == ErrorCodes::ShutdownInProgress) {
                return;
            }

            drCopy.reset();
            lk.lock();

            if (status == ErrorCodes::CallbackCanceled) {
                log() << "Initial Sync has been cancelled: " << status.getStatus();
                return;
            } else if (!status.isOK()) {
                if (_inShutdown) {
                    log() << "Initial Sync failed during shutdown due to " << status.getStatus();
                    return;
                } else {
                    error() << "Initial sync failed, shutting down now. Restart the server to "
                               "attempt a new initial sync.";
                    fassertFailedWithStatusNoTrace(40088, status.getStatus());
                }
            }

            const auto lastApplied = status.getValue();
            _setMyLastAppliedOpTime_inlock(lastApplied.opTime, false);
            lk.unlock();

            // Clear maint. mode.
            while (getMaintenanceMode()) {
                setMaintenanceMode(false);
            }

            if (startCompleted) {
                startCompleted();
            }
            // Repair local db (to compact it).
            uassertStatusOK(_externalState->runRepairOnLocalDB(txn));
            _externalState->startSteadyStateReplication(txn, this);
        });
    } else {
        _externalState->startInitialSync([this, startCompleted](OperationContext* txn) {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            if (!_inShutdown) {
                if (startCompleted) {
                    startCompleted();
                }
                _externalState->startSteadyStateReplication(txn, this);
            }
        });
    }
}

void ReplicationCoordinatorImpl::startup(OperationContext* txn) {
    if (!isReplEnabled()) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _setConfigState_inlock(kConfigReplicationDisabled);
        return;
    }

    {
        OID rid = _externalState->ensureMe(txn);

        stdx::lock_guard<stdx::mutex> lk(_mutex);
        fassert(18822, !_inShutdown);
        _setConfigState_inlock(kConfigStartingUp);
        _myRID = rid;
        _slaveInfo[_getMyIndexInSlaveInfo_inlock()].rid = rid;
    }

    if (!_settings.usingReplSets()) {
        // Must be Master/Slave
        invariant(_settings.isMaster() || _settings.isSlave());
        _externalState->startMasterSlave(txn);
        return;
    }

    _replExecutor.startup();

    _topCoord->setStorageEngineSupportsReadCommitted(
        _externalState->isReadCommittedSupportedByStorageEngine(txn));

    bool doneLoadingConfig = _startLoadLocalConfig(txn);
    if (doneLoadingConfig) {
        // If we're not done loading the config, then the config state will be set by
        // _finishLoadLocalConfig.
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        invariant(!_rsConfig.isInitialized());
        _setConfigState_inlock(kConfigUninitialized);
    }
}

void ReplicationCoordinatorImpl::shutdown(OperationContext* txn) {
    // Shutdown must:
    // * prevent new threads from blocking in awaitReplication
    // * wake up all existing threads blocking in awaitReplication
    // * tell the ReplicationExecutor to shut down
    // * wait for the thread running the ReplicationExecutor to finish

    if (!_settings.usingReplSets()) {
        return;
    }

    log() << "shutting down replication subsystems";

    // Used to shut down outside of the lock.
    std::shared_ptr<DataReplicator> drCopy;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        fassert(28533, !_inShutdown);
        _inShutdown = true;
        if (_rsConfigState == kConfigPreStart) {
            warning() << "ReplicationCoordinatorImpl::shutdown() called before "
                         "startup() finished.  Shutting down without cleaning up the "
                         "replication system";
            return;
        }
        fassert(18823, _rsConfigState != kConfigStartingUp);
        _replicationWaiterList.signalAndRemoveAll_inlock();
        _opTimeWaiterList.signalAndRemoveAll_inlock();
        _currentCommittedSnapshotCond.notify_all();
        _dr.swap(drCopy);
    }

    {
        stdx::lock_guard<stdx::mutex> topoLock(_topoMutex);
        _signalStepDownWaiter_inlock();
    }

    // joining the replication executor is blocking so it must be run outside of the mutex
    if (drCopy) {
        LOG(1) << "ReplicationCoordinatorImpl::shutdown calling DataReplicator::shutdown.";
        const auto status = drCopy->shutdown();
        if (!status.isOK()) {
            warning() << "DataReplicator shutdown failed: " << status;
        }
        drCopy.reset();
    }
    _externalState->shutdown(txn);
    _replExecutor.shutdown();
    _replExecutor.join();
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
    LockGuard topoLock(_topoMutex);
    _topCoord->clearSyncSourceBlacklist();
}

ReplicationExecutor::EventHandle ReplicationCoordinatorImpl::setFollowerMode_nonBlocking(
    const MemberState& newState, bool* success) {
    StatusWith<ReplicationExecutor::EventHandle> finishedSettingFollowerMode =
        _replExecutor.makeEvent();
    if (finishedSettingFollowerMode.getStatus() == ErrorCodes::ShutdownInProgress) {
        return ReplicationExecutor::EventHandle();
    }
    fassert(18812, finishedSettingFollowerMode.getStatus());
    _setFollowerModeFinish(newState, finishedSettingFollowerMode.getValue(), success);
    return finishedSettingFollowerMode.getValue();
}

bool ReplicationCoordinatorImpl::setFollowerMode(const MemberState& newState) {
    bool success = false;
    if (auto eventHandle = setFollowerMode_nonBlocking(newState, &success)) {
        _replExecutor.waitForEvent(eventHandle);
    }
    return success;
}

void ReplicationCoordinatorImpl::_setFollowerModeFinish(
    const MemberState& newState,
    const ReplicationExecutor::EventHandle& finishedSettingFollowerMode,
    bool* success) {
    LockGuard topoLock(_topoMutex);

    if (newState == _topCoord->getMemberState()) {
        *success = true;
        _replExecutor.signalEvent(finishedSettingFollowerMode);
        return;
    }
    if (_topCoord->getRole() == TopologyCoordinator::Role::leader) {
        *success = false;
        _replExecutor.signalEvent(finishedSettingFollowerMode);
        return;
    }

    if (_topCoord->getRole() == TopologyCoordinator::Role::candidate) {
        // We are a candidate, which means _topCoord believes us to be in state RS_SECONDARY, and
        // we know that newState != RS_SECONDARY because we would have returned early, above if
        // the old and new state were equal.  So, cancel the running election and try again to
        // finish setting the follower mode.
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
        _replExecutor.onEvent(
            _electionFinishedEvent,
            _wrapAsCallbackFn(stdx::bind(&ReplicationCoordinatorImpl::_setFollowerModeFinish,
                                         this,
                                         newState,
                                         finishedSettingFollowerMode,
                                         success)));
        return;
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _topCoord->setFollowerMode(newState.s);

    const PostMemberStateUpdateAction action = _updateMemberStateFromTopologyCoordinator_inlock();
    lk.unlock();
    _performPostMemberStateUpdateAction(action);
    *success = true;
    _replExecutor.signalEvent(finishedSettingFollowerMode);
}

bool ReplicationCoordinatorImpl::isWaitingForApplierToDrain() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _isWaitingForDrainToComplete;
}

bool ReplicationCoordinatorImpl::isCatchingUp() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _isCatchingUp;
}


void ReplicationCoordinatorImpl::signalDrainComplete(OperationContext* txn) {
    // This logic is a little complicated in order to avoid acquiring the global exclusive lock
    // unnecessarily.  This is important because the applier may call signalDrainComplete()
    // whenever it wants, not only when the ReplicationCoordinator is expecting it.
    //
    // The steps are:
    // 1.) Check to see if we're waiting for this signal.  If not, return early.
    // 2.) Otherwise, release the mutex while acquiring the global exclusive lock,
    //     since that might take a while (NB there's a deadlock cycle otherwise, too).
    // 3.) Re-check to see if we've somehow left drain mode.  If we have not, clear
    //     _isWaitingForDrainToComplete, set the flag allowing non-local database writes and
    //     drop the mutex.  At this point, no writes can occur from other threads, due to the
    //     global exclusive lock.
    // 4.) Drop all temp collections, and log the drops to the oplog.
    // 5.) Log transition to primary in the oplog and set that OpTime as the floor for what we will
    //     consider to be committed.
    // 6.) Drop the global exclusive lock.
    //
    // Because replicatable writes are forbidden while in drain mode, and we don't exit drain
    // mode until we have the global exclusive lock, which forbids all other threads from making
    // writes, we know that from the time that _isWaitingForDrainToComplete is set in
    // _performPostMemberStateUpdateAction(kActionWinElection) until this method returns, no
    // external writes will be processed.  This is important so that a new temp collection isn't
    // introduced on the new primary before we drop all the temp collections.

    // When we go to drop all temp collections, we must replicate the drops.
    invariant(txn->writesAreReplicated());

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (!_isWaitingForDrainToComplete) {
        return;
    }
    lk.unlock();

    _externalState->onDrainComplete(txn);

    ScopedTransaction transaction(txn, MODE_X);
    Lock::GlobalWrite globalWriteLock(txn->lockState());
    lk.lock();

    if (!_isWaitingForDrainToComplete) {
        return;
    }
    invariant(!_isCatchingUp);
    invariant(!_canAcceptNonLocalWrites);
    _isWaitingForDrainToComplete = false;
    _drainFinishedCond_forTest.notify_all();

    if (!_getMemberState_inlock().primary()) {
        // We must have decided not to transition to primary while waiting for the applier to drain.
        // Skip the rest of this function since it should only be done when really transitioning.
        return;
    }

    _canAcceptNonLocalWrites = true;

    lk.unlock();
    _setFirstOpTimeOfMyTerm(_externalState->onTransitionToPrimary(txn, isV1ElectionProtocol()));
    lk.lock();

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
    auto pred = [this]() { return !_isCatchingUp && !_isWaitingForDrainToComplete; };
    if (!_drainFinishedCond_forTest.wait_for(lk, timeout.toSystemDuration(), pred)) {
        return Status(ErrorCodes::ExceededTimeLimit,
                      "Timed out waiting to finish draining applier buffer");
    }

    return Status::OK();
}

void ReplicationCoordinatorImpl::signalUpstreamUpdater() {
    _externalState->forwardSlaveProgress();
}

ReplicationCoordinatorImpl::SlaveInfo* ReplicationCoordinatorImpl::_findSlaveInfoByMemberID_inlock(
    int memberId) {
    for (SlaveInfoVector::iterator it = _slaveInfo.begin(); it != _slaveInfo.end(); ++it) {
        if (it->memberId == memberId) {
            return &(*it);
        }
    }
    return NULL;
}

ReplicationCoordinatorImpl::SlaveInfo* ReplicationCoordinatorImpl::_findSlaveInfoByRID_inlock(
    const OID& rid) {
    for (SlaveInfoVector::iterator it = _slaveInfo.begin(); it != _slaveInfo.end(); ++it) {
        if (it->rid == rid) {
            return &(*it);
        }
    }
    return NULL;
}

void ReplicationCoordinatorImpl::_addSlaveInfo_inlock(const SlaveInfo& slaveInfo) {
    invariant(getReplicationMode() == modeMasterSlave);
    _slaveInfo.push_back(slaveInfo);

    _updateLastCommittedOpTime_inlock();
    // Wake up any threads waiting for replication that now have their replication
    // check satisfied
    _wakeReadyWaiters_inlock();
}

void ReplicationCoordinatorImpl::_updateSlaveInfoAppliedOpTime_inlock(SlaveInfo* slaveInfo,
                                                                      const OpTime& opTime) {
    slaveInfo->lastAppliedOpTime = opTime;
    slaveInfo->lastUpdate = _replExecutor.now();
    slaveInfo->down = false;

    _updateLastCommittedOpTime_inlock();
    // Wake up any threads waiting for replication that now have their replication
    // check satisfied
    _wakeReadyWaiters_inlock();
}

void ReplicationCoordinatorImpl::_updateSlaveInfoDurableOpTime_inlock(SlaveInfo* slaveInfo,
                                                                      const OpTime& opTime) {
    // lastAppliedOpTime cannot be behind lastDurableOpTime.
    if (slaveInfo->lastAppliedOpTime < opTime) {
        log() << "Durable progress (" << opTime << ") is ahead of the applied progress ("
              << slaveInfo->lastAppliedOpTime << ". This is likely due to a "
                                                 "rollback. slaveInfo: "
              << slaveInfo->toString();
        return;
    }
    slaveInfo->lastDurableOpTime = opTime;
    slaveInfo->lastUpdate = _replExecutor.now();
    slaveInfo->down = false;

    _updateLastCommittedOpTime_inlock();
    // Wake up any threads waiting for replication that now have their replication
    // check satisfied
    _wakeReadyWaiters_inlock();
}

void ReplicationCoordinatorImpl::_updateSlaveInfoFromConfig_inlock() {
    invariant(_settings.usingReplSets());

    SlaveInfoVector oldSlaveInfos;
    _slaveInfo.swap(oldSlaveInfos);

    if (_selfIndex == -1) {
        // If we aren't in the config then the only data we care about is for ourself
        for (SlaveInfoVector::const_iterator it = oldSlaveInfos.begin(); it != oldSlaveInfos.end();
             ++it) {
            if (it->self) {
                SlaveInfo slaveInfo = *it;
                slaveInfo.memberId = -1;
                _slaveInfo.push_back(slaveInfo);
                return;
            }
        }
        invariant(false);  // There should always have been an entry for ourself
    }

    for (int i = 0; i < _rsConfig.getNumMembers(); ++i) {
        const MemberConfig& memberConfig = _rsConfig.getMemberAt(i);
        int memberId = memberConfig.getId();
        const HostAndPort& memberHostAndPort = memberConfig.getHostAndPort();

        SlaveInfo slaveInfo;

        // Check if the node existed with the same member ID and hostname in the old data
        for (SlaveInfoVector::const_iterator it = oldSlaveInfos.begin(); it != oldSlaveInfos.end();
             ++it) {
            if ((it->memberId == memberId && it->hostAndPort == memberHostAndPort) ||
                (i == _selfIndex && it->self)) {
                slaveInfo = *it;
            }
        }

        // Make sure you have the most up-to-date info for member ID and hostAndPort.
        slaveInfo.memberId = memberId;
        slaveInfo.hostAndPort = memberHostAndPort;
        _slaveInfo.push_back(slaveInfo);
    }
    invariant(static_cast<int>(_slaveInfo.size()) == _rsConfig.getNumMembers());
}

size_t ReplicationCoordinatorImpl::_getMyIndexInSlaveInfo_inlock() const {
    if (getReplicationMode() == modeMasterSlave) {
        // Self data always lives in the first entry in _slaveInfo for master/slave
        return 0;
    } else {
        invariant(_settings.usingReplSets());
        if (_selfIndex == -1) {
            invariant(_slaveInfo.size() == 1);
            return 0;
        } else {
            return _selfIndex;
        }
    }
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
    SlaveInfo* slaveInfo = _findSlaveInfoByRID_inlock(rid);
    if (slaveInfo) {
        if (slaveInfo->lastAppliedOpTime < opTime) {
            _updateSlaveInfoAppliedOpTime_inlock(slaveInfo, opTime);
        }
    } else {
        SlaveInfo newSlaveInfo;
        newSlaveInfo.rid = rid;
        newSlaveInfo.lastAppliedOpTime = opTime;
        _addSlaveInfo_inlock(newSlaveInfo);
    }
    return Status::OK();
}

void ReplicationCoordinatorImpl::setMyHeartbeatMessage(const std::string& msg) {
    LockGuard topoLock(_topoMutex);
    _topCoord->setMyHeartbeatMessage(_replExecutor.now(), msg);
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
    SlaveInfo* mySlaveInfo = &_slaveInfo[_getMyIndexInSlaveInfo_inlock()];
    invariant(isRollbackAllowed || mySlaveInfo->lastAppliedOpTime <= opTime);
    _updateSlaveInfoAppliedOpTime_inlock(mySlaveInfo, opTime);

    _opTimeWaiterList.signalAndRemoveIf_inlock(
        [opTime](WaiterInfo* waiter) { return waiter->opTime <= opTime; });
}

void ReplicationCoordinatorImpl::_setMyLastDurableOpTime_inlock(const OpTime& opTime,
                                                                bool isRollbackAllowed) {
    SlaveInfo* mySlaveInfo = &_slaveInfo[_getMyIndexInSlaveInfo_inlock()];
    invariant(isRollbackAllowed || mySlaveInfo->lastDurableOpTime <= opTime);
    // lastAppliedOpTime cannot be behind lastDurableOpTime.
    if (mySlaveInfo->lastAppliedOpTime < opTime) {
        log() << "My durable progress (" << opTime << ") is ahead of my applied progress ("
              << mySlaveInfo->lastAppliedOpTime << ". This is likely due to a "
                                                   "rollback. slaveInfo: "
              << mySlaveInfo->toString();
        return;
    }
    _updateSlaveInfoDurableOpTime_inlock(mySlaveInfo, opTime);
}

OpTime ReplicationCoordinatorImpl::getMyLastAppliedOpTime() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _getMyLastAppliedOpTime_inlock();
}

OpTime ReplicationCoordinatorImpl::getMyLastDurableOpTime() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _getMyLastDurableOpTime_inlock();
}

Status ReplicationCoordinatorImpl::waitUntilOpTimeForRead(OperationContext* txn,
                                                          const ReadConcernArgs& settings) {
    // We should never wait for replication if we are holding any locks, because this can
    // potentially block for long time while doing network activity.
    if (txn->lockState()->isLocked()) {
        return {ErrorCodes::IllegalOperation,
                "Waiting for replication not allowed while holding a lock"};
    }

    const bool isMajorityReadConcern =
        settings.getLevel() == ReadConcernLevel::kMajorityReadConcern;

    if (isMajorityReadConcern && !getSettings().isMajorityReadConcernEnabled()) {
        // This is an opt-in feature. Fail if the user didn't opt-in.
        return {ErrorCodes::ReadConcernMajorityNotEnabled,
                "Majority read concern requested, but server was not started with "
                "--enableMajorityReadConcern."};
    }

    const auto targetOpTime = settings.getOpTime();

    if (targetOpTime.isNull()) {
        return Status::OK();
    }

    if (getReplicationMode() != repl::ReplicationCoordinator::modeReplSet) {
        // For master/slave and standalone nodes, readAfterOpTime is not supported, so we return an
        // error. However, we consider all writes "committed" and can treat MajorityReadConcern as
        // LocalReadConcern, which is immediately satisfied since there is no OpTime to wait for.
        return {ErrorCodes::NotAReplicaSet,
                "node needs to be a replica set member to use read concern"};
    }

    stdx::unique_lock<stdx::mutex> lock(_mutex);

    if (isMajorityReadConcern && !_externalState->snapshotsEnabled()) {
        return {ErrorCodes::CommandNotSupported,
                "Current storage engine does not support majority readConcerns"};
    }

    auto getCurrentOpTime = [this, isMajorityReadConcern] {
        auto committedOptime =
            _currentCommittedSnapshot ? _currentCommittedSnapshot->opTime : OpTime();
        return isMajorityReadConcern ? committedOptime : _getMyLastAppliedOpTime_inlock();
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
            LOG(3) << "waitUntilOpTime: waiting for a new snapshot until " << txn->getDeadline();

            auto waitStatus =
                txn->waitForConditionOrInterruptNoAssert(_currentCommittedSnapshotCond, lock);
            if (!waitStatus.isOK()) {
                return waitStatus;
            }
            LOG(3) << "Got notified of new snapshot: " << _currentCommittedSnapshot->toString();
            continue;
        }

        // We just need to wait for the opTime to catch up to what we need (not majority RC).
        stdx::condition_variable condVar;
        WaiterInfoGuard waitInfo(
            &_opTimeWaiterList, txn->getOpID(), targetOpTime, nullptr, &condVar);

        LOG(3) << "waituntilOpTime: waiting for OpTime " << waitInfo.waiter << " until "
               << txn->getDeadline();

        auto waitStatus = txn->waitForConditionOrInterruptNoAssert(condVar, lock);
        if (!waitStatus.isOK()) {
            return waitStatus;
        }
    }

    return Status::OK();
}

OpTime ReplicationCoordinatorImpl::_getMyLastAppliedOpTime_inlock() const {
    return _slaveInfo[_getMyIndexInSlaveInfo_inlock()].lastAppliedOpTime;
}

OpTime ReplicationCoordinatorImpl::_getMyLastDurableOpTime_inlock() const {
    return _slaveInfo[_getMyIndexInSlaveInfo_inlock()].lastDurableOpTime;
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

    SlaveInfo* slaveInfo = NULL;
    if (args.cfgver != _rsConfig.getConfigVersion()) {
        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << args.memberId
            << " whose config version of " << args.cfgver << " doesn't match our config version of "
            << _rsConfig.getConfigVersion();
        LOG(1) << errmsg;
        *configVersion = _rsConfig.getConfigVersion();
        return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
    }

    slaveInfo = _findSlaveInfoByMemberID_inlock(args.memberId);
    if (!slaveInfo) {
        invariant(!_rsConfig.findMemberByID(args.memberId));

        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << args.memberId
            << " which doesn't exist in our config";
        LOG(1) << errmsg;
        return Status(ErrorCodes::NodeNotFound, errmsg);
    }

    invariant(args.memberId == slaveInfo->memberId);

    LOG(3) << "Node with memberID " << args.memberId << " has durably applied operationss through "
           << slaveInfo->lastDurableOpTime << " and has applied operations through "
           << slaveInfo->lastAppliedOpTime << "; updating to new durable operation with timestamp "
           << args.ts;

    // Only update remote optimes if they increase.
    if (slaveInfo->lastAppliedOpTime < args.ts) {
        _updateSlaveInfoAppliedOpTime_inlock(slaveInfo, args.ts);
    }
    if (slaveInfo->lastDurableOpTime < args.ts) {
        _updateSlaveInfoDurableOpTime_inlock(slaveInfo, args.ts);
    }


    // Update liveness for this node.
    slaveInfo->lastUpdate = _replExecutor.now();
    slaveInfo->down = false;
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

    SlaveInfo* slaveInfo = NULL;
    if (args.cfgver != _rsConfig.getConfigVersion()) {
        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << args.memberId
            << " whose config version of " << args.cfgver << " doesn't match our config version of "
            << _rsConfig.getConfigVersion();
        LOG(1) << errmsg;
        *configVersion = _rsConfig.getConfigVersion();
        return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
    }

    slaveInfo = _findSlaveInfoByMemberID_inlock(args.memberId);
    if (!slaveInfo) {
        invariant(!_rsConfig.findMemberByID(args.memberId));

        std::string errmsg = str::stream()
            << "Received replSetUpdatePosition for node with memberId " << args.memberId
            << " which doesn't exist in our config";
        LOG(1) << errmsg;
        return Status(ErrorCodes::NodeNotFound, errmsg);
    }

    invariant(args.memberId == slaveInfo->memberId);

    LOG(3) << "Node with memberID " << args.memberId << " currently has optime "
           << slaveInfo->lastAppliedOpTime << " durable through " << slaveInfo->lastDurableOpTime
           << "; updating to optime " << args.appliedOpTime << " and durable through "
           << args.durableOpTime;


    // Only update remote optimes if they increase.
    if (slaveInfo->lastAppliedOpTime < args.appliedOpTime) {
        _updateSlaveInfoAppliedOpTime_inlock(slaveInfo, args.appliedOpTime);
    }
    if (slaveInfo->lastDurableOpTime < args.durableOpTime) {
        _updateSlaveInfoDurableOpTime_inlock(slaveInfo, args.durableOpTime);
    }

    // Update liveness for this node.
    slaveInfo->lastUpdate = _replExecutor.now();
    slaveInfo->down = false;
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
        return _haveNumNodesReachedOpTime_inlock(opTime, writeConcern.wNumNodes, useDurableOpTime);
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
        patternName = ReplicaSetConfig::kMajorityWriteConcernModeName;
    } else {
        patternName = writeConcern.wMode;
    }

    StatusWith<ReplicaSetTagPattern> tagPattern = _rsConfig.findCustomWriteMode(patternName);
    if (!tagPattern.isOK()) {
        return true;
    }
    return _haveTaggedNodesReachedOpTime_inlock(opTime, tagPattern.getValue(), useDurableOpTime);
}

bool ReplicationCoordinatorImpl::_haveNumNodesReachedOpTime_inlock(const OpTime& targetOpTime,
                                                                   int numNodes,
                                                                   bool durablyWritten) {
    // Replication progress that is for some reason ahead of us should not allow us to
    // satisfy a write concern if we aren't caught up ourselves.
    OpTime myOpTime =
        durablyWritten ? _getMyLastDurableOpTime_inlock() : _getMyLastAppliedOpTime_inlock();
    if (myOpTime < targetOpTime) {
        return false;
    }

    for (SlaveInfoVector::iterator it = _slaveInfo.begin(); it != _slaveInfo.end(); ++it) {
        const OpTime& slaveTime = durablyWritten ? it->lastDurableOpTime : it->lastAppliedOpTime;
        if (slaveTime >= targetOpTime) {
            --numNodes;
        }

        if (numNodes <= 0) {
            return true;
        }
    }
    return false;
}

bool ReplicationCoordinatorImpl::_haveTaggedNodesReachedOpTime_inlock(
    const OpTime& opTime, const ReplicaSetTagPattern& tagPattern, bool durablyWritten) {
    ReplicaSetTagMatch matcher(tagPattern);
    for (SlaveInfoVector::iterator it = _slaveInfo.begin(); it != _slaveInfo.end(); ++it) {
        const OpTime& slaveTime = durablyWritten ? it->lastDurableOpTime : it->lastAppliedOpTime;
        if (slaveTime >= opTime) {
            // This node has reached the desired optime, now we need to check if it is a part
            // of the tagPattern.
            const MemberConfig* memberConfig = _rsConfig.findMemberByID(it->memberId);
            invariant(memberConfig);
            for (MemberConfig::TagIterator it = memberConfig->tagsBegin();
                 it != memberConfig->tagsEnd();
                 ++it) {
                if (matcher.update(*it)) {
                    return true;
                }
            }
        }
    }
    return false;
}

ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorImpl::awaitReplication(
    OperationContext* txn, const OpTime& opTime, const WriteConcernOptions& writeConcern) {
    Timer timer;
    WriteConcernOptions fixedWriteConcern = populateUnsetWriteConcernOptionsSyncMode(writeConcern);
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    auto status =
        _awaitReplication_inlock(&lock, txn, opTime, SnapshotName::min(), fixedWriteConcern);
    return {std::move(status), duration_cast<Milliseconds>(timer.elapsed())};
}

ReplicationCoordinator::StatusAndDuration
ReplicationCoordinatorImpl::awaitReplicationOfLastOpForClient(
    OperationContext* txn, const WriteConcernOptions& writeConcern) {
    Timer timer;
    WriteConcernOptions fixedWriteConcern = populateUnsetWriteConcernOptionsSyncMode(writeConcern);
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    const auto& clientInfo = ReplClientInfo::forClient(txn->getClient());
    auto status = _awaitReplication_inlock(
        &lock, txn, clientInfo.getLastOp(), clientInfo.getLastSnapshot(), fixedWriteConcern);
    return {std::move(status), duration_cast<Milliseconds>(timer.elapsed())};
}

Status ReplicationCoordinatorImpl::_awaitReplication_inlock(
    stdx::unique_lock<stdx::mutex>* lock,
    OperationContext* txn,
    const OpTime& opTime,
    SnapshotName minSnapshot,
    const WriteConcernOptions& writeConcern) {

    // We should never wait for replication if we are holding any locks, because this can
    // potentially block for long time while doing network activity.
    if (txn->lockState()->isLocked()) {
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

        if (opTime.getTerm() != _cachedTerm) {
            return {
                ErrorCodes::PrimarySteppedDown,
                str::stream() << "Term changed from " << opTime.getTerm() << " to " << _cachedTerm
                              << " while waiting for replication, indicating that this node must "
                                 "have stepped down."};
        }

        if (_stepDownPending) {
            return {ErrorCodes::PrimarySteppedDown,
                    "Received stepdown request while waiting for replication"};
        }
        return Status::OK();
    };

    Status stepdownStatus = checkForStepDown();
    if (!stepdownStatus.isOK()) {
        return stepdownStatus;
    }

    auto interruptStatus = txn->checkForInterruptNoAssert();
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

    auto clockSource = txn->getServiceContext()->getFastClockSource();
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
    WaiterInfoGuard waitInfo(
        &_replicationWaiterList, txn->getOpID(), opTime, &writeConcern, &condVar);
    while (!_doneWaitingForReplication_inlock(opTime, minSnapshot, writeConcern)) {

        if (_inShutdown) {
            return {ErrorCodes::ShutdownInProgress, "Replication is being shut down"};
        }

        auto status = txn->waitForConditionOrInterruptNoAssertUntil(condVar, *lock, wTimeoutDate);
        if (!status.isOK()) {
            return status.getStatus();
        }

        if (status.getValue() == stdx::cv_status::timeout) {
            if (Command::testCommandsEnabled) {
                // log state of replica set on timeout to help with diagnosis.
                BSONObjBuilder progress;
                _appendSlaveInfoData_inlock(&progress);
                log() << "Replication for failed WC: " << writeConcern.toBSON()
                      << ", waitInfo:" << waitInfo.waiter.toBSON()
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

Status ReplicationCoordinatorImpl::stepDown(OperationContext* txn,
                                            const bool force,
                                            const Milliseconds& waitTime,
                                            const Milliseconds& stepdownTime) {

    const Date_t startTime = _replExecutor.now();
    const Date_t stepDownUntil = startTime + stepdownTime;
    const Date_t waitUntil = startTime + waitTime;

    if (!getMemberState().primary()) {
        // Note this check is inherently racy - it's always possible for the node to
        // stepdown from some other path before we acquire the global shared lock, but
        // that's okay because we are resiliant to that happening in _stepDownContinue.
        return {ErrorCodes::NotMaster, "not primary so can't step down"};
    }

    Lock::GlobalLock globalReadLock(txn->lockState(), MODE_S, Lock::GlobalLock::EnqueueOnly());

    // We've requested the global shared lock which will stop new writes from coming in,
    // but existing writes could take a long time to finish, so kill all user operations
    // to help us get the global lock faster.
    _externalState->killAllUserOperations(txn);

    globalReadLock.waitForLock(durationCount<Milliseconds>(stepdownTime));

    if (!globalReadLock.isLocked()) {
        return {ErrorCodes::ExceededTimeLimit,
                "Could not acquire the global shared lock within the amount of time "
                "specified that we should step down for"};
    }

    try {
        stdx::unique_lock<stdx::mutex> topoLock(_topoMutex);
        bool restartHeartbeats = true;
        txn->checkForInterrupt();
        while (!_tryToStepDown(waitUntil, stepDownUntil, force)) {
            if (restartHeartbeats) {
                // We send out a fresh round of heartbeats because stepping down successfully
                // without
                // {force: true} is dependent on timely heartbeat data.
                stdx::lock_guard<stdx::mutex> lk(_mutex);
                _restartHeartbeats_inlock();
                restartHeartbeats = false;
            }
            txn->waitForConditionOrInterruptUntil(
                _stepDownWaiters, topoLock, std::min(stepDownUntil, waitUntil));
        }
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    return Status::OK();
}

void ReplicationCoordinatorImpl::_signalStepDownWaiter_inlock() {
    _stepDownWaiters.notify_all();
}

bool ReplicationCoordinatorImpl::_tryToStepDown(const Date_t waitUntil,
                                                const Date_t stepDownUntil,
                                                const bool force) {

    if (_topCoord->getRole() != TopologyCoordinator::Role::leader) {
        uasserted(ErrorCodes::NotMaster,
                  "Already stepped down from primary while processing step down request");
    }
    const Date_t now = _replExecutor.now();
    if (now >= stepDownUntil) {
        uasserted(ErrorCodes::ExceededTimeLimit,
                  "By the time we were ready to step down, we were already past the "
                  "time we were supposed to step down until");
    }

    const bool forceNow = now >= waitUntil ? force : false;
    if (!_topCoord->stepDown(
            stepDownUntil, forceNow, getMyLastAppliedOpTime(), getLastCommittedOpTime())) {
        if (now >= waitUntil) {
            uasserted(ErrorCodes::ExceededTimeLimit,
                      str::stream() << "No electable secondaries caught up as of "
                                    << dateToISOStringLocal(now)
                                    << ". Please use {force: true} to force node to step down.");
        }
        return false;
    }

    const auto action = [&] {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _updateMemberStateFromTopologyCoordinator_inlock();
    }();
    _performPostMemberStateUpdateAction(action);

    // Schedule work to (potentially) step back up once the stepdown period has ended.
    _scheduleWorkAt(
        stepDownUntil,
        stdx::bind(&ReplicationCoordinatorImpl::_handleTimePassing, this, stdx::placeholders::_1));
    return true;
}

void ReplicationCoordinatorImpl::_handleTimePassing(
    const ReplicationExecutor::CallbackArgs& cbData) {
    if (!cbData.status.isOK()) {
        return;
    }

    LockGuard topoLock(_topoMutex);
    if (_topCoord->becomeCandidateIfStepdownPeriodOverAndSingleNodeSet(_replExecutor.now())) {
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

bool ReplicationCoordinatorImpl::canAcceptWritesForDatabase(StringData dbName) {
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

bool ReplicationCoordinatorImpl::canAcceptWritesFor(const NamespaceString& ns) {
    if (_memberState.rollback() && ns.isOplog()) {
        return false;
    }
    StringData dbName = ns.db();
    return canAcceptWritesForDatabase(dbName);
}

Status ReplicationCoordinatorImpl::checkCanServeReadsFor(OperationContext* txn,
                                                         const NamespaceString& ns,
                                                         bool slaveOk) {
    auto client = txn->getClient();
    // Oplog reads are not allowed during STARTUP state, but we make an exception for internal
    // reads and master-slave replication. Internel reads are required for cleaning up unfinished
    // apply batches. Master-slave never sets the state so we make an exception for it as well.
    if (((_memberState.startup() && client->isFromUserConnection() &&
          getReplicationMode() == modeReplSet) ||
         _memberState.startup2() || _memberState.rollback()) &&
        ns.isOplog()) {
        return {ErrorCodes::NotMasterOrSecondary,
                "Oplog collection reads are not allowed while in the rollback or startup state."};
    }
    if (client->isInDirectClient()) {
        return Status::OK();
    }
    if (canAcceptWritesFor(ns)) {
        return Status::OK();
    }
    if (_settings.isSlave() || _settings.isMaster()) {
        return Status::OK();
    }
    if (slaveOk) {
        if (_canServeNonLocalReads.loadRelaxed()) {
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

bool ReplicationCoordinatorImpl::shouldRelaxIndexConstraints(const NamespaceString& ns) {
    return !canAcceptWritesFor(ns);
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

Status ReplicationCoordinatorImpl::resyncData(OperationContext* txn, bool waitUntilCompleted) {
    _stopDataReplication(txn);
    auto finishedEvent = uassertStatusOK(_replExecutor.makeEvent());
    stdx::function<void()> f;
    if (waitUntilCompleted)
        f = [&finishedEvent, this]() { _replExecutor.signalEvent(finishedEvent); };

    UniqueLock lk(_mutex);
    _resetMyLastOpTimes_inlock();
    lk.unlock();  // unlock before calling into replCoordExtState.
    _startDataReplication(txn, f);
    if (waitUntilCompleted) {
        _replExecutor.waitForEvent(finishedEvent);
    }
    return Status::OK();
}

StatusWith<BSONObj> ReplicationCoordinatorImpl::prepareReplSetUpdatePositionCommand(
    ReplicationCoordinator::ReplSetUpdatePositionCommandStyle commandStyle) const {
    BSONObjBuilder cmdBuilder;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_rsConfig.isInitialized());
        // Do not send updates if we have been removed from the config.
        if (_selfIndex == -1) {
            return Status(ErrorCodes::NodeNotFound,
                          "This node is not in the current replset configuration.");
        }
        cmdBuilder.append(UpdatePositionArgs::kCommandFieldName, 1);
        // Create an array containing objects each live member connected to us and for ourself.
        BSONArrayBuilder arrayBuilder(cmdBuilder.subarrayStart("optimes"));
        for (const auto& slaveInfo : _slaveInfo) {
            if (slaveInfo.lastAppliedOpTime.isNull()) {
                // Don't include info on members we haven't heard from yet.
                continue;
            }
            // Don't include members we think are down.
            if (!slaveInfo.self && slaveInfo.down) {
                continue;
            }

            BSONObjBuilder entry(arrayBuilder.subobjStart());
            switch (commandStyle) {
                case ReplSetUpdatePositionCommandStyle::kNewStyle:
                    slaveInfo.lastDurableOpTime.append(&entry,
                                                       UpdatePositionArgs::kDurableOpTimeFieldName);
                    slaveInfo.lastAppliedOpTime.append(&entry,
                                                       UpdatePositionArgs::kAppliedOpTimeFieldName);
                    break;
                case ReplSetUpdatePositionCommandStyle::kOldStyle:
                    entry.append("_id", slaveInfo.rid);
                    if (isV1ElectionProtocol()) {
                        slaveInfo.lastDurableOpTime.append(&entry, "optime");
                    } else {
                        entry.append("optime", slaveInfo.lastDurableOpTime.getTimestamp());
                    }
                    break;
            }
            entry.append(UpdatePositionArgs::kMemberIdFieldName, slaveInfo.memberId);
            entry.append(UpdatePositionArgs::kConfigVersionFieldName, _rsConfig.getConfigVersion());
        }
        arrayBuilder.done();
    }

    // Add metadata to command. Old style parsing logic will reject the metadata.
    if (commandStyle == ReplSetUpdatePositionCommandStyle::kNewStyle) {
        prepareReplMetadata(OpTime(), &cmdBuilder);
    }
    return cmdBuilder.obj();
}

Status ReplicationCoordinatorImpl::processReplSetGetStatus(
    BSONObjBuilder* response, ReplSetGetStatusResponseStyle responseStyle) {

    BSONObj initialSyncProgress;
    if (responseStyle == ReplSetGetStatusResponseStyle::kInitialSync) {
        LockGuard lk(_mutex);
        if (_dr) {
            initialSyncProgress = _dr->getInitialSyncProgress();
        }
    }

    LockGuard topoLock(_topoMutex);

    Status result(ErrorCodes::InternalError, "didn't set status in prepareStatusResponse");
    _topCoord->prepareStatusResponse(
        TopologyCoordinator::ReplSetStatusArgs{
            _replExecutor.now(),
            static_cast<unsigned>(time(0) - serverGlobalParams.started),
            getMyLastAppliedOpTime(),
            getMyLastDurableOpTime(),
            getLastCommittedOpTime(),
            getCurrentCommittedSnapshotOpTime(),
            initialSyncProgress},
        response,
        &result);
    return result;
}

void ReplicationCoordinatorImpl::fillIsMasterForReplSet(IsMasterResponse* response) {
    invariant(getSettings().usingReplSets());

    {
        LockGuard topoLock(_topoMutex);
        _topCoord->fillIsMasterForReplSet(response);
    }

    OpTime lastOpTime = getMyLastAppliedOpTime();
    response->setLastWrite(lastOpTime, lastOpTime.getTimestamp().getSecs());
    if (_currentCommittedSnapshot) {
        OpTime majorityOpTime = _currentCommittedSnapshot->opTime;
        response->setLastMajorityWrite(majorityOpTime, majorityOpTime.getTimestamp().getSecs());
    }

    if (isWaitingForApplierToDrain() || isCatchingUp()) {
        // Report that we are secondary to ismaster callers until drain completes.
        response->setIsMaster(false);
        response->setIsSecondary(true);
    }
}

void ReplicationCoordinatorImpl::appendSlaveInfoData(BSONObjBuilder* result) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _appendSlaveInfoData_inlock(result);
}

void ReplicationCoordinatorImpl::_appendSlaveInfoData_inlock(BSONObjBuilder* result) {
    BSONArrayBuilder replicationProgress(result->subarrayStart("replicationProgress"));
    {
        for (SlaveInfoVector::const_iterator itr = _slaveInfo.begin(); itr != _slaveInfo.end();
             ++itr) {
            BSONObjBuilder entry(replicationProgress.subobjStart());
            entry.append("rid", itr->rid);
            if (isV1ElectionProtocol()) {
                BSONObjBuilder opTime(entry.subobjStart("optime"));
                opTime.append("ts", itr->lastDurableOpTime.getTimestamp());
                opTime.append("term", itr->lastDurableOpTime.getTerm());
                opTime.done();
            } else {
                entry.append("optime", itr->lastDurableOpTime.getTimestamp());
            }
            entry.append("host", itr->hostAndPort.toString());
            if (getReplicationMode() == modeReplSet) {
                if (_selfIndex == -1) {
                    continue;
                }
                invariant(itr->memberId >= 0);
                entry.append("memberId", itr->memberId);
            }
        }
    }
}

ReplicaSetConfig ReplicationCoordinatorImpl::getConfig() const {
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
        LockGuard topoLock(_topoMutex);
        evh = _processReplSetMetadata_incallback(replMetadata);
    }

    if (evh) {
        _replExecutor.waitForEvent(evh);
    }
}

void ReplicationCoordinatorImpl::cancelAndRescheduleElectionTimeout() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _cancelAndRescheduleElectionTimeout_inlock();
}

EventHandle ReplicationCoordinatorImpl::_processReplSetMetadata_incallback(
    const rpc::ReplSetMetadata& replMetadata) {
    if (replMetadata.getConfigVersion() != _rsConfig.getConfigVersion()) {
        return EventHandle();
    }
    _setLastCommittedOpTime(replMetadata.getLastOpCommitted());
    return _updateTerm_incallback(replMetadata.getTerm());
}

bool ReplicationCoordinatorImpl::getMaintenanceMode() {
    LockGuard topoLock(_topoMutex);
    return _topCoord->getMaintenanceCount() > 0;
}

Status ReplicationCoordinatorImpl::setMaintenanceMode(bool activate) {
    if (getReplicationMode() != modeReplSet) {
        return Status(ErrorCodes::NoReplicationEnabled,
                      "can only set maintenance mode on replica set members");
    }

    LockGuard topoLock(_topoMutex);
    if (_topCoord->getRole() == TopologyCoordinator::Role::candidate) {
        return Status(ErrorCodes::NotSecondary, "currently running for election");
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);
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

Status ReplicationCoordinatorImpl::processReplSetSyncFrom(OperationContext* txn,
                                                          const HostAndPort& target,
                                                          BSONObjBuilder* resultObj) {
    Status result(ErrorCodes::InternalError, "didn't set status in prepareSyncFromResponse");
    auto doResync = false;
    {
        LockGuard topoLock(_topoMutex);
        LockGuard lk(_mutex);
        auto opTime = _getMyLastAppliedOpTime_inlock();
        _topCoord->prepareSyncFromResponse(target, opTime, resultObj, &result);
        // If we are in the middle of an initial sync, do a resync.
        doResync = result.isOK() && _dr && _dr->getState() == DataReplicatorState::InitialSync;
    }

    if (doResync) {
        return resyncData(txn, false);
    }

    return result;
}

Status ReplicationCoordinatorImpl::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
    LockGuard topoLock(_topoMutex);

    auto result = _topCoord->prepareFreezeResponse(_replExecutor.now(), secs, resultObj);
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

    LockGuard topoLock(_topoMutex);
    const Date_t now = _replExecutor.now();
    Status result = _topCoord->prepareHeartbeatResponse(now,
                                                        args,
                                                        _settings.ourSetName(),
                                                        getMyLastAppliedOpTime(),
                                                        getMyLastDurableOpTime(),
                                                        response);
    if ((result.isOK() || result == ErrorCodes::InvalidReplicaSetConfig) && _selfIndex < 0) {
        // If this node does not belong to the configuration it knows about, send heartbeats
        // back to any node that sends us a heartbeat, in case one of those remote nodes has
        // a configuration that contains us.  Chances are excellent that it will, since that
        // is the only reason for a remote node to send this node a heartbeat request.
        if (!senderHost.empty() && _seedList.insert(senderHost).second) {
            _scheduleHeartbeatToTarget(senderHost, -1, now);
        }
    } else if (result.isOK() && response->getConfigVersion() < args.getConfigVersion()) {
        // Schedule a heartbeat to the sender to fetch the new config.
        // We cannot cancel the enqueued heartbeat, but either this one or the enqueued heartbeat
        // will trigger reconfig, which cancels and reschedules all heartbeats.

        if (args.hasSenderHost()) {
            int senderIndex = _rsConfig.findMemberIndexByHostAndPort(senderHost);
            _scheduleHeartbeatToTarget(senderHost, senderIndex, now);
        }
    }
    return result;
}

Status ReplicationCoordinatorImpl::processReplSetReconfig(OperationContext* txn,
                                                          const ReplSetReconfigArgs& args,
                                                          BSONObjBuilder* resultObj) {
    log() << "replSetReconfig admin command received from client";

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

    ReplicaSetConfig oldConfig = _rsConfig;
    lk.unlock();

    ReplicaSetConfig newConfig;
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
        _externalState.get(), oldConfig, newConfig, txn->getServiceContext(), args.force);
    if (!myIndex.isOK()) {
        error() << "replSetReconfig got " << myIndex.getStatus() << " while validating "
                << newConfigObj;
        return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      myIndex.getStatus().reason());
    }

    log() << "replSetReconfig config object with " << newConfig.getNumMembers()
          << " members parses ok";

    if (!args.force) {
        status = checkQuorumForReconfig(&_replExecutor, newConfig, myIndex.getValue());
        if (!status.isOK()) {
            error() << "replSetReconfig failed; " << status;
            return status;
        }
    }

    status = _externalState->storeLocalConfigDocument(txn, newConfig.toBSON());
    if (!status.isOK()) {
        error() << "replSetReconfig failed to store config document; " << status;
        return status;
    }

    const stdx::function<void(const ReplicationExecutor::CallbackArgs&)> reconfigFinishFn(
        stdx::bind(&ReplicationCoordinatorImpl::_finishReplSetReconfig,
                   this,
                   stdx::placeholders::_1,
                   newConfig,
                   myIndex.getValue()));

    // If it's a force reconfig, the primary node may not be electable after the configuration
    // change.  In case we are that primary node, finish the reconfig under the global lock,
    // so that the step down occurs safely.
    CBHStatus cbhStatus(ErrorCodes::InternalError, "reconfigFinishFn hasn't been scheduled");
    if (args.force) {
        cbhStatus = _replExecutor.scheduleWorkWithGlobalExclusiveLock(reconfigFinishFn);
    } else {
        cbhStatus = _replExecutor.scheduleWork(reconfigFinishFn);
    }
    if (cbhStatus.getStatus() == ErrorCodes::ShutdownInProgress) {
        return cbhStatus.getStatus();
    }

    fassert(18824, cbhStatus.getStatus());

    configStateGuard.Dismiss();
    _replExecutor.wait(cbhStatus.getValue());
    return Status::OK();
}

void ReplicationCoordinatorImpl::_finishReplSetReconfig(
    const ReplicationExecutor::CallbackArgs& cbData,
    const ReplicaSetConfig& newConfig,
    int myIndex) {
    LockGuard topoLock(_topoMutex);

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_rsConfigState == kConfigReconfiguring);
    invariant(_rsConfig.isInitialized());

    // Do not conduct an election during a reconfig, as the node may not be electable post-reconfig.
    if (_topCoord->getRole() == TopologyCoordinator::Role::candidate) {
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
        // Wait for the election to complete and the node's Role to be set to follower.
        _replExecutor.onEvent(_electionFinishedEvent,
                              stdx::bind(&ReplicationCoordinatorImpl::_finishReplSetReconfig,
                                         this,
                                         stdx::placeholders::_1,
                                         newConfig,
                                         myIndex));
        return;
    }


    const ReplicaSetConfig oldConfig = _rsConfig;
    const PostMemberStateUpdateAction action = _setCurrentRSConfig_inlock(newConfig, myIndex);

    // On a reconfig we drop all snapshots so we don't mistakenely read from the wrong one.
    // For example, if we change the meaning of the "committed" snapshot from applied -> durable.
    _dropAllSnapshots_inlock();

    lk.unlock();
    auto evh = _resetElectionInfoOnProtocolVersionUpgrade(oldConfig, newConfig);
    if (evh) {
        _replExecutor.onEvent(evh, [this, action](const CallbackArgs& cbArgs) {
            LockGuard topoLock(_topoMutex);
            _performPostMemberStateUpdateAction(action);
        });
    } else {
        _performPostMemberStateUpdateAction(action);
    }
}

Status ReplicationCoordinatorImpl::processReplSetInitiate(OperationContext* txn,
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

    ReplicaSetConfig newConfig;
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
        validateConfigForInitiate(_externalState.get(), newConfig, txn->getServiceContext());
    if (!myIndex.isOK()) {
        error() << "replSet initiate got " << myIndex.getStatus() << " while validating "
                << configObj;
        return Status(ErrorCodes::InvalidReplicaSetConfig, myIndex.getStatus().reason());
    }

    log() << "replSetInitiate config object with " << newConfig.getNumMembers()
          << " members parses ok";

    status = checkQuorumForInitiate(&_replExecutor, newConfig, myIndex.getValue());

    if (!status.isOK()) {
        error() << "replSetInitiate failed; " << status;
        return status;
    }

    status = _externalState->initializeReplSetStorage(txn, newConfig.toBSON());
    if (!status.isOK()) {
        error() << "replSetInitiate failed to store config document or create the oplog; "
                << status;
        return status;
    }

    // Since the JournalListener has not yet been set up, we must manually set our
    // durableOpTime.
    setMyLastDurableOpTime(getMyLastAppliedOpTime());

    {
        LockGuard topoLock(_topoMutex);
        _finishReplSetInitiate(newConfig, myIndex.getValue());
    }

    // A configuration passed to replSetInitiate() with the current node as an arbiter
    // will fail validation with a "replSet initiate got ... while validating" reason.
    invariant(!newConfig.getMemberAt(myIndex.getValue()).isArbiter());
    _externalState->startThreads(_settings);
    _startDataReplication(txn);

    configStateGuard.Dismiss();
    return Status::OK();
}

void ReplicationCoordinatorImpl::_finishReplSetInitiate(const ReplicaSetConfig& newConfig,
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
        // Clean up primary states.
        _canAcceptNonLocalWrites = false;
        _isCatchingUp = false;
        _isWaitingForDrainToComplete = false;
        _stepDownPending = false;
        _drainFinishedCond_forTest.notify_all();
        serverGlobalParams.featureCompatibility.validateFeaturesAsMaster.store(false);
        result = kActionCloseAllConnections;
    } else {
        result = kActionFollowerModeStateChange;
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
            _topCoord->processWinElection(_electionId, getNextGlobalTimestamp());
            invariant(!_isCatchingUp);
            invariant(!_isWaitingForDrainToComplete);
            _isCatchingUp = true;
            const PostMemberStateUpdateAction nextAction =
                _updateMemberStateFromTopologyCoordinator_inlock();
            invariant(nextAction != kActionWinElection);
            lk.unlock();
            _performPostMemberStateUpdateAction(nextAction);
            // Notify all secondaries of the election win.
            _scheduleElectionWinNotification();
            lk.lock();
            if (isV1ElectionProtocol()) {
                _scanOpTimeForCatchUp_inlock();
            } else {
                _finishCatchUpOplog_inlock(true);
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

void ReplicationCoordinatorImpl::_scanOpTimeForCatchUp_inlock() {
    auto scanner = std::make_shared<FreshnessScanner>();
    auto scanStartTime = _replExecutor.now();
    auto evhStatus =
        scanner->start(&_replExecutor, _rsConfig, _selfIndex, _rsConfig.getCatchUpTimeoutPeriod());
    if (evhStatus == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassertStatusOK(40254, evhStatus.getStatus());
    long long term = _cachedTerm;
    _replExecutor.onEvent(
        evhStatus.getValue(), [this, scanner, scanStartTime, term](const CallbackArgs& cbData) {
            LockGuard lk(_mutex);
            if (cbData.status == ErrorCodes::CallbackCanceled) {
                _finishCatchUpOplog_inlock(false);
                return;
            }
            auto totalTimeout = _rsConfig.getCatchUpTimeoutPeriod();
            auto catchUpTimeout = totalTimeout - (_replExecutor.now() - scanStartTime);
            _catchUpOplogToLatest_inlock(*scanner, catchUpTimeout, term);
        });
}

void ReplicationCoordinatorImpl::_catchUpOplogToLatest_inlock(const FreshnessScanner& scanner,
                                                              Milliseconds timeout,
                                                              long long originalTerm) {
    // On stepping down, the node doesn't update its term immediately due to SERVER-21425.
    // Term is also checked in case the catchup timeout is so long that the node becomes primary
    // again.
    if (!_memberState.primary() || originalTerm != _cachedTerm) {
        log() << "Stopped transition to primary of term " << originalTerm
              << " because I've already stepped down.";
        _finishCatchUpOplog_inlock(false);
        return;
    }

    auto result = scanner.getResult();

    // Cannot access any nodes within timeout.
    if (result.size() == 0) {
        log() << "Could not access any nodes within timeout when checking for "
              << "additional ops to apply before finishing transition to primary. "
              << "Will move forward with becoming primary anyway.";
        _finishCatchUpOplog_inlock(true);
        return;
    }

    // I'm most up-to-date as far as I know.
    auto freshnessInfo = result.front();
    if (freshnessInfo.opTime <= _getMyLastAppliedOpTime_inlock()) {
        log() << "My optime is most up-to-date, skipping catch-up "
              << "and completing transition to primary.";
        _finishCatchUpOplog_inlock(true);
        return;
    }

    // Wait for the replication level to reach the latest opTime within timeout.
    auto latestOpTime = freshnessInfo.opTime;
    auto finishCB = [this, latestOpTime]() {
        if (latestOpTime > _getMyLastAppliedOpTime_inlock()) {
            log() << "Cannot catch up oplog after becoming primary.";
        } else {
            log() << "Finished catch-up oplog after becoming primary.";
        }

        _finishCatchUpOplog_inlock(true);
    };
    auto waiterInfo = std::make_shared<WaiterInfo>(freshnessInfo.opTime, finishCB);

    _opTimeWaiterList.add_inlock(waiterInfo.get());
    auto timeoutCB = [this, waiterInfo, finishCB](const CallbackArgs& cbData) {
        LockGuard lk(_mutex);
        if (_opTimeWaiterList.remove_inlock(waiterInfo.get())) {
            finishCB();
        }
    };
    // Schedule the timeout callback. It may signal after we have already caught up.
    _replExecutor.scheduleWorkAt(_replExecutor.now() + timeout, timeoutCB);
}

void ReplicationCoordinatorImpl::_finishCatchUpOplog_inlock(bool startToDrain) {
    _isCatchingUp = false;
    // If the node steps down during the catch-up, we don't go into drain mode.
    if (startToDrain) {
        invariant(_getMemberState_inlock().primary());
        invariant(!_canAcceptNonLocalWrites);
        invariant(!_isWaitingForDrainToComplete);
        _isWaitingForDrainToComplete = true;
        // Signal applier in executor to avoid the deadlock with bgsync's mutex that is required to
        // cancel fetcher.
        _replExecutor.scheduleWork(
            _wrapAsCallbackFn([this]() { _externalState->signalApplierToCancelFetcher(); }));
    }
}

Status ReplicationCoordinatorImpl::processReplSetGetRBID(BSONObjBuilder* resultObj) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    resultObj->append("rbid", _rbid);
    return Status::OK();
}

void ReplicationCoordinatorImpl::incrementRollbackID() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    ++_rbid;
}

Status ReplicationCoordinatorImpl::processReplSetFresh(const ReplSetFreshArgs& args,
                                                       BSONObjBuilder* resultObj) {
    LockGuard topoLock(_topoMutex);
    Status result(ErrorCodes::InternalError, "didn't set status in prepareFreshResponse");
    _topCoord->prepareFreshResponse(
        args, _replExecutor.now(), getMyLastAppliedOpTime(), resultObj, &result);
    return result;
}

Status ReplicationCoordinatorImpl::processReplSetElect(const ReplSetElectArgs& args,
                                                       BSONObjBuilder* responseObj) {
    LockGuard topoLock(_topoMutex);
    Status result = Status(ErrorCodes::InternalError, "status not set by callback");
    _topCoord->prepareElectResponse(
        args, _replExecutor.now(), getMyLastAppliedOpTime(), responseObj, &result);
    return result;
}

ReplicationCoordinatorImpl::PostMemberStateUpdateAction
ReplicationCoordinatorImpl::_setCurrentRSConfig_inlock(const ReplicaSetConfig& newConfig,
                                                       int myIndex) {
    invariant(_settings.usingReplSets());
    _cancelHeartbeats_inlock();
    _setConfigState_inlock(kConfigSteady);

    // Must get this before changing our config.
    OpTime myOptime = _getMyLastAppliedOpTime_inlock();
    _topCoord->updateConfig(newConfig, myIndex, _replExecutor.now(), myOptime);
    _cachedTerm = _topCoord->getTerm();
    const ReplicaSetConfig oldConfig = _rsConfig;
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

    _cancelPriorityTakeover_inlock();
    _cancelAndRescheduleElectionTimeout_inlock();

    const PostMemberStateUpdateAction action = _updateMemberStateFromTopologyCoordinator_inlock();
    _updateSlaveInfoFromConfig_inlock();
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
            _topCoord->setElectionInfo(_electionId, getNextGlobalTimestamp());
        } else if (oldConfig.getProtocolVersion() < newConfig.getProtocolVersion()) {
            // Upgrade
            invariant(newConfig.getProtocolVersion() == 1);
            invariant(_topCoord->getTerm() != OpTime::kUninitializedTerm);
            _electionId = OID::fromTerm(_topCoord->getTerm());
            _topCoord->setElectionInfo(_electionId, getNextGlobalTimestamp());
        }
    }

    _wakeReadyWaiters_inlock();
    return action;
}

void ReplicationCoordinatorImpl::_wakeReadyWaiters_inlock() {
    _replicationWaiterList.signalAndRemoveIf_inlock([this](WaiterInfo* waiter) {
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

Status ReplicationCoordinatorImpl::processHandshake(OperationContext* txn,
                                                    const HandshakeArgs& handshake) {
    LOG(2) << "Received handshake " << handshake.toBSON();

    stdx::unique_lock<stdx::mutex> lock(_mutex);

    if (getReplicationMode() != modeMasterSlave) {
        return Status(ErrorCodes::IllegalOperation,
                      "The handshake command is only used for master/slave replication");
    }

    SlaveInfo* slaveInfo = _findSlaveInfoByRID_inlock(handshake.getRid());
    if (slaveInfo) {
        return Status::OK();  // nothing to do
    }

    SlaveInfo newSlaveInfo;
    newSlaveInfo.rid = handshake.getRid();
    newSlaveInfo.memberId = -1;
    newSlaveInfo.hostAndPort = _externalState->getClientHostAndPort(txn);
    // Don't call _addSlaveInfo_inlock as that would wake sleepers unnecessarily.
    _slaveInfo.push_back(newSlaveInfo);

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
    std::vector<HostAndPort> hosts;
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    for (size_t i = 0; i < _slaveInfo.size(); ++i) {
        const SlaveInfo& slaveInfo = _slaveInfo[i];
        if (getReplicationMode() == modeMasterSlave && slaveInfo.rid == _getMyRID_inlock()) {
            // Master-slave doesn't know the HostAndPort for itself at this point.
            continue;
        }

        if (durablyWritten) {
            if (slaveInfo.lastDurableOpTime < op) {
                continue;
            }
        } else if (slaveInfo.lastAppliedOpTime < op) {
            continue;
        }

        hosts.push_back(slaveInfo.hostAndPort);
    }
    return hosts;
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

HostAndPort ReplicationCoordinatorImpl::chooseNewSyncSource(const Timestamp& lastTimestampFetched) {
    LockGuard topoLock(_topoMutex);

    HostAndPort oldSyncSource = _topCoord->getSyncSourceAddress();
    auto chainingPreference = isCatchingUp()
        ? TopologyCoordinator::ChainingPreference::kAllowChaining
        : TopologyCoordinator::ChainingPreference::kUseConfiguration;
    HostAndPort newSyncSource = _topCoord->chooseNewSyncSource(
        _replExecutor.now(), lastTimestampFetched, chainingPreference);

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    // If we lost our sync source, schedule new heartbeats immediately to update our knowledge
    // of other members's state, allowing us to make informed sync source decisions.
    if (newSyncSource.empty() && !oldSyncSource.empty() && _selfIndex >= 0 &&
        !_getMemberState_inlock().primary()) {
        _restartHeartbeats_inlock();
    }

    return newSyncSource;
}

void ReplicationCoordinatorImpl::_unblacklistSyncSource(
    const ReplicationExecutor::CallbackArgs& cbData, const HostAndPort& host) {
    if (cbData.status == ErrorCodes::CallbackCanceled)
        return;

    LockGuard topoLock(_topoMutex);
    _topCoord->unblacklistSyncSource(host, _replExecutor.now());
}

void ReplicationCoordinatorImpl::blacklistSyncSource(const HostAndPort& host, Date_t until) {
    LockGuard topoLock(_topoMutex);
    _topCoord->blacklistSyncSource(host, until);
    _scheduleWorkAt(until,
                    stdx::bind(&ReplicationCoordinatorImpl::_unblacklistSyncSource,
                               this,
                               stdx::placeholders::_1,
                               host));
}

void ReplicationCoordinatorImpl::resetLastOpTimesFromOplog(OperationContext* txn) {
    StatusWith<OpTime> lastOpTimeStatus = _externalState->loadLastOpTime(txn);
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

    _externalState->setGlobalTimestamp(lastOpTime.getTimestamp());
}

bool ReplicationCoordinatorImpl::shouldChangeSyncSource(const HostAndPort& currentSource,
                                                        const rpc::ReplSetMetadata& metadata) {
    LockGuard topoLock(_topoMutex);
    return _topCoord->shouldChangeSyncSource(
        currentSource, getMyLastAppliedOpTime(), metadata, _replExecutor.now());
}

void ReplicationCoordinatorImpl::_updateLastCommittedOpTime_inlock() {
    if (!_getMemberState_inlock().primary()) {
        return;
    }

    std::vector<OpTime> votingNodesOpTimes;

    // Whether we use the applied or durable OpTime for the commit point is decided here.
    const bool useDurableOpTime = getWriteConcernMajorityShouldJournal_inlock();

    for (const auto& sI : _slaveInfo) {
        auto memberConfig = _rsConfig.findMemberByID(sI.memberId);
        invariant(memberConfig);
        if (memberConfig->isVoter()) {
            const auto opTime = useDurableOpTime ? sI.lastDurableOpTime : sI.lastAppliedOpTime;
            votingNodesOpTimes.push_back(opTime);
        }
    }

    invariant(votingNodesOpTimes.size() > 0);
    if (votingNodesOpTimes.size() < static_cast<unsigned long>(_rsConfig.getWriteMajority())) {
        return;
    }
    std::sort(votingNodesOpTimes.begin(), votingNodesOpTimes.end());

    // need the majority to have this OpTime
    OpTime committedOpTime =
        votingNodesOpTimes[votingNodesOpTimes.size() - _rsConfig.getWriteMajority()];
    _setLastCommittedOpTime_inlock(committedOpTime);
}

void ReplicationCoordinatorImpl::_setLastCommittedOpTime(const OpTime& committedOpTime) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _setLastCommittedOpTime_inlock(committedOpTime);
}

void ReplicationCoordinatorImpl::_setLastCommittedOpTime_inlock(const OpTime& committedOpTime) {
    if (committedOpTime == _lastCommittedOpTime) {
        return;  // Hasn't changed, so ignore it.
    } else if (committedOpTime < _lastCommittedOpTime) {
        LOG(1) << "Ignoring older committed snapshot optime: " << committedOpTime
               << ", currentCommittedOpTime: " << _lastCommittedOpTime;
        return;  // This may have come from an out-of-order heartbeat. Ignore it.
    }

    // This check is performed to ensure primaries do not commit an OpTime from a previous term.
    if (_getMemberState_inlock().primary() && committedOpTime < _firstOpTimeOfMyTerm) {
        LOG(1) << "Ignoring older committed snapshot from before I became primary, optime: "
               << committedOpTime << ", firstOpTimeOfMyTerm: " << _firstOpTimeOfMyTerm;
        return;
    }

    if (_getMemberState_inlock().arbiter()) {
        _setMyLastAppliedOpTime_inlock(committedOpTime, false);
    }

    LOG(2) << "Updating _lastCommittedOpTime to " << committedOpTime;
    _lastCommittedOpTime = committedOpTime;

    _externalState->notifyOplogMetadataWaiters();

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

void ReplicationCoordinatorImpl::_setFirstOpTimeOfMyTerm(const OpTime& newOpTime) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _firstOpTimeOfMyTerm = newOpTime;
}

OpTime ReplicationCoordinatorImpl::getLastCommittedOpTime() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _lastCommittedOpTime;
}

Status ReplicationCoordinatorImpl::processReplSetRequestVotes(
    OperationContext* txn,
    const ReplSetRequestVotesArgs& args,
    ReplSetRequestVotesResponse* response) {
    if (!isV1ElectionProtocol()) {
        return {ErrorCodes::BadValue, "not using election protocol v1"};
    }

    auto termStatus = updateTerm(txn, args.getTerm());
    if (!termStatus.isOK() && termStatus.code() != ErrorCodes::StaleTerm)
        return termStatus;

    {
        LockGuard topoLock(_topoMutex);
        LockGuard lk(_mutex);
        _topCoord->processReplSetRequestVotes(args, response, _getMyLastAppliedOpTime_inlock());
    }

    if (response->getVoteGranted()) {
        LastVote lastVote;
        lastVote.setTerm(args.getTerm());
        lastVote.setCandidateIndex(args.getCandidateIndex());

        Status status = _externalState->storeLocalLastVoteDocument(txn, lastVote);
        if (!status.isOK()) {
            error() << "replSetRequestVotes failed to store LastVote document; " << status;
            return status;
        }
    }
    return Status::OK();
}

void ReplicationCoordinatorImpl::prepareReplMetadata(const OpTime& lastOpTimeFromClient,
                                                     BSONObjBuilder* builder) const {
    rpc::ReplSetMetadata metadata;
    LockGuard topoLock(_topoMutex);

    OpTime lastReadableOpTime = getCurrentCommittedSnapshotOpTime();
    OpTime lastVisibleOpTime = std::max(lastOpTimeFromClient, lastReadableOpTime);
    _topCoord->prepareReplMetadata(&metadata, lastVisibleOpTime, _lastCommittedOpTime);
    metadata.writeToMetadata(builder);
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
    LockGuard topoLock(_topoMutex);

    auto senderHost(args.getSenderHost());
    const Date_t now = _replExecutor.now();
    result = _topCoord->prepareHeartbeatResponseV1(now,
                                                   args,
                                                   _settings.ourSetName(),
                                                   getMyLastAppliedOpTime(),
                                                   getMyLastDurableOpTime(),
                                                   response);

    if ((result.isOK() || result == ErrorCodes::InvalidReplicaSetConfig) && _selfIndex < 0) {
        // If this node does not belong to the configuration it knows about, send heartbeats
        // back to any node that sends us a heartbeat, in case one of those remote nodes has
        // a configuration that contains us.  Chances are excellent that it will, since that
        // is the only reason for a remote node to send this node a heartbeat request.
        if (!senderHost.empty() && _seedList.insert(senderHost).second) {
            _scheduleHeartbeatToTarget(senderHost, -1, now);
        }
    } else if (result.isOK() && response->getConfigVersion() < args.getConfigVersion()) {
        // Schedule a heartbeat to the sender to fetch the new config.
        // We cannot cancel the enqueued heartbeat, but either this one or the enqueued heartbeat
        // will trigger reconfig, which cancels and reschedules all heartbeats.
        if (args.hasSender()) {
            int senderIndex = _rsConfig.findMemberIndexByHostAndPort(senderHost);
            _scheduleHeartbeatToTarget(senderHost, senderIndex, now);
        }
    } else if (result.isOK()) {
        // Update liveness for sending node.
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        auto slaveInfo = _findSlaveInfoByMemberID_inlock(args.getSenderId());
        if (!slaveInfo) {
            return result;
        }
        slaveInfo->lastUpdate = _replExecutor.now();
        slaveInfo->down = false;
    }
    return result;
}

void ReplicationCoordinatorImpl::summarizeAsHtml(ReplSetHtmlSummary* output) {
    LockGuard topoLock(_topoMutex);

    // TODO(dannenberg) consider putting both optimes into the htmlsummary.
    output->setSelfOptime(getMyLastAppliedOpTime());
    output->setSelfUptime(time(0) - serverGlobalParams.started);
    output->setNow(_replExecutor.now());

    _topCoord->summarizeAsHtml(output);
}

long long ReplicationCoordinatorImpl::getTerm() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _cachedTerm;
}

EventHandle ReplicationCoordinatorImpl::updateTerm_forTest(
    long long term, TopologyCoordinator::UpdateTermResult* updateResult) {
    LockGuard topoLock(_topoMutex);

    EventHandle finishEvh;
    finishEvh = _updateTerm_incallback(term, updateResult);
    if (!finishEvh) {
        auto finishEvhStatus = _replExecutor.makeEvent();
        invariantOK(finishEvhStatus.getStatus());
        finishEvh = finishEvhStatus.getValue();
        _replExecutor.signalEvent(finishEvh);
    }
    return finishEvh;
}

Status ReplicationCoordinatorImpl::updateTerm(OperationContext* txn, long long term) {
    // Term is only valid if we are replicating.
    if (getReplicationMode() != modeReplSet) {
        return {ErrorCodes::BadValue, "cannot supply 'term' without active replication"};
    }

    if (!isV1ElectionProtocol()) {
        // Do not update if not in V1 protocol.
        return Status::OK();
    }

    // Check we haven't acquired any lock, because potential stepdown needs global lock.
    dassert(!txn->lockState()->isLocked());
    TopologyCoordinator::UpdateTermResult updateTermResult;
    EventHandle finishEvh;

    {
        LockGuard topoLock(_topoMutex);
        finishEvh = _updateTerm_incallback(term, &updateTermResult);
    }

    // Wait for potential stepdown to finish.
    if (finishEvh.isValid()) {
        _replExecutor.waitForEvent(finishEvh);
    }
    if (updateTermResult == TopologyCoordinator::UpdateTermResult::kUpdatedTerm ||
        updateTermResult == TopologyCoordinator::UpdateTermResult::kTriggerStepDown) {
        return {ErrorCodes::StaleTerm, "Replication term of this node was stale; retry query"};
    }

    return Status::OK();
}

EventHandle ReplicationCoordinatorImpl::_updateTerm_incallback(
    long long term, TopologyCoordinator::UpdateTermResult* updateTermResult) {
    if (!isV1ElectionProtocol()) {
        LOG(3) << "Cannot update term in election protocol version 0";
        return EventHandle();
    }

    auto now = _replExecutor.now();
    TopologyCoordinator::UpdateTermResult localUpdateTermResult = _topCoord->updateTerm(term, now);
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _cachedTerm = _topCoord->getTerm();

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
        return _stepDownStart(false);
    }
    return EventHandle();
}

SnapshotName ReplicationCoordinatorImpl::reserveSnapshotName(OperationContext* txn) {
    auto reservedName = SnapshotName(_snapshotNameGenerator.addAndFetch(1));
    dassert(reservedName > SnapshotName::min());
    dassert(reservedName < SnapshotName::max());
    if (txn) {
        ReplClientInfo::forClient(txn->getClient()).setLastSnapshot(reservedName);
    }
    return reservedName;
}

void ReplicationCoordinatorImpl::forceSnapshotCreation() {
    _externalState->forceSnapshotCreation();
}

void ReplicationCoordinatorImpl::waitUntilSnapshotCommitted(OperationContext* txn,
                                                            const SnapshotName& untilSnapshot) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    while (!_currentCommittedSnapshot || _currentCommittedSnapshot->name < untilSnapshot) {
        txn->waitForConditionOrInterrupt(_currentCommittedSnapshotCond, lock);
    }
}

size_t ReplicationCoordinatorImpl::getNumUncommittedSnapshots() {
    return _uncommittedSnapshotsSize.load();
}

void ReplicationCoordinatorImpl::onSnapshotCreate(OpTime timeOfSnapshot, SnapshotName name) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    auto snapshotInfo = SnapshotInfo{timeOfSnapshot, name};

    if (timeOfSnapshot <= _lastCommittedOpTime) {
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
    invariant(newCommittedSnapshot.opTime <= _lastCommittedOpTime);
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
        _replExecutor.waitForEvent(_electionFinishedEvent);
    }
}

void ReplicationCoordinatorImpl::waitForElectionDryRunFinish_forTest() {
    if (_electionDryRunFinishedEvent.isValid()) {
        _replExecutor.waitForEvent(_electionDryRunFinishedEvent);
    }
}

EventHandle ReplicationCoordinatorImpl::_resetElectionInfoOnProtocolVersionUpgrade(
    const ReplicaSetConfig& oldConfig, const ReplicaSetConfig& newConfig) {
    // On protocol version upgrade, reset last vote as if I just learned the term 0 from other
    // nodes.
    if (!oldConfig.isInitialized() ||
        oldConfig.getProtocolVersion() >= newConfig.getProtocolVersion()) {
        return {};
    }
    invariant(newConfig.getProtocolVersion() == 1);

    // Write last vote
    auto evhStatus = _replExecutor.makeEvent();
    if (evhStatus.getStatus() == ErrorCodes::ShutdownInProgress) {
        return {};
    }
    invariant(evhStatus.isOK());
    auto evh = evhStatus.getValue();

    auto cbStatus = _replExecutor.scheduleDBWork([this, evh](const CallbackArgs& cbData) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        invariant(cbData.txn);

        LastVote lastVote;
        lastVote.setTerm(OpTime::kInitialTerm);
        lastVote.setCandidateIndex(-1);
        auto status = _externalState->storeLocalLastVoteDocument(cbData.txn, lastVote);
        invariant(status.isOK());
        _replExecutor.signalEvent(evh);
    });
    if (cbStatus.getStatus() == ErrorCodes::ShutdownInProgress) {
        return {};
    }
    invariant(cbStatus.isOK());
    return evh;
}

CallbackHandle ReplicationCoordinatorImpl::_scheduleWork(const CallbackFn& work) {
    auto scheduleFn = [this](const CallbackFn& workWrapped) {
        return _replExecutor.scheduleWork(workWrapped);
    };
    return _wrapAndScheduleWork(scheduleFn, work);
}

CallbackHandle ReplicationCoordinatorImpl::_scheduleWorkAt(Date_t when, const CallbackFn& work) {
    auto scheduleFn = [this, when](const CallbackFn& workWrapped) {
        return _replExecutor.scheduleWorkAt(when, workWrapped);
    };
    return _wrapAndScheduleWork(scheduleFn, work);
}

void ReplicationCoordinatorImpl::_scheduleWorkAndWaitForCompletion(const CallbackFn& work) {
    if (auto handle = _scheduleWork(work)) {
        _replExecutor.wait(handle);
    }
}

void ReplicationCoordinatorImpl::_scheduleWorkAtAndWaitForCompletion(Date_t when,
                                                                     const CallbackFn& work) {
    if (auto handle = _scheduleWorkAt(when, work)) {
        _replExecutor.wait(handle);
    }
}

CallbackHandle ReplicationCoordinatorImpl::_scheduleDBWork(const CallbackFn& work) {
    auto scheduleFn = [this](const CallbackFn& workWrapped) {
        return _replExecutor.scheduleDBWork(workWrapped);
    };
    return _wrapAndScheduleWork(scheduleFn, work);
}

CallbackHandle ReplicationCoordinatorImpl::_wrapAndScheduleWork(ScheduleFn scheduleFn,
                                                                const CallbackFn& work) {
    auto workWrapped = [this, work](const CallbackArgs& args) {
        if (args.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        work(args);
    };
    auto cbh = scheduleFn(workWrapped);
    if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return CallbackHandle();
    }
    fassert(28800, cbh.getStatus());
    return cbh.getValue();
}

EventHandle ReplicationCoordinatorImpl::_makeEvent() {
    auto eventResult = this->_replExecutor.makeEvent();
    if (eventResult.getStatus() == ErrorCodes::ShutdownInProgress) {
        return EventHandle();
    }
    fassert(28825, eventResult.getStatus());
    return eventResult.getValue();
}

void ReplicationCoordinatorImpl::_scheduleElectionWinNotification() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (!_getMemberState_inlock().primary()) {
        return;
    }

    _restartHeartbeats_inlock();
}

WriteConcernOptions ReplicationCoordinatorImpl::populateUnsetWriteConcernOptionsSyncMode(
    WriteConcernOptions wc) {
    WriteConcernOptions writeConcern(wc);
    if (writeConcern.syncMode == WriteConcernOptions::SyncMode::UNSET) {
        if (writeConcern.wMode == WriteConcernOptions::kMajority && _isDurableStorageEngine() &&
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

    _startElectSelfIfEligibleV1(false);
    EventHandle finishEvent;
    {
        LockGuard lk(_mutex);
        finishEvent = _electionFinishedEvent;
    }
    if (finishEvent.isValid()) {
        _replExecutor.waitForEvent(finishEvent);
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


}  // namespace repl
}  // namespace mongo

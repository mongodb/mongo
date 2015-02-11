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
#include <boost/thread.hpp>

#include "mongo/base/status.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/global_optime.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/check_quorum_for_config_change.h"
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/db/repl/handshake_args.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replica_set_config_checks.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/server_options.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {

namespace {
    typedef StatusWith<ReplicationExecutor::CallbackHandle> CBHStatus;

    void lockAndCall(boost::unique_lock<boost::mutex>* lk, const stdx::function<void ()>& fn) {
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
            if (elem.fieldNameStringData() == ReplicaSetConfig::kVersionFieldName &&
                elem.isNumber()) {

                boost::scoped_ptr<SecureRandom> generator(SecureRandom::create());
                const int random = std::abs(static_cast<int>(generator->nextInt64()) % 100000);
                builder.appendIntOrLL(ReplicaSetConfig::kVersionFieldName,
                                      elem.numberLong() + 10000 + random);
            }
            else {
                builder.append(elem);
            }
        }
        return builder.obj();
    }

} //namespace

    struct ReplicationCoordinatorImpl::WaiterInfo {

        /**
         * Constructor takes the list of waiters and enqueues itself on the list, removing itself
         * in the destructor.
         */
        WaiterInfo(std::vector<WaiterInfo*>* _list,
                   unsigned int _opID,
                   const OpTime* _opTime,
                   const WriteConcernOptions* _writeConcern,
                   boost::condition_variable* _condVar) : list(_list),
                                                          master(true),
                                                          opID(_opID),
                                                          opTime(_opTime),
                                                          writeConcern(_writeConcern),
                                                          condVar(_condVar) {
            list->push_back(this);
        }

        ~WaiterInfo() {
            list->erase(std::remove(list->begin(), list->end(), this), list->end());
        }

        std::vector<WaiterInfo*>* list;
        bool master; // Set to false to indicate that stepDown was called while waiting
        const unsigned int opID;
        const OpTime* opTime;
        const WriteConcernOptions* writeConcern;
        boost::condition_variable* condVar;
    };

namespace {
    ReplicationCoordinator::Mode getReplicationModeFromSettings(const ReplSettings& settings) {
        if (settings.usingReplSets()) {
            return ReplicationCoordinator::modeReplSet;
        }
        if (settings.master || settings.slave) {
            return ReplicationCoordinator::modeMasterSlave;
        }
        return ReplicationCoordinator::modeNone;
    }
}  // namespace

    ReplicationCoordinatorImpl::ReplicationCoordinatorImpl(
            const ReplSettings& settings,
            ReplicationCoordinatorExternalState* externalState,
            ReplicationExecutor::NetworkInterface* network,
            TopologyCoordinator* topCoord,
            int64_t prngSeed) :
        _settings(settings),
        _replMode(getReplicationModeFromSettings(settings)),
        _topCoord(topCoord),
        _replExecutor(network, prngSeed),
        _externalState(externalState),
        _inShutdown(false),
        _memberState(MemberState::RS_STARTUP),
        _isWaitingForDrainToComplete(false),
        _rsConfigState(kConfigPreStart),
        _selfIndex(-1),
        _sleptLastElection(false),
        _canAcceptNonLocalWrites(!(settings.usingReplSets() || settings.slave)),
        _canServeNonLocalReads(0U) {

        if (!isReplEnabled()) {
            return;
        }

        boost::scoped_ptr<SecureRandom> rbidGenerator(SecureRandom::create());
        _rbid = static_cast<int>(rbidGenerator->nextInt64());
        if (_rbid < 0) {
            // Ensure _rbid is always positive
            _rbid = -_rbid;
        }

        // Make sure there is always an entry in _slaveInfo for ourself.
        SlaveInfo selfInfo;
        selfInfo.self = true;
        _slaveInfo.push_back(selfInfo);
    }

    ReplicationCoordinatorImpl::~ReplicationCoordinatorImpl() {}

    void ReplicationCoordinatorImpl::waitForStartUpComplete() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        while (_rsConfigState == kConfigPreStart || _rsConfigState == kConfigStartingUp) {
            _rsConfigStateChange.wait(lk);
        }
    }

    ReplicaSetConfig ReplicationCoordinatorImpl::getReplicaSetConfig_forTest() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _rsConfig;
    }

    bool ReplicationCoordinatorImpl::_startLoadLocalConfig(OperationContext* txn) {

        StatusWith<BSONObj> cfg = _externalState->loadLocalConfigDocument(txn);
        if (!cfg.isOK()) {
            log() << "Did not find local replica set configuration document at startup;  " <<
                cfg.getStatus();
            return true;
        }
        ReplicaSetConfig localConfig;
        Status status = localConfig.initialize(cfg.getValue());
        if (!status.isOK()) {
            error() << "Locally stored replica set configuration does not parse; See "
                    "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config "
                    "for information on how to recover from this. Got \"" <<
                    status << "\" while parsing " << cfg.getValue();
            fassertFailedNoTrace(28545);
        }

        StatusWith<OpTime> lastOpTimeStatus = _externalState->loadLastOpTime(txn);

        // Use a callback here, because _finishLoadLocalConfig calls isself() which requires
        // that the server's networking layer be up and running and accepting connections, which
        // doesn't happen until startReplication finishes.
        _replExecutor.scheduleWork(
                stdx::bind(&ReplicationCoordinatorImpl::_finishLoadLocalConfig,
                           this,
                           stdx::placeholders::_1,
                           localConfig,
                           lastOpTimeStatus));
        return false;
    }

    void ReplicationCoordinatorImpl::_finishLoadLocalConfig(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicaSetConfig& localConfig,
            const StatusWith<OpTime>& lastOpTimeStatus) {
        if (!cbData.status.isOK()) {
            LOG(1) << "Loading local replica set configuration failed due to " << cbData.status;
            return;
        }

        StatusWith<int> myIndex = validateConfigForStartUp(_externalState.get(),
                                                           _rsConfig,
                                                           localConfig);
        if (!myIndex.isOK()) {
            if (myIndex.getStatus() == ErrorCodes::NodeNotFound ||
                    myIndex.getStatus() == ErrorCodes::DuplicateKey) {
                warning() << "Locally stored replica set configuration does not have a valid entry "
                        "for the current node; waiting for reconfig or remote heartbeat; Got \"" <<
                        myIndex.getStatus() << "\" while validating " << localConfig.toBSON();
                myIndex = StatusWith<int>(-1);
            }
            else {
                error() << "Locally stored replica set configuration is invalid; See "
                        "http://www.mongodb.org/dochub/core/recover-replica-set-from-invalid-config"
                        " for information on how to recover from this. Got \"" <<
                        myIndex.getStatus() << "\" while validating " << localConfig.toBSON();
                fassertFailedNoTrace(28544);
            }
        }

        if (localConfig.getReplSetName() != _settings.ourSetName()) {
            warning() << "Local replica set configuration document reports set name of " <<
                localConfig.getReplSetName() << ", but command line reports " <<
                _settings.ourSetName() << "; waitng for reconfig or remote heartbeat";
            myIndex = StatusWith<int>(-1);
        }

        // Do not check optime, if this node is an arbiter.
        bool isArbiter = myIndex.getValue() != -1 &&
            localConfig.getMemberAt(myIndex.getValue()).isArbiter();
        OpTime lastOpTime(0, 0);
        if (!isArbiter) {
            if (!lastOpTimeStatus.isOK()) {
                warning() << "Failed to load timestamp of most recently applied operation; " <<
                    lastOpTimeStatus.getStatus();
            }
            else {
                lastOpTime = lastOpTimeStatus.getValue();
            }
        }

        boost::unique_lock<boost::mutex> lk(_mutex);
        invariant(_rsConfigState == kConfigStartingUp);
        const PostMemberStateUpdateAction action =
            _setCurrentRSConfig_inlock(localConfig, myIndex.getValue());
        _setMyLastOptime_inlock(&lk, lastOpTime, false);
        _externalState->setGlobalOpTime(lastOpTime);
        if (lk.owns_lock()) {
            lk.unlock();
        }
        _performPostMemberStateUpdateAction(action);
        _externalState->startThreads();
    }

    void ReplicationCoordinatorImpl::startReplication(OperationContext* txn) {
        if (!isReplEnabled()) {
            boost::lock_guard<boost::mutex> lk(_mutex);
            _setConfigState_inlock(kConfigReplicationDisabled);
            return;
        }

        {
            OID rid = _externalState->ensureMe(txn);

            boost::lock_guard<boost::mutex> lk(_mutex);
            fassert(18822, !_inShutdown);
            _setConfigState_inlock(kConfigStartingUp);
            _myRID = rid;
            _slaveInfo[_getMyIndexInSlaveInfo_inlock()].rid = rid;
        }

        if (!_settings.usingReplSets()) {
            // Must be Master/Slave
            invariant(_settings.master || _settings.slave);
            _externalState->startMasterSlave(txn);
            return;
        }

        _topCoordDriverThread.reset(new boost::thread(stdx::bind(&ReplicationExecutor::run,
                                                                 &_replExecutor)));

        bool doneLoadingConfig = _startLoadLocalConfig(txn);
        if (doneLoadingConfig) {
            // If we're not done loading the config, then the config state will be set by
            // _finishLoadLocalConfig.
            boost::lock_guard<boost::mutex> lk(_mutex);
            invariant(!_rsConfig.isInitialized());
            _setConfigState_inlock(kConfigUninitialized);
        }
    }

    void ReplicationCoordinatorImpl::shutdown() {
        // Shutdown must:
        // * prevent new threads from blocking in awaitReplication
        // * wake up all existing threads blocking in awaitReplication
        // * tell the ReplicationExecutor to shut down
        // * wait for the thread running the ReplicationExecutor to finish

        if (!_settings.usingReplSets()) {
            return;
        }

        boost::thread* hbReconfigThread = NULL;
        {
            boost::lock_guard<boost::mutex> lk(_mutex);
            fassert(28533, !_inShutdown);
            _inShutdown = true;
            if (_rsConfigState == kConfigPreStart) {
                warning() << "ReplicationCoordinatorImpl::shutdown() called before "
                        "startReplication() finished.  Shutting down without cleaning up the "
                        "replication system";
                return;
            }
            fassert(18823, _rsConfigState != kConfigStartingUp);
            for (std::vector<WaiterInfo*>::iterator it = _replicationWaiterList.begin();
                    it != _replicationWaiterList.end(); ++it) {
                WaiterInfo* waiter = *it;
                waiter->condVar->notify_all();
            }

            // Since we've set _inShutdown we know that _heartbeatReconfigThread will not be
            // changed again, which makes it safe to store the pointer to it to be accessed outside
            // of _mutex.
            hbReconfigThread = _heartbeatReconfigThread.get();
        }

        if (hbReconfigThread) {
            hbReconfigThread->join();
        }

        _replExecutor.shutdown();
        _topCoordDriverThread->join(); // must happen outside _mutex
        _externalState->shutdown();
    }

    const ReplSettings& ReplicationCoordinatorImpl::getSettings() const {
        return _settings;
    }

    ReplicationCoordinator::Mode ReplicationCoordinatorImpl::getReplicationMode() const {
        return _getReplicationMode_inlock();
    }

    ReplicationCoordinator::Mode ReplicationCoordinatorImpl::_getReplicationMode_inlock() const {
        return _replMode;
    }

    MemberState ReplicationCoordinatorImpl::getMemberState() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _getMemberState_inlock();
    }

    MemberState ReplicationCoordinatorImpl::_getMemberState_inlock() const {
        return _memberState;
    }

    Seconds ReplicationCoordinatorImpl::getSlaveDelaySecs() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(_rsConfig.isInitialized());
        uassert(28524,
                "Node not a member of the current set configuration",
                _selfIndex != -1);
        return _rsConfig.getMemberAt(_selfIndex).getSlaveDelay();
    }

    void ReplicationCoordinatorImpl::clearSyncSourceBlacklist() {
        CBHStatus cbh = _replExecutor.scheduleWork(
                stdx::bind(&ReplicationCoordinatorImpl::_clearSyncSourceBlacklist_finish,
                           this,
                           stdx::placeholders::_1));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return;
        }
        fassert(18907, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
    }

    void ReplicationCoordinatorImpl::_clearSyncSourceBlacklist_finish(
            const ReplicationExecutor::CallbackData& cbData) {
        if (cbData.status == ErrorCodes::CallbackCanceled)
            return;
        _topCoord->clearSyncSourceBlacklist();
    }

    bool ReplicationCoordinatorImpl::setFollowerMode(const MemberState& newState) {
        StatusWith<ReplicationExecutor::EventHandle> finishedSettingFollowerState =
            _replExecutor.makeEvent();
        if (finishedSettingFollowerState.getStatus() == ErrorCodes::ShutdownInProgress) {
            return false;
        }
        fassert(18812, finishedSettingFollowerState.getStatus());
        bool success = false;
        CBHStatus cbh = _replExecutor.scheduleWork(
                stdx::bind(&ReplicationCoordinatorImpl::_setFollowerModeFinish,
                           this,
                           stdx::placeholders::_1,
                           newState,
                           finishedSettingFollowerState.getValue(),
                           &success));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return false;
        }
        fassert(18699, cbh.getStatus());
        _replExecutor.waitForEvent(finishedSettingFollowerState.getValue());
        return success;
    }

    void ReplicationCoordinatorImpl::_setFollowerModeFinish(
            const ReplicationExecutor::CallbackData& cbData,
            const MemberState& newState,
            const ReplicationExecutor::EventHandle& finishedSettingFollowerMode,
            bool* success) {

        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
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
            // We are a candidate, which means _topCoord believs us to be in state RS_SECONDARY, and
            // we know that newState != RS_SECONDARY because we would have returned early, above if
            // the old and new state were equal.  So, cancel the running election and try again to
            // finish setting the follower mode.
            invariant(_freshnessChecker);
            _freshnessChecker->cancel(&_replExecutor);
            if (_electCmdRunner) {
                _electCmdRunner->cancel(&_replExecutor);
            }
            _replExecutor.onEvent(
                    _electionFinishedEvent,
                    stdx::bind(&ReplicationCoordinatorImpl::_setFollowerModeFinish,
                               this,
                               stdx::placeholders::_1,
                               newState,
                               finishedSettingFollowerMode,
                               success));
            return;
        }

        boost::unique_lock<boost::mutex> lk(_mutex);
        _topCoord->setFollowerMode(newState.s);

        const PostMemberStateUpdateAction action =
            _updateMemberStateFromTopologyCoordinator_inlock();
        *success = true;
        _replExecutor.signalEvent(finishedSettingFollowerMode);
        lk.unlock();
        _performPostMemberStateUpdateAction(action);
    }

    bool ReplicationCoordinatorImpl::isWaitingForApplierToDrain() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _isWaitingForDrainToComplete;
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
        // 4.) Drop all temp collections.
        // 5.) Drop the global exclusive lock.
        //
        // Because replicatable writes are forbidden while in drain mode, and we don't exit drain
        // mode until we have the global exclusive lock, which forbids all other threads from making
        // writes, we know that from the time that _isWaitingForDrainToComplete is set in
        // _performPostMemberStateUpdateAction(kActionWinElection) until this method returns, no
        // external writes will be processed.  This is important so that a new temp collection isn't
        // introduced on the new primary before we drop all the temp collections.

        boost::unique_lock<boost::mutex> lk(_mutex);
        if (!_isWaitingForDrainToComplete) {
            return;
        }
        lk.unlock();
        ScopedTransaction transaction(txn, MODE_X);
        Lock::GlobalWrite globalWriteLock(txn->lockState());
        lk.lock();
        if (!_isWaitingForDrainToComplete) {
            return;
        }
        _isWaitingForDrainToComplete = false;
        _canAcceptNonLocalWrites = true;
        lk.unlock();
        _externalState->dropAllTempCollections(txn);
        log() << "transition to primary complete; database writes are now permitted" << rsLog;
    }

    void ReplicationCoordinatorImpl::signalUpstreamUpdater() {
        _externalState->forwardSlaveHandshake();
    }

    ReplicationCoordinatorImpl::SlaveInfo*
    ReplicationCoordinatorImpl::_findSlaveInfoByMemberID_inlock(int memberId) {
        for (SlaveInfoVector::iterator it = _slaveInfo.begin(); it != _slaveInfo.end(); ++it) {
            if (it->memberId == memberId) {
                return &(*it);
            }
        }
        return NULL;
    }

    ReplicationCoordinatorImpl::SlaveInfo*
    ReplicationCoordinatorImpl::_findSlaveInfoByRID_inlock(const OID& rid) {
        for (SlaveInfoVector::iterator it = _slaveInfo.begin(); it != _slaveInfo.end(); ++it) {
            if (it->rid == rid) {
                return &(*it);
            }
        }
        return NULL;
    }

    void ReplicationCoordinatorImpl::_addSlaveInfo_inlock(const SlaveInfo& slaveInfo) {
        invariant(_getReplicationMode_inlock() == modeMasterSlave);
        _slaveInfo.push_back(slaveInfo);

        // Wake up any threads waiting for replication that now have their replication
        // check satisfied
        _wakeReadyWaiters_inlock();
    }

    void ReplicationCoordinatorImpl::_updateSlaveInfoOptime_inlock(SlaveInfo* slaveInfo,
                                                                   OpTime ts) {

        slaveInfo->opTime = ts;

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
            for (SlaveInfoVector::const_iterator it = oldSlaveInfos.begin();
                    it != oldSlaveInfos.end(); ++it) {
                if (it->self) {
                    SlaveInfo slaveInfo = *it;
                    slaveInfo.memberId = -1;
                    _slaveInfo.push_back(slaveInfo);
                    return;
                }
            }
            invariant(false); // There should always have been an entry for ourself
        }

        for (int i = 0; i < _rsConfig.getNumMembers(); ++i) {
            const MemberConfig& memberConfig = _rsConfig.getMemberAt(i);
            int memberId = memberConfig.getId();
            const HostAndPort& memberHostAndPort = memberConfig.getHostAndPort();

            SlaveInfo slaveInfo;

            // Check if the node existed with the same member ID and hostname in the old data
            for (SlaveInfoVector::const_iterator it = oldSlaveInfos.begin();
                    it != oldSlaveInfos.end(); ++it) {
                if ((it->memberId == memberId && it->hostAndPort == memberHostAndPort)
                        || (i == _selfIndex && it->self)) {
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
        if (_getReplicationMode_inlock() == modeMasterSlave) {
            // Self data always lives in the first entry in _slaveInfo for master/slave
            return 0;
        }
        else {
            invariant(_settings.usingReplSets());
            if (_selfIndex == -1) {
                invariant(_slaveInfo.size() == 1);
                return 0;
            }
            else {
                return _selfIndex;
            }
        }
    }

    Status ReplicationCoordinatorImpl::setLastOptimeForSlave(const OID& rid,
                                                             const OpTime& ts) {
        boost::unique_lock<boost::mutex> lock(_mutex);
        massert(28576,
                "Received an old style replication progress update, which is only used for Master/"
                    "Slave replication now, but this node is not using Master/Slave replication. "
                    "This is likely caused by an old (pre-2.6) member syncing from this node.",
                _getReplicationMode_inlock() == modeMasterSlave);

        SlaveInfo* slaveInfo = _findSlaveInfoByRID_inlock(rid);
        if (slaveInfo) {
            if (slaveInfo->opTime < ts) {
                _updateSlaveInfoOptime_inlock(slaveInfo, ts);
            }
        }
        else {
            SlaveInfo newSlaveInfo;
            newSlaveInfo.rid = rid;
            newSlaveInfo.opTime = ts;
            _addSlaveInfo_inlock(newSlaveInfo);
        }
        return Status::OK();
    }

    void ReplicationCoordinatorImpl::setMyHeartbeatMessage(const std::string& msg) {
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&TopologyCoordinator::setMyHeartbeatMessage,
                       _topCoord.get(),
                       _replExecutor.now(),
                       msg));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return;
        }
        fassert(28540, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
    }

    void ReplicationCoordinatorImpl::setMyLastOptime(const OpTime& ts) {
        boost::unique_lock<boost::mutex> lock(_mutex);
        _setMyLastOptime_inlock(&lock, ts, false);
    }

    void ReplicationCoordinatorImpl::resetMyLastOptime() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        _setMyLastOptime_inlock(&lock, OpTime(), true);
    }

    void ReplicationCoordinatorImpl::_setMyLastOptime_inlock(
            boost::unique_lock<boost::mutex>* lock, const OpTime& ts, bool isRollbackAllowed) {
        invariant(lock->owns_lock());
        SlaveInfo* mySlaveInfo = &_slaveInfo[_getMyIndexInSlaveInfo_inlock()];
        invariant(isRollbackAllowed || mySlaveInfo->opTime <= ts);
        _updateSlaveInfoOptime_inlock(mySlaveInfo, ts);

        if (_getReplicationMode_inlock() != modeReplSet) {
            return;
        }
        if (_getMemberState_inlock().primary()) {
            return;
        }
        lock->unlock();
        _externalState->forwardSlaveProgress(); // Must do this outside _mutex
    }

    OpTime ReplicationCoordinatorImpl::getMyLastOptime() const {
        boost::lock_guard<boost::mutex> lock(_mutex);
        return _getMyLastOptime_inlock();
    }

    OpTime ReplicationCoordinatorImpl::_getMyLastOptime_inlock() const {
        return _slaveInfo[_getMyIndexInSlaveInfo_inlock()].opTime;
    }

    Status ReplicationCoordinatorImpl::setLastOptime_forTest(const OID& rid, const OpTime& ts) {
        boost::lock_guard<boost::mutex> lock(_mutex);
        invariant(_getReplicationMode_inlock() == modeReplSet);

        const UpdatePositionArgs::UpdateInfo update(rid, ts, -1, -1);
        return _setLastOptime_inlock(update);
    }

    Status ReplicationCoordinatorImpl::_setLastOptime_inlock(
            const UpdatePositionArgs::UpdateInfo& args) {

        if (_selfIndex == -1) {
            // Ignore updates when we're in state REMOVED
            return Status(ErrorCodes::NotMasterOrSecondaryCode,
                          "Received replSetUpdatePosition command but we are in state REMOVED");
        }
        invariant(_getReplicationMode_inlock() == modeReplSet);

        if (args.rid == _getMyRID_inlock() ||
                args.memberId == _rsConfig.getMemberAt(_selfIndex).getId()) {
            // Do not let remote nodes tell us what our optime is.
            return Status::OK();
        }

        LOG(2) << "received notification that node with RID " << args.rid <<
                " has reached optime: " << args.ts;

        SlaveInfo* slaveInfo = NULL;
        if (args.memberId >= 0) {
            if (args.cfgver != _rsConfig.getConfigVersion()) {
                std::string errmsg = str::stream()
                          << "Received replSetUpdatePosition for node with memberId "
                          << args.memberId << " whose config version of " << args.cfgver
                          << " doesn't match our config version of "
                          << _rsConfig.getConfigVersion();
                LOG(1) << errmsg;
                return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
            }

            slaveInfo = _findSlaveInfoByMemberID_inlock(args.memberId);
            if (!slaveInfo) {
                invariant(!_rsConfig.findMemberByID(args.memberId));

                std::string errmsg = str::stream()
                          << "Received replSetUpdatePosition for node with memberId "
                          << args.memberId << " which doesn't exist in our config";
                LOG(1) << errmsg;
                return Status(ErrorCodes::NodeNotFound, errmsg);
            }
        }
        else {
            // The command we received didn't contain a memberId, most likely this is because it
            // came from a member running something prior to 3.0.
            // Fall back to finding the node by RID.
            slaveInfo = _findSlaveInfoByRID_inlock(args.rid);
            if (!slaveInfo) {
                std::string errmsg = str::stream()
                          << "Received replSetUpdatePosition for node with RID " << args.rid
                          << ", but we haven't yet received a handshake for that node.";
                LOG(1) << errmsg;
                return Status(ErrorCodes::NodeNotFound, errmsg);
            }
            invariant(slaveInfo->memberId >= 0);
        }
        invariant(slaveInfo);
        invariant(args.memberId < 0 || args.memberId == slaveInfo->memberId);

        LOG(3) << "Node with RID " << args.rid << " and memberId " << slaveInfo->memberId
               << " currently has optime " << slaveInfo->opTime << "; updating to " << args.ts;

        // Only update remote optimes if they increase.
        if (slaveInfo->opTime < args.ts) {
            _updateSlaveInfoOptime_inlock(slaveInfo, args.ts);
        }
        return Status::OK();
    }

    void ReplicationCoordinatorImpl::interrupt(unsigned opId) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        for (std::vector<WaiterInfo*>::iterator it = _replicationWaiterList.begin();
                it != _replicationWaiterList.end(); ++it) {
            WaiterInfo* info = *it;
            if (info->opID == opId) {
                info->condVar->notify_all();
                return;
            }
        }

        _replExecutor.scheduleWork(
                stdx::bind(&ReplicationCoordinatorImpl::_signalStepDownWaitersFromCallback,
                           this,
                           stdx::placeholders::_1));
    }

    void ReplicationCoordinatorImpl::interruptAll() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        for (std::vector<WaiterInfo*>::iterator it = _replicationWaiterList.begin();
                it != _replicationWaiterList.end(); ++it) {
            WaiterInfo* info = *it;
            info->condVar->notify_all();
        }

        _replExecutor.scheduleWork(
                stdx::bind(&ReplicationCoordinatorImpl::_signalStepDownWaitersFromCallback,
                           this,
                           stdx::placeholders::_1));
    }

    bool ReplicationCoordinatorImpl::_doneWaitingForReplication_inlock(
        const OpTime& opTime, const WriteConcernOptions& writeConcern) {
        Status status = _checkIfWriteConcernCanBeSatisfied_inlock(writeConcern);
        if (!status.isOK()) {
            return true;
        }

        if (!writeConcern.wMode.empty()) {
            StringData patternName;
            if (writeConcern.wMode == "majority") {
                patternName = "$majority";
            }
            else {
                patternName = writeConcern.wMode;
            }
            StatusWith<ReplicaSetTagPattern> tagPattern =
                _rsConfig.findCustomWriteMode(patternName);
            if (!tagPattern.isOK()) {
                return true;
            }
            return _haveTaggedNodesReachedOpTime_inlock(opTime, tagPattern.getValue());
        }
        else {
            return _haveNumNodesReachedOpTime_inlock(opTime, writeConcern.wNumNodes);
        }
    }

    bool ReplicationCoordinatorImpl::_haveNumNodesReachedOpTime_inlock(const OpTime& opTime, 
                                                                       int numNodes) {
        if (_getMyLastOptime_inlock() < opTime) {
            // Secondaries that are for some reason ahead of us should not allow us to
            // satisfy a write concern if we aren't caught up ourselves.
            return false;
        }

        for (SlaveInfoVector::iterator it = _slaveInfo.begin();
                it != _slaveInfo.end(); ++it) {

            const OpTime& slaveTime = it->opTime;
            if (slaveTime >= opTime) {
                --numNodes;
            }

            if (numNodes <= 0) {
                return true;
            }
        }
        return false;
    }

    bool ReplicationCoordinatorImpl::_haveTaggedNodesReachedOpTime_inlock(
        const OpTime& opTime, const ReplicaSetTagPattern& tagPattern) {

        ReplicaSetTagMatch matcher(tagPattern);
        for (SlaveInfoVector::iterator it = _slaveInfo.begin();
                it != _slaveInfo.end(); ++it) {

            const OpTime& slaveTime = it->opTime;
            if (slaveTime >= opTime) {
                // This node has reached the desired optime, now we need to check if it is a part
                // of the tagPattern.
                const MemberConfig* memberConfig = _rsConfig.findMemberByID(it->memberId);
                invariant(memberConfig);
                for (MemberConfig::TagIterator it = memberConfig->tagsBegin();
                        it != memberConfig->tagsEnd(); ++it) {
                    if (matcher.update(*it)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorImpl::awaitReplication(
            const OperationContext* txn,
            const OpTime& opTime,
            const WriteConcernOptions& writeConcern) {
        Timer timer;
        boost::unique_lock<boost::mutex> lock(_mutex);
        return _awaitReplication_inlock(&timer, &lock, txn, opTime, writeConcern);
    }

    ReplicationCoordinator::StatusAndDuration
            ReplicationCoordinatorImpl::awaitReplicationOfLastOpForClient(
                    const OperationContext* txn,
                    const WriteConcernOptions& writeConcern) {
        Timer timer;
        boost::unique_lock<boost::mutex> lock(_mutex);
        return _awaitReplication_inlock(
                &timer, &lock, txn, txn->getClient()->getLastOp(), writeConcern);
    }

    ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorImpl::_awaitReplication_inlock(
            const Timer* timer,
            boost::unique_lock<boost::mutex>* lock,
            const OperationContext* txn,
            const OpTime& opTime,
            const WriteConcernOptions& writeConcern) {

        const Mode replMode = _getReplicationMode_inlock();
        if (replMode == modeNone || serverGlobalParams.configsvr) {
            // no replication check needed (validated above)
            return StatusAndDuration(Status::OK(), Milliseconds(timer->millis()));
        }

        if (replMode == modeMasterSlave && writeConcern.wMode == "majority") {
            // with master/slave, majority is equivalent to w=1
            return StatusAndDuration(Status::OK(), Milliseconds(timer->millis()));
        }

        if (opTime.isNull()) {
            // If waiting for the empty optime, always say it's been replicated.
            return StatusAndDuration(Status::OK(), Milliseconds(timer->millis()));
        }

        if (replMode == modeReplSet && !_memberState.primary()) {
            return StatusAndDuration(Status(ErrorCodes::NotMaster,
                                            "Not master while waiting for replication"),
                                     Milliseconds(timer->millis()));
        }

        if (writeConcern.wMode.empty()) {
            if (writeConcern.wNumNodes < 1) {
                return StatusAndDuration(Status::OK(), Milliseconds(timer->millis()));
            }
            else if (writeConcern.wNumNodes == 1 && _getMyLastOptime_inlock() >= opTime) {
                return StatusAndDuration(Status::OK(), Milliseconds(timer->millis()));
            }
        }

        // Must hold _mutex before constructing waitInfo as it will modify _replicationWaiterList
        boost::condition_variable condVar;
        WaiterInfo waitInfo(
                &_replicationWaiterList, txn->getOpID(), &opTime, &writeConcern, &condVar);
        while (!_doneWaitingForReplication_inlock(opTime, writeConcern)) {
            const int elapsed = timer->millis();

            Status interruptedStatus = txn->checkForInterruptNoAssert();
            if (!interruptedStatus.isOK()) {
                return StatusAndDuration(interruptedStatus, Milliseconds(elapsed));
            }

            if (!waitInfo.master) {
                return StatusAndDuration(Status(ErrorCodes::NotMaster,
                                                "Not master anymore while waiting for replication"
                                                        " - this most likely means that a step down"
                                                        " occurred while waiting for replication"),
                                         Milliseconds(elapsed));
            }

            if (writeConcern.wTimeout != WriteConcernOptions::kNoTimeout &&
                    elapsed > writeConcern.wTimeout) {
                return StatusAndDuration(Status(ErrorCodes::ExceededTimeLimit,
                                                "waiting for replication timed out"),
                                         Milliseconds(elapsed));
            }

            if (_inShutdown) {
                return StatusAndDuration(Status(ErrorCodes::ShutdownInProgress,
                                                "Replication is being shut down"),
                                         Milliseconds(elapsed));
            }

            try {
                if (writeConcern.wTimeout == WriteConcernOptions::kNoTimeout) {
                    condVar.wait(*lock);
                }
                else {
                    condVar.timed_wait(*lock, Milliseconds(writeConcern.wTimeout - elapsed));
                }
            } catch (const boost::thread_interrupted&) {}
        }

        Status status = _checkIfWriteConcernCanBeSatisfied_inlock(writeConcern);
        if (!status.isOK()) {
            return StatusAndDuration(status, Milliseconds(timer->millis()));
        }

        return StatusAndDuration(Status::OK(), Milliseconds(timer->millis()));
    }

    Status ReplicationCoordinatorImpl::stepDown(OperationContext* txn,
                                                bool force,
                                                const Milliseconds& waitTime,
                                                const Milliseconds& stepdownTime) {
        const Date_t startTime = _replExecutor.now();
        const Date_t stepDownUntil(startTime.millis + stepdownTime.total_milliseconds());
        const Date_t waitUntil(startTime.millis + waitTime.total_milliseconds());

        if (!getMemberState().primary()) {
            // Note this check is inherently racy - it's always possible for the node to
            // stepdown from some other path before we acquire the global shared lock, but
            // that's okay because we are resiliant to that happening in _stepDownContinue.
            return Status(ErrorCodes::NotMaster, "not primary so can't step down");
        }

        LockResult lockState = txn->lockState()->lockGlobalBegin(MODE_S);
        // We've requested the global shared lock which will stop new writes from coming in,
        // but existing writes could take a long time to finish, so kill all user operations
        // to help us get the global lock faster.
        _externalState->killAllUserOperations(txn);

        if (lockState == LOCK_WAITING) {
            lockState = txn->lockState()->lockGlobalComplete(stepdownTime.total_milliseconds());
            if (lockState == LOCK_TIMEOUT) {
                return Status(ErrorCodes::ExceededTimeLimit,
                              "Could not acquire the global shared lock within the amount of time "
                                      "specified that we should step down for");
            }
        }
        invariant(lockState == LOCK_OK);
        ON_BLOCK_EXIT(&Locker::unlockAll, txn->lockState());
        // From this point onward we are guaranteed to be holding the global shared lock.

        StatusWith<ReplicationExecutor::EventHandle> finishedEvent = _replExecutor.makeEvent();
        if (finishedEvent.getStatus() == ErrorCodes::ShutdownInProgress) {
            return finishedEvent.getStatus();
        }
        fassert(26000, finishedEvent.getStatus());
        Status result(ErrorCodes::InternalError, "didn't set status in _stepDownContinue");
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&ReplicationCoordinatorImpl::_stepDownContinue,
                       this,
                       stdx::placeholders::_1,
                       finishedEvent.getValue(),
                       txn,
                       waitUntil,
                       stepDownUntil,
                       force,
                       &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return cbh.getStatus();
        }
        fassert(18809, cbh.getStatus());
        cbh = _replExecutor.scheduleWorkAt(
                waitUntil,
                stdx::bind(&ReplicationCoordinatorImpl::_signalStepDownWaitersFromCallback,
                           this,
                           stdx::placeholders::_1));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return cbh.getStatus();
        }
        fassert(26001, cbh.getStatus());
        _replExecutor.waitForEvent(finishedEvent.getValue());
        return result;
    }

    void ReplicationCoordinatorImpl::_signalStepDownWaitersFromCallback(
            const ReplicationExecutor::CallbackData& cbData) {
        if (!cbData.status.isOK()) {
            return;
        }

        _signalStepDownWaiters();
    }

    void ReplicationCoordinatorImpl::_signalStepDownWaiters() {
        std::for_each(_stepDownWaiters.begin(),
                      _stepDownWaiters.end(),
                      stdx::bind(&ReplicationExecutor::signalEvent,
                                 &_replExecutor,
                                 stdx::placeholders::_1));
        _stepDownWaiters.clear();
    }

    void ReplicationCoordinatorImpl::_stepDownContinue(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicationExecutor::EventHandle finishedEvent,
            OperationContext* txn,
            const Date_t waitUntil,
            const Date_t stepDownUntil,
            bool force,
            Status* result) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            // Cancelation only occurs on shutdown, which will also handle signaling the event.
            *result = Status(ErrorCodes::ShutdownInProgress, "Shutting down replication");
            return;
        }

        ScopeGuard allFinishedGuard = MakeGuard(
                stdx::bind(&ReplicationExecutor::signalEvent, &_replExecutor, finishedEvent));
        if (!cbData.status.isOK()) {
            *result = cbData.status;
            return;
        }

        Status interruptedStatus = txn->checkForInterruptNoAssert();
        if (!interruptedStatus.isOK()) {
            *result = interruptedStatus;
            return;
        }

        if (_topCoord->getRole() != TopologyCoordinator::Role::leader) {
            *result = Status(ErrorCodes::NotMaster,
                             "Already stepped down from primary while processing step down "
                             "request");
            return;
        }
        const Date_t now = _replExecutor.now();
        if (now >= stepDownUntil) {
            *result = Status(ErrorCodes::ExceededTimeLimit,
                             "By the time we were ready to step down, we were already past the "
                             "time we were supposed to step down until");
            return;
        }
        bool forceNow = now >= waitUntil ? force : false;
        if (_topCoord->stepDown(stepDownUntil, forceNow, getMyLastOptime())) {
            // Schedule work to (potentially) step back up once the stepdown period has ended.
            _replExecutor.scheduleWorkAt(stepDownUntil,
                                         stdx::bind(&ReplicationCoordinatorImpl::_handleTimePassing,
                                                    this,
                                                    stdx::placeholders::_1));

            boost::unique_lock<boost::mutex> lk(_mutex);
            const PostMemberStateUpdateAction action =
                _updateMemberStateFromTopologyCoordinator_inlock();
            lk.unlock();
            _performPostMemberStateUpdateAction(action);
            *result = Status::OK();
            return;
        }

        // Step down failed.  Keep waiting if we can, otherwise finish.
        if (now >= waitUntil) {
            *result = Status(ErrorCodes::ExceededTimeLimit, str::stream() <<
                             "No electable secondaries caught up as of " <<
                             dateToISOStringLocal(now));
            return;
        }

        if (_stepDownWaiters.empty()) {
            StatusWith<ReplicationExecutor::EventHandle> reschedEvent =
                _replExecutor.makeEvent();
            if (!reschedEvent.isOK()) {
                *result = reschedEvent.getStatus();
                return;
            }
            _stepDownWaiters.push_back(reschedEvent.getValue());
        }
        CBHStatus cbh = _replExecutor.onEvent(
                _stepDownWaiters.back(),
                stdx::bind(&ReplicationCoordinatorImpl::_stepDownContinue,
                           this,
                           stdx::placeholders::_1,
                           finishedEvent,
                           txn,
                           waitUntil,
                           stepDownUntil,
                           force,
                           result));
        if (!cbh.isOK()) {
            *result = cbh.getStatus();
            return;
        }
        allFinishedGuard.Dismiss();
    }

    void ReplicationCoordinatorImpl::_handleTimePassing(
            const ReplicationExecutor::CallbackData& cbData) {
        if (!cbData.status.isOK()) {
            return;
        }

        if (_topCoord->becomeCandidateIfStepdownPeriodOverAndSingleNodeSet(_replExecutor.now())) {
            _performPostMemberStateUpdateAction(kActionWinElection);
        }
    }

    bool ReplicationCoordinatorImpl::isMasterForReportingPurposes() {
        if (_settings.usingReplSets()) {
            boost::lock_guard<boost::mutex> lock(_mutex);
            if (_getReplicationMode_inlock() == modeReplSet &&
                    _getMemberState_inlock().primary()) {
                return true;
            }
            return false;
        }

        if (!_settings.slave)
            return true;


        // TODO(dannenberg) replAllDead is bad and should be removed when master slave is removed
        if (replAllDead) {
            return false;
        }

        if (_settings.master) {
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
        if (dbName == "local") {
            return true;
        }
        return !replAllDead && _settings.master;
    }

    Status ReplicationCoordinatorImpl::checkCanServeReadsFor(OperationContext* txn,
                                                             const NamespaceString& ns,
                                                             bool slaveOk) {
        if (txn->isGod()) {
            return Status::OK();
        }
        if (canAcceptWritesForDatabase(ns.db())) {
            return Status::OK();
        }
        if (_settings.slave || _settings.master) {
            return Status::OK();
        }
        if (slaveOk) {
            if (_canServeNonLocalReads.loadRelaxed()) {
                return Status::OK();
            }
            return Status(
                    ErrorCodes::NotMasterOrSecondaryCode,
                    "not master or secondary; cannot currently read from this replSet member");
        }
        return Status(ErrorCodes::NotMasterNoSlaveOkCode, "not master and slaveOk=false");
    }

    bool ReplicationCoordinatorImpl::isInPrimaryOrSecondaryState() const {
        return _canServeNonLocalReads.loadRelaxed();
    }

    bool ReplicationCoordinatorImpl::shouldIgnoreUniqueIndex(const IndexDescriptor* idx) {
        if (!idx->unique()) {
            return false;
        }
        // Never ignore _id index
        if (idx->isIdIndex()) {
            return false;
        }
        if (nsToDatabaseSubstring(idx->parentNS()) == "local" ) {
            // always enforce on local
            return false;
        }
        boost::lock_guard<boost::mutex> lock(_mutex);
        if (_getReplicationMode_inlock() != modeReplSet) {
            return false;
        }
        // see SERVER-6671
        MemberState ms = _getMemberState_inlock();
        switch ( ms.s ) {
        case MemberState::RS_SECONDARY:
        case MemberState::RS_RECOVERING:
        case MemberState::RS_ROLLBACK:
        case MemberState::RS_STARTUP2:
            return true;
        default:
            return false;
        }
    }

    OID ReplicationCoordinatorImpl::getElectionId() {
        boost::lock_guard<boost::mutex> lock(_mutex);
        return _electionId;
    }

    OID ReplicationCoordinatorImpl::getMyRID() const {
        boost::lock_guard<boost::mutex> lock(_mutex);
        return _getMyRID_inlock();
    }

    OID ReplicationCoordinatorImpl::_getMyRID_inlock() const {
        return _myRID;
    }

    int ReplicationCoordinatorImpl::getMyId() const {
        boost::lock_guard<boost::mutex> lock(_mutex);
        return _getMyId_inlock();
    }

    int ReplicationCoordinatorImpl::_getMyId_inlock() const {
        const MemberConfig& self = _rsConfig.getMemberAt(_selfIndex);
        return self.getId();
    }

    bool ReplicationCoordinatorImpl::prepareReplSetUpdatePositionCommand(
            BSONObjBuilder* cmdBuilder) {
        boost::lock_guard<boost::mutex> lock(_mutex);
        invariant(_rsConfig.isInitialized());
        // do not send updates if we have been removed from the config
        if (_selfIndex == -1) {
            return false;
        }
        cmdBuilder->append("replSetUpdatePosition", 1);
        // create an array containing objects each member connected to us and for ourself
        BSONArrayBuilder arrayBuilder(cmdBuilder->subarrayStart("optimes"));
        {
            for (SlaveInfoVector::const_iterator itr = _slaveInfo.begin();
                    itr != _slaveInfo.end(); ++itr) {
                if (itr->opTime.isNull()) {
                    // Don't include info on members we haven't heard from yet.
                    continue;
                }
                BSONObjBuilder entry(arrayBuilder.subobjStart());
                entry.append("_id", itr->rid);
                entry.append("optime", itr->opTime);
                entry.append("memberId", itr->memberId);
                entry.append("cfgver", _rsConfig.getConfigVersion());
                // SERVER-14550 Even though the "config" field isn't used on the other end in 3.0,
                // we need to keep sending it for 2.6 compatibility.
                // TODO(spencer): Remove this after 3.0 is released.
                const MemberConfig* member = _rsConfig.findMemberByID(itr->memberId);
                fassert(18651, member); // We ensured the member existed in processHandshake.
                entry.append("config", member->toBSON(_rsConfig.getTagConfig()));
            }
        }
        return true;
    }

    void ReplicationCoordinatorImpl::prepareReplSetUpdatePositionCommandHandshakes(
            std::vector<BSONObj>* handshakes) {
        boost::lock_guard<boost::mutex> lock(_mutex);
        // handshake objs for ourself and all chained members
        for (SlaveInfoVector::const_iterator itr = _slaveInfo.begin();
                itr != _slaveInfo.end(); ++itr) {
            if (!itr->rid.isSet()) {
                // Don't include info on members we haven't heard from yet.
                continue;
            }

            BSONObjBuilder cmd;
            cmd.append("replSetUpdatePosition", 1);
            {
                BSONObjBuilder subCmd (cmd.subobjStart("handshake"));
                subCmd.append("handshake", itr->rid);
                subCmd.append("member", itr->memberId);
                // SERVER-14550 Even though the "config" field isn't used on the other end in 3.0,
                // we need to keep sending it for 2.6 compatibility.
                // TODO(spencer): Remove this after 3.0 is released.
                const MemberConfig* member = _rsConfig.findMemberByID(itr->memberId);
                fassert(18650, member); // We ensured the member existed in processHandshake.
                subCmd.append("config", member->toBSON(_rsConfig.getTagConfig()));
            }
            handshakes->push_back(cmd.obj());
        }
    }

    Status ReplicationCoordinatorImpl::processReplSetGetStatus(BSONObjBuilder* response) {
        Status result(ErrorCodes::InternalError, "didn't set status in prepareStatusResponse");
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&TopologyCoordinator::prepareStatusResponse,
                       _topCoord.get(),
                       stdx::placeholders::_1,
                       _replExecutor.now(),
                       time(0) - serverGlobalParams.started,
                       getMyLastOptime(),
                       response,
                       &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18640, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());

        return result;
    }

    void ReplicationCoordinatorImpl::fillIsMasterForReplSet(IsMasterResponse* response) {
        invariant(getSettings().usingReplSets());

        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&ReplicationCoordinatorImpl::_fillIsMasterForReplSet_finish,
                       this,
                       stdx::placeholders::_1,
                       response));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            response->markAsShutdownInProgress();
            return;
        }
        fassert(28602, cbh.getStatus());

        _replExecutor.wait(cbh.getValue());
        if (isWaitingForApplierToDrain()) {
            // Report that we are secondary to ismaster callers until drain completes.
            response->setIsMaster(false);
            response->setIsSecondary(true);
        }
    }

    void ReplicationCoordinatorImpl::_fillIsMasterForReplSet_finish(
            const ReplicationExecutor::CallbackData& cbData, IsMasterResponse* response) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            response->markAsShutdownInProgress();
            return;
        }
        _topCoord->fillIsMasterForReplSet(response);
    }

    void ReplicationCoordinatorImpl::appendSlaveInfoData(BSONObjBuilder* result) {
        boost::lock_guard<boost::mutex> lock(_mutex);
        BSONArrayBuilder slaves(result->subarrayStart("slaves"));
        {
            for (SlaveInfoVector::const_iterator itr = _slaveInfo.begin();
                    itr != _slaveInfo.end(); ++itr) {
                BSONObjBuilder entry(slaves.subobjStart());
                entry.append("rid", itr->rid);
                entry.append("optime", itr->opTime);
                entry.append("host", itr->hostAndPort.toString());
                if (_getReplicationMode_inlock() == modeReplSet) {
                    if (_selfIndex == -1) {
                        continue;
                    }
                    invariant(itr->memberId >= 0);
                    entry.append("memberId", itr->memberId);
                }
            }
        }
    }

    void ReplicationCoordinatorImpl::processReplSetGetConfig(BSONObjBuilder* result) {
        boost::lock_guard<boost::mutex> lock(_mutex);
        result->append("config", _rsConfig.toBSON());
    }

    bool ReplicationCoordinatorImpl::getMaintenanceMode() {
        bool maintenanceMode(false);
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&ReplicationCoordinatorImpl::_getMaintenanceMode_helper,
                       this,
                       stdx::placeholders::_1,
                       &maintenanceMode));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return false;
        }
        fassert(18811, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return maintenanceMode;
    }

    void ReplicationCoordinatorImpl::_getMaintenanceMode_helper(
            const ReplicationExecutor::CallbackData& cbData,
            bool* maintenanceMode) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        *maintenanceMode = _topCoord->getMaintenanceCount() > 0;
    }

    Status ReplicationCoordinatorImpl::setMaintenanceMode(bool activate) {
        if (_getReplicationMode_inlock() != modeReplSet) {
            return Status(ErrorCodes::NoReplicationEnabled,
                          "can only set maintenance mode on replica set members");
        }

        Status result(ErrorCodes::InternalError, "didn't set status in _setMaintenanceMode_helper");
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&ReplicationCoordinatorImpl::_setMaintenanceMode_helper,
                       this,
                       stdx::placeholders::_1,
                       activate,
                       &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return cbh.getStatus();
        }
        fassert(18698, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return result;
    }

    void ReplicationCoordinatorImpl::_setMaintenanceMode_helper(
            const ReplicationExecutor::CallbackData& cbData,
            bool activate,
            Status* result) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
            return;
        }

        boost::unique_lock<boost::mutex> lk(_mutex);
        if (_getMemberState_inlock().primary()) {
            *result = Status(ErrorCodes::NotSecondary, "primaries can't modify maintenance mode");
            return;
        }

        int curMaintenanceCalls = _topCoord->getMaintenanceCount();
        if (activate) {
            log() << "going into maintenance mode with " << curMaintenanceCalls
                  << " other maintenance mode tasks in progress" << rsLog;
            _topCoord->adjustMaintenanceCountBy(1);
        }
        else if (curMaintenanceCalls > 0) {
            invariant(_topCoord->getRole() == TopologyCoordinator::Role::follower);

            _topCoord->adjustMaintenanceCountBy(-1);

            log() << "leaving maintenance mode (" << curMaintenanceCalls-1
                  << " other maintenance mode tasks ongoing)" << rsLog;
        } else {
            warning() << "Attempted to leave maintenance mode but it is not currently active";
            *result = Status(ErrorCodes::OperationFailed, "already out of maintenance mode");
            return;
        }

        const PostMemberStateUpdateAction action =
            _updateMemberStateFromTopologyCoordinator_inlock();
        *result = Status::OK();
        lk.unlock();
        _performPostMemberStateUpdateAction(action);
    }

    Status ReplicationCoordinatorImpl::processReplSetSyncFrom(const HostAndPort& target,
                                                              BSONObjBuilder* resultObj) {
        Status result(ErrorCodes::InternalError, "didn't set status in prepareSyncFromResponse");
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&TopologyCoordinator::prepareSyncFromResponse,
                       _topCoord.get(),
                       stdx::placeholders::_1,
                       target,
                       _getMyLastOptime_inlock(),
                       resultObj,
                       &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18649, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return result;
    }

    Status ReplicationCoordinatorImpl::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
        Status result(ErrorCodes::InternalError, "didn't set status in prepareFreezeResponse");
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&ReplicationCoordinatorImpl::_processReplSetFreeze_finish,
                       this,
                       stdx::placeholders::_1,
                       secs,
                       resultObj,
                       &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return cbh.getStatus();
        }
        fassert(18641, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return result;
    }

    void ReplicationCoordinatorImpl::_processReplSetFreeze_finish(
            const ReplicationExecutor::CallbackData& cbData,
            int secs,
            BSONObjBuilder* response,
            Status* result) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
            return;
        }

        _topCoord->prepareFreezeResponse(_replExecutor.now(), secs, response);

        if (_topCoord->getRole() == TopologyCoordinator::Role::candidate) {
            // If we just unfroze and ended our stepdown period and we are a one node replica set,
            // the topology coordinator will have gone into the candidate role to signal that we
            // need to elect ourself.
            _performPostMemberStateUpdateAction(kActionWinElection);
        }
        *result = Status::OK();
    }

    Status ReplicationCoordinatorImpl::processHeartbeat(const ReplSetHeartbeatArgs& args,
                                                        ReplSetHeartbeatResponse* response) {
        {
            boost::lock_guard<boost::mutex> lock(_mutex);
            if (_rsConfigState == kConfigPreStart || _rsConfigState == kConfigStartingUp) {
                return Status(ErrorCodes::NotYetInitialized,
                              "Received heartbeat while still initializing replication system");
            }
        }

        Status result(ErrorCodes::InternalError, "didn't set status in prepareHeartbeatResponse");
        CBHStatus cbh = _replExecutor.scheduleWork(
                stdx::bind(&ReplicationCoordinatorImpl::_processHeartbeatFinish,
                           this,
                           stdx::placeholders::_1,
                           args,
                           response,
                           &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18508, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return result;
    }

    void ReplicationCoordinatorImpl::_processHeartbeatFinish(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplSetHeartbeatArgs& args,
            ReplSetHeartbeatResponse* response,
            Status* outStatus) {

        if (cbData.status == ErrorCodes::CallbackCanceled) {
            *outStatus = Status(ErrorCodes::ShutdownInProgress, "Replication shutdown in progress");
            return;
        }
        fassert(18910, cbData.status);
        const Date_t now = _replExecutor.now();
        *outStatus = _topCoord->prepareHeartbeatResponse(
                now,
                args,
                _settings.ourSetName(),
                getMyLastOptime(),
                response);
        if ((outStatus->isOK() || *outStatus == ErrorCodes::InvalidReplicaSetConfig) &&
                _selfIndex < 0) {
            // If this node does not belong to the configuration it knows about, send heartbeats
            // back to any node that sends us a heartbeat, in case one of those remote nodes has
            // a configuration that contains us.  Chances are excellent that it will, since that
            // is the only reason for a remote node to send this node a heartbeat request.
            if (!args.getSenderHost().empty() && _seedList.insert(args.getSenderHost()).second) {
                _scheduleHeartbeatToTarget(args.getSenderHost(), -1, now);
            }
        }
    }

    Status ReplicationCoordinatorImpl::processReplSetReconfig(OperationContext* txn,
                                                              const ReplSetReconfigArgs& args,
                                                              BSONObjBuilder* resultObj) {

        log() << "replSetReconfig admin command received from client";

        boost::unique_lock<boost::mutex> lk(_mutex);

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
            invariant(false); // should be unreachable due to !_settings.usingReplSets() check above
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
            return Status(ErrorCodes::NotMaster, str::stream() <<
                          "replSetReconfig should only be run on PRIMARY, but my state is " <<
                          _getMemberState_inlock().toString() <<
                          "; use the \"force\" argument to override");
        }

        _setConfigState_inlock(kConfigReconfiguring);
        ScopeGuard configStateGuard = MakeGuard(
                lockAndCall,
                &lk,
                stdx::bind(&ReplicationCoordinatorImpl::_setConfigState_inlock,
                           this,
                           kConfigSteady));

        ReplicaSetConfig oldConfig = _rsConfig;
        lk.unlock();

        ReplicaSetConfig newConfig;
        BSONObj newConfigObj = args.newConfigObj;
        if (args.force) {
            newConfigObj = incrementConfigVersionByRandom(newConfigObj);
        }
        Status status = newConfig.initialize(newConfigObj);
        if (!status.isOK()) {
            error() << "replSetReconfig got " << status << " while parsing " << newConfigObj;
            return Status(ErrorCodes::InvalidReplicaSetConfig, status.reason());;
        }
        if (newConfig.getReplSetName() != _settings.ourSetName()) {
            str::stream errmsg;
            errmsg << "Attempting to reconfigure a replica set with name " <<
                newConfig.getReplSetName() << ", but command line reports " <<
                _settings.ourSetName() << "; rejecting";
            error() << std::string(errmsg);
            return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
        }

        StatusWith<int> myIndex = validateConfigForReconfig(
                _externalState.get(),
                oldConfig,
                newConfig,
                args.force);
        if (!myIndex.isOK()) {
            error() << "replSetReconfig got " << myIndex.getStatus() << " while validating " <<
                newConfigObj;
            return Status(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                          myIndex.getStatus().reason());
        }

        log() << "replSetReconfig config object with " << newConfig.getNumMembers() <<
            " members parses ok";

        if (!args.force) {
            status = checkQuorumForReconfig(&_replExecutor,
                                            newConfig,
                                            myIndex.getValue());
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

        const stdx::function<void (const ReplicationExecutor::CallbackData&)> reconfigFinishFn(
                stdx::bind(&ReplicationCoordinatorImpl::_finishReplSetReconfig,
                           this,
                           stdx::placeholders::_1,
                           newConfig,
                           myIndex.getValue()));

        // If it's a force reconfig, the primary node may not be electable after the configuration
        // change.  In case we are that primary node, finish the reconfig under the global lock,
        // so that the step down occurs safely.
        CBHStatus cbh =
            args.force ?
            _replExecutor.scheduleWorkWithGlobalExclusiveLock(reconfigFinishFn) :
            _replExecutor.scheduleWork(reconfigFinishFn);
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return status;
        }
        fassert(18824, cbh.getStatus());
        configStateGuard.Dismiss();
        _replExecutor.wait(cbh.getValue());
        return Status::OK();
    }

    void ReplicationCoordinatorImpl::_finishReplSetReconfig(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicaSetConfig& newConfig,
            int myIndex) {

        boost::unique_lock<boost::mutex> lk(_mutex);
        invariant(_rsConfigState == kConfigReconfiguring);
        invariant(_rsConfig.isInitialized());
        const PostMemberStateUpdateAction action = _setCurrentRSConfig_inlock(newConfig, myIndex);
        lk.unlock();
        _performPostMemberStateUpdateAction(action);
    }

    Status ReplicationCoordinatorImpl::processReplSetInitiate(OperationContext* txn,
                                                              const BSONObj& configObj,
                                                              BSONObjBuilder* resultObj) {
        log() << "replSetInitiate admin command received from client";

        boost::unique_lock<boost::mutex> lk(_mutex);
        if (!_settings.usingReplSets()) {
            return Status(ErrorCodes::NoReplicationEnabled, "server is not running with --replSet");
        }

        while (_rsConfigState == kConfigPreStart || _rsConfigState == kConfigStartingUp) {
            _rsConfigStateChange.wait(lk);
        }

        if (_rsConfigState != kConfigUninitialized) {
            resultObj->append("info",
                              "try querying local.system.replset to see current configuration");
            return Status(ErrorCodes::AlreadyInitialized, "already initialized");
        }
        invariant(!_rsConfig.isInitialized());
        _setConfigState_inlock(kConfigInitiating);
        ScopeGuard configStateGuard = MakeGuard(
                lockAndCall,
                &lk,
                stdx::bind(&ReplicationCoordinatorImpl::_setConfigState_inlock,
                           this,
                           kConfigUninitialized));
        lk.unlock();

        ReplicaSetConfig newConfig;
        Status status = newConfig.initialize(configObj);
        if (!status.isOK()) {
            error() << "replSet initiate got " << status << " while parsing " << configObj;
            return Status(ErrorCodes::InvalidReplicaSetConfig, status.reason());;
        }
        if (newConfig.getReplSetName() != _settings.ourSetName()) {
            str::stream errmsg;
            errmsg << "Attempting to initiate a replica set with name " <<
                newConfig.getReplSetName() << ", but command line reports " <<
                _settings.ourSetName() << "; rejecting";
            error() << std::string(errmsg);
            return Status(ErrorCodes::InvalidReplicaSetConfig, errmsg);
        }

        StatusWith<int> myIndex = validateConfigForInitiate(_externalState.get(), newConfig);
        if (!myIndex.isOK()) {
            error() << "replSet initiate got " << myIndex.getStatus() << " while validating " <<
                configObj;
            return Status(ErrorCodes::InvalidReplicaSetConfig, myIndex.getStatus().reason());
        }

        log() << "replSetInitiate config object with " << newConfig.getNumMembers() <<
            " members parses ok";

        status = checkQuorumForInitiate(
                &_replExecutor,
                newConfig,
                myIndex.getValue());

        if (!status.isOK()) {
            error() << "replSetInitiate failed; " << status;
            return status;
        }

        status = _externalState->storeLocalConfigDocument(txn, newConfig.toBSON());
        if (!status.isOK()) {
            error() << "replSetInitiate failed to store config document; " << status;
            return status;
        }

        CBHStatus cbh = _replExecutor.scheduleWork(
                stdx::bind(&ReplicationCoordinatorImpl::_finishReplSetInitiate,
                           this,
                           stdx::placeholders::_1,
                           newConfig,
                           myIndex.getValue()));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return status;
        }
        configStateGuard.Dismiss();
        fassert(18654, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());

        if (status.isOK()) {
            // Create the oplog with the first entry, and start repl threads.
            _externalState->initiateOplog(txn);
            _externalState->startThreads();
        }
        return status;
    }

    void ReplicationCoordinatorImpl::_finishReplSetInitiate(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicaSetConfig& newConfig,
            int myIndex) {

        boost::unique_lock<boost::mutex> lk(_mutex);
        invariant(_rsConfigState == kConfigInitiating);
        invariant(!_rsConfig.isInitialized());
        const PostMemberStateUpdateAction action = _setCurrentRSConfig_inlock(newConfig, myIndex);
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
                invariant(_rsConfig.getNumMembers() == 1 &&
                          _selfIndex == 0 &&
                          _rsConfig.getMemberAt(0).isElectable());
                return kActionWinElection;
            }
            return kActionNone;
        }
        PostMemberStateUpdateAction result;
        if (_memberState.primary() || newState.removed()) {
            // Wake up any threads blocked in awaitReplication, close connections, etc.
            for (std::vector<WaiterInfo*>::iterator it = _replicationWaiterList.begin();
                 it != _replicationWaiterList.end(); ++it) {
                WaiterInfo* info = *it;
                info->master = false;
                info->condVar->notify_all();
            }
            _isWaitingForDrainToComplete = false;
            _canAcceptNonLocalWrites = false;
            result = kActionCloseAllConnections;
        }
        else {
            if (_memberState.secondary() && !newState.primary()) {
                // Switching out of SECONDARY, but not to PRIMARY.
                _canServeNonLocalReads.store(0U);
            }
            else if (newState.secondary()) {
                // Switching into SECONDARY, but not from PRIMARY.
                _canServeNonLocalReads.store(1U);
            }
            result = kActionChooseNewSyncSource;
        }
        if (newState.secondary() && _topCoord->getRole() == TopologyCoordinator::Role::candidate) {
            // When transitioning to SECONDARY, the only way for _topCoord to report the candidate
            // role is if the configuration represents a single-node replica set.  In that case, the
            // overriding requirement is to elect this singleton node primary.
            invariant(_rsConfig.getNumMembers() == 1 &&
                      _selfIndex == 0 &&
                      _rsConfig.getMemberAt(0).isElectable());
            result = kActionWinElection;
        }

        _memberState = newState;
        log() << "transition to " << newState.toString() << rsLog;
        return result;
    }

    void ReplicationCoordinatorImpl::_performPostMemberStateUpdateAction(
            PostMemberStateUpdateAction action) {

        switch (action) {
        case kActionNone:
            break;
        case kActionChooseNewSyncSource:
            _externalState->signalApplierToChooseNewSyncSource();
            break;
        case kActionCloseAllConnections:
            _externalState->closeConnections();
            _externalState->clearShardingState();
            break;
        case kActionWinElection: {
            boost::unique_lock<boost::mutex> lk(_mutex);
            _electionId = OID::gen();
            _topCoord->processWinElection(_electionId, getNextGlobalOptime());
            _isWaitingForDrainToComplete = true;
            const PostMemberStateUpdateAction nextAction =
                _updateMemberStateFromTopologyCoordinator_inlock();
            invariant(nextAction != kActionWinElection);
            lk.unlock();
            _performPostMemberStateUpdateAction(nextAction);
            break;
        }
        default:
            severe() << "Unknown post member state update action " << static_cast<int>(action);
            fassertFailed(26010);
        }
    }

    Status ReplicationCoordinatorImpl::processReplSetGetRBID(BSONObjBuilder* resultObj) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        resultObj->append("rbid", _rbid);
        return Status::OK();
    }

    void ReplicationCoordinatorImpl::incrementRollbackID() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        ++_rbid;
    }

    Status ReplicationCoordinatorImpl::processReplSetFresh(const ReplSetFreshArgs& args,
                                                           BSONObjBuilder* resultObj) {

        Status result(ErrorCodes::InternalError, "didn't set status in prepareFreshResponse");
        CBHStatus cbh = _replExecutor.scheduleWork(
                stdx::bind(&ReplicationCoordinatorImpl::_processReplSetFresh_finish,
                           this,
                           stdx::placeholders::_1,
                           args,
                           resultObj,
                           &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18652, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return result;
    }

    void ReplicationCoordinatorImpl::_processReplSetFresh_finish(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplSetFreshArgs& args,
            BSONObjBuilder* response,
            Status* result) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
            return;
        }

        _topCoord->prepareFreshResponse(
                args, _replExecutor.now(), getMyLastOptime(), response, result);
    }

    Status ReplicationCoordinatorImpl::processReplSetElect(const ReplSetElectArgs& args,
                                                           BSONObjBuilder* responseObj) {
        Status result = Status(ErrorCodes::InternalError, "status not set by callback");
        CBHStatus cbh = _replExecutor.scheduleWork(
                stdx::bind(&ReplicationCoordinatorImpl::_processReplSetElect_finish,
                           this,
                           stdx::placeholders::_1,
                           args,
                           responseObj,
                           &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18657, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return result;
    }

    void ReplicationCoordinatorImpl::_processReplSetElect_finish(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplSetElectArgs& args,
            BSONObjBuilder* response,
            Status* result) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
            return;
        }

        _topCoord->prepareElectResponse(
                args, _replExecutor.now(), getMyLastOptime(), response, result);
    }

    ReplicationCoordinatorImpl::PostMemberStateUpdateAction
    ReplicationCoordinatorImpl::_setCurrentRSConfig_inlock(
            const ReplicaSetConfig& newConfig,
            int myIndex) {
         invariant(_settings.usingReplSets());
         _cancelHeartbeats();
         _setConfigState_inlock(kConfigSteady);
         OpTime myOptime = _getMyLastOptime_inlock(); // Must get this before changing our config.
         _topCoord->updateConfig(
                 newConfig,
                 myIndex,
                 _replExecutor.now(),
                 myOptime);
         _rsConfig = newConfig;
         log() << "New replica set config in use: " << _rsConfig.toBSON() << rsLog;
         _selfIndex = myIndex;
         if (_selfIndex >= 0) {
             log() << "This node is " <<
                 _rsConfig.getMemberAt(_selfIndex).getHostAndPort() << " in the config";
         }
         else {
             log() << "This node is not a member of the config";
         }

         const PostMemberStateUpdateAction action =
             _updateMemberStateFromTopologyCoordinator_inlock();
         _updateSlaveInfoFromConfig_inlock();
         if (_selfIndex >= 0) {
             // Don't send heartbeats if we're not in the config, if we get re-added one of the
             // nodes in the set will contact us.
             _startHeartbeats();
         }
         _wakeReadyWaiters_inlock();
         return action;
     }

    void ReplicationCoordinatorImpl::_wakeReadyWaiters_inlock(){
        for (std::vector<WaiterInfo*>::iterator it = _replicationWaiterList.begin();
                it != _replicationWaiterList.end(); ++it) {
            WaiterInfo* info = *it;
            if (_doneWaitingForReplication_inlock(*info->opTime, *info->writeConcern)) {
                info->condVar->notify_all();
            }
        }
    }

    Status ReplicationCoordinatorImpl::processReplSetUpdatePosition(
            const UpdatePositionArgs& updates) {

        boost::unique_lock<boost::mutex> lock(_mutex);
        Status status = Status::OK();
        bool somethingChanged = false;
        for (UpdatePositionArgs::UpdateIterator update = updates.updatesBegin();
                update != updates.updatesEnd();
                ++update) {
            status = _setLastOptime_inlock(*update);
            if (!status.isOK()) {
                break;
            }
            somethingChanged = true;
        }

        if (somethingChanged && !_getMemberState_inlock().primary()) {
            lock.unlock();
            _externalState->forwardSlaveProgress(); // Must do this outside _mutex
        }
        return status;
    }

    Status ReplicationCoordinatorImpl::processHandshake(OperationContext* txn,
                                                        const HandshakeArgs& handshake) {
        LOG(2) << "Received handshake " << handshake.toBSON();

        boost::unique_lock<boost::mutex> lock(_mutex);
        if (_getReplicationMode_inlock() == modeReplSet) {
            if (_selfIndex == -1) {
                // Ignore updates when we're in state REMOVED
                return Status(ErrorCodes::NotMasterOrSecondaryCode,
                              "Received replSetUpdatePosition command but we are in state REMOVED");
            }

            int memberId = handshake.getMemberId();
            const MemberConfig* member = _rsConfig.findMemberByID(memberId);
            if (!member) {
                return Status(ErrorCodes::NodeNotFound,
                              str::stream() << "Node with replica set memberId " << memberId <<
                                      " could not be found in replica set config while attempting"
                                      " to associate it with RID " << handshake.getRid() <<
                                      " in replication handshake.  ReplSet Config: " <<
                                      _rsConfig.toBSON().toString());
            }
            SlaveInfo* slaveInfo = _findSlaveInfoByMemberID_inlock(handshake.getMemberId());
            invariant(slaveInfo); // If it's in the config it must be in _slaveInfo
            slaveInfo->rid = handshake.getRid();
            slaveInfo->hostAndPort = member->getHostAndPort();

            if (!_getMemberState_inlock().primary()) {
                lock.unlock();
                _externalState->forwardSlaveHandshake(); // must do outside _mutex
            }
            return Status::OK();
        }

        // master-slave from here down
        SlaveInfo* slaveInfo = _findSlaveInfoByRID_inlock(handshake.getRid());
        if (slaveInfo) {
            return Status::OK(); // nothing to do
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
        boost::lock_guard<boost::mutex> lk(_mutex);
        if (_selfIndex == -1) {
            return true;
        }
        const MemberConfig& self = _rsConfig.getMemberAt(_selfIndex);
        return self.shouldBuildIndexes();
    }

    std::vector<HostAndPort> ReplicationCoordinatorImpl::getHostsWrittenTo(const OpTime& op) {
        std::vector<HostAndPort> hosts;
        boost::lock_guard<boost::mutex> lk(_mutex);
        for (size_t i = 0; i < _slaveInfo.size(); ++i) {
            const SlaveInfo& slaveInfo = _slaveInfo[i];
            if (slaveInfo.opTime < op) {
                continue;
            }

            if (_getReplicationMode_inlock() == modeMasterSlave &&
                    slaveInfo.rid == _getMyRID_inlock()) {
                // Master-slave doesn't know the HostAndPort for itself at this point.
                continue;
            }
            hosts.push_back(slaveInfo.hostAndPort);
        }
        return hosts;
    }

    std::vector<HostAndPort> ReplicationCoordinatorImpl::getOtherNodesInReplSet() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
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
        boost::lock_guard<boost::mutex> lock(_mutex);
        return _checkIfWriteConcernCanBeSatisfied_inlock(writeConcern);
    }

    Status ReplicationCoordinatorImpl::_checkIfWriteConcernCanBeSatisfied_inlock(
                const WriteConcernOptions& writeConcern) const {
        if (_getReplicationMode_inlock() == modeNone) {
            return Status(ErrorCodes::NoReplicationEnabled,
                          "No replication enabled when checking if write concern can be satisfied");
        }

        if (_getReplicationMode_inlock() == modeMasterSlave) {
            if (!writeConcern.wMode.empty()) {
                return Status(ErrorCodes::UnknownReplWriteConcern,
                              "Cannot use named write concern modes in master-slave");
            }
            // No way to know how many slaves there are, so assume any numeric mode is possible.
            return Status::OK();
        }

        invariant(_getReplicationMode_inlock() == modeReplSet);
        return _rsConfig.checkIfWriteConcernCanBeSatisfied(writeConcern);
    }

    WriteConcernOptions ReplicationCoordinatorImpl::getGetLastErrorDefault() {
        boost::mutex::scoped_lock lock(_mutex);
        if (_rsConfig.isInitialized()) {
            return _rsConfig.getDefaultWriteConcern();
        }
        return WriteConcernOptions();
    }

    Status ReplicationCoordinatorImpl::checkReplEnabledForCommand(BSONObjBuilder* result) {
        if (!_settings.usingReplSets()) {
            if (serverGlobalParams.configsvr) {
                result->append("info", "configsvr"); // for shell prompt
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

    void ReplicationCoordinatorImpl::_chooseNewSyncSource(
            const ReplicationExecutor::CallbackData& cbData,
            HostAndPort* newSyncSource) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        *newSyncSource = _topCoord->chooseNewSyncSource(_replExecutor.now(), getMyLastOptime());
    }

    HostAndPort ReplicationCoordinatorImpl::chooseNewSyncSource() {
        HostAndPort newSyncSource;
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&ReplicationCoordinatorImpl::_chooseNewSyncSource,
                       this,
                       stdx::placeholders::_1,
                       &newSyncSource));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return newSyncSource; // empty
        }
        fassert(18740, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return newSyncSource;
    }

    void ReplicationCoordinatorImpl::_blacklistSyncSource(
        const ReplicationExecutor::CallbackData& cbData,
        const HostAndPort& host,
        Date_t until) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        _topCoord->blacklistSyncSource(host, until);
    }

    void ReplicationCoordinatorImpl::blacklistSyncSource(const HostAndPort& host, Date_t until) {
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&ReplicationCoordinatorImpl::_blacklistSyncSource,
                       this,
                       stdx::placeholders::_1,
                       host,
                       until));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return;
        }
        fassert(18741, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
    }

    void ReplicationCoordinatorImpl::resetLastOpTimeFromOplog(OperationContext* txn) {
        StatusWith<OpTime> lastOpTimeStatus = _externalState->loadLastOpTime(txn);
        OpTime lastOpTime(0, 0);
        if (!lastOpTimeStatus.isOK()) {
            warning() << "Failed to load timestamp of most recently applied operation; " <<
                lastOpTimeStatus.getStatus();
        }
        else {
            lastOpTime = lastOpTimeStatus.getValue();
        }
        boost::unique_lock<boost::mutex> lk(_mutex);
        _setMyLastOptime_inlock(&lk, lastOpTime, true);
        _externalState->setGlobalOpTime(lastOpTime);
    }

    void ReplicationCoordinatorImpl::_shouldChangeSyncSource(
            const ReplicationExecutor::CallbackData& cbData,
            const HostAndPort& currentSource,
            bool* shouldChange) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        *shouldChange = _topCoord->shouldChangeSyncSource(currentSource);
    }

    bool ReplicationCoordinatorImpl::shouldChangeSyncSource(const HostAndPort& currentSource) {
        bool shouldChange(false);
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&ReplicationCoordinatorImpl::_shouldChangeSyncSource,
                       this,
                       stdx::placeholders::_1,
                       currentSource,
                       &shouldChange));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return false;
        }
        fassert(18906, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return shouldChange;
    }

} // namespace repl
} // namespace mongo

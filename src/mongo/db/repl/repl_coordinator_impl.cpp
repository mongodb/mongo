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

#include "mongo/db/repl/repl_coordinator_impl.h"

#include <algorithm>
#include <boost/thread.hpp>

#include "mongo/base/status.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/check_quorum_for_config_change.h"
#include "mongo/db/repl/handshake_args.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replica_set_config_checks.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/rs.h"
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
        const unsigned int opID;
        const OpTime* opTime;
        const WriteConcernOptions* writeConcern;
        boost::condition_variable* condVar;
    };

    ReplicationCoordinatorImpl::ReplicationCoordinatorImpl(
            const ReplSettings& settings,
            ReplicationCoordinatorExternalState* externalState,
            ReplicationExecutor::NetworkInterface* network,
            TopologyCoordinator* topCoord,
            int64_t prngSeed) :
        _settings(settings),
        _topCoord(topCoord),
        _replExecutor(network),
        _externalState(externalState),
        _inShutdown(false),
        _rsConfigState(kConfigStartingUp),
        _thisMembersConfigIndex(-1),
        _random(prngSeed),
        _sleptLastElection(false) {

        if (!isReplEnabled()) {
            return;
        }

        // this is ok but micros or combo with some rand() and/or 64 bits might be better --
        // imagine a restart and a clock correction simultaneously (very unlikely but possible...)
        _rbid = static_cast<int>(_replExecutor.now().asInt64());
    }

    ReplicationCoordinatorImpl::~ReplicationCoordinatorImpl() {}

    void ReplicationCoordinatorImpl::waitForStartUpComplete() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        while (_rsConfigState == kConfigStartingUp) {
            _rsConfigStateChange.wait(lk);
        }
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
            warning() << "Locally stored replica set configuration does not parse; "
                "waiting for rsInitiate or remote heartbeat; Got " << status << " while parsing " <<
                cfg.getValue();
            return true;
        }
        if (localConfig.getReplSetName() != _settings.ourSetName()) {
            warning() << "Local replica set configuration document reports set name of " <<
                localConfig.getReplSetName() << ", but command line reports " <<
                _settings.ourSetName() << "; ignoring local configuration document";
            return true;
        }

        // Use a callback here, because _finishLoadLocalConfig calls isself() which requires
        // that the server's networking layer be up and running and accepting connections, which
        // doesn't happen until startReplication finishes.
        _replExecutor.scheduleWork(
                stdx::bind(&ReplicationCoordinatorImpl::_finishLoadLocalConfig,
                           this,
                           stdx::placeholders::_1,
                           localConfig));
        return false;
    }

    void ReplicationCoordinatorImpl::_finishLoadLocalConfig(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicaSetConfig& localConfig) {
        if (!cbData.status.isOK()) {
            LOG(1) << "Loading local replica set configuration failed due to " << cbData.status;
            return;
        }
        _finishLoadLocalConfig_helper(cbData, localConfig);

        // Make sure that no matter how _finishLoadLocalConfig_helper terminates (short of
        // throwing an exception, which it shouldn't do and would cause the process to terminate),
        // we always set _rsConfigState to either kConfigSteady or kConfigUninitialized.
        if (_rsConfigState != kConfigStartingUp) {
            invariant(_rsConfigState == kConfigSteady);
            invariant(_rsConfig.isInitialized());
        }
        else {
            invariant(!_rsConfig.isInitialized());
            boost::lock_guard<boost::mutex> lk(_mutex);
            _setConfigState_inlock(kConfigUninitialized);
        }
    }

    void ReplicationCoordinatorImpl::_finishLoadLocalConfig_helper(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicaSetConfig& localConfig) {

        invariant(_rsConfigState == kConfigStartingUp);

        StatusWith<int> myIndex = validateConfigForStartUp(_externalState.get(),
                                                           _rsConfig,
                                                           localConfig);
        if (!myIndex.isOK()) {
            warning() << "Locally stored replica set configuration not valid for current node; "
                "waiting for rsInitiate or remote heartbeat; Got " << myIndex.getStatus() <<
                " while validating " << localConfig.toBSON();
            return;
        }

        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(_rsConfigState == kConfigStartingUp);
        _setCurrentRSConfig_inlock(localConfig, myIndex.getValue());
    }

    void ReplicationCoordinatorImpl::startReplication(OperationContext* txn) {
        if (!isReplEnabled()) {
            boost::lock_guard<boost::mutex> lk(_mutex);
            _setConfigState_inlock(kConfigReplicationDisabled);
            return;
        }

        {
            boost::lock_guard<boost::mutex> lk(_mutex);
            _myRID = _externalState->ensureMe(txn);
        }

        _topCoordDriverThread.reset(new boost::thread(stdx::bind(&ReplicationExecutor::run,
                                                                 &_replExecutor)));

        // TODO(spencer): Start this thread once we're no longer starting a SyncSourceFeedback
        // thread in the Legacy coordinator
        //_syncSourceFeedbackThread.reset(new boost::thread(
        //        stdx::bind(&ReplicationCoordinatorExternalState::runSyncSourceFeedback,
        //                   _externalState.get())));

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

        if (!isReplEnabled()) {
            return;
        }

        {
            boost::lock_guard<boost::mutex> lk(_mutex);
            _inShutdown = true;
            for (std::vector<WaiterInfo*>::iterator it = _replicationWaiterList.begin();
                    it != _replicationWaiterList.end(); ++it) {
                WaiterInfo* waiter = *it;
                waiter->condVar->notify_all();
            }
        }

        _replExecutor.shutdown();
        _topCoordDriverThread->join(); // must happen outside _mutex
        _externalState->shutdown();
        // _syncSourceFeedbackThread->join(); // TODO(spencer): put back once the thread is started
    }

    ReplSettings& ReplicationCoordinatorImpl::getSettings() {
        return _settings;
    }

    ReplicationCoordinator::Mode ReplicationCoordinatorImpl::getReplicationMode() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _getReplicationMode_inlock();
    }

    ReplicationCoordinator::Mode ReplicationCoordinatorImpl::_getReplicationMode_inlock() const {
        if (_rsConfig.isInitialized()) {
            return modeReplSet;
        }
        else if (_settings.slave || _settings.master) {
            return modeMasterSlave;
        }
        return modeNone;
    }

    MemberState ReplicationCoordinatorImpl::getCurrentMemberState() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _getCurrentMemberState_inlock();
    }

    MemberState ReplicationCoordinatorImpl::_getCurrentMemberState_inlock() const {
        invariant(_settings.usingReplSets());
        return _currentState;
    }

    Status ReplicationCoordinatorImpl::setMyLastOptime(OperationContext* txn, const OpTime& ts) {
        boost::unique_lock<boost::mutex> lock(_mutex);
        return _setLastOptime_inlock(&lock, _getMyRID_inlock(), ts);
    }

    Status ReplicationCoordinatorImpl::setLastOptime(OperationContext* txn,
                                                     const OID& rid,
                                                     const OpTime& ts) {
        boost::unique_lock<boost::mutex> lock(_mutex);
        return _setLastOptime_inlock(&lock, rid, ts);
    }

    Status ReplicationCoordinatorImpl::_setLastOptime_inlock(boost::unique_lock<boost::mutex>* lock,
                                                             const OID& rid,
                                                             const OpTime& ts) {
        invariant(lock->owns_lock());

        LOG(2) << "received notification that node with RID " << rid <<
                " has reached optime: " << ts;

        SlaveInfo& slaveInfo = _slaveInfoMap[rid];
        if (slaveInfo.memberID < 0 && _getReplicationMode_inlock() == modeReplSet) {
            warning() << "Received replSetUpdatePosition for node with RID" << rid
                      << ", but we haven't yet received a handshake for that node. Stored "
                      << "member ID: " << slaveInfo.memberID << ", stored member hostAndPort: "
                      << slaveInfo.hostAndPort.toString() << ".  Our RID: " << getMyRID();
        }
        invariant(slaveInfo.memberID >= 0 || _getReplicationMode_inlock() == modeMasterSlave);

        LOG(3) << "Node with RID " << rid << " currently has optime " << slaveInfo.opTime <<
                "; updating to " << ts;
        if (slaveInfo.opTime < ts) {
            slaveInfo.opTime = ts;

            // Wake up any threads waiting for replication that now have their replication
            // check satisfied
            for (std::vector<WaiterInfo*>::iterator it = _replicationWaiterList.begin();
                    it != _replicationWaiterList.end(); ++it) {
                WaiterInfo* info = *it;
                if (_doneWaitingForReplication_inlock(*info->opTime, *info->writeConcern)) {
                    info->condVar->notify_all();
                }
            }

            if (_getReplicationMode_inlock() == modeReplSet &&
                    !_getCurrentMemberState_inlock().primary()) {
                // pass along if we are not primary
                lock->unlock();
                _externalState->forwardSlaveProgress(); // Must do this outside _mutex
            }
        }
        return Status::OK();
    }

    OpTime ReplicationCoordinatorImpl::_getLastOpApplied() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _getLastOpApplied_inlock();
    }

    OpTime ReplicationCoordinatorImpl::_getLastOpApplied_inlock() {
        return _slaveInfoMap[_getMyRID_inlock()].opTime;
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
    }

    void ReplicationCoordinatorImpl::interruptAll() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        for (std::vector<WaiterInfo*>::iterator it = _replicationWaiterList.begin();
                it != _replicationWaiterList.end(); ++it) {
            WaiterInfo* info = *it;
            info->condVar->notify_all();
        }
    }

    bool ReplicationCoordinatorImpl::_doneWaitingForReplication_inlock(
            const OpTime& opTime, const WriteConcernOptions& writeConcern) {
        if (!writeConcern.wMode.empty()) {
            if (writeConcern.wMode == "majority") {
                return _doneWaitingForReplication_numNodes_inlock(opTime,
                                                                  _rsConfig.getMajorityNumber());
            } else {
                StatusWith<ReplicaSetTagPattern> tagPattern =
                        _rsConfig.findCustomWriteMode(writeConcern.wMode);
                if (!tagPattern.isOK()) {
                    return true;
                }
                return _doneWaitingForReplication_gleMode_inlock(opTime, tagPattern.getValue());
            }
        }
        else {
            return _doneWaitingForReplication_numNodes_inlock(opTime, writeConcern.wNumNodes);
        }
    }

    bool ReplicationCoordinatorImpl::_doneWaitingForReplication_numNodes_inlock(
            const OpTime& opTime, int numNodes) {
        for (SlaveInfoMap::iterator it = _slaveInfoMap.begin();
                it != _slaveInfoMap.end(); ++it) {
            const OpTime& slaveTime = it->second.opTime;
            if (slaveTime >= opTime) {
                --numNodes;
            }
            if (numNodes <= 0) {
                return true;
            }
        }
        return false;
    }

    bool ReplicationCoordinatorImpl::_doneWaitingForReplication_gleMode_inlock(
            const OpTime& opTime, const ReplicaSetTagPattern& tagPattern) {
        ReplicaSetTagMatch matcher(tagPattern);
        for (SlaveInfoMap::iterator it = _slaveInfoMap.begin();
                it != _slaveInfoMap.end(); ++it) {
            const OpTime& slaveTime = it->second.opTime;
            if (slaveTime >= opTime) {
                // This node has reached the desired optime, now we need to check if it is a part
                // of the tagPattern.
                const MemberConfig* memberConfig = _rsConfig.findMemberByID(it->second.memberID);
                invariant(memberConfig);
                for (MemberConfig::TagIterator it = memberConfig->tagsBegin();
                        it != memberConfig->tagsEnd(); ++it) {
                    matcher.update(*it);
                }
            }
            if (matcher.isSatisfied()) {
                return true;
            }
        }
        return false;
    }

    ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorImpl::awaitReplication(
            const OperationContext* txn,
            const OpTime& opTime,
            const WriteConcernOptions& writeConcern) {
        // TODO(spencer): handle killop


        if (writeConcern.wNumNodes <= 1 && writeConcern.wMode.empty()) {
            // no desired replication check
            return StatusAndDuration(Status::OK(), Milliseconds(0));
        }

        Timer timer;
        boost::condition_variable condVar;
        boost::unique_lock<boost::mutex> lk(_mutex);

        const Mode replMode = _getReplicationMode_inlock();
        if (replMode == modeNone || serverGlobalParams.configsvr) {
            // no replication check needed (validated above)
            return StatusAndDuration(Status::OK(), Milliseconds(0));
        }

        if (writeConcern.wMode == "majority" && replMode == modeMasterSlave) {
            // with master/slave, majority is equivalent to w=1
            return StatusAndDuration(Status::OK(), Milliseconds(0));
        }

        // Must hold _mutex before constructing waitInfo as it will modify _replicationWaiterList
        WaiterInfo waitInfo(
                &_replicationWaiterList, txn->getOpID(), &opTime, &writeConcern, &condVar);
        while (!_doneWaitingForReplication_inlock(opTime, writeConcern)) {
            const int elapsed = timer.millis();

            try {
                txn->checkForInterrupt();
            } catch (const DBException& e) {
                return StatusAndDuration(e.toStatus(), Milliseconds(elapsed));
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
                    condVar.wait(lk);
                }
                else {
                    condVar.timed_wait(lk, Milliseconds(writeConcern.wTimeout - elapsed));
                }
            } catch (const boost::thread_interrupted&) {}
        }

        Status status = _checkIfWriteConcernCanBeSatisfied_inlock(writeConcern);
        if (!status.isOK()) {
            return StatusAndDuration(status, Milliseconds(timer.millis()));
        }

        return StatusAndDuration(Status::OK(), Milliseconds(timer.millis()));
    }

    ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorImpl::awaitReplicationOfLastOp(
            const OperationContext* txn,
            const WriteConcernOptions& writeConcern) {
        // TODO
        return StatusAndDuration(Status::OK(), Milliseconds(0));
    }

    Status ReplicationCoordinatorImpl::stepDown(OperationContext* txn,
                                                bool force,
                                                const Milliseconds& waitTime,
                                                const Milliseconds& stepdownTime) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorImpl::stepDownAndWaitForSecondary(
            OperationContext* txn,
            const Milliseconds& initialWaitTime,
            const Milliseconds& stepdownTime,
            const Milliseconds& postStepdownWaitTime) {
        // TODO
        return Status::OK();
    }

    bool ReplicationCoordinatorImpl::isMasterForReportingPurposes() {
        if (_settings.usingReplSets()) {
            boost::lock_guard<boost::mutex> lock(_mutex);
            if (_getReplicationMode_inlock() == modeReplSet &&
                    _getCurrentMemberState_inlock().primary()) {
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

    bool ReplicationCoordinatorImpl::canAcceptWritesForDatabase(const StringData& dbName) {
        // we must check _settings since getReplicationMode() isn't aware of modeReplSet
        // until a valid replica set config has been loaded
        boost::lock_guard<boost::mutex> lk(_mutex);
        if (_settings.usingReplSets()) {
            if (_getReplicationMode_inlock() == modeReplSet &&
                    _getCurrentMemberState_inlock().primary()) {
                return true;
            }
            return dbName == "local";
        }

        if (!_settings.slave)
            return true;

        // TODO(dannenberg) replAllDead is bad and should be removed when master slave is removed
        if (replAllDead) {
            return dbName == "local";
        }

        if (_settings.master) {
            // if running with --master --slave, allow.
            return true;
        }

        return dbName == "local";
    }

    Status ReplicationCoordinatorImpl::canServeReadsFor(OperationContext* txn,
                                                        const NamespaceString& ns,
                                                        bool slaveOk) {
        if (txn->isGod()) {
            return Status::OK();
        }
        if (canAcceptWritesForDatabase(ns.db())) {
            return Status::OK();
        }
        boost::lock_guard<boost::mutex> lk(_mutex);
        Mode replMode = _getReplicationMode_inlock();
        if (replMode == modeMasterSlave && _settings.slave == SimpleSlave) {
            return Status::OK();
        }
        if (slaveOk) {
            if (replMode == modeMasterSlave || replMode == modeNone) {
                return Status::OK();
            }
            if (_getCurrentMemberState_inlock().secondary()) {
                return Status::OK();
            }
            return Status(ErrorCodes::NotMasterOrSecondaryCode,
                         "not master or secondary; cannot currently read from this replSet member");
        }
        return Status(ErrorCodes::NotMasterNoSlaveOkCode,
                      "not master and slaveOk=false");
    }

    bool ReplicationCoordinatorImpl::shouldIgnoreUniqueIndex(const IndexDescriptor* idx) {
        if (!idx->unique()) {
            return false;
        }
        // Never ignore _id index
        if (idx->isIdIndex()) {
            return false;
        }
        boost::lock_guard<boost::mutex> lock(_mutex);
        if (_getReplicationMode_inlock() != modeReplSet) {
            return false;
        }
        // see SERVER-6671
        MemberState ms = _getCurrentMemberState_inlock();
        if (! ((ms == MemberState::RS_STARTUP2) ||
               (ms == MemberState::RS_RECOVERING) ||
               (ms == MemberState::RS_ROLLBACK))) {
            return false;
        }
        // TODO(spencer): SERVER-14233 Remove support for old oplog versions, or move oplogVersion
        // into the repl coordinator
        /* // 2 is the oldest oplog version where operations
        // are fully idempotent.
        if (theReplSet->oplogVersion < 2) {
            return false;
        }*/

        return true;
    }

    OID ReplicationCoordinatorImpl::getElectionId() {
        boost::lock_guard<boost::mutex> lock(_mutex);
        return _electionID;
    }

    OID ReplicationCoordinatorImpl::getMyRID() {
        boost::lock_guard<boost::mutex> lock(_mutex);
        return _getMyRID_inlock();
    }

    OID ReplicationCoordinatorImpl::_getMyRID_inlock() {
        return _myRID;
    }

    void ReplicationCoordinatorImpl::prepareReplSetUpdatePositionCommand(
            OperationContext* txn,
            BSONObjBuilder* cmdBuilder) {
        boost::lock_guard<boost::mutex> lock(_mutex);
        cmdBuilder->append("replSetUpdatePosition", 1);
        // create an array containing objects each member connected to us and for ourself
        BSONArrayBuilder arrayBuilder(cmdBuilder->subarrayStart("optimes"));
        {
            for (SlaveInfoMap::const_iterator itr = _slaveInfoMap.begin();
                    itr != _slaveInfoMap.end(); ++itr) {
                const OID& rid = itr->first;
                const SlaveInfo& info = itr->second;
                BSONObjBuilder entry(arrayBuilder.subobjStart());
                entry.append("_id", rid);
                entry.append("optime", info.opTime);
                // SERVER-14550 Even though the "config" field isn't used on the other end in 2.8,
                // we need to keep sending it for 2.6 compatibility.
                // TODO(spencer): Remove this after 2.8 is released.
                const MemberConfig* member = _rsConfig.findMemberByID(info.memberID);
                fassert(18651, member); // We ensured the member existed in processHandshake.
                entry.append("config", member->toBSON(_rsConfig.getTagConfig()));
            }
        }
    }

    void ReplicationCoordinatorImpl::prepareReplSetUpdatePositionCommandHandshakes(
            OperationContext* txn,
            std::vector<BSONObj>* handshakes) {
        boost::lock_guard<boost::mutex> lock(_mutex);
        // handshake objs for ourself and all chained members
        for (SlaveInfoMap::const_iterator itr = _slaveInfoMap.begin();
             itr != _slaveInfoMap.end(); ++itr) {
            const OID& oid = itr->first;
            BSONObjBuilder cmd;
            cmd.append("replSetUpdatePosition", 1);
            {
                BSONObjBuilder subCmd (cmd.subobjStart("handshake"));
                subCmd.append("handshake", oid);
                int memberID = itr->second.memberID;
                subCmd.append("member", memberID);
                // SERVER-14550 Even though the "config" field isn't used on the other end in 2.8,
                // we need to keep sending it for 2.6 compatibility.
                // TODO(spencer): Remove this after 2.8 is released.
                const MemberConfig* member = _rsConfig.findMemberByID(memberID);
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
                       _getLastOpApplied(),
                       response,
                       &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18640, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return result;
    }

    void ReplicationCoordinatorImpl::processReplSetGetConfig(BSONObjBuilder* result) {
        boost::lock_guard<boost::mutex> lock(_mutex);
        result->append("config", _rsConfig.toBSON());
    }

    bool ReplicationCoordinatorImpl::setMaintenanceMode(OperationContext* txn, bool activate) {
        // TODO
        return false;
    }

    Status ReplicationCoordinatorImpl::processReplSetSyncFrom(const HostAndPort& target,
                                                              BSONObjBuilder* resultObj) {
        Status result(ErrorCodes::InternalError, "didn't set status in prepareSyncFromResponse");
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&TopologyCoordinator::prepareSyncFromResponse,
                       _topCoord.get(),
                       stdx::placeholders::_1,
                       target,
                       _getLastOpApplied_inlock(),
                       resultObj,
                       &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18649, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return result;
    }

    Status ReplicationCoordinatorImpl::processReplSetMaintenance(OperationContext* txn,
                                                                 bool activate,
                                                                 BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorImpl::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
        Status result(ErrorCodes::InternalError, "didn't set status in prepareFreezeResponse");
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&TopologyCoordinator::prepareFreezeResponse,
                       _topCoord.get(),
                       stdx::placeholders::_1,
                       _replExecutor.now(),
                       secs,
                       resultObj,
                       &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18641, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return result;
    }

    Status ReplicationCoordinatorImpl::processHeartbeat(const ReplSetHeartbeatArgs& args,
                                                        ReplSetHeartbeatResponse* response) {
        {
            boost::lock_guard<boost::mutex> lock(_mutex);
            if (_rsConfigState == kConfigStartingUp) {
                return Status(ErrorCodes::NotYetInitialized,
                              "Received heartbeat while still initializing replication system");
            }
        }

        Status result(ErrorCodes::InternalError, "didn't set status in prepareHeartbeatResponse");
        CBHStatus cbh = _replExecutor.scheduleWork(
            stdx::bind(&TopologyCoordinator::prepareHeartbeatResponse,
                       _topCoord.get(),
                       stdx::placeholders::_1,
                       _replExecutor.now(),
                       args,
                       _settings.ourSetName(),
                       _getLastOpApplied(),
                       response,
                       &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18508, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return result;
    }

    Status ReplicationCoordinatorImpl::processReplSetReconfig(OperationContext* txn,
                                                              const ReplSetReconfigArgs& args,
                                                              BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorImpl::processReplSetInitiate(OperationContext* txn,
                                                              const BSONObj& configObj,
                                                              BSONObjBuilder* resultObj) {
        log() << "replSet replSetInitiate admin command received from client" << rsLog;

        boost::unique_lock<boost::mutex> lk(_mutex);

        if (!_settings.usingReplSets()) {
            return Status(ErrorCodes::NoReplicationEnabled,
                          "server is not running with --replSet");
        }

        while (_rsConfigState == kConfigStartingUp) {
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
            error() << "replSet initiate got " << status << " while parsing " << configObj << rsLog;
            return status;
        }
        StatusWith<int> myIndex = validateConfigForInitiate(_externalState.get(), newConfig);
        if (!myIndex.isOK()) {
            error() << "replSet initiate got " << myIndex.getStatus() << " while validating " <<
                configObj << rsLog;
            return myIndex.getStatus();
        }

        if (newConfig.getReplSetName() != _settings.ourSetName()) {
            str::stream errmsg;
            errmsg << "Attempting to initiate a replica set with name " <<
                newConfig.getReplSetName() << ", but command line reports " <<
                _settings.ourSetName() << "; rejecting";
            error() << std::string(errmsg);
            return Status(ErrorCodes::BadValue, errmsg);
        }

        log() << "replSet replSetInitiate config object with " << newConfig.getNumMembers() <<
            " members parses ok" << rsLog;

        status = checkQuorumForInitiate(
                &_replExecutor,
                newConfig,
                myIndex.getValue());

        if (!status.isOK()) {
            error() << "replSet replSetInitiate failed; " << status;
            return status;
        }

        status = _externalState->storeLocalConfigDocument(txn, configObj);
        if (!status.isOK()) {
            error() << "replSet replSetInitiate failed to store config document; " << status;
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
        return status;
    }

    void ReplicationCoordinatorImpl::_finishReplSetInitiate(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicaSetConfig& newConfig,
            int myIndex) {

        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(_rsConfigState == kConfigInitiating);
        invariant(!_rsConfig.isInitialized());
        _setCurrentRSConfig_inlock(newConfig, myIndex);
    }

    void ReplicationCoordinatorImpl::_setConfigState_inlock(ConfigState newState) {
        if (newState != _rsConfigState) {
            _rsConfigState = newState;
            _rsConfigStateChange.notify_all();
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
                stdx::bind(&TopologyCoordinator::prepareFreshResponse,
                           _topCoord.get(),
                           stdx::placeholders::_1,
                           args,
                           _getLastOpApplied_inlock(),
                           resultObj,
                           &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18652, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return result;
    }

    Status ReplicationCoordinatorImpl::processReplSetElect(const ReplSetElectArgs& args,
                                                           BSONObjBuilder* resultObj) {
        CBHStatus cbh = _replExecutor.scheduleWork(
                stdx::bind(&TopologyCoordinator::prepareElectResponse,
                           _topCoord.get(),
                           stdx::placeholders::_1,
                           args,
                           _replExecutor.now(),
                           resultObj));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18657, cbh.getStatus());
        _replExecutor.wait(cbh.getValue());
        return Status::OK();
    }

    void ReplicationCoordinatorImpl::_setCurrentRSConfig(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicaSetConfig& newConfig,
            int myIndex) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }

        boost::lock_guard<boost::mutex> lk(_mutex);
        _setCurrentRSConfig_inlock(newConfig, myIndex);
    }

    void ReplicationCoordinatorImpl::_setCurrentRSConfig_inlock(
            const ReplicaSetConfig& newConfig,
            int myIndex) {
         invariant(_settings.usingReplSets());
         if (_rsConfig.isInitialized()) {
             cancelHeartbeats();
         }
         _setConfigState_inlock(kConfigSteady);
         _rsConfig = newConfig;
         _thisMembersConfigIndex = myIndex;
         _topCoord->updateConfig(
                 newConfig,
                 myIndex,
                 _replExecutor.now(),
                 _getLastOpApplied_inlock());

         // Ensure that there's an entry in the _slaveInfoMap for ourself
         _slaveInfoMap[_getMyRID_inlock()].memberID = _rsConfig.getMemberAt(myIndex).getId();
         _slaveInfoMap[_getMyRID_inlock()].hostAndPort =
                 _rsConfig.getMemberAt(myIndex).getHostAndPort();
         _startHeartbeats();
     }

    void ReplicationCoordinatorImpl::forceCurrentRSConfigHack(const BSONObj& configObj,
                                                              int myIndex) {
        LOG(2) << "Force setting rs config in ReplCoordinatorImpl to " << configObj.toString() <<
                " with self at index " << myIndex;
        ReplicaSetConfig config;
        fassert(18647, config.initialize(configObj));


        // Wait until we're done loading our local config
        boost::unique_lock<boost::mutex> lock(_mutex);
        while (_rsConfigState == kConfigStartingUp) {
            _rsConfigStateChange.wait(lock);
        }
        lock.unlock();

        CBHStatus cbh = _replExecutor.scheduleWork(
                stdx::bind(&ReplicationCoordinatorImpl::_setCurrentRSConfig,
                           this,
                           stdx::placeholders::_1,
                           config,
                           myIndex));
        if (cbh.isOK()) {
            _replExecutor.wait(cbh.getValue());
        }
    }

    Status ReplicationCoordinatorImpl::processReplSetUpdatePosition(
            OperationContext* txn,
            const UpdatePositionArgs& updates) {

        for (UpdatePositionArgs::UpdateIterator update = updates.updatesBegin();
                update != updates.updatesEnd();
                ++update) {
            Status status = setLastOptime(txn, update->rid, update->ts);
            if (!status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }

    Status ReplicationCoordinatorImpl::processHandshake(const OperationContext* txn,
                                                        const HandshakeArgs& handshake) {
        LOG(2) << "Received handshake " << handshake.toBSON();

        boost::lock_guard<boost::mutex> lock(_mutex);
        if (_getReplicationMode_inlock() == modeReplSet) {
            int memberID = handshake.getMemberId();
            const MemberConfig* member = _rsConfig.findMemberByID(memberID);
            if (!member) {
                return Status(ErrorCodes::NodeNotFound,
                              str::stream() << "Node with replica set member ID " << memberID <<
                                      " could not be found in replica set config during handshake");
            }
            SlaveInfo& slaveInfo = _slaveInfoMap[handshake.getRid()];
            slaveInfo.memberID = memberID;
            slaveInfo.hostAndPort = member->getHostAndPort();

            if (!_getCurrentMemberState_inlock().primary()) {
                // pass along if we are not primary
                _externalState->forwardSlaveHandshake();
            }
        }
        else {
            // master/slave
            SlaveInfo& slaveInfo = _slaveInfoMap[handshake.getRid()];
            slaveInfo.memberID = -1;
            slaveInfo.hostAndPort = _externalState->getClientHostAndPort(txn);
        }

        return Status::OK();
    }

    void ReplicationCoordinatorImpl::waitUpToOneSecondForOptimeChange(const OpTime& ot) {
        //TODO
    }

    bool ReplicationCoordinatorImpl::buildsIndexes() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        const MemberConfig& self = _rsConfig.getMemberAt(_thisMembersConfigIndex);
        return self.shouldBuildIndexes();
    }

    std::vector<BSONObj> ReplicationCoordinatorImpl::getHostsWrittenTo(const OpTime& op) {
        // TODO
        return std::vector<BSONObj>();
    }

    Status ReplicationCoordinatorImpl::checkIfWriteConcernCanBeSatisfied(
            const WriteConcernOptions& writeConcern) const {
        boost::lock_guard<boost::mutex> lock(_mutex);
        return _checkIfWriteConcernCanBeSatisfied_inlock(writeConcern);
    }

    Status ReplicationCoordinatorImpl::_checkIfWriteConcernCanBeSatisfied_inlock(
                const WriteConcernOptions& writeConcern) const {
        // TODO Finish implementing this

        if (!writeConcern.wMode.empty() && writeConcern.wMode != "majority") {
            StatusWith<ReplicaSetTagPattern> tagPatternStatus =
                    _rsConfig.findCustomWriteMode(writeConcern.wMode);
            if (!tagPatternStatus.isOK()) {
                return tagPatternStatus.getStatus();
            }
        }
        return Status::OK();
    }

    BSONObj ReplicationCoordinatorImpl::getGetLastErrorDefault() {
        boost::mutex::scoped_lock lock(_mutex);
        return _rsConfig.getDefaultWriteConcern().toBSON();
    }

    Status ReplicationCoordinatorImpl::checkReplEnabledForCommand(BSONObjBuilder* result) {
        if (!_settings.usingReplSets()) {
            if (serverGlobalParams.configsvr) {
                result->append("info", "configsvr"); // for shell prompt
            }
            return Status(ErrorCodes::NoReplicationEnabled, "not running with --replSet");
        }

        if (getReplicationMode() != modeReplSet) {
            result->append("info", "run rs.initiate(...) if not yet done for the set");
            return Status(ErrorCodes::NotYetInitialized, "no replset config has been received");
        }

        return Status::OK();
    }

    bool ReplicationCoordinatorImpl::isReplEnabled() const {
        return _settings.usingReplSets() || _settings.master || _settings.slave;
    }

} // namespace repl
} // namespace mongo

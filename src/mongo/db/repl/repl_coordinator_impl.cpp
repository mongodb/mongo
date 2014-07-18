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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/repl_coordinator_impl.h"

#include <algorithm>
#include <boost/thread.hpp>

#include "mongo/base/status.h"
#include "mongo/db/server_options.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {

    struct ReplicationCoordinatorImpl::WaiterInfo {

        /**
         * Constructor takes the list of waiters and enqueues itself on the list, removing itself
         * in the destructor.
         */
        WaiterInfo(std::vector<WaiterInfo*>* _list,
                   const OpTime* _opTime,
                   const WriteConcernOptions* _writeConcern,
                   boost::condition_variable* _condVar) : list(_list),
                                                          opTime(_opTime),
                                                          writeConcern(_writeConcern),
                                                          condVar(_condVar) {
            list->push_back(this);
        }

        ~WaiterInfo() {
            list->erase(std::remove(list->begin(), list->end(), this), list->end());
        }

        std::vector<WaiterInfo*>* list;
        const OpTime* opTime;
        const WriteConcernOptions* writeConcern;
        boost::condition_variable* condVar;
    };

    ReplicationCoordinatorImpl::ReplicationCoordinatorImpl(const ReplSettings& settings) :
            _inShutdown(false), _settings(settings) {}

    ReplicationCoordinatorImpl::~ReplicationCoordinatorImpl() {}

    void ReplicationCoordinatorImpl::startReplication(
            TopologyCoordinator* topCoord,
            ReplicationExecutor::NetworkInterface* network) {
        if (!isReplEnabled()) {
            return;
        }

        _topCoord.reset(topCoord);
        _topCoord->registerConfigChangeCallback(
                stdx::bind(&ReplicationCoordinatorImpl::setCurrentReplicaSetConfig,
                           this,
                           stdx::placeholders::_1));
        _topCoord->registerStateChangeCallback(
                stdx::bind(&ReplicationCoordinatorImpl::setCurrentMemberState,
                           this,
                           stdx::placeholders::_1));

        _replExecutor.reset(new ReplicationExecutor(network));
        _topCoordDriverThread.reset(new boost::thread(stdx::bind(&ReplicationExecutor::run,
                                                                 _replExecutor.get())));
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

        _replExecutor->shutdown();
        _topCoordDriverThread->join(); // must happen outside _mutex
    }

    bool ReplicationCoordinatorImpl::isShutdownOkay() const {
        // TODO
        return false;
    }

    ReplSettings& ReplicationCoordinatorImpl::getSettings() {
        return _settings;
    }

    ReplicationCoordinator::Mode ReplicationCoordinatorImpl::getReplicationMode() const {
        if (_settings.usingReplSets()) {
            return modeReplSet;
        } else if (_settings.slave || _settings.master) {
            return modeMasterSlave;
        }
        return modeNone;
    }

    void ReplicationCoordinatorImpl::setCurrentMemberState(const MemberState& newState) {
        invariant(getReplicationMode() == modeReplSet);
        boost::lock_guard<boost::mutex> lk(_mutex);
        _currentState = newState;
    }

    MemberState ReplicationCoordinatorImpl::getCurrentMemberState() const {
        invariant(getReplicationMode() == modeReplSet);
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _currentState;
    }

    Status ReplicationCoordinatorImpl::setLastOptime(const OID& rid,
                                                     const OpTime& ts) {
        // TODO(spencer): update slave tracking thread for local.slaves
        // TODO(spencer): pass info upstream if we're not primary
        boost::lock_guard<boost::mutex> lk(_mutex);

        OpTime& slaveOpTime = _slaveOpTimeMap[rid];
        if (slaveOpTime < ts) {
            slaveOpTime = ts;
            // TODO(spencer): update write concern tags if we're a replSet

            // Wake up any threads waiting for replication that now have their replication
            // check satisfied
            for (std::vector<WaiterInfo*>::iterator it = _replicationWaiterList.begin();
                    it != _replicationWaiterList.end(); ++it) {
                WaiterInfo* info = *it;
                if (_opReplicatedEnough_inlock(*info->opTime, *info->writeConcern)) {
                    info->condVar->notify_all();
                }
            }
        }
        return Status::OK();
    }


    bool ReplicationCoordinatorImpl::_opReplicatedEnough_inlock(
            const OpTime& opId, const WriteConcernOptions& writeConcern) {
        int numNodes;
        if (!writeConcern.wMode.empty()) {
            fassert(18524, writeConcern.wMode == "majority"); // TODO(spencer): handle tags
            numNodes = _rsConfig.majorityNumber;
        } else {
            numNodes = writeConcern.wNumNodes;
        }

        for (SlaveOpTimeMap::iterator it = _slaveOpTimeMap.begin();
                it != _slaveOpTimeMap.end(); ++it) {
            const OpTime& slaveTime = it->second;
            if (slaveTime >= opId) {
                --numNodes;
            }
            if (numNodes <= 0) {
                return true;
            }
        }
        return false;
    }

    ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorImpl::awaitReplication(
            const OperationContext* txn,
            const OpTime& opId,
            const WriteConcernOptions& writeConcern) {
        // TODO(spencer): handle killop


        if (writeConcern.wNumNodes <= 1 && writeConcern.wMode.empty()) {
            // no desired replication check
            return StatusAndDuration(Status::OK(), Milliseconds(0));
        }

        const Mode replMode = getReplicationMode();
        if (replMode == modeNone || serverGlobalParams.configsvr) {
            // no replication check needed (validated above)
            return StatusAndDuration(Status::OK(), Milliseconds(0));
        }

        if (writeConcern.wMode == "majority" && replMode == modeMasterSlave) {
            // with master/slave, majority is equivalent to w=1
            return StatusAndDuration(Status::OK(), Milliseconds(0));
        }

        Timer timer;
        boost::condition_variable condVar;
        boost::unique_lock<boost::mutex> lk(_mutex);
        // Must hold _mutex before constructing waitInfo as it will modify _replicationWaiterList
        WaiterInfo waitInfo(&_replicationWaiterList, &opId, &writeConcern, &condVar);

        while (!_opReplicatedEnough_inlock(opId, writeConcern)) {
            const int elapsed = timer.millis();
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
                } else {
                    condVar.timed_wait(lk, Milliseconds(writeConcern.wTimeout - elapsed));
                }
            } catch (const boost::thread_interrupted&) {}
        }

        return StatusAndDuration(Status::OK(), Milliseconds(timer.millis()));
    }

    ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorImpl::awaitReplicationOfLastOp(
            const OperationContext* txn,
            const WriteConcernOptions& writeConcern) {
        return StatusAndDuration(Status::OK(), Milliseconds(0));
    }

    Status ReplicationCoordinatorImpl::stepDown(bool force,
                                                const Milliseconds& waitTime,
                                                const Milliseconds& stepdownTime) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorImpl::stepDownAndWaitForSecondary(
            const Milliseconds& initialWaitTime,
            const Milliseconds& stepdownTime,
            const Milliseconds& postStepdownWaitTime) {
        // TODO
        return Status::OK();
    }

    bool ReplicationCoordinatorImpl::isMasterForReportingPurposes() {
        // TODO
        return false;
    }

    bool ReplicationCoordinatorImpl::canAcceptWritesForDatabase(const StringData& collection) {
        // TODO
        return false;
    }

    Status ReplicationCoordinatorImpl::canServeReadsFor(const NamespaceString& ns, bool slaveOk) {
        // TODO
        return Status::OK();
    }

    bool ReplicationCoordinatorImpl::shouldIgnoreUniqueIndex(const IndexDescriptor* idx) {
        if (!idx->unique()) {
            return false;
        }
        // Never ignore _id index
        if (idx->isIdIndex()) {
            return false;
        }
        if (getReplicationMode() != modeReplSet) {
            return false;
        }
        // see SERVER-6671
        MemberState ms = getCurrentMemberState();
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
        // TODO
        return OID();
    }

    void ReplicationCoordinatorImpl::processReplSetGetStatus(BSONObjBuilder* result) {
        // TODO
    }

    bool ReplicationCoordinatorImpl::setMaintenanceMode(bool activate) {
        // TODO
        return false;
    }

    Status ReplicationCoordinatorImpl::processReplSetSyncFrom(const std::string& target,
                                                              BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorImpl::processReplSetMaintenance(bool activate,
                                                                 BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorImpl::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorImpl::processHeartbeat(const BSONObj& cmdObj, 
                                                        BSONObjBuilder* resultObj) {
        Status result(ErrorCodes::InternalError, "didn't set status in prepareHeartbeatResponse");
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = _replExecutor->scheduleWork(
            stdx::bind(&TopologyCoordinator::prepareHeartbeatResponse,
                       _topCoord.get(),
                       stdx::placeholders::_1,
                       Date_t(curTimeMillis64()),
                       cmdObj,
                       resultObj,
                       &result));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return Status(ErrorCodes::ShutdownInProgress, "replication shutdown in progress");
        }
        fassert(18508, cbh.getStatus());
        _replExecutor->wait(cbh.getValue());
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
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorImpl::processReplSetGetRBID(BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    void ReplicationCoordinatorImpl::incrementRollbackID() { /* TODO */ }

    Status ReplicationCoordinatorImpl::processReplSetFresh(const ReplSetFreshArgs& args,
                                                           BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorImpl::processReplSetElect(const ReplSetElectArgs& args,
                                                           BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    void ReplicationCoordinatorImpl::setCurrentReplicaSetConfig(
            const TopologyCoordinator::ReplicaSetConfig& newConfig) {
        invariant(getReplicationMode() == modeReplSet);
        boost::lock_guard<boost::mutex> lk(_mutex);
        _rsConfig = newConfig;

        // TODO: Cancel heartbeats, start new ones
    }

    Status ReplicationCoordinatorImpl::processReplSetUpdatePosition(const BSONArray& updates,
                                                                    BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorImpl::processReplSetUpdatePositionHandshake(
            const BSONObj& handshake,
            BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    bool ReplicationCoordinatorImpl::processHandshake(const OID& remoteID,
                                                      const BSONObj& handshake) {
        // TODO
        return false;
    }

    void ReplicationCoordinatorImpl::waitUpToOneSecondForOptimeChange(const OpTime& ot) {
        //TODO
    }

    bool ReplicationCoordinatorImpl::buildsIndexes() {
        // TODO
        return false;
    }

    std::vector<BSONObj> ReplicationCoordinatorImpl::getHostsWrittenTo(const OpTime& op) {
        // TODO
        return std::vector<BSONObj>();
    }

    Status ReplicationCoordinatorImpl::checkIfWriteConcernCanBeSatisfied(
            const WriteConcernOptions& writeConcern) const {
        // TODO
        return Status::OK();
    }

    void ReplicationCoordinatorImpl::doMemberHeartbeat(ReplicationExecutor* executor,
                                                       const Status& inStatus,
                                                       const HostAndPort& hap) {
        // TODO
    }

    void ReplicationCoordinatorImpl::_handleHeartbeatResponse(
                                const ReplicationExecutor::RemoteCommandCallbackData& cbData,
                                StatusWith<BSONObj>* outStatus,
                                const HostAndPort& hap,
                                Date_t firstCallDate,
                                int retriesLeft) {
        // TODO
    }

    void ReplicationCoordinatorImpl::cancelHeartbeats() {
        // TODO
    }

} // namespace repl
} // namespace mongo

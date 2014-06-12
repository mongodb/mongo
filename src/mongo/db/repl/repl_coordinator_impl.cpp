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

#include <boost/thread.hpp>

#include "mongo/base/status.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/rs.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

    ReplicationCoordinatorImpl::ReplicationCoordinatorImpl() {}

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
        if (!isReplEnabled()) {
            return;
        }

        _replExecutor->shutdown();
        _topCoordDriverThread->join();
    }

    bool ReplicationCoordinatorImpl::isShutdownOkay() const {
        // TODO
        return false;
    }

    ReplicationCoordinator::Mode ReplicationCoordinatorImpl::getReplicationMode() const {
        // TODO(spencer): Don't rely on global replSettings object
        if (replSettings.usingReplSets()) {
            return modeReplSet;
        } else if (replSettings.slave || replSettings.master) {
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

    ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorImpl::awaitReplication(
            const OperationContext* txn,
            const OpTime& ts,
            const WriteConcernOptions& writeConcern) {
        // TODO
        return StatusAndDuration(Status::OK(), Milliseconds(0));
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

    bool ReplicationCoordinatorImpl::canServeReadsFor(const NamespaceString& collection) {
        // TODO
        return false;
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

    Status ReplicationCoordinatorImpl::setLastOptime(const OID& rid,
                                                     const OpTime& ts,
                                                     const BSONObj& config) {
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

    void ReplicationCoordinatorImpl::setCurrentReplicaSetConfig(
            const TopologyCoordinator::ReplicaSetConfig& newConfig) {
        invariant(getReplicationMode() == modeReplSet);
        boost::lock_guard<boost::mutex> lk(_mutex);
        _rsConfig = newConfig;
    }

} // namespace repl
} // namespace mongo

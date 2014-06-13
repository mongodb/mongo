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
#include "mongo/util/assert_util.h" // TODO: remove along with invariant from getCurrentMemberState

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
        // TODO
    }

    MemberState ReplicationCoordinatorImpl::getCurrentMemberState() const {
        // TODO
        invariant(false);
    }

    Status ReplicationCoordinatorImpl::awaitReplication(const OpTime& ts,
                                                        const WriteConcernOptions& writeConcern,
                                                        Milliseconds timeout) {
        // TODO
        return Status::OK();
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
        // TODO
        return false;
    }

    Status ReplicationCoordinatorImpl::setLastOptime(const HostAndPort& member,
                                                     const OpTime& ts) {
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
        // TODO
    }

} // namespace repl
} // namespace mongo

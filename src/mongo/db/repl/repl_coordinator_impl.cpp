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

#include "mongo/base/status.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/assert_util.h" // TODO: remove along with invariant from getCurrentMemberState

namespace mongo {
namespace repl {

    ReplicationCoordinatorImpl::ReplicationCoordinatorImpl() {}
    ReplicationCoordinatorImpl::~ReplicationCoordinatorImpl() {}

    void ReplicationCoordinatorImpl::startReplication() {
        // TODO
    }

    void ReplicationCoordinatorImpl::shutdown() {
        // TODO
    }

    bool ReplicationCoordinatorImpl::isShutdownOkay() const {
        // TODO
        return false;
    }

    ReplicationCoordinator::Mode ReplicationCoordinatorImpl::getReplicationMode() const {
        // TODO
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

    bool ReplicationCoordinatorImpl::processHeartbeat(OperationContext* txn, 
                                                        const BSONObj& cmdObj, 
                                                        std::string* errmsg, 
                                                        BSONObjBuilder* result) {
        return false;
    }

    void ReplicationCoordinatorImpl::setCurrentReplicaSetConfig(
            const TopologyCoordinatorImpl::ReplicaSetConfig& newConfig) {
        // TODO
    }

} // namespace repl
} // namespace mongo

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

#include "mongo/db/repl/repl_coordinator_mock.h"

#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

    ReplicationCoordinatorMock::ReplicationCoordinatorMock() {}
    ReplicationCoordinatorMock::~ReplicationCoordinatorMock() {}

    void ReplicationCoordinatorMock::startReplication(
            TopologyCoordinator* topCoord,
            ReplicationExecutor::NetworkInterface* network) {
        // TODO
    }

    void ReplicationCoordinatorMock::shutdown() {
        // TODO
    }

    bool ReplicationCoordinatorMock::isShutdownOkay() const {
        // TODO
        return false;
    }

    bool ReplicationCoordinatorMock::isReplEnabled() const {
        return false;
    }

    ReplicationCoordinator::Mode ReplicationCoordinatorMock::getReplicationMode() const {
        return modeNone;
    }

    MemberState ReplicationCoordinatorMock::getCurrentMemberState() const {
        // TODO
        invariant(false);
    }

    ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorMock::awaitReplication(
            const OperationContext* txn,
            const OpTime& ts,
            const WriteConcernOptions& writeConcern) {
        // TODO
        return StatusAndDuration(Status::OK(), Milliseconds(0));
    }

    ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorMock::awaitReplicationOfLastOp(
            const OperationContext* txn,
            const WriteConcernOptions& writeConcern) {
        // TODO
        return StatusAndDuration(Status::OK(), Milliseconds(0));
    }

    Status ReplicationCoordinatorMock::stepDown(bool force,
                                                const Milliseconds& waitTime,
                                                const Milliseconds& stepdownTime) {
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::stepDownAndWaitForSecondary(
            const Milliseconds& initialWaitTime,
            const Milliseconds& stepdownTime,
            const Milliseconds& postStepdownWaitTime) {
        return Status::OK();
    }

    bool ReplicationCoordinatorMock::isMasterForReportingPurposes() {
        // TODO
        return true;
    }

    bool ReplicationCoordinatorMock::canAcceptWritesForDatabase(const StringData& dbName) {
        // TODO
        return true;
    }

    bool ReplicationCoordinatorMock::canServeReadsFor(const NamespaceString& collection) {
        // TODO
        return true;
    }

    bool ReplicationCoordinatorMock::shouldIgnoreUniqueIndex(const IndexDescriptor* idx) {
        // TODO
        return false;
    }

    Status ReplicationCoordinatorMock::setLastOptime(const OID& rid,
                                                     const OpTime& ts,
                                                     const BSONObj& config) {
        // TODO
        return Status::OK();
    }

    void ReplicationCoordinatorMock::processReplSetGetStatus(BSONObjBuilder* result) {
        //TODO
    }

    Status ReplicationCoordinatorMock::processHeartbeat(const BSONObj& cmdObj, 
                                                      BSONObjBuilder* resultObj) {
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processReplSetReconfig(OperationContext* txn,
                                                              const BSONObj& newConfigObj,
                                                              bool force,
                                                              BSONObjBuilder* resultObj) {
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processReplSetInitiate(OperationContext* txn,
                                                              const BSONObj& configObj,
                                                              BSONObjBuilder* resultObj) {
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processReplSetGetRBID(BSONObjBuilder* resultObj) {
        return Status::OK();
    }

    void ReplicationCoordinatorMock::incrementRollbackID() {}

    Status ReplicationCoordinatorMock::processReplSetFresh(const StringData& setName,
                                                           const StringData& who,
                                                           unsigned id,
                                                           int cfgver,
                                                           const OpTime& opTime,
                                                           BSONObjBuilder* resultObj) {
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processReplSetElect(const StringData& set,
                                                           unsigned whoid,
                                                           int cfgver,
                                                           const OID& round,
                                                           BSONObjBuilder* resultObj) {
        return Status::OK();
    }
} // namespace repl
} // namespace mongo

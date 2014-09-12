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

    ReplicationCoordinatorMock::ReplicationCoordinatorMock(const ReplSettings& settings) :
            _settings(settings) {}
    ReplicationCoordinatorMock::~ReplicationCoordinatorMock() {}

    void ReplicationCoordinatorMock::startReplication(OperationContext* txn) {
        // TODO
    }

    void ReplicationCoordinatorMock::shutdown() {
        // TODO
    }

    ReplSettings& ReplicationCoordinatorMock::getSettings() {
        return _settings;
    }

    bool ReplicationCoordinatorMock::isReplEnabled() const {
        return _settings.usingReplSets() || _settings.master || _settings.slave;
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

    ReplicationCoordinator::StatusAndDuration
            ReplicationCoordinatorMock::awaitReplicationOfLastOpForClient(
                    const OperationContext* txn,
                    const WriteConcernOptions& writeConcern) {
        return StatusAndDuration(Status::OK(), Milliseconds(0));
    }

    ReplicationCoordinator::StatusAndDuration
            ReplicationCoordinatorMock::awaitReplicationOfLastOpApplied(
                    const OperationContext* txn,
                    const WriteConcernOptions& writeConcern) {
        return StatusAndDuration(Status::OK(), Milliseconds(0));
    }

    Status ReplicationCoordinatorMock::stepDown(OperationContext* txn,
                                                bool force,
                                                const Milliseconds& waitTime,
                                                const Milliseconds& stepdownTime) {
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

    Status ReplicationCoordinatorMock::canServeReadsFor(OperationContext* txn,
                                                        const NamespaceString& ns,
                                                        bool slaveOk) {
        // TODO
        return Status::OK();
    }

    bool ReplicationCoordinatorMock::shouldIgnoreUniqueIndex(const IndexDescriptor* idx) {
        // TODO
        return false;
    }

    Status ReplicationCoordinatorMock::setLastOptime(OperationContext* txn,
                                                     const OID& rid,
                                                     const OpTime& ts) {
        return Status::OK();
    }
    
    Status ReplicationCoordinatorMock::setMyLastOptime(OperationContext* txn, const OpTime& ts) {
        return Status::OK();
    }

    OpTime ReplicationCoordinatorMock::getMyLastOptime() const {
        // TODO
        return OpTime();
    }


    OID ReplicationCoordinatorMock::getElectionId() {
        // TODO
        return OID();
    }

    OID ReplicationCoordinatorMock::getMyRID() const {
        return OID();
    }

    void ReplicationCoordinatorMock::setFollowerMode(const MemberState& newState) {
    }

    void ReplicationCoordinatorMock::prepareReplSetUpdatePositionCommand(
            OperationContext* txn,
            BSONObjBuilder* cmdBuilder) {}

    void ReplicationCoordinatorMock::prepareReplSetUpdatePositionCommandHandshakes(
            OperationContext* txn,
            std::vector<BSONObj>* handshakes) {}

    void ReplicationCoordinatorMock::processReplSetGetConfig(BSONObjBuilder* result) {
        // TODO
    }

    Status ReplicationCoordinatorMock::processReplSetGetStatus(BSONObjBuilder* result) {
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::setMaintenanceMode(OperationContext* txn, bool activate) {
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processReplSetSyncFrom(const HostAndPort& target,
                                                              BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processHeartbeat(const ReplSetHeartbeatArgs& args,
                                                        ReplSetHeartbeatResponse* response) {
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processReplSetReconfig(OperationContext* txn,
                                                              const ReplSetReconfigArgs& args,
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

    Status ReplicationCoordinatorMock::processReplSetFresh(const ReplSetFreshArgs& args,
                                                           BSONObjBuilder* resultObj) {
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processReplSetElect(const ReplSetElectArgs& args,
                                                           BSONObjBuilder* resultObj) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processReplSetUpdatePosition(
            OperationContext* txn,
            const UpdatePositionArgs& updates) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processHandshake(const OperationContext* txn,
                                                        const HandshakeArgs& handshake) {
        return Status::OK();
    }

    void ReplicationCoordinatorMock::waitUpToOneSecondForOptimeChange(const OpTime& ot) {
        // TODO
    }

    bool ReplicationCoordinatorMock::buildsIndexes() {
        // TODO
        return false;
    }

    std::vector<BSONObj> ReplicationCoordinatorMock::getHostsWrittenTo(const OpTime& op) {
        // TODO
        return std::vector<BSONObj>();
    }

    Status ReplicationCoordinatorMock::checkIfWriteConcernCanBeSatisfied(
            const WriteConcernOptions& writeConcern) const {
        return Status::OK();
    }

    BSONObj ReplicationCoordinatorMock::getGetLastErrorDefault() {
        // TODO
        return BSONObj();
    }

    Status ReplicationCoordinatorMock::checkReplEnabledForCommand(BSONObjBuilder* result) {
        // TODO
        return Status::OK();
    }

} // namespace repl
} // namespace mongo

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
#include "mongo/db/write_concern_options.h"
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

    const ReplSettings& ReplicationCoordinatorMock::getSettings() const {
        return _settings;
    }

    bool ReplicationCoordinatorMock::isReplEnabled() const {
        return _settings.usingReplSets() || _settings.master || _settings.slave;
    }

    ReplicationCoordinator::Mode ReplicationCoordinatorMock::getReplicationMode() const {
        return modeNone;
    }

    MemberState ReplicationCoordinatorMock::getMemberState() const {
        // TODO
        invariant(false);
    }

    bool ReplicationCoordinatorMock::isInPrimaryOrSecondaryState() const {
        invariant(false);
    }

    Seconds ReplicationCoordinatorMock::getSlaveDelaySecs() const {
        return Seconds(0);
    }

    void ReplicationCoordinatorMock::clearSyncSourceBlacklist() {}

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

    Status ReplicationCoordinatorMock::checkCanServeReadsFor(OperationContext* txn,
                                                             const NamespaceString& ns,
                                                             bool slaveOk) {
        // TODO
        return Status::OK();
    }

    bool ReplicationCoordinatorMock::shouldIgnoreUniqueIndex(const IndexDescriptor* idx) {
        // TODO
        return false;
    }

    Status ReplicationCoordinatorMock::setLastOptimeForSlave(const OID& rid, const OpTime& ts) {
        return Status::OK();
    }
    
    void ReplicationCoordinatorMock::setMyHeartbeatMessage(const std::string& msg) {
        // TODO
    }

    void ReplicationCoordinatorMock::setMyLastOptime(const OpTime& ts) {}

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

    int ReplicationCoordinatorMock::getMyId() const {
        return 0;
    }

    bool ReplicationCoordinatorMock::setFollowerMode(const MemberState& newState) {
        return true;
    }

    bool ReplicationCoordinatorMock::isWaitingForApplierToDrain() {
        return false;
    }

    void ReplicationCoordinatorMock::signalDrainComplete(OperationContext*) {}

    void ReplicationCoordinatorMock::signalUpstreamUpdater() {}

    void ReplicationCoordinatorMock::prepareReplSetUpdatePositionCommand(
            BSONObjBuilder* cmdBuilder) {}

    void ReplicationCoordinatorMock::prepareReplSetUpdatePositionCommandHandshakes(
            std::vector<BSONObj>* handshakes) {}

    void ReplicationCoordinatorMock::processReplSetGetConfig(BSONObjBuilder* result) {
        // TODO
    }

    Status ReplicationCoordinatorMock::processReplSetGetStatus(BSONObjBuilder* result) {
        return Status::OK();
    }

    void ReplicationCoordinatorMock::fillIsMasterForReplSet(IsMasterResponse* result) {}

    void ReplicationCoordinatorMock::appendSlaveInfoData(BSONObjBuilder* result) {}

    Status ReplicationCoordinatorMock::setMaintenanceMode(bool activate) {
        return Status::OK();
    }

    bool ReplicationCoordinatorMock::getMaintenanceMode() {
        return false;
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
            const UpdatePositionArgs& updates) {
        // TODO
        return Status::OK();
    }

    Status ReplicationCoordinatorMock::processHandshake(OperationContext* txn,
                                                        const HandshakeArgs& handshake) {
        return Status::OK();
    }

    bool ReplicationCoordinatorMock::buildsIndexes() {
        // TODO
        return true;
    }

    std::vector<HostAndPort> ReplicationCoordinatorMock::getHostsWrittenTo(const OpTime& op) {
        return std::vector<HostAndPort>();
    }

    vector<HostAndPort> ReplicationCoordinatorMock::getOtherNodesInReplSet() const {
        return std::vector<HostAndPort>();
    }

    Status ReplicationCoordinatorMock::checkIfWriteConcernCanBeSatisfied(
            const WriteConcernOptions& writeConcern) const {
        return Status::OK();
    }

    WriteConcernOptions ReplicationCoordinatorMock::getGetLastErrorDefault() {
        return WriteConcernOptions();
    }

    Status ReplicationCoordinatorMock::checkReplEnabledForCommand(BSONObjBuilder* result) {
        // TODO
        return Status::OK();
    }

    HostAndPort ReplicationCoordinatorMock::chooseNewSyncSource() {
        invariant(false);
        return HostAndPort();
    }

    void ReplicationCoordinatorMock::blacklistSyncSource(const HostAndPort& host, Date_t until) {
        invariant(false);
    }

    void ReplicationCoordinatorMock::resetLastOpTimeFromOplog(OperationContext* txn) {
        invariant(false);
    }

    bool ReplicationCoordinatorMock::shouldChangeSyncSource(const HostAndPort& currentSource) {
        invariant(false);
    }

} // namespace repl
} // namespace mongo

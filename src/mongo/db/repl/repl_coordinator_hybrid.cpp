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

#include "mongo/db/repl/repl_coordinator_hybrid.h"

#include "mongo/db/repl/repl_coordinator_external_state_impl.h"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kReplication);

namespace repl {

    HybridReplicationCoordinator::HybridReplicationCoordinator(const ReplSettings& settings) :
            _legacy(settings), _impl(settings, new ReplicationCoordinatorExternalStateImpl()) {}
    HybridReplicationCoordinator::~HybridReplicationCoordinator() {}

    void HybridReplicationCoordinator::startReplication(
            TopologyCoordinator* topCoord,
            ReplicationExecutor::NetworkInterface* network) {
        _legacy.startReplication(topCoord, network);
        _impl.startReplication(topCoord, network);
    }

    void HybridReplicationCoordinator::shutdown() {
        _legacy.shutdown();
        _impl.shutdown();
    }

    ReplSettings& HybridReplicationCoordinator::getSettings() {
        ReplSettings& legacySettings = _legacy.getSettings();
        return legacySettings;
    }

    ReplicationCoordinator::Mode HybridReplicationCoordinator::getReplicationMode() const {
        Mode legacyMode = _legacy.getReplicationMode();
        // TODO(dannenberg) uncomment below once impl has implemented mode related functionality
        // Mode implMode = _impl.getReplicationMode();
        // invariant(legacyMode == implMode);
        return legacyMode;
    }

    MemberState HybridReplicationCoordinator::getCurrentMemberState() const {
        MemberState legacyState = _legacy.getCurrentMemberState();
        return legacyState;
    }

    ReplicationCoordinator::StatusAndDuration HybridReplicationCoordinator::awaitReplication(
            const OperationContext* txn,
            const OpTime& ts,
            const WriteConcernOptions& writeConcern) {
        StatusAndDuration legacyStatus = _legacy.awaitReplication(txn, ts, writeConcern);
        return legacyStatus;
    }

    ReplicationCoordinator::StatusAndDuration 
            HybridReplicationCoordinator::awaitReplicationOfLastOp(
                    const OperationContext* txn,
                    const WriteConcernOptions& writeConcern) {
        StatusAndDuration legacyStatus = _legacy.awaitReplicationOfLastOp(txn, writeConcern);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::stepDown(OperationContext* txn,
                                                  bool force,
                                                  const Milliseconds& waitTime,
                                                  const Milliseconds& stepdownTime) {
        Status legacyStatus = _legacy.stepDown(txn, force, waitTime, stepdownTime);
        Status implStatus = _impl.stepDown(txn, force, waitTime, stepdownTime);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::stepDownAndWaitForSecondary(
            OperationContext* txn,
            const Milliseconds& initialWaitTime,
            const Milliseconds& stepdownTime,
            const Milliseconds& postStepdownWaitTime) {
        Status legacyStatus = _legacy.stepDownAndWaitForSecondary(txn,
                                                                  initialWaitTime,
                                                                  stepdownTime,
                                                                  postStepdownWaitTime);
        Status implStatus = _impl.stepDownAndWaitForSecondary(txn,
                                                              initialWaitTime,
                                                              stepdownTime,
                                                              postStepdownWaitTime);
        return legacyStatus;
    }

    bool HybridReplicationCoordinator::isMasterForReportingPurposes() {
        bool legacyResponse = _legacy.isMasterForReportingPurposes();
        // TODO(dannenberg) uncomment below once the impl state changes are fully implemented
        // bool implResponse = _impl.isMasterForReportingPurposes();
        // invariant(legacyResponse == implResponse);
        return legacyResponse;
    }

    bool HybridReplicationCoordinator::canAcceptWritesForDatabase(const StringData& dbName) {
        bool legacyResponse = _legacy.canAcceptWritesForDatabase(dbName);
        // TODO(dannenberg) uncomment below once the impl state changes are fully implemented
        // bool implResponse = _impl.canAcceptWritesForDatabase(dbName);
        // invariant(legacyResponse == implResponse);
        return legacyResponse;
    }

    Status HybridReplicationCoordinator::canServeReadsFor(OperationContext* txn,
                                                          const NamespaceString& ns,
                                                          bool slaveOk) {
        Status legacyStatus = _legacy.canServeReadsFor(txn, ns, slaveOk);
        // TODO(dannenberg) uncomment below once the impl state changes are full implemeneted
        // Status implStatus = _impl.canServeReadsFor(txn, ns, slaveOk);
        // invariant(legacyStatus == implStatus);
        return legacyStatus;
    }

    bool HybridReplicationCoordinator::shouldIgnoreUniqueIndex(const IndexDescriptor* idx) {
        bool legacyResponse = _legacy.shouldIgnoreUniqueIndex(idx);
        _impl.shouldIgnoreUniqueIndex(idx);
        return legacyResponse;
    }

    Status HybridReplicationCoordinator::setLastOptime(OperationContext* txn,
                                                       const OID& rid,
                                                       const OpTime& ts) {
        Status legacyStatus = _legacy.setLastOptime(txn, rid, ts);
        Status implStatus = _impl.setLastOptime(txn, rid, ts);
        return legacyStatus;
    }
    
    OID HybridReplicationCoordinator::getElectionId() {
        OID legacyOID = _legacy.getElectionId();
        _impl.getElectionId();
        return legacyOID;
    }

    OID HybridReplicationCoordinator::getMyRID(OperationContext* txn) {
        OID legacyRID = _legacy.getMyRID(txn);
        _impl.getMyRID(txn);
        return legacyRID;
    }

    void HybridReplicationCoordinator::prepareReplSetUpdatePositionCommand(OperationContext* txn,
                                                                           BSONObjBuilder* result) {
        _legacy.prepareReplSetUpdatePositionCommand(txn, result);
        // TODO(spencer): Can't call into the impl until it can load a valid config
        //BSONObjBuilder implResult;
        //_impl.prepareReplSetUpdatePositionCommand(&implResult);
    }

    void HybridReplicationCoordinator::prepareReplSetUpdatePositionCommandHandshakes(
            OperationContext* txn,
            std::vector<BSONObj>* handshakes) {
        _legacy.prepareReplSetUpdatePositionCommandHandshakes(txn, handshakes);
        // TODO(spencer): Can't call into the impl until it can load a valid config
        //std::vector<BSONObj> implResult;
        //_impl.prepareReplSetUpdatePositionCommandHandshakes(&implResult);
    }

    Status HybridReplicationCoordinator::processReplSetGetStatus(BSONObjBuilder* result) {
        Status legacyStatus = _legacy.processReplSetGetStatus(result);
        BSONObjBuilder implResult;
        _impl.processReplSetGetStatus(&implResult);
        return legacyStatus;
    }

    void HybridReplicationCoordinator::processReplSetGetConfig(BSONObjBuilder* result) {
        _legacy.processReplSetGetConfig(result);
        BSONObjBuilder implResult;
        _impl.processReplSetGetConfig(&implResult);
    }

    bool HybridReplicationCoordinator::setMaintenanceMode(OperationContext* txn, bool activate) {
        bool legacyResponse = _legacy.setMaintenanceMode(txn, activate);
        _impl.setMaintenanceMode(txn, activate);
        return legacyResponse;
    }

    Status HybridReplicationCoordinator::processHeartbeat(const ReplSetHeartbeatArgs& args,
                                                          ReplSetHeartbeatResponse* response) {
        Status legacyStatus = _legacy.processHeartbeat(args, response);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetReconfig(OperationContext* txn,
                                                                const ReplSetReconfigArgs& args,
                                                                BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetReconfig(txn, args, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetReconfig(txn, args, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetInitiate(OperationContext* txn,
                                                                const BSONObj& givenConfig,
                                                                BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetInitiate(txn, givenConfig, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetInitiate(txn, givenConfig, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetGetRBID(BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetGetRBID(resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetGetRBID(&implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetFresh(const ReplSetFreshArgs& args,
                                                             BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetFresh(args, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetFresh(args, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetElect(const ReplSetElectArgs& args,
                                                             BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetElect(args, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetElect(args, &implResult);
        return legacyStatus;
    }

    void HybridReplicationCoordinator::incrementRollbackID() {
        _legacy.incrementRollbackID();
        _impl.incrementRollbackID();
    }

    Status HybridReplicationCoordinator::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetFreeze(secs, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetFreeze(secs, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetMaintenance(OperationContext* txn,
                                                                   bool activate,
                                                                   BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetMaintenance(txn, activate, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetMaintenance(txn, activate, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetSyncFrom(const std::string& target,
                                                                BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetSyncFrom(target, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetSyncFrom(target, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetUpdatePosition(OperationContext* txn,
                                                                      const BSONArray& updates,
                                                                      BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetUpdatePosition(txn, updates, resultObj);
        // TODO(spencer): Can't uncomment this until we uncomment processHandshake below
        //BSONObjBuilder implResult;
        //Status implStatus = _impl.processReplSetUpdatePosition(txn, updates, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processHandshake(const OperationContext* txn,
                                                          const HandshakeArgs& handshake) {
        Status legacyResponse = _legacy.processHandshake(txn, handshake);
        // TODO(spencer): Can't call into the impl until it can load a valid config
        //_impl.processHandshake(txn, handshake);
        return legacyResponse;
    }

    void HybridReplicationCoordinator::waitUpToOneSecondForOptimeChange(const OpTime& ot) {
        _legacy.waitUpToOneSecondForOptimeChange(ot);
        //TODO(spencer) switch to _impl.waitUpToOneSecondForOptimeChange(ot); once implemented
    }

    bool HybridReplicationCoordinator::buildsIndexes() {
        bool legacyResponse = _legacy.buildsIndexes();
        // TODO(dannenberg) uncomment once config loading is working properly in impl
        // bool implResponse = _impl.buildsIndexes();
        // invariant(legacyResponse == implResponse);
        return legacyResponse;
    }

    vector<BSONObj> HybridReplicationCoordinator::getHostsWrittenTo(const OpTime& op) {
        vector<BSONObj> legacyResponse = _legacy.getHostsWrittenTo(op);
        vector<BSONObj> implResponse = _impl.getHostsWrittenTo(op);
        return legacyResponse;
    }

    Status HybridReplicationCoordinator::checkIfWriteConcernCanBeSatisfied(
            const WriteConcernOptions& writeConcern) const {
        Status legacyStatus = _legacy.checkIfWriteConcernCanBeSatisfied(writeConcern);
        Status implStatus = _impl.checkIfWriteConcernCanBeSatisfied(writeConcern);
        return legacyStatus;
    }

    BSONObj HybridReplicationCoordinator::getGetLastErrorDefault() {
        BSONObj legacyGLE = _legacy.getGetLastErrorDefault();
        BSONObj implGLE = _impl.getGetLastErrorDefault();
        return legacyGLE;
    }

    Status HybridReplicationCoordinator::checkReplEnabledForCommand(BSONObjBuilder* result) {
        Status legacyStatus = _legacy.checkReplEnabledForCommand(result);
        Status implStatus = _impl.checkReplEnabledForCommand(result);
        return legacyStatus;
    }

    bool HybridReplicationCoordinator::isReplEnabled() const {
        bool legacyResponse = _legacy.isReplEnabled();
        // TODO(dannenberg) uncomment once config loading is working properly in impl
        // bool implResponse = _impl.isReplEnabled();
        // invariant(legacyResponse == implResponse);
        return legacyResponse;
    }

} // namespace repl
} // namespace mongo

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

#include "mongo/db/repl/repl_coordinator_hybrid.h"

#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/network_interface_impl.h"
#include "mongo/db/repl/repl_coordinator_external_state_impl.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/rs_config.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/util/log.h"

namespace mongo {

namespace repl {

    HybridReplicationCoordinator::HybridReplicationCoordinator(const ReplSettings& settings) :
        _legacy(settings),
        _impl(settings,
              new ReplicationCoordinatorExternalStateImpl,
              new NetworkInterfaceImpl,
              new TopologyCoordinatorImpl(Seconds(maxSyncSourceLagSecs)),
              static_cast<int64_t>(curTimeMillis64())) {
        getGlobalEnvironment()->registerKillOpListener(&_impl);
    }

    HybridReplicationCoordinator::~HybridReplicationCoordinator() {}

    void HybridReplicationCoordinator::startReplication(OperationContext* txn) {
        _legacy.startReplication(txn);
        _impl.startReplication(txn);
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
        Mode implMode = _impl.getReplicationMode();
        if (legacyMode != implMode) {
            // Can't assert this b/c there could always be a race condition around who detects
            // the config first
            warning() << "Replication mode mismatch. Legacy: " << static_cast<int>(legacyMode) <<
                    ", Impl: " << static_cast<int>(implMode);
        }
        return legacyMode;
    }

    MemberState HybridReplicationCoordinator::getCurrentMemberState() const {
        MemberState legacyState = _legacy.getCurrentMemberState();
        return legacyState;
    }

    void HybridReplicationCoordinator::clearSyncSourceBlacklist() {
        _legacy.clearSyncSourceBlacklist();
        _impl.clearSyncSourceBlacklist();
    }

    ReplicationCoordinator::StatusAndDuration HybridReplicationCoordinator::awaitReplication(
            const OperationContext* txn,
            const OpTime& ts,
            const WriteConcernOptions& writeConcern) {
        StatusAndDuration implStatus = _impl.awaitReplication(txn, ts, writeConcern);
        if (implStatus.status.isOK()) {
            WriteConcernOptions legacyWriteConcern = writeConcern;
            legacyWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
            StatusAndDuration legacyStatus = _legacy.awaitReplication(txn, ts, legacyWriteConcern);
            fassert(18691, legacyStatus.status);
        }
        return implStatus;
    }

    ReplicationCoordinator::StatusAndDuration 
            HybridReplicationCoordinator::awaitReplicationOfLastOpForClient(
                    const OperationContext* txn,
                    const WriteConcernOptions& writeConcern) {
        StatusAndDuration implStatus = _impl.awaitReplicationOfLastOpForClient(txn, writeConcern);
        if (implStatus.status.isOK()) {
            WriteConcernOptions legacyWriteConcern = writeConcern;
            legacyWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
            StatusAndDuration legacyStatus = _legacy.awaitReplicationOfLastOpForClient(
                    txn, legacyWriteConcern);
            fassert(18669, legacyStatus.status);
        }
        return implStatus;
    }

    ReplicationCoordinator::StatusAndDuration
            HybridReplicationCoordinator::awaitReplicationOfLastOpApplied(
                    const OperationContext* txn,
                    const WriteConcernOptions& writeConcern) {
        StatusAndDuration implStatus = _impl.awaitReplicationOfLastOpApplied(txn, writeConcern);
        if (implStatus.status.isOK()) {
            WriteConcernOptions legacyWriteConcern = writeConcern;
            legacyWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
            StatusAndDuration legacyStatus = _legacy.awaitReplicationOfLastOpApplied(
                    txn, legacyWriteConcern);
            fassert(18694, legacyStatus.status);
        }
        return implStatus;
    }

    Status HybridReplicationCoordinator::stepDown(OperationContext* txn,
                                                  bool force,
                                                  const Milliseconds& waitTime,
                                                  const Milliseconds& stepdownTime) {
        Status legacyStatus = _legacy.stepDown(txn, force, waitTime, stepdownTime);
        //        Status implStatus = _impl.stepDown(txn, force, waitTime, stepdownTime);
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

    Status HybridReplicationCoordinator::checkCanServeReadsFor(OperationContext* txn,
                                                               const NamespaceString& ns,
                                                               bool slaveOk) {
        Status legacyStatus = _legacy.checkCanServeReadsFor(txn, ns, slaveOk);
        // TODO(dannenberg) uncomment below once the impl state changes are full implemeneted
        // Status implStatus = _impl.checkCanServeReadsFor(txn, ns, slaveOk);
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
        if (legacyStatus.code() != implStatus.code()) {
            warning() << "Hybrid response difference in setLastOptime. Legacy response: "
                      << legacyStatus << ", impl response: " << implStatus;
        }
        fassert(18667, legacyStatus.code() == implStatus.code());
        return implStatus;
    }

    Status HybridReplicationCoordinator::setMyLastOptime(OperationContext* txn, const OpTime& ts) {
        Status legacyStatus = _legacy.setMyLastOptime(txn, ts);
        // Currently, the legacy code calls logOp before we have a config, which
        // means we can't set the last optime in the impl because we have no place to store it.
        if (getReplicationMode() == ReplicationCoordinator::modeReplSet ||
            getReplicationMode() == ReplicationCoordinator::modeMasterSlave) {
            Status implStatus = _impl.setMyLastOptime(txn, ts);
            if (legacyStatus.code() != implStatus.code()) {
                warning() << "Hybrid response difference in setMyLastOptime. Legacy response: "
                          << legacyStatus << ", impl response: " << implStatus;
            }
            fassert(18666, legacyStatus.code() == implStatus.code());
        }
        return legacyStatus;
    }

    OpTime HybridReplicationCoordinator::getMyLastOptime() const {
        OpTime legacyOpTime = _legacy.getMyLastOptime();
        _impl.getMyLastOptime();
        // Returning the legacy one for now, because at startup we can only set the legacy and not
        // the impl (see comment in setMyLastOptime() above).
        return legacyOpTime;
    }

    OID HybridReplicationCoordinator::getElectionId() {
        OID legacyOID = _legacy.getElectionId();
        _impl.getElectionId();
        return legacyOID;
    }

    OID HybridReplicationCoordinator::getMyRID() const {
        OID legacyRID = _legacy.getMyRID();
        fassert(18696, legacyRID == _impl.getMyRID());
        return legacyRID;
    }

    int HybridReplicationCoordinator::getMyId() const {
        return _impl.getMyId();
    }

    void HybridReplicationCoordinator::setFollowerMode(const MemberState& newState) {
        _legacy.setFollowerMode(newState);
        _impl.setFollowerMode(newState);
    }

    bool HybridReplicationCoordinator::isWaitingForApplierToDrain() {
        bool legacyWaiting = _legacy.isWaitingForApplierToDrain();
        // fassert(18813, legacyWaiting == _impl.isWaitingForApplierToDrain());
        return legacyWaiting;
    }

    void HybridReplicationCoordinator::signalDrainComplete() {
        _legacy.signalDrainComplete();
        _impl.signalDrainComplete();
    }

    void HybridReplicationCoordinator::prepareReplSetUpdatePositionCommand(OperationContext* txn,
                                                                           BSONObjBuilder* result) {
        _impl.prepareReplSetUpdatePositionCommand(txn, result);
        BSONObjBuilder legacyResult;
        _legacy.prepareReplSetUpdatePositionCommand(txn, &legacyResult);
    }

    void HybridReplicationCoordinator::prepareReplSetUpdatePositionCommandHandshakes(
            OperationContext* txn,
            std::vector<BSONObj>* handshakes) {
        _impl.prepareReplSetUpdatePositionCommandHandshakes(txn, handshakes);
        std::vector<BSONObj> legacyResult;
        _legacy.prepareReplSetUpdatePositionCommandHandshakes(txn, &legacyResult);
    }

    Status HybridReplicationCoordinator::processReplSetGetStatus(BSONObjBuilder* result) {
        Status legacyStatus = _legacy.processReplSetGetStatus(result);
        BSONObjBuilder implResult;
        _impl.processReplSetGetStatus(&implResult);
        return legacyStatus;
    }

    void HybridReplicationCoordinator::fillIsMasterForReplSet(IsMasterResponse* result) {
        _legacy.fillIsMasterForReplSet(result);
    }

    void HybridReplicationCoordinator::processReplSetGetConfig(BSONObjBuilder* result) {
        _legacy.processReplSetGetConfig(result);
        BSONObjBuilder implResult;
        _impl.processReplSetGetConfig(&implResult);
    }

    Status HybridReplicationCoordinator::setMaintenanceMode(OperationContext* txn, bool activate) {
        _impl.setMaintenanceMode(txn, activate);
        Status legacyResponse = _legacy.setMaintenanceMode(txn, activate);
        return legacyResponse;
    }

    bool HybridReplicationCoordinator::getMaintenanceMode() {
        //bool legacyMode(_legacy.getMaintenanceMode());
        bool implMode(_impl.getMaintenanceMode());
//        if (legacyMode != implMode) {
//            severe() << "maintenance mode mismatch between legacy and impl: " <<
//                legacyMode << " and " << implMode;
//            fassertFailed(18810);
//        }
        return implMode;
    }

    Status HybridReplicationCoordinator::processHeartbeat(const ReplSetHeartbeatArgs& args,
                                                          ReplSetHeartbeatResponse* response) {
        Status legacyStatus = _legacy.processHeartbeat(args, response);
        ReplSetHeartbeatResponse implResponse;
        _impl.processHeartbeat(args, &implResponse);
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
        // TODO(spencer): Enable this once all config modifying paths are hooked up and we no
        // longer use forceCurrentRSConfigHack.
        //BSONObjBuilder implResult;
        //Status implStatus = _impl.processReplSetInitiate(txn, givenConfig, &implResult);
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

    Status HybridReplicationCoordinator::processReplSetSyncFrom(const HostAndPort& target,
                                                                BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetSyncFrom(target, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetSyncFrom(target, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetUpdatePosition(
            OperationContext* txn,
            const UpdatePositionArgs& updates) {
        Status legacyStatus = _legacy.processReplSetUpdatePosition(txn, updates);
        Status implStatus = _impl.processReplSetUpdatePosition(txn, updates);
        if (legacyStatus.code() != implStatus.code()) {
            warning() << "Hybrid response difference in processReplSetUpdatePosition. "
                    "Legacy response: " << legacyStatus << ", impl response: " << implStatus;
            // Only valid way they can be different is legacy not finding the node and impl
            // succeeding.  This is valid b/c legacy clears it's _members array on reconfigs
            // and then rebuilds it in a non-atomic way.
            fassert(18690, legacyStatus == ErrorCodes::NodeNotFound && implStatus.isOK());
        }
        return implStatus;
    }

    Status HybridReplicationCoordinator::processHandshake(const OperationContext* txn,
                                                          const HandshakeArgs& handshake) {
        Status legacyResponse = _legacy.processHandshake(txn, handshake);
        Status implResponse = _impl.processHandshake(txn, handshake);
        if (legacyResponse.code() != implResponse.code()) {
            warning() << "Hybrid response difference in processHandshake. Legacy response: "
                      << legacyResponse << ", impl response: " << implResponse;
            // Can't fassert that the codes match because when doing a replSetReconfig that adds or
            // removes nodes there is always a race condition between the two coordinators switching
            // to the new config.
            if (implResponse.isOK()) {
                // If either coordinator has a problem have to return whichever has the non-OK
                // status so that the handshake will be retried by the sender, otherwise whichever
                // coordinator failed to process the handshake will fail later on when processing
                // replSetUpdatePosition
                return legacyResponse;
            }
        }
        return implResponse;
    }

    bool HybridReplicationCoordinator::buildsIndexes() {
        bool legacyResponse = _legacy.buildsIndexes();
        bool implResponse = _impl.buildsIndexes();
        invariant(legacyResponse == implResponse);
        return legacyResponse;
    }

    vector<HostAndPort> HybridReplicationCoordinator::getHostsWrittenTo(const OpTime& op) {
        vector<HostAndPort> implResponse = _impl.getHostsWrittenTo(op);
        return implResponse;
    }

    vector<HostAndPort> HybridReplicationCoordinator::getOtherNodesInReplSet() const {
        return _legacy.getOtherNodesInReplSet();
    }

    Status HybridReplicationCoordinator::checkIfWriteConcernCanBeSatisfied(
            const WriteConcernOptions& writeConcern) const {
        return _impl.checkIfWriteConcernCanBeSatisfied(writeConcern);
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
        bool implResponse = _impl.isReplEnabled();
        invariant(legacyResponse == implResponse);
        return legacyResponse;
    }

    HostAndPort HybridReplicationCoordinator::chooseNewSyncSource() {
        return _legacy.chooseNewSyncSource();
        //return _impl.chooseNewSyncSource();
    }

    void HybridReplicationCoordinator::blacklistSyncSource(const HostAndPort& host, Date_t until) {
        _legacy.blacklistSyncSource(host, until);
        _impl.blacklistSyncSource(host, until);
    }

    void HybridReplicationCoordinator::resetLastOpTimeFromOplog(OperationContext* txn) {
        _impl.resetLastOpTimeFromOplog(txn);
        _legacy.resetLastOpTimeFromOplog(txn);
    }

    bool HybridReplicationCoordinator::shouldChangeSyncSource(const HostAndPort& currentSource) {
        // Doesn't yet return the correct answer because we have no heartbeats.
        _impl.shouldChangeSyncSource(currentSource);
        return _legacy.shouldChangeSyncSource(currentSource);
    }

    void HybridReplicationCoordinator::setImplConfigHack(const ReplSetConfig* config) {
        int myIndex = -1;
        for (size_t i = 0; i < config->members.size(); ++i) { // find my index in the config
            if (isSelf(config->members[i].h)) {
                myIndex = i;
                break;
            }
        }
        fassert(18646, myIndex >= 0);
        _impl.forceCurrentRSConfigHack(config->asBson(), myIndex);
    }

} // namespace repl
} // namespace mongo

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

#include "mongo/db/repl/replication_coordinator_mock.h"

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/storage/snapshot_name.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

using std::vector;

ReplicationCoordinatorMock::ReplicationCoordinatorMock(const ReplSettings& settings)
    : _settings(settings) {}

ReplicationCoordinatorMock::~ReplicationCoordinatorMock() {}

void ReplicationCoordinatorMock::startup(OperationContext* txn) {
    // TODO
}

void ReplicationCoordinatorMock::shutdown(OperationContext*) {
    // TODO
}

const ReplSettings& ReplicationCoordinatorMock::getSettings() const {
    return _settings;
}

bool ReplicationCoordinatorMock::isReplEnabled() const {
    return _settings.usingReplSets() || _settings.isMaster() || _settings.isSlave();
}

ReplicationCoordinator::Mode ReplicationCoordinatorMock::getReplicationMode() const {
    if (_settings.usingReplSets()) {
        return modeReplSet;
    }
    if (_settings.isMaster() || _settings.isSlave()) {
        return modeMasterSlave;
    }
    return modeNone;
}

MemberState ReplicationCoordinatorMock::getMemberState() const {
    return _memberState;
}

Status ReplicationCoordinatorMock::waitForMemberState(MemberState expectedState,
                                                      Milliseconds timeout) {
    invariant(false);
    return Status::OK();
}

bool ReplicationCoordinatorMock::isInPrimaryOrSecondaryState() const {
    invariant(false);
}

Seconds ReplicationCoordinatorMock::getSlaveDelaySecs() const {
    return Seconds(0);
}

void ReplicationCoordinatorMock::clearSyncSourceBlacklist() {}

ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorMock::awaitReplication(
    OperationContext* txn, const OpTime& opTime, const WriteConcernOptions& writeConcern) {
    // TODO
    return StatusAndDuration(Status::OK(), Milliseconds(0));
}

ReplicationCoordinator::StatusAndDuration
ReplicationCoordinatorMock::awaitReplicationOfLastOpForClient(
    OperationContext* txn, const WriteConcernOptions& writeConcern) {
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

bool ReplicationCoordinatorMock::canAcceptWritesForDatabase(StringData dbName) {
    // TODO
    return true;
}

bool ReplicationCoordinatorMock::canAcceptWritesFor(const NamespaceString& ns) {
    // TODO
    return canAcceptWritesForDatabase(ns.db());
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

Status ReplicationCoordinatorMock::setLastOptimeForSlave(const OID& rid, const Timestamp& ts) {
    return Status::OK();
}

void ReplicationCoordinatorMock::setMyHeartbeatMessage(const std::string& msg) {
    // TODO
}

void ReplicationCoordinatorMock::setMyLastAppliedOpTime(const OpTime& opTime) {
    _myLastAppliedOpTime = opTime;
}

void ReplicationCoordinatorMock::setMyLastDurableOpTime(const OpTime& opTime) {
    _myLastDurableOpTime = opTime;
}

void ReplicationCoordinatorMock::setMyLastAppliedOpTimeForward(const OpTime& opTime) {
    if (opTime > _myLastAppliedOpTime) {
        _myLastAppliedOpTime = opTime;
    }
}

void ReplicationCoordinatorMock::setMyLastDurableOpTimeForward(const OpTime& opTime) {
    if (opTime > _myLastDurableOpTime) {
        _myLastDurableOpTime = opTime;
    }
}

void ReplicationCoordinatorMock::resetMyLastOpTimes() {
    _myLastDurableOpTime = OpTime();
}

OpTime ReplicationCoordinatorMock::getMyLastAppliedOpTime() const {
    return _myLastAppliedOpTime;
}

OpTime ReplicationCoordinatorMock::getMyLastDurableOpTime() const {
    return _myLastDurableOpTime;
}

Status ReplicationCoordinatorMock::waitUntilOpTimeForRead(OperationContext* txn,
                                                          const ReadConcernArgs& settings) {
    return Status::OK();
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
    _memberState = newState;
    return true;
}

bool ReplicationCoordinatorMock::isWaitingForApplierToDrain() {
    return false;
}

void ReplicationCoordinatorMock::signalDrainComplete(OperationContext*) {}

Status ReplicationCoordinatorMock::waitForDrainFinish(Milliseconds timeout) {
    invariant(false);
    return Status::OK();
}

void ReplicationCoordinatorMock::signalUpstreamUpdater() {}

StatusWith<BSONObj> ReplicationCoordinatorMock::prepareReplSetUpdatePositionCommand(
    ReplicationCoordinator::ReplSetUpdatePositionCommandStyle commandStyle) const {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("replSetUpdatePosition", 1);
    return cmdBuilder.obj();
}

ReplicaSetConfig ReplicationCoordinatorMock::getConfig() const {
    return _getConfigReturnValue;
}

void ReplicationCoordinatorMock::setGetConfigReturnValue(ReplicaSetConfig returnValue) {
    _getConfigReturnValue = std::move(returnValue);
}

void ReplicationCoordinatorMock::processReplSetGetConfig(BSONObjBuilder* result) {
    // TODO
}

void ReplicationCoordinatorMock::processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) {}

void ReplicationCoordinatorMock::cancelAndRescheduleElectionTimeout() {}

Status ReplicationCoordinatorMock::processReplSetGetStatus(BSONObjBuilder* result) {
    return Status::OK();
}

void ReplicationCoordinatorMock::fillIsMasterForReplSet(IsMasterResponse* result) {}

void ReplicationCoordinatorMock::appendSlaveInfoData(BSONObjBuilder* result) {}

void ReplicationCoordinatorMock::appendConnectionStats(executor::ConnectionPoolStats* stats) const {
}

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
    const OldUpdatePositionArgs& updates, long long* configVersion) {
    // TODO
    return Status::OK();
}

Status ReplicationCoordinatorMock::processReplSetUpdatePosition(const UpdatePositionArgs& updates,
                                                                long long* configVersion) {
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

std::vector<HostAndPort> ReplicationCoordinatorMock::getHostsWrittenTo(const OpTime& op,
                                                                       bool durablyWritten) {
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

HostAndPort ReplicationCoordinatorMock::chooseNewSyncSource(const Timestamp& lastTimestampFetched) {
    return HostAndPort();
}

void ReplicationCoordinatorMock::blacklistSyncSource(const HostAndPort& host, Date_t until) {}

void ReplicationCoordinatorMock::resetLastOpTimesFromOplog(OperationContext* txn) {
    invariant(false);
}

bool ReplicationCoordinatorMock::shouldChangeSyncSource(const HostAndPort& currentSource,
                                                        const rpc::ReplSetMetadata& metadata) {
    invariant(false);
}

SyncSourceResolverResponse ReplicationCoordinatorMock::selectSyncSource(
    OperationContext* txn, const OpTime& lastOpTimeFetched) {
    return SyncSourceResolverResponse();
}

OpTime ReplicationCoordinatorMock::getLastCommittedOpTime() const {
    return OpTime();
}

Status ReplicationCoordinatorMock::processReplSetRequestVotes(
    OperationContext* txn,
    const ReplSetRequestVotesArgs& args,
    ReplSetRequestVotesResponse* response) {
    return Status::OK();
}

void ReplicationCoordinatorMock::prepareReplMetadata(const OpTime& lastOpTimeFromClient,
                                                     BSONObjBuilder* builder) const {}

Status ReplicationCoordinatorMock::processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                                      ReplSetHeartbeatResponse* response) {
    return Status::OK();
}

bool ReplicationCoordinatorMock::isV1ElectionProtocol() const {
    return true;
}

bool ReplicationCoordinatorMock::getWriteConcernMajorityShouldJournal() {
    return true;
}

void ReplicationCoordinatorMock::summarizeAsHtml(ReplSetHtmlSummary* output) {}

long long ReplicationCoordinatorMock::getTerm() {
    return OpTime::kInitialTerm;
}

Status ReplicationCoordinatorMock::updateTerm(OperationContext* txn, long long term) {
    return Status::OK();
}

SnapshotName ReplicationCoordinatorMock::reserveSnapshotName(OperationContext* txn) {
    return SnapshotName(_snapshotNameGenerator.addAndFetch(1));
}

void ReplicationCoordinatorMock::forceSnapshotCreation() {}

void ReplicationCoordinatorMock::onSnapshotCreate(OpTime timeOfSnapshot, SnapshotName name) {}

void ReplicationCoordinatorMock::dropAllSnapshots() {}

OpTime ReplicationCoordinatorMock::getCurrentCommittedSnapshotOpTime() const {
    return OpTime();
}

void ReplicationCoordinatorMock::waitUntilSnapshotCommitted(OperationContext* txn,
                                                            const SnapshotName& untilSnapshot) {}

size_t ReplicationCoordinatorMock::getNumUncommittedSnapshots() {
    return 0;
}

WriteConcernOptions ReplicationCoordinatorMock::populateUnsetWriteConcernOptionsSyncMode(
    WriteConcernOptions wc) {
    if (wc.syncMode == WriteConcernOptions::SyncMode::UNSET) {
        if (wc.wMode == WriteConcernOptions::kMajority) {
            wc.syncMode = WriteConcernOptions::SyncMode::JOURNAL;
        } else {
            wc.syncMode = WriteConcernOptions::SyncMode::NONE;
        }
    }
    return wc;
}

bool ReplicationCoordinatorMock::getInitialSyncRequestedFlag() const {
    return false;
}

void ReplicationCoordinatorMock::setInitialSyncRequestedFlag(bool value) {}

ReplSettings::IndexPrefetchConfig ReplicationCoordinatorMock::getIndexPrefetchConfig() const {
    return ReplSettings::IndexPrefetchConfig();
}

void ReplicationCoordinatorMock::setIndexPrefetchConfig(
    const ReplSettings::IndexPrefetchConfig cfg) {}

Status ReplicationCoordinatorMock::stepUpIfEligible() {
    return Status::OK();
}

}  // namespace repl
}  // namespace mongo

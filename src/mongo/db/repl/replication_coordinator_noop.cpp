/**
 *    Copyright (C) 2019 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "replication_coordinator_noop.h"

namespace mongo {
namespace repl {

ReplicationCoordinatorNoOp::ReplicationCoordinatorNoOp(ServiceContext* service)
    : _service(service) {}

void ReplicationCoordinatorNoOp::startup(OperationContext* opCtx) {}

void ReplicationCoordinatorNoOp::enterTerminalShutdown() {}

void ReplicationCoordinatorNoOp::shutdown(OperationContext* opCtx) {}

ReplicationCoordinator::Mode ReplicationCoordinatorNoOp::getReplicationMode() const {
    return modeReplSet;
}

bool ReplicationCoordinatorNoOp::isReplEnabled() const {
    return getReplicationMode() == modeReplSet;
}

MemberState ReplicationCoordinatorNoOp::getMemberState() const {
    return MemberState::RS_PRIMARY;
}

OpTime ReplicationCoordinatorNoOp::getMyLastAppliedOpTime() const {
    return OpTime{};
}

const ReplSettings& ReplicationCoordinatorNoOp::getSettings() const {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::isMasterForReportingPurposes() {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::canAcceptWritesForDatabase(OperationContext* opCtx,
                                                            StringData dbName) {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                                                   StringData dbName) {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                                           const NamespaceString& ns) {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::canAcceptWritesFor(OperationContext* opCtx,
                                                    const NamespaceString& ns) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                                const NamespaceString& ns,
                                                                bool slaveOk) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::checkCanServeReadsFor(OperationContext* opCtx,
                                                         const NamespaceString& ns,
                                                         bool slaveOk) {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::isInPrimaryOrSecondaryState_UNSAFE() const {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::isInPrimaryOrSecondaryState(OperationContext* opCtx) const {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::shouldRelaxIndexConstraints(OperationContext* opCtx,
                                                             const NamespaceString& ns) {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::getMaintenanceMode() {
    MONGO_UNREACHABLE;
}

WriteConcernOptions ReplicationCoordinatorNoOp::getGetLastErrorDefault() {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::buildsIndexes() {
    MONGO_UNREACHABLE;
}

OpTimeAndWallTime ReplicationCoordinatorNoOp::getMyLastAppliedOpTimeAndWallTime() const {
    MONGO_UNREACHABLE;
}

WriteConcernOptions ReplicationCoordinatorNoOp::populateUnsetWriteConcernOptionsSyncMode(
    WriteConcernOptions wc) {
    MONGO_UNREACHABLE;
}

OpTime ReplicationCoordinatorNoOp::getCurrentCommittedSnapshotOpTime() const {
    MONGO_UNREACHABLE;
}

OpTimeAndWallTime ReplicationCoordinatorNoOp::getCurrentCommittedSnapshotOpTimeAndWallTime() const {
    MONGO_UNREACHABLE;
}
void ReplicationCoordinatorNoOp::appendDiagnosticBSON(mongo::BSONObjBuilder*) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::appendConnectionStats(executor::ConnectionPoolStats* stats) const {
    MONGO_UNREACHABLE;
}

std::vector<repl::MemberData> ReplicationCoordinatorNoOp::getMemberData() const {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::canAcceptNonLocalWrites() const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::waitForMemberState(MemberState, Milliseconds) {
    MONGO_UNREACHABLE;
}

Seconds ReplicationCoordinatorNoOp::getSlaveDelaySecs() const {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::clearSyncSourceBlacklist() {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::setFollowerMode(const MemberState&) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::setFollowerModeStrict(OperationContext* opCtx,
                                                         const MemberState&) {
    MONGO_UNREACHABLE;
}

ReplicationCoordinator::ApplierState ReplicationCoordinatorNoOp::getApplierState() {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::signalDrainComplete(OperationContext*, long long) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::waitForDrainFinish(Milliseconds) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::signalUpstreamUpdater() {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::setMyHeartbeatMessage(const std::string&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::setMyLastAppliedOpTimeAndWallTimeForward(const OpTimeAndWallTime&,
                                                                          DataConsistency) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::setMyLastDurableOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::setMyLastAppliedOpTimeAndWallTime(const OpTimeAndWallTime&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::setMyLastDurableOpTimeAndWallTime(const OpTimeAndWallTime&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::resetMyLastOpTimes() {
    MONGO_UNREACHABLE;
}

OpTimeAndWallTime ReplicationCoordinatorNoOp::getMyLastDurableOpTimeAndWallTime() const {
    MONGO_UNREACHABLE;
}

OpTime ReplicationCoordinatorNoOp::getMyLastDurableOpTime() const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::waitUntilOpTimeForRead(OperationContext*,
                                                          const ReadConcernArgs& readConcern) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::waitUntilOpTimeForReadUntil(OperationContext*,
                                                               const ReadConcernArgs&,
                                                               boost::optional<Date_t>) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) {
    MONGO_UNREACHABLE;
}

ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorNoOp::awaitReplication(
    OperationContext*, const OpTime&, const WriteConcernOptions&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::stepDown(OperationContext*,
                                          const bool,
                                          const Milliseconds&,
                                          const Milliseconds&) {
    MONGO_UNREACHABLE;
}

OID ReplicationCoordinatorNoOp::getElectionId() {
    MONGO_UNREACHABLE;
}

int ReplicationCoordinatorNoOp::getMyId() const {
    MONGO_UNREACHABLE;
}

HostAndPort ReplicationCoordinatorNoOp::getMyHostAndPort() const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::resyncData(OperationContext*, bool) {
    MONGO_UNREACHABLE;
}

StatusWith<BSONObj> ReplicationCoordinatorNoOp::prepareReplSetUpdatePositionCommand() const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::processReplSetGetStatus(BSONObjBuilder*,
                                                           ReplSetGetStatusResponseStyle) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::fillIsMasterForReplSet(IsMasterResponse*,
                                                        const SplitHorizon::Parameters&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::appendSlaveInfoData(BSONObjBuilder*) {
    MONGO_UNREACHABLE;
}

ReplSetConfig ReplicationCoordinatorNoOp::getConfig() const {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::processReplSetGetConfig(BSONObjBuilder*) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::processReplSetMetadata(const rpc::ReplSetMetadata&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::cancelAndRescheduleElectionTimeout() {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::setMaintenanceMode(OperationContext*, bool) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::processReplSetSyncFrom(OperationContext*,
                                                          const HostAndPort&,
                                                          BSONObjBuilder*) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::processReplSetFreeze(int, BSONObjBuilder*) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::processReplSetReconfig(OperationContext*,
                                                          const ReplSetReconfigArgs&,
                                                          BSONObjBuilder*) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::processReplSetInitiate(OperationContext*,
                                                          const BSONObj&,
                                                          BSONObjBuilder*) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::incrementNumCatchUpOpsIfCatchingUp(long numOps) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::processReplSetUpdatePosition(const UpdatePositionArgs&,
                                                                long long*) {
    MONGO_UNREACHABLE;
}

std::vector<HostAndPort> ReplicationCoordinatorNoOp::getHostsWrittenTo(const OpTime&, bool) {
    MONGO_UNREACHABLE;
}

std::vector<HostAndPort> ReplicationCoordinatorNoOp::getOtherNodesInReplSet() const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::checkIfWriteConcernCanBeSatisfied(
    const WriteConcernOptions&) const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::checkIfCommitQuorumCanBeSatisfied(
    const CommitQuorumOptions& commitQuorum) const {
    MONGO_UNREACHABLE;
}

StatusWith<bool> ReplicationCoordinatorNoOp::checkIfCommitQuorumIsSatisfied(
    const CommitQuorumOptions& commitQuorum,
    const std::vector<HostAndPort>& commitReadyMembers) const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::checkReplEnabledForCommand(BSONObjBuilder*) {
    return Status(ErrorCodes::NoReplicationEnabled, "no replication on embedded");
}

HostAndPort ReplicationCoordinatorNoOp::chooseNewSyncSource(const OpTime&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::blacklistSyncSource(const HostAndPort&, Date_t) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::resetLastOpTimesFromOplog(OperationContext*, DataConsistency) {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::shouldChangeSyncSource(const HostAndPort&,
                                                        const rpc::ReplSetMetadata&,
                                                        boost::optional<rpc::OplogQueryMetadata>) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::advanceCommitPoint(const OpTimeAndWallTime&, bool fromSyncSource) {
    MONGO_UNREACHABLE;
}

OpTime ReplicationCoordinatorNoOp::getLastCommittedOpTime() const {
    MONGO_UNREACHABLE;
}

OpTimeAndWallTime ReplicationCoordinatorNoOp::getLastCommittedOpTimeAndWallTime() const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::processReplSetRequestVotes(OperationContext*,
                                                              const ReplSetRequestVotesArgs&,
                                                              ReplSetRequestVotesResponse*) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::prepareReplMetadata(const BSONObj&,
                                                     const OpTime&,
                                                     BSONObjBuilder*) const {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::getWriteConcernMajorityShouldJournal() {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::processHeartbeatV1(const ReplSetHeartbeatArgsV1&,
                                                      ReplSetHeartbeatResponse*) {
    MONGO_UNREACHABLE;
}

long long ReplicationCoordinatorNoOp::getTerm() {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::updateTerm(OperationContext*, long long) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::waitUntilSnapshotCommitted(OperationContext*, const Timestamp&) {
    MONGO_UNREACHABLE;
}

size_t ReplicationCoordinatorNoOp::getNumUncommittedSnapshots() {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::dropAllSnapshots() {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::stepUpIfEligible(bool skipDryRun) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::signalDropPendingCollectionsRemovedFromStorage() {
    MONGO_UNREACHABLE;
}

boost::optional<Timestamp> ReplicationCoordinatorNoOp::getRecoveryTimestamp() {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::setContainsArbiter() const {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::attemptToAdvanceStableTimestamp() {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::updateAndLogStateTransitionMetrics(
    const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
    const size_t numOpsKilled,
    const size_t numOpsRunning) const {
    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo

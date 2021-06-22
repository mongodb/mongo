/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/embedded/replication_coordinator_embedded.h"

#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/embedded/not_implemented.h"

namespace mongo {
namespace embedded {

using namespace repl;

ReplicationCoordinatorEmbedded::ReplicationCoordinatorEmbedded(ServiceContext* service)
    : _service(service) {}

ReplicationCoordinatorEmbedded::~ReplicationCoordinatorEmbedded() = default;

void ReplicationCoordinatorEmbedded::startup(OperationContext* opCtx) {}

void ReplicationCoordinatorEmbedded::enterTerminalShutdown() {}

void ReplicationCoordinatorEmbedded::shutdown(OperationContext* opCtx) {}

const ReplSettings& ReplicationCoordinatorEmbedded::getSettings() const {
    static ReplSettings _settings;
    return _settings;
}

ReplicationCoordinator::Mode ReplicationCoordinatorEmbedded::getReplicationMode() const {
    return ReplicationCoordinator::Mode::modeNone;
}

bool ReplicationCoordinatorEmbedded::isMasterForReportingPurposes() {
    return true;
}

bool ReplicationCoordinatorEmbedded::canAcceptWritesForDatabase(OperationContext* opCtx,
                                                                StringData dbName) {
    return true;
}

bool ReplicationCoordinatorEmbedded::canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                                                       StringData dbName) {
    return true;
}

bool ReplicationCoordinatorEmbedded::canAcceptWritesFor(OperationContext* opCtx,
                                                        const NamespaceString& ns) {
    return true;
}

bool ReplicationCoordinatorEmbedded::canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                                               const NamespaceString& ns) {
    return true;
}

Status ReplicationCoordinatorEmbedded::checkCanServeReadsFor(OperationContext* opCtx,
                                                             const NamespaceString& ns,
                                                             bool slaveOk) {
    return Status::OK();
}

Status ReplicationCoordinatorEmbedded::checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                                    const NamespaceString& ns,
                                                                    bool slaveOk) {
    return Status::OK();
}

bool ReplicationCoordinatorEmbedded::isInPrimaryOrSecondaryState(OperationContext* opCtx) const {
    return false;
}

bool ReplicationCoordinatorEmbedded::isInPrimaryOrSecondaryState_UNSAFE() const {
    return false;
}

bool ReplicationCoordinatorEmbedded::shouldRelaxIndexConstraints(OperationContext* opCtx,
                                                                 const NamespaceString& ns) {
    return false;
}

bool ReplicationCoordinatorEmbedded::getMaintenanceMode() {
    return false;
}

WriteConcernOptions ReplicationCoordinatorEmbedded::getGetLastErrorDefault() {
    return WriteConcernOptions();
}

bool ReplicationCoordinatorEmbedded::isReplEnabled() const {
    return false;
}

WriteConcernOptions ReplicationCoordinatorEmbedded::populateUnsetWriteConcernOptionsSyncMode(
    WriteConcernOptions wc) {
    WriteConcernOptions writeConcern(wc);
    writeConcern.syncMode = WriteConcernOptions::SyncMode::NONE;
    return writeConcern;
}

bool ReplicationCoordinatorEmbedded::buildsIndexes() {
    return true;
}

OpTime ReplicationCoordinatorEmbedded::getCurrentCommittedSnapshotOpTime() const {
    UASSERT_NOT_IMPLEMENTED;
}

OpTimeAndWallTime ReplicationCoordinatorEmbedded::getCurrentCommittedSnapshotOpTimeAndWallTime()
    const {
    UASSERT_NOT_IMPLEMENTED;
}
void ReplicationCoordinatorEmbedded::appendDiagnosticBSON(mongo::BSONObjBuilder*) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::appendConnectionStats(
    executor::ConnectionPoolStats* stats) const {
    UASSERT_NOT_IMPLEMENTED;
}

MemberState ReplicationCoordinatorEmbedded::getMemberState() const {
    UASSERT_NOT_IMPLEMENTED;
}

std::vector<repl::MemberData> ReplicationCoordinatorEmbedded::getMemberData() const {
    UASSERT_NOT_IMPLEMENTED;
}

bool ReplicationCoordinatorEmbedded::canAcceptNonLocalWrites() const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::waitForMemberState(MemberState, Milliseconds) {
    UASSERT_NOT_IMPLEMENTED;
}

Seconds ReplicationCoordinatorEmbedded::getSlaveDelaySecs() const {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::clearSyncSourceBlacklist() {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::setFollowerMode(const MemberState&) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::setFollowerModeStrict(OperationContext* opCtx,
                                                             const MemberState&) {
    UASSERT_NOT_IMPLEMENTED;
}

ReplicationCoordinator::ApplierState ReplicationCoordinatorEmbedded::getApplierState() {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::signalDrainComplete(OperationContext*, long long) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::waitForDrainFinish(Milliseconds) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::signalUpstreamUpdater() {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::setMyHeartbeatMessage(const std::string&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::setMyLastAppliedOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&, DataConsistency) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::setMyLastDurableOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::setMyLastAppliedOpTimeAndWallTime(const OpTimeAndWallTime&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::setMyLastDurableOpTimeAndWallTime(const OpTimeAndWallTime&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::resetMyLastOpTimes() {
    UASSERT_NOT_IMPLEMENTED;
}

OpTimeAndWallTime ReplicationCoordinatorEmbedded::getMyLastAppliedOpTimeAndWallTime() const {
    UASSERT_NOT_IMPLEMENTED;
}

OpTime ReplicationCoordinatorEmbedded::getMyLastAppliedOpTime() const {
    UASSERT_NOT_IMPLEMENTED;
}

OpTimeAndWallTime ReplicationCoordinatorEmbedded::getMyLastDurableOpTimeAndWallTime() const {
    UASSERT_NOT_IMPLEMENTED;
}

OpTime ReplicationCoordinatorEmbedded::getMyLastDurableOpTime() const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::waitUntilOpTimeForRead(OperationContext*,
                                                              const ReadConcernArgs& readConcern) {
    // nothing to wait for
    auto level = readConcern.getLevel();
    if ((level == ReadConcernLevel::kLocalReadConcern ||
         level == ReadConcernLevel::kAvailableReadConcern) &&
        (!readConcern.getArgsAfterClusterTime() && !readConcern.getArgsOpTime() &&
         !readConcern.getArgsAtClusterTime())) {
        return Status::OK();
    }

    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::waitUntilOpTimeForReadUntil(OperationContext*,
                                                                   const ReadConcernArgs&,
                                                                   boost::optional<Date_t>) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::awaitTimestampCommitted(OperationContext* opCtx,
                                                               Timestamp ts) {
    UASSERT_NOT_IMPLEMENTED;
}

ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorEmbedded::awaitReplication(
    OperationContext*, const OpTime&, const WriteConcernOptions&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::stepDown(OperationContext*,
                                              const bool,
                                              const Milliseconds&,
                                              const Milliseconds&) {
    UASSERT_NOT_IMPLEMENTED;
}

OID ReplicationCoordinatorEmbedded::getElectionId() {
    UASSERT_NOT_IMPLEMENTED;
}

int ReplicationCoordinatorEmbedded::getMyId() const {
    UASSERT_NOT_IMPLEMENTED;
}

HostAndPort ReplicationCoordinatorEmbedded::getMyHostAndPort() const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::resyncData(OperationContext*, bool) {
    UASSERT_NOT_IMPLEMENTED;
}

StatusWith<BSONObj> ReplicationCoordinatorEmbedded::prepareReplSetUpdatePositionCommand() const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::processReplSetGetStatus(BSONObjBuilder*,
                                                               ReplSetGetStatusResponseStyle) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::fillIsMasterForReplSet(IsMasterResponse*,
                                                            const SplitHorizon::Parameters&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::appendSlaveInfoData(BSONObjBuilder*) {
    UASSERT_NOT_IMPLEMENTED;
}

ReplSetConfig ReplicationCoordinatorEmbedded::getConfig() const {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::processReplSetGetConfig(BSONObjBuilder*) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::processReplSetMetadata(const rpc::ReplSetMetadata&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::cancelAndRescheduleElectionTimeout() {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::setMaintenanceMode(OperationContext*, bool) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::processReplSetSyncFrom(OperationContext*,
                                                              const HostAndPort&,
                                                              BSONObjBuilder*) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::processReplSetFreeze(int, BSONObjBuilder*) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::processReplSetReconfig(OperationContext*,
                                                              const ReplSetReconfigArgs&,
                                                              BSONObjBuilder*) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::processReplSetInitiate(OperationContext*,
                                                              const BSONObj&,
                                                              BSONObjBuilder*) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::incrementNumCatchUpOpsIfCatchingUp(long numOps) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::processReplSetUpdatePosition(const UpdatePositionArgs&,
                                                                    long long*) {
    UASSERT_NOT_IMPLEMENTED;
}

std::vector<HostAndPort> ReplicationCoordinatorEmbedded::getHostsWrittenTo(const OpTime&, bool) {
    UASSERT_NOT_IMPLEMENTED;
}

std::vector<HostAndPort> ReplicationCoordinatorEmbedded::getOtherNodesInReplSet() const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::checkIfWriteConcernCanBeSatisfied(
    const WriteConcernOptions&) const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::checkIfCommitQuorumCanBeSatisfied(
    const CommitQuorumOptions& commitQuorum) const {
    UASSERT_NOT_IMPLEMENTED;
}

StatusWith<bool> ReplicationCoordinatorEmbedded::checkIfCommitQuorumIsSatisfied(
    const CommitQuorumOptions& commitQuorum,
    const std::vector<HostAndPort>& commitReadyMembers) const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::checkReplEnabledForCommand(BSONObjBuilder*) {
    return Status(ErrorCodes::NoReplicationEnabled, "no replication on embedded");
}

HostAndPort ReplicationCoordinatorEmbedded::chooseNewSyncSource(const OpTime&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::blacklistSyncSource(const HostAndPort&, Date_t) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::resetLastOpTimesFromOplog(OperationContext*, DataConsistency) {
    UASSERT_NOT_IMPLEMENTED;
}

bool ReplicationCoordinatorEmbedded::shouldChangeSyncSource(
    const HostAndPort&, const rpc::ReplSetMetadata&, boost::optional<rpc::OplogQueryMetadata>) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::advanceCommitPoint(const OpTimeAndWallTime&,
                                                        bool fromSyncSource) {
    UASSERT_NOT_IMPLEMENTED;
}

OpTime ReplicationCoordinatorEmbedded::getLastCommittedOpTime() const {
    UASSERT_NOT_IMPLEMENTED;
}

OpTimeAndWallTime ReplicationCoordinatorEmbedded::getLastCommittedOpTimeAndWallTime() const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::processReplSetRequestVotes(OperationContext*,
                                                                  const ReplSetRequestVotesArgs&,
                                                                  ReplSetRequestVotesResponse*) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::prepareReplMetadata(const BSONObj&,
                                                         const OpTime&,
                                                         BSONObjBuilder*) const {
    UASSERT_NOT_IMPLEMENTED;
}

bool ReplicationCoordinatorEmbedded::getWriteConcernMajorityShouldJournal() {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::processHeartbeatV1(const ReplSetHeartbeatArgsV1&,
                                                          ReplSetHeartbeatResponse*) {
    UASSERT_NOT_IMPLEMENTED;
}

long long ReplicationCoordinatorEmbedded::getTerm() {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::updateTerm(OperationContext*, long long) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::waitUntilSnapshotCommitted(OperationContext*,
                                                                const Timestamp&) {
    UASSERT_NOT_IMPLEMENTED;
}

size_t ReplicationCoordinatorEmbedded::getNumUncommittedSnapshots() {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::dropAllSnapshots() {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::stepUpIfEligible(bool skipDryRun) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::signalDropPendingCollectionsRemovedFromStorage() {
    UASSERT_NOT_IMPLEMENTED;
}

boost::optional<Timestamp> ReplicationCoordinatorEmbedded::getRecoveryTimestamp() {
    UASSERT_NOT_IMPLEMENTED;
}

bool ReplicationCoordinatorEmbedded::setContainsArbiter() const {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::attemptToAdvanceStableTimestamp() {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::updateAndLogStateTransitionMetrics(
    const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
    const size_t numOpsKilled,
    const size_t numOpsRunning) const {
    UASSERT_NOT_IMPLEMENTED;
}

}  // namespace embedded
}  // namespace mongo

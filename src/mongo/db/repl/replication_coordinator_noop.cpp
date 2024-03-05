/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "replication_coordinator_noop.h"

#include <boost/move/utility_core.hpp>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

ReplSettings makeDefaultReplSettings() {
    ReplSettings settings;
    settings.setReplSetString("ReplicationCoordinatorNoOpDefaultSetName");
    return settings;
}

ReplicationCoordinatorNoOp::ReplicationCoordinatorNoOp(ServiceContext* service)
    : _service(service), _settings(makeDefaultReplSettings()) {}

void ReplicationCoordinatorNoOp::startup(OperationContext* opCtx,
                                         StorageEngine::LastShutdownState lastShutdownState) {}

void ReplicationCoordinatorNoOp::enterTerminalShutdown() {}

bool ReplicationCoordinatorNoOp::enterQuiesceModeIfSecondary(Milliseconds quiesceTime) {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::inQuiesceMode() const {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::shutdown(OperationContext* opCtx,
                                          BSONObjBuilder* shutdownTimeElapsedBuilder) {}

MemberState ReplicationCoordinatorNoOp::getMemberState() const {
    return MemberState::RS_PRIMARY;
}

OpTime ReplicationCoordinatorNoOp::getMyLastAppliedOpTime() const {
    return OpTime{};
}

const ReplSettings& ReplicationCoordinatorNoOp::getSettings() const {
    return _settings;
}

bool ReplicationCoordinatorNoOp::isWritablePrimaryForReportingPurposes() {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::canAcceptWritesForDatabase(OperationContext* opCtx,
                                                            const DatabaseName& dbName) {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                                                   const DatabaseName& dbName) {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                                           const NamespaceStringOrUUID& nsOrUUID) {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::canAcceptWritesFor(OperationContext* opCtx,
                                                    const NamespaceStringOrUUID& nsOrUUID) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                                const NamespaceString& ns,
                                                                bool secondaryOkay) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::checkCanServeReadsFor(OperationContext* opCtx,
                                                         const NamespaceString& ns,
                                                         bool secondaryOkay) {
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

void ReplicationCoordinatorNoOp::appendDiagnosticBSON(mongo::BSONObjBuilder*, StringData) {
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

Status ReplicationCoordinatorNoOp::waitForMemberState(Interruptible*, MemberState, Milliseconds) {
    MONGO_UNREACHABLE;
}

Seconds ReplicationCoordinatorNoOp::getSecondaryDelaySecs() const {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::clearSyncSourceDenylist() {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::setFollowerMode(const MemberState&) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::setFollowerModeRollback(OperationContext* opCtx) {
    MONGO_UNREACHABLE;
}

ReplicationCoordinator::ApplierState ReplicationCoordinatorNoOp::getApplierState() {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::signalDrainComplete(OperationContext*, long long) noexcept {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::signalUpstreamUpdater() {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::setMyHeartbeatMessage(const std::string&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::setMyLastWrittenOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::setMyLastAppliedOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::setMyLastDurableOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::setMyLastAppliedAndLastWrittenOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::setMyLastDurableAndLastWrittenOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
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

Status ReplicationCoordinatorNoOp::waitUntilMajorityOpTime(OperationContext* opCtx,
                                                           OpTime targetOpTime,
                                                           boost::optional<Date_t> deadline) {
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

Status ReplicationCoordinatorNoOp::waitUntilOpTimeWrittenUntil(OperationContext*,
                                                               LogicalTime,
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

SharedSemiFuture<void> ReplicationCoordinatorNoOp::awaitReplicationAsyncNoWTimeout(
    const OpTime& opTime, const WriteConcernOptions& writeConcern) {
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

StatusWith<BSONObj> ReplicationCoordinatorNoOp::prepareReplSetUpdatePositionCommand() const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::processReplSetGetStatus(OperationContext* opCtx,
                                                           BSONObjBuilder*,
                                                           ReplSetGetStatusResponseStyle) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::appendSecondaryInfoData(BSONObjBuilder*) {
    MONGO_UNREACHABLE;
}

ReplSetConfig ReplicationCoordinatorNoOp::getConfig() const {
    MONGO_UNREACHABLE;
}

ConnectionString ReplicationCoordinatorNoOp::getConfigConnectionString() const {
    MONGO_UNREACHABLE;
}

Milliseconds ReplicationCoordinatorNoOp::getConfigElectionTimeoutPeriod() const {
    MONGO_UNREACHABLE;
}

std::vector<MemberConfig> ReplicationCoordinatorNoOp::getConfigVotingMembers() const {
    MONGO_UNREACHABLE;
}

size_t ReplicationCoordinatorNoOp::getNumConfigVotingMembers() const {
    MONGO_UNREACHABLE;
}

std::int64_t ReplicationCoordinatorNoOp::getConfigTerm() const {
    MONGO_UNREACHABLE;
}

std::int64_t ReplicationCoordinatorNoOp::getConfigVersion() const {
    MONGO_UNREACHABLE;
}

ConfigVersionAndTerm ReplicationCoordinatorNoOp::getConfigVersionAndTerm() const {
    MONGO_UNREACHABLE;
}

int ReplicationCoordinatorNoOp::getConfigNumMembers() const {
    MONGO_UNREACHABLE;
}

Milliseconds ReplicationCoordinatorNoOp::getConfigHeartbeatTimeoutPeriodMillis() const {
    MONGO_UNREACHABLE;
}

BSONObj ReplicationCoordinatorNoOp::getConfigBSON() const {
    MONGO_UNREACHABLE;
}

boost::optional<MemberConfig> ReplicationCoordinatorNoOp::findConfigMemberByHostAndPort_deprecated(
    const HostAndPort& hap) const {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::isConfigLocalHostAllowed() const {
    MONGO_UNREACHABLE;
}

Milliseconds ReplicationCoordinatorNoOp::getConfigHeartbeatInterval() const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::validateWriteConcern(
    const WriteConcernOptions& writeConcern) const {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::processReplSetGetConfig(BSONObjBuilder* result,
                                                         bool commitmentStatus,
                                                         bool includeNewlyAdded) {
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

ChangeSyncSourceAction ReplicationCoordinatorNoOp::shouldChangeSyncSourceOnError(
    const HostAndPort&, const OpTime&) const {
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

Status ReplicationCoordinatorNoOp::doReplSetReconfig(OperationContext* opCtx,
                                                     GetNewConfigFn getNewConfig,
                                                     bool force) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::doOptimizedReconfig(OperationContext* opCtx,
                                                       GetNewConfigFn getNewConfig) {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::awaitConfigCommitment(OperationContext* opCtx,
                                                         bool waitForOplogCommitment,
                                                         long long term) {
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

Status ReplicationCoordinatorNoOp::processReplSetUpdatePosition(const UpdatePositionArgs&) {
    MONGO_UNREACHABLE;
}

std::vector<HostAndPort> ReplicationCoordinatorNoOp::getHostsWrittenTo(const OpTime&, bool) {
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

bool ReplicationCoordinatorNoOp::isCommitQuorumSatisfied(
    const CommitQuorumOptions& commitQuorum, const std::vector<mongo::HostAndPort>& members) const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::checkReplEnabledForCommand(BSONObjBuilder*) {
    return Status(ErrorCodes::NoReplicationEnabled, "no replication on embedded");
}

HostAndPort ReplicationCoordinatorNoOp::chooseNewSyncSource(const OpTime&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::denylistSyncSource(const HostAndPort&, Date_t) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::resetLastOpTimesFromOplog(OperationContext*) {
    MONGO_UNREACHABLE;
}

ChangeSyncSourceAction ReplicationCoordinatorNoOp::shouldChangeSyncSource(
    const HostAndPort&,
    const rpc::ReplSetMetadata&,
    const rpc::OplogQueryMetadata&,
    const OpTime&,
    const OpTime&) const {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::shouldDropSyncSourceAfterShardSplit(OID replicaSetId) const {
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

long long ReplicationCoordinatorNoOp::getTerm() const {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::updateTerm(OperationContext*, long long) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::waitUntilSnapshotCommitted(OperationContext*, const Timestamp&) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::clearCommittedSnapshot() {
    MONGO_UNREACHABLE;
}

Status ReplicationCoordinatorNoOp::stepUpIfEligible(bool skipDryRun) {
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

void ReplicationCoordinatorNoOp::finishRecoveryIfEligible(OperationContext* opCtx) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::updateAndLogStateTransitionMetrics(
    const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
    const size_t numOpsKilled,
    const size_t numOpsRunning) const {
    MONGO_UNREACHABLE;
}

TopologyVersion ReplicationCoordinatorNoOp::getTopologyVersion() const {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::incrementTopologyVersion() {
    MONGO_UNREACHABLE;
}

std::shared_ptr<const HelloResponse> ReplicationCoordinatorNoOp::awaitHelloResponse(
    OperationContext* opCtx,
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion,
    boost::optional<Date_t> deadline) {
    MONGO_UNREACHABLE;
}


SharedSemiFuture<std::shared_ptr<const HelloResponse>>
ReplicationCoordinatorNoOp::getHelloResponseFuture(
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion) {
    MONGO_UNREACHABLE;
}

StatusWith<OpTime> ReplicationCoordinatorNoOp::getLatestWriteOpTime(
    OperationContext* opCtx) const noexcept {
    return getMyLastAppliedOpTime();
}

HostAndPort ReplicationCoordinatorNoOp::getCurrentPrimaryHostAndPort() const {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::cancelCbkHandle(
    executor::TaskExecutor::CallbackHandle activeHandle) {
    MONGO_UNREACHABLE;
}

BSONObj ReplicationCoordinatorNoOp::runCmdOnPrimaryAndAwaitResponse(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    OnRemoteCmdScheduledFn onRemoteCmdScheduled,
    OnRemoteCmdCompleteFn onRemoteCmdComplete) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::restartScheduledHeartbeats_forTest() {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorNoOp::recordIfCWWCIsSetOnConfigServerOnStartup(OperationContext* opCtx) {
    MONGO_UNREACHABLE;
}

ReplicationCoordinatorNoOp::WriteConcernTagChanges*
ReplicationCoordinatorNoOp::getWriteConcernTagChanges() {
    MONGO_UNREACHABLE;
}

SplitPrepareSessionManager* ReplicationCoordinatorNoOp::getSplitPrepareSessionManager() {
    MONGO_UNREACHABLE;
}

bool ReplicationCoordinatorNoOp::isRetryableWrite(OperationContext* opCtx) const {
    MONGO_UNREACHABLE;
}

boost::optional<UUID> ReplicationCoordinatorNoOp::getInitialSyncId(OperationContext* opCtx) {
    MONGO_UNREACHABLE;
}

OpTime ReplicationCoordinatorNoOp::getMyLastWrittenOpTime() const {
    MONGO_UNREACHABLE;
}

OpTimeAndWallTime ReplicationCoordinatorNoOp::getMyLastWrittenOpTimeAndWallTime(
    bool rollbackSafe) const {
    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo

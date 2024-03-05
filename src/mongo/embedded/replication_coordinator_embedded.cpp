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


#include <boost/move/utility_core.hpp>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/embedded/not_implemented.h"
#include "mongo/embedded/replication_coordinator_embedded.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace embedded {

using namespace repl;

ReplicationCoordinatorEmbedded::ReplicationCoordinatorEmbedded(ServiceContext* service)
    : _service(service) {}

ReplicationCoordinatorEmbedded::~ReplicationCoordinatorEmbedded() = default;

void ReplicationCoordinatorEmbedded::startup(OperationContext* opCtx,
                                             StorageEngine::LastShutdownState lastShutdownState) {}

void ReplicationCoordinatorEmbedded::enterTerminalShutdown() {}

bool ReplicationCoordinatorEmbedded::enterQuiesceModeIfSecondary(Milliseconds quiesceTime) {
    return true;
}

bool ReplicationCoordinatorEmbedded::inQuiesceMode() const {
    return false;
}

void ReplicationCoordinatorEmbedded::shutdown(OperationContext* opCtx,
                                              BSONObjBuilder* shutdownTimeElapsedBuilder) {}

const ReplSettings& ReplicationCoordinatorEmbedded::getSettings() const {
    static ReplSettings _settings;
    return _settings;
}

bool ReplicationCoordinatorEmbedded::isWritablePrimaryForReportingPurposes() {
    return true;
}

bool ReplicationCoordinatorEmbedded::canAcceptWritesForDatabase(OperationContext* opCtx,
                                                                const DatabaseName& dbName) {
    return true;
}

bool ReplicationCoordinatorEmbedded::canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                                                       const DatabaseName& dbName) {
    return true;
}

bool ReplicationCoordinatorEmbedded::canAcceptWritesFor(OperationContext* opCtx,
                                                        const NamespaceStringOrUUID& nsOrUUID) {
    return true;
}

bool ReplicationCoordinatorEmbedded::canAcceptWritesFor_UNSAFE(
    OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID) {
    return true;
}

Status ReplicationCoordinatorEmbedded::checkCanServeReadsFor(OperationContext* opCtx,
                                                             const NamespaceString& ns,
                                                             bool secondaryOk) {
    return Status::OK();
}

Status ReplicationCoordinatorEmbedded::checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                                    const NamespaceString& ns,
                                                                    bool secondaryOk) {
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

void ReplicationCoordinatorEmbedded::appendDiagnosticBSON(mongo::BSONObjBuilder*, StringData) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::appendConnectionStats(
    executor::ConnectionPoolStats* stats) const {
    UASSERT_NOT_IMPLEMENTED;
}

MemberState ReplicationCoordinatorEmbedded::getMemberState() const {
    return MemberState::RS_PRIMARY;
}

std::vector<repl::MemberData> ReplicationCoordinatorEmbedded::getMemberData() const {
    UASSERT_NOT_IMPLEMENTED;
}

bool ReplicationCoordinatorEmbedded::canAcceptNonLocalWrites() const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::waitForMemberState(Interruptible*,
                                                          MemberState,
                                                          Milliseconds) {
    UASSERT_NOT_IMPLEMENTED;
}

Seconds ReplicationCoordinatorEmbedded::getSecondaryDelaySecs() const {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::clearSyncSourceDenylist() {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::setFollowerMode(const MemberState&) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::setFollowerModeRollback(OperationContext* opCtx) {
    UASSERT_NOT_IMPLEMENTED;
}

ReplicationCoordinator::ApplierState ReplicationCoordinatorEmbedded::getApplierState() {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::signalDrainComplete(OperationContext*, long long) noexcept {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::signalUpstreamUpdater() {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::setMyHeartbeatMessage(const std::string&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::setMyLastWrittenOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::setMyLastAppliedOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::setMyLastDurableOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::setMyLastAppliedAndLastWrittenOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::setMyLastDurableAndLastWrittenOpTimeAndWallTimeForward(
    const OpTimeAndWallTime&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::resetMyLastOpTimes() {
    UASSERT_NOT_IMPLEMENTED;
}

OpTimeAndWallTime ReplicationCoordinatorEmbedded::getMyLastWrittenOpTimeAndWallTime(
    bool rollbackSafe) const {
    UASSERT_NOT_IMPLEMENTED;
}

OpTime ReplicationCoordinatorEmbedded::getMyLastWrittenOpTime() const {
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

Status ReplicationCoordinatorEmbedded::waitUntilMajorityOpTime(OperationContext* opCtx,
                                                               repl::OpTime targetOpTime,
                                                               boost::optional<Date_t> deadline) {
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

Status ReplicationCoordinatorEmbedded::waitUntilOpTimeWrittenUntil(OperationContext*,
                                                                   LogicalTime,
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

SharedSemiFuture<void> ReplicationCoordinatorEmbedded::awaitReplicationAsyncNoWTimeout(
    const OpTime&, const WriteConcernOptions&) {
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

StatusWith<BSONObj> ReplicationCoordinatorEmbedded::prepareReplSetUpdatePositionCommand() const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::processReplSetGetStatus(OperationContext* opCtx,
                                                               BSONObjBuilder*,
                                                               ReplSetGetStatusResponseStyle) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::appendSecondaryInfoData(BSONObjBuilder*) {
    UASSERT_NOT_IMPLEMENTED;
}

ReplSetConfig ReplicationCoordinatorEmbedded::getConfig() const {
    UASSERT_NOT_IMPLEMENTED;
}

ConnectionString ReplicationCoordinatorEmbedded::getConfigConnectionString() const {
    UASSERT_NOT_IMPLEMENTED;
}

Milliseconds ReplicationCoordinatorEmbedded::getConfigElectionTimeoutPeriod() const {
    UASSERT_NOT_IMPLEMENTED;
}

std::vector<MemberConfig> ReplicationCoordinatorEmbedded::getConfigVotingMembers() const {
    UASSERT_NOT_IMPLEMENTED;
}

size_t ReplicationCoordinatorEmbedded::getNumConfigVotingMembers() const {
    UASSERT_NOT_IMPLEMENTED;
}

std::int64_t ReplicationCoordinatorEmbedded::getConfigTerm() const {
    UASSERT_NOT_IMPLEMENTED;
}

std::int64_t ReplicationCoordinatorEmbedded::getConfigVersion() const {
    UASSERT_NOT_IMPLEMENTED;
}

ConfigVersionAndTerm ReplicationCoordinatorEmbedded::getConfigVersionAndTerm() const {
    UASSERT_NOT_IMPLEMENTED;
}

int ReplicationCoordinatorEmbedded::getConfigNumMembers() const {
    UASSERT_NOT_IMPLEMENTED;
}

Milliseconds ReplicationCoordinatorEmbedded::getConfigHeartbeatTimeoutPeriodMillis() const {
    UASSERT_NOT_IMPLEMENTED;
}

BSONObj ReplicationCoordinatorEmbedded::getConfigBSON() const {
    UASSERT_NOT_IMPLEMENTED;
}

boost::optional<MemberConfig>
ReplicationCoordinatorEmbedded::findConfigMemberByHostAndPort_deprecated(
    const HostAndPort& hap) const {
    UASSERT_NOT_IMPLEMENTED;
}

bool ReplicationCoordinatorEmbedded::isConfigLocalHostAllowed() const {
    UASSERT_NOT_IMPLEMENTED;
}

Milliseconds ReplicationCoordinatorEmbedded::getConfigHeartbeatInterval() const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::validateWriteConcern(
    const WriteConcernOptions& writeConcern) const {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::processReplSetGetConfig(BSONObjBuilder*,
                                                             bool commitmentStatus,
                                                             bool includeNewlyAdded) {
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

bool ReplicationCoordinatorEmbedded::shouldDropSyncSourceAfterShardSplit(OID replicaSetId) const {
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

Status ReplicationCoordinatorEmbedded::doReplSetReconfig(OperationContext* opCtx,
                                                         GetNewConfigFn getNewConfig,
                                                         bool force) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::doOptimizedReconfig(OperationContext* opCtx,
                                                           GetNewConfigFn getNewConfig) {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::awaitConfigCommitment(OperationContext* opCtx,
                                                             bool waitForOplogCommitment,
                                                             long long term) {
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

Status ReplicationCoordinatorEmbedded::processReplSetUpdatePosition(const UpdatePositionArgs&) {
    UASSERT_NOT_IMPLEMENTED;
}

std::vector<HostAndPort> ReplicationCoordinatorEmbedded::getHostsWrittenTo(const OpTime&, bool) {
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

bool ReplicationCoordinatorEmbedded::isCommitQuorumSatisfied(
    const CommitQuorumOptions& commitQuorum, const std::vector<mongo::HostAndPort>& members) const {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::checkReplEnabledForCommand(BSONObjBuilder*) {
    return Status(ErrorCodes::NoReplicationEnabled, "no replication on embedded");
}

HostAndPort ReplicationCoordinatorEmbedded::chooseNewSyncSource(const OpTime&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::denylistSyncSource(const HostAndPort&, Date_t) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::resetLastOpTimesFromOplog(OperationContext*) {
    UASSERT_NOT_IMPLEMENTED;
}

ChangeSyncSourceAction ReplicationCoordinatorEmbedded::shouldChangeSyncSource(
    const HostAndPort&,
    const rpc::ReplSetMetadata&,
    const rpc::OplogQueryMetadata&,
    const OpTime&,
    const OpTime&) const {
    UASSERT_NOT_IMPLEMENTED;
}

ChangeSyncSourceAction ReplicationCoordinatorEmbedded::shouldChangeSyncSourceOnError(
    const HostAndPort&, const OpTime&) const {
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

long long ReplicationCoordinatorEmbedded::getTerm() const {
    return 3;  // arbitrary constant number
}

Status ReplicationCoordinatorEmbedded::updateTerm(OperationContext*, long long) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::waitUntilSnapshotCommitted(OperationContext*,
                                                                const Timestamp&) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::clearCommittedSnapshot() {
    UASSERT_NOT_IMPLEMENTED;
}

Status ReplicationCoordinatorEmbedded::stepUpIfEligible(bool skipDryRun) {
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

void ReplicationCoordinatorEmbedded::finishRecoveryIfEligible(OperationContext* opCtx) {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::updateAndLogStateTransitionMetrics(
    const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
    const size_t numOpsKilled,
    const size_t numOpsRunning) const {
    UASSERT_NOT_IMPLEMENTED;
}

TopologyVersion ReplicationCoordinatorEmbedded::getTopologyVersion() const {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::incrementTopologyVersion() {
    UASSERT_NOT_IMPLEMENTED;
}

std::shared_ptr<const repl::HelloResponse> ReplicationCoordinatorEmbedded::awaitHelloResponse(
    OperationContext* opCtx,
    const repl::SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> previous,
    boost::optional<Date_t> deadline) {
    UASSERT_NOT_IMPLEMENTED;
};

SharedSemiFuture<std::shared_ptr<const HelloResponse>>
ReplicationCoordinatorEmbedded::getHelloResponseFuture(
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion) {
    UASSERT_NOT_IMPLEMENTED;
}

StatusWith<OpTime> ReplicationCoordinatorEmbedded::getLatestWriteOpTime(
    OperationContext* opCtx) const noexcept {
    return getMyLastAppliedOpTime();
}

HostAndPort ReplicationCoordinatorEmbedded::getCurrentPrimaryHostAndPort() const {
    UASSERT_NOT_IMPLEMENTED;
}

void ReplicationCoordinatorEmbedded::cancelCbkHandle(
    executor::TaskExecutor::CallbackHandle activeHandle) {
    MONGO_UNREACHABLE;
}

BSONObj ReplicationCoordinatorEmbedded::runCmdOnPrimaryAndAwaitResponse(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    OnRemoteCmdScheduledFn onRemoteCmdScheduled,
    OnRemoteCmdCompleteFn onRemoteCmdComplete) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorEmbedded::restartScheduledHeartbeats_forTest() {
    MONGO_UNREACHABLE;
}


void ReplicationCoordinatorEmbedded::recordIfCWWCIsSetOnConfigServerOnStartup(
    OperationContext* opCtx) {
    MONGO_UNREACHABLE;
}

ReplicationCoordinatorEmbedded::WriteConcernTagChanges*
ReplicationCoordinatorEmbedded::getWriteConcernTagChanges() {
    UASSERT_NOT_IMPLEMENTED;
}

repl::SplitPrepareSessionManager* ReplicationCoordinatorEmbedded::getSplitPrepareSessionManager() {
    UASSERT_NOT_IMPLEMENTED;
}

bool ReplicationCoordinatorEmbedded::isRetryableWrite(OperationContext* opCtx) const {
    return false;
}

boost::optional<UUID> ReplicationCoordinatorEmbedded::getInitialSyncId(OperationContext* opCtx) {
    UASSERT_NOT_IMPLEMENTED;
}

}  // namespace embedded
}  // namespace mongo

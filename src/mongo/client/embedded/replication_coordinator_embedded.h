/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#pragma once

#include "mongo/db/repl/replication_coordinator.h"

namespace mongo {
namespace embedded {

class ReplicationCoordinatorEmbedded final : public repl::ReplicationCoordinator {

public:
    ReplicationCoordinatorEmbedded(ServiceContext* serviceContext);
    ~ReplicationCoordinatorEmbedded();

    ReplicationCoordinatorEmbedded(ReplicationCoordinatorEmbedded&) = delete;
    ReplicationCoordinatorEmbedded& operator=(ReplicationCoordinatorEmbedded&) = delete;

    // Members that are implemented and safe to call of public ReplicationCoordinator API

    void startup(OperationContext* opCtx) override;

    void shutdown(OperationContext* opCtx) override;

    // Returns the ServiceContext where this instance runs.
    ServiceContext* getServiceContext() override {
        return _service;
    }

    const repl::ReplSettings& getSettings() const override;

    Mode getReplicationMode() const override;
    bool getMaintenanceMode() override;

    bool isReplEnabled() const override;
    bool isMasterForReportingPurposes() override;
    bool isInPrimaryOrSecondaryState() const override;

    bool canAcceptWritesForDatabase(OperationContext* opCtx, StringData dbName) override;
    bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx, StringData dbName) override;

    bool canAcceptWritesFor(OperationContext* opCtx, const NamespaceString& ns) override;
    bool canAcceptWritesFor_UNSAFE(OperationContext* opCtx, const NamespaceString& ns) override;

    Status checkCanServeReadsFor(OperationContext* opCtx,
                                 const NamespaceString& ns,
                                 bool slaveOk) override;
    Status checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        bool slaveOk) override;

    bool shouldRelaxIndexConstraints(OperationContext* opCtx, const NamespaceString& ns) override;

    WriteConcernOptions getGetLastErrorDefault() override;

    WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(WriteConcernOptions wc) override;

    bool buildsIndexes() override;

    // Not implemented members that should not be called. Will assert or invariant.


    repl::MemberState getMemberState() const override;

    Status waitForMemberState(repl::MemberState, Milliseconds) override;

    Seconds getSlaveDelaySecs() const override;

    void clearSyncSourceBlacklist() override;

    repl::ReplicationCoordinator::StatusAndDuration awaitReplication(
        OperationContext*, const repl::OpTime&, const WriteConcernOptions&) override;

    Status stepDown(OperationContext*, bool, const Milliseconds&, const Milliseconds&) override;

    Status checkIfWriteConcernCanBeSatisfied(const WriteConcernOptions&) const override;

    void setMyLastAppliedOpTime(const repl::OpTime&) override;
    void setMyLastDurableOpTime(const repl::OpTime&) override;

    void setMyLastAppliedOpTimeForward(const repl::OpTime&, DataConsistency) override;
    void setMyLastDurableOpTimeForward(const repl::OpTime&) override;

    void resetMyLastOpTimes() override;

    void setMyHeartbeatMessage(const std::string&) override;

    repl::OpTime getMyLastAppliedOpTime() const override;
    repl::OpTime getMyLastDurableOpTime() const override;

    Status waitUntilOpTimeForReadUntil(OperationContext*,
                                       const repl::ReadConcernArgs&,
                                       boost::optional<Date_t>) override;

    Status waitUntilOpTimeForRead(OperationContext*, const repl::ReadConcernArgs&) override;

    OID getElectionId() override;

    int getMyId() const override;

    Status setFollowerMode(const repl::MemberState&) override;

    ApplierState getApplierState() override;

    void signalDrainComplete(OperationContext*, long long) override;

    Status waitForDrainFinish(Milliseconds) override;

    void signalUpstreamUpdater() override;

    Status resyncData(OperationContext*, bool) override;

    StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const override;

    Status processReplSetGetStatus(BSONObjBuilder*, ReplSetGetStatusResponseStyle) override;

    void fillIsMasterForReplSet(repl::IsMasterResponse*) override;

    void appendSlaveInfoData(BSONObjBuilder*) override;

    repl::ReplSetConfig getConfig() const override;

    void processReplSetGetConfig(BSONObjBuilder*) override;

    void processReplSetMetadata(const rpc::ReplSetMetadata&) override;

    void advanceCommitPoint(const repl::OpTime& committedOpTime) override;

    void cancelAndRescheduleElectionTimeout() override;

    Status setMaintenanceMode(bool) override;

    Status processReplSetSyncFrom(OperationContext*, const HostAndPort&, BSONObjBuilder*) override;

    Status processReplSetFreeze(int, BSONObjBuilder*) override;

    Status processHeartbeat(const repl::ReplSetHeartbeatArgs&,
                            repl::ReplSetHeartbeatResponse*) override;

    Status processReplSetReconfig(OperationContext*,
                                  const ReplSetReconfigArgs&,
                                  BSONObjBuilder*) override;

    Status processReplSetInitiate(OperationContext*, const BSONObj&, BSONObjBuilder*) override;

    Status processReplSetFresh(const ReplSetFreshArgs&, BSONObjBuilder*) override;

    Status processReplSetElect(const ReplSetElectArgs& args, BSONObjBuilder* response) override;

    Status processReplSetUpdatePosition(const repl::UpdatePositionArgs&, long long*) override;

    std::vector<HostAndPort> getHostsWrittenTo(const repl::OpTime&, bool) override;

    std::vector<HostAndPort> getOtherNodesInReplSet() const override;

    Status checkReplEnabledForCommand(BSONObjBuilder*) override;


    HostAndPort chooseNewSyncSource(const repl::OpTime&) override;

    void blacklistSyncSource(const HostAndPort&, Date_t) override;

    void resetLastOpTimesFromOplog(OperationContext*, DataConsistency) override;

    bool shouldChangeSyncSource(const HostAndPort&,
                                const rpc::ReplSetMetadata&,
                                boost::optional<rpc::OplogQueryMetadata>) override;

    repl::OpTime getLastCommittedOpTime() const override;

    Status processReplSetRequestVotes(OperationContext*,
                                      const repl::ReplSetRequestVotesArgs&,
                                      repl::ReplSetRequestVotesResponse*) override;

    void prepareReplMetadata(const BSONObj&, const repl::OpTime&, BSONObjBuilder*) const override;

    Status processHeartbeatV1(const repl::ReplSetHeartbeatArgsV1&,
                              repl::ReplSetHeartbeatResponse*) override;

    bool isV1ElectionProtocol() const override;

    bool getWriteConcernMajorityShouldJournal() override;

    void summarizeAsHtml(repl::ReplSetHtmlSummary*) override;

    void dropAllSnapshots() override;

    long long getTerm() override;

    Status updateTerm(OperationContext*, long long) override;

    repl::OpTime getCurrentCommittedSnapshotOpTime() const override;

    void waitUntilSnapshotCommitted(OperationContext*, const Timestamp&) override;

    void appendDiagnosticBSON(BSONObjBuilder*) override;

    void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    size_t getNumUncommittedSnapshots() override;

    repl::ReplSettings::IndexPrefetchConfig getIndexPrefetchConfig() const override;
    void setIndexPrefetchConfig(const repl::ReplSettings::IndexPrefetchConfig) override;

    Status stepUpIfEligible() override;

    Status abortCatchupIfNeeded() override;

    void signalDropPendingCollectionsRemovedFromStorage() final;

private:
    // Back pointer to the ServiceContext that has started the instance.
    ServiceContext* const _service;
};

}  // namespace embedded
}  // namespace mongo

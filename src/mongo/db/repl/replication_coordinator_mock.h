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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

namespace executor {
struct ConnectionPoolStats;
}  // namespace executor

namespace repl {

/**
 * A mock ReplicationCoordinator.  Currently it is extremely simple and exists solely to link
 * into dbtests.
 */
class ReplicationCoordinatorMock : public ReplicationCoordinator {
    MONGO_DISALLOW_COPYING(ReplicationCoordinatorMock);

public:
    ReplicationCoordinatorMock(ServiceContext* service, const ReplSettings& settings);
    virtual ~ReplicationCoordinatorMock();

    virtual void startup(OperationContext* opCtx);

    virtual void shutdown(OperationContext* opCtx);

    virtual ReplicationExecutor* getExecutor() override {
        return nullptr;
    };

    virtual const ReplSettings& getSettings() const;

    virtual bool isReplEnabled() const;

    virtual Mode getReplicationMode() const;

    virtual MemberState getMemberState() const;

    virtual Status waitForMemberState(MemberState expectedState, Milliseconds timeout) override;

    virtual bool isInPrimaryOrSecondaryState() const;

    virtual Seconds getSlaveDelaySecs() const;

    virtual void clearSyncSourceBlacklist();

    virtual ReplicationCoordinator::StatusAndDuration awaitReplication(
        OperationContext* opCtx, const OpTime& opTime, const WriteConcernOptions& writeConcern);

    virtual ReplicationCoordinator::StatusAndDuration awaitReplicationOfLastOpForClient(
        OperationContext* opCtx, const WriteConcernOptions& writeConcern);

    virtual Status stepDown(OperationContext* opCtx,
                            bool force,
                            const Milliseconds& waitTime,
                            const Milliseconds& stepdownTime);

    virtual bool isMasterForReportingPurposes();

    virtual bool canAcceptWritesForDatabase(OperationContext* opCtx, StringData dbName);

    virtual bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx, StringData dbName);

    bool canAcceptWritesFor(OperationContext* opCtx, const NamespaceString& ns) override;

    bool canAcceptWritesFor_UNSAFE(OperationContext* opCtx, const NamespaceString& ns) override;

    virtual Status checkIfWriteConcernCanBeSatisfied(const WriteConcernOptions& writeConcern) const;

    virtual Status checkCanServeReadsFor(OperationContext* opCtx,
                                         const NamespaceString& ns,
                                         bool slaveOk);
    virtual Status checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                const NamespaceString& ns,
                                                bool slaveOk);

    virtual bool shouldRelaxIndexConstraints(OperationContext* opCtx, const NamespaceString& ns);

    virtual Status setLastOptimeForSlave(const OID& rid, const Timestamp& ts);

    virtual void setMyLastAppliedOpTime(const OpTime& opTime);
    virtual void setMyLastDurableOpTime(const OpTime& opTime);

    virtual void setMyLastAppliedOpTimeForward(const OpTime& opTime);
    virtual void setMyLastDurableOpTimeForward(const OpTime& opTime);

    virtual void resetMyLastOpTimes();

    virtual void setMyHeartbeatMessage(const std::string& msg);

    virtual OpTime getMyLastAppliedOpTime() const;
    virtual OpTime getMyLastDurableOpTime() const;

    virtual Status waitUntilOpTimeForRead(OperationContext* opCtx,
                                          const ReadConcernArgs& settings) override;

    virtual OID getElectionId();

    virtual OID getMyRID() const;

    virtual int getMyId() const;

    virtual bool setFollowerMode(const MemberState& newState);

    virtual ApplierState getApplierState();

    virtual void signalDrainComplete(OperationContext*, long long);

    virtual Status waitForDrainFinish(Milliseconds timeout) override;

    virtual void signalUpstreamUpdater();

    virtual Status resyncData(OperationContext* opCtx, bool waitUntilCompleted) override;

    virtual StatusWith<BSONObj> prepareReplSetUpdatePositionCommand(
        ReplSetUpdatePositionCommandStyle commandStyle) const override;

    virtual Status processReplSetGetStatus(BSONObjBuilder*, ReplSetGetStatusResponseStyle);

    virtual void fillIsMasterForReplSet(IsMasterResponse* result);

    virtual void appendSlaveInfoData(BSONObjBuilder* result);

    void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    virtual ReplSetConfig getConfig() const;

    virtual void processReplSetGetConfig(BSONObjBuilder* result);

    virtual void processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) override;

    virtual void advanceCommitPoint(const OpTime& committedOptime) override;

    virtual void cancelAndRescheduleElectionTimeout() override;

    virtual Status setMaintenanceMode(bool activate);

    virtual bool getMaintenanceMode();

    virtual Status processReplSetSyncFrom(OperationContext* opCtx,
                                          const HostAndPort& target,
                                          BSONObjBuilder* resultObj);

    virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj);

    virtual Status processHeartbeat(const ReplSetHeartbeatArgs& args,
                                    ReplSetHeartbeatResponse* response);

    virtual Status processReplSetReconfig(OperationContext* opCtx,
                                          const ReplSetReconfigArgs& args,
                                          BSONObjBuilder* resultObj);

    virtual Status processReplSetInitiate(OperationContext* opCtx,
                                          const BSONObj& configObj,
                                          BSONObjBuilder* resultObj);

    virtual Status processReplSetGetRBID(BSONObjBuilder* resultObj);

    virtual void incrementRollbackID();

    virtual Status processReplSetFresh(const ReplSetFreshArgs& args, BSONObjBuilder* resultObj);

    virtual Status processReplSetElect(const ReplSetElectArgs& args, BSONObjBuilder* resultObj);

    virtual Status processReplSetUpdatePosition(const OldUpdatePositionArgs& updates,
                                                long long* configVersion);
    virtual Status processReplSetUpdatePosition(const UpdatePositionArgs& updates,
                                                long long* configVersion);

    virtual Status processHandshake(OperationContext* opCtx, const HandshakeArgs& handshake);

    virtual bool buildsIndexes();

    virtual std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op, bool durablyWritten);

    virtual std::vector<HostAndPort> getOtherNodesInReplSet() const;

    virtual WriteConcernOptions getGetLastErrorDefault();

    virtual Status checkReplEnabledForCommand(BSONObjBuilder* result);

    virtual HostAndPort chooseNewSyncSource(const OpTime& lastOpTimeFetched);

    virtual void blacklistSyncSource(const HostAndPort& host, Date_t until);

    virtual void resetLastOpTimesFromOplog(OperationContext* opCtx);

    virtual bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                        const rpc::ReplSetMetadata& replMetadata,
                                        boost::optional<rpc::OplogQueryMetadata> oqMetadata);

    virtual OpTime getLastCommittedOpTime() const;

    virtual Status processReplSetRequestVotes(OperationContext* opCtx,
                                              const ReplSetRequestVotesArgs& args,
                                              ReplSetRequestVotesResponse* response);

    void prepareReplMetadata(const BSONObj& metadataRequestObj,
                             const OpTime& lastOpTimeFromClient,
                             BSONObjBuilder* builder) const override;

    virtual Status processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                      ReplSetHeartbeatResponse* response);

    virtual bool isV1ElectionProtocol() const override;

    virtual bool getWriteConcernMajorityShouldJournal();

    virtual void summarizeAsHtml(ReplSetHtmlSummary* output);

    virtual long long getTerm();

    virtual Status updateTerm(OperationContext* opCtx, long long term);

    virtual SnapshotName reserveSnapshotName(OperationContext* opCtx);

    virtual void forceSnapshotCreation() override;

    virtual void createSnapshot(OperationContext* opCtx,
                                OpTime timeOfSnapshot,
                                SnapshotName name) override;

    virtual void dropAllSnapshots() override;

    virtual OpTime getCurrentCommittedSnapshotOpTime() const override;

    virtual void waitUntilSnapshotCommitted(OperationContext* opCtx,
                                            const SnapshotName& untilSnapshot) override;

    virtual size_t getNumUncommittedSnapshots() override;

    virtual WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(
        WriteConcernOptions wc) override;

    virtual ReplSettings::IndexPrefetchConfig getIndexPrefetchConfig() const override;
    virtual void setIndexPrefetchConfig(const ReplSettings::IndexPrefetchConfig cfg) override;

    virtual Status stepUpIfEligible() override;

    /**
     * Sets the return value for calls to getConfig.
     */
    void setGetConfigReturnValue(ReplSetConfig returnValue);

    /**
     * Always allow writes even if this node is not master. Used by sharding unit tests.
     */
    void alwaysAllowWrites(bool allowWrites);

    void setMaster(bool isMaster);

    virtual ServiceContext* getServiceContext() override {
        return _service;
    }

private:
    AtomicUInt64 _snapshotNameGenerator;
    ServiceContext* const _service;
    ReplSettings _settings;
    MemberState _memberState;
    OpTime _myLastDurableOpTime;
    OpTime _myLastAppliedOpTime;
    ReplSetConfig _getConfigReturnValue;
    bool _alwaysAllowWrites = false;
};

}  // namespace repl
}  // namespace mongo

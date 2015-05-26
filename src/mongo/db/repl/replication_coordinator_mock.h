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
#include "mongo/db/repl/replication_coordinator.h"

namespace mongo {
namespace repl {

    /**
     * A mock ReplicationCoordinator.  Currently it is extremely simple and exists solely to link
     * into dbtests.
     */
    class ReplicationCoordinatorMock : public ReplicationCoordinator {
        MONGO_DISALLOW_COPYING(ReplicationCoordinatorMock);

    public:

        ReplicationCoordinatorMock(const ReplSettings& settings);
        virtual ~ReplicationCoordinatorMock();

        virtual void startReplication(OperationContext* txn);

        virtual void shutdown();

        virtual const ReplSettings& getSettings() const;

        virtual bool isReplEnabled() const;

        virtual Mode getReplicationMode() const;

        virtual MemberState getMemberState() const;

        virtual bool isInPrimaryOrSecondaryState() const;

        virtual Seconds getSlaveDelaySecs() const;

        virtual void clearSyncSourceBlacklist();

        virtual ReplicationCoordinator::StatusAndDuration awaitReplication(
                const OperationContext* txn,
                const OpTime& opTime,
                const WriteConcernOptions& writeConcern);

        virtual ReplicationCoordinator::StatusAndDuration awaitReplicationOfLastOpForClient(
                const OperationContext* txn,
                const WriteConcernOptions& writeConcern);

        virtual Status stepDown(OperationContext* txn, 
                                bool force,
                                const Milliseconds& waitTime,
                                const Milliseconds& stepdownTime);

        virtual bool isMasterForReportingPurposes();

        virtual bool canAcceptWritesForDatabase(StringData dbName);

        virtual Status checkIfWriteConcernCanBeSatisfied(
                const WriteConcernOptions& writeConcern) const;

        virtual Status checkCanServeReadsFor(OperationContext* txn,
                                             const NamespaceString& ns,
                                             bool slaveOk);

        virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx);

        virtual Status setLastOptimeForSlave(const OID& rid, const Timestamp& ts);

        virtual void setMyLastOptime(const OpTime& opTime);

        virtual void resetMyLastOptime();

        virtual void setMyHeartbeatMessage(const std::string& msg);

        virtual OpTime getMyLastOptime() const;

        virtual ReadAfterOpTimeResponse waitUntilOpTime(
                        const OperationContext* txn,
                        const ReadAfterOpTimeArgs& settings) override;

        virtual OID getElectionId();

        virtual OID getMyRID() const;

        virtual int getMyId() const;

        virtual bool setFollowerMode(const MemberState& newState);

        virtual bool isWaitingForApplierToDrain();

        virtual void signalDrainComplete(OperationContext*);

        virtual void signalUpstreamUpdater();

        virtual bool prepareReplSetUpdatePositionCommand(BSONObjBuilder* cmdBuilder);

        virtual Status processReplSetGetStatus(BSONObjBuilder* result);

        virtual void fillIsMasterForReplSet(IsMasterResponse* result);

        virtual void appendSlaveInfoData(BSONObjBuilder* result);

        virtual ReplicaSetConfig getConfig() const;

        virtual void processReplSetGetConfig(BSONObjBuilder* result);

        virtual Status setMaintenanceMode(bool activate);

        virtual bool getMaintenanceMode();

        virtual Status processReplSetSyncFrom(const HostAndPort& target,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj);

        virtual Status processHeartbeat(const ReplSetHeartbeatArgs& args,
                                        ReplSetHeartbeatResponse* response);

        virtual Status processReplSetReconfig(OperationContext* txn,
                                              const ReplSetReconfigArgs& args,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetInitiate(OperationContext* txn,
                                              const BSONObj& configObj,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetGetRBID(BSONObjBuilder* resultObj);

        virtual void incrementRollbackID();

        virtual Status processReplSetFresh(const ReplSetFreshArgs& args,
                                           BSONObjBuilder* resultObj);

        virtual Status processReplSetElect(const ReplSetElectArgs& args,
                                           BSONObjBuilder* resultObj);

        virtual Status processReplSetUpdatePosition(const UpdatePositionArgs& updates,
                                                    long long* configVersion);

        virtual Status processHandshake(OperationContext* txn, const HandshakeArgs& handshake);

        virtual bool buildsIndexes();

        virtual std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op);

        virtual std::vector<HostAndPort> getOtherNodesInReplSet() const;

        virtual WriteConcernOptions getGetLastErrorDefault();

        virtual Status checkReplEnabledForCommand(BSONObjBuilder* result);

        virtual HostAndPort chooseNewSyncSource();

        virtual void blacklistSyncSource(const HostAndPort& host, Date_t until);

        virtual void resetLastOpTimeFromOplog(OperationContext* txn);

        virtual bool shouldChangeSyncSource(const HostAndPort& currentSource);

        virtual OpTime getLastCommittedOpTime() const;

        virtual Status processReplSetRequestVotes(OperationContext* txn,
                                                  const ReplSetRequestVotesArgs& args,
                                                  ReplSetRequestVotesResponse* response);

        virtual Status processReplSetDeclareElectionWinner(
                const ReplSetDeclareElectionWinnerArgs& args,
                long long* responseTerm);

        virtual void prepareCursorResponseInfo(BSONObjBuilder* objBuilder);

        virtual Status processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                          ReplSetHeartbeatResponse* response);

        virtual bool isV1ElectionProtocol();

        virtual void summarizeAsHtml(ReplSetHtmlSummary* output);

        virtual long long getTerm();

        virtual bool updateTerm(long long term);

    private:

        const ReplSettings _settings;
    };

} // namespace repl
} // namespace mongo

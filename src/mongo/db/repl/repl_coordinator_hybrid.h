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
#include "mongo/db/repl/repl_coordinator.h"
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/repl_coordinator_legacy.h"

namespace mongo {
namespace repl {

    class ReplSetConfig;

    /**
     * An implementation of ReplicationCoordinator that will evolve with ReplicationCoordinatorImpl
     * to aid in the transition from LegacyReplicationCoordinator to ReplicationCoordinatorImpl and
     * to verify the correctness of ReplicationCoordinatorImpl.
     */
    class HybridReplicationCoordinator : public ReplicationCoordinator {
        MONGO_DISALLOW_COPYING(HybridReplicationCoordinator);

    public:

        HybridReplicationCoordinator(const ReplSettings& settings);
        virtual ~HybridReplicationCoordinator();

        virtual void startReplication(OperationContext* txn);

        virtual void shutdown();

        virtual ReplSettings& getSettings();

        virtual Mode getReplicationMode() const;

        virtual MemberState getCurrentMemberState() const;

        virtual ReplicationCoordinator::StatusAndDuration awaitReplication(
                const OperationContext* txn,
                const OpTime& ts,
                const WriteConcernOptions& writeConcern);

        virtual ReplicationCoordinator::StatusAndDuration awaitReplicationOfLastOpForClient(
                const OperationContext* txn,
                const WriteConcernOptions& writeConcern);

        virtual ReplicationCoordinator::StatusAndDuration awaitReplicationOfLastOpApplied(
                const OperationContext* txn,
                const WriteConcernOptions& writeConcern);

        virtual Status stepDown(OperationContext* txn,
                                bool force,
                                const Milliseconds& waitTime,
                                const Milliseconds& stepdownTime);

        virtual bool isMasterForReportingPurposes();

        virtual bool canAcceptWritesForDatabase(const StringData& dbName);

        virtual Status checkIfWriteConcernCanBeSatisfied(
                const WriteConcernOptions& writeConcern) const;

        virtual Status checkCanServeReadsFor(OperationContext* txn,
                                             const NamespaceString& ns,
                                             bool slaveOk);

        virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx);

        virtual Status setLastOptime(OperationContext* txn, const OID& rid, const OpTime& ts);

        virtual Status setMyLastOptime(OperationContext* txn, const OpTime& ts);

        virtual OpTime getMyLastOptime() const;

        virtual OID getElectionId();

        virtual OID getMyRID() const;

        virtual int getMyId() const;

        virtual void setFollowerMode(const MemberState& newState);

        virtual bool isWaitingForApplierToDrain();

        virtual void signalDrainComplete();

        virtual void prepareReplSetUpdatePositionCommand(OperationContext* txn,
                                                         BSONObjBuilder* cmdBuilder);

        virtual void prepareReplSetUpdatePositionCommandHandshakes(
                OperationContext* txn,
                std::vector<BSONObj>* handshakes);

        virtual Status processReplSetGetStatus(BSONObjBuilder* result);

        virtual void fillIsMasterForReplSet(IsMasterResponse* result);

        virtual void processReplSetGetConfig(BSONObjBuilder* result);

        virtual Status setMaintenanceMode(OperationContext* txn, bool activate);

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

        virtual Status processReplSetUpdatePosition(OperationContext* txn,
                                                    const UpdatePositionArgs& updates);

        virtual Status processHandshake(const OperationContext* txn,
                                        const HandshakeArgs& handshake);

        virtual bool buildsIndexes();

        virtual std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op);

        virtual std::vector<HostAndPort> getOtherNodesInReplSet() const;

        virtual BSONObj getGetLastErrorDefault();

        virtual Status checkReplEnabledForCommand(BSONObjBuilder* result);

        virtual bool isReplEnabled() const;

        virtual HostAndPort chooseNewSyncSource();

        virtual void blacklistSyncSource(const HostAndPort& host, Date_t until);

        virtual void resetLastOpTimeFromOplog(OperationContext* txn);

        virtual bool shouldChangeSyncSource(const HostAndPort& currentSource);

        /**
         * This is a temporary hack to force _impl to set its replset config to the one loaded by
         * _legacy.
         * TODO(spencer): Remove this once ReplicationCoordinatorImpl can load its own config.
         */
        void setImplConfigHack(const ReplSetConfig* config);

    private:
        LegacyReplicationCoordinator _legacy;
        ReplicationCoordinatorImpl _impl;
    };

} // namespace repl
} // namespace mongo

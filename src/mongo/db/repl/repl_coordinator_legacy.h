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

namespace mongo {
namespace repl {

    /**
     * An implementation of ReplicationCoordinator that simply delegates to existing code.
     */
    class LegacyReplicationCoordinator : public ReplicationCoordinator {
        MONGO_DISALLOW_COPYING(LegacyReplicationCoordinator);

    public:

        LegacyReplicationCoordinator();
        virtual ~LegacyReplicationCoordinator();

        virtual void startReplication(TopologyCoordinator*,
                                      ReplicationExecutor::NetworkInterface*);

        virtual void shutdown();

        virtual bool isShutdownOkay() const;

        virtual Mode getReplicationMode() const;

        virtual MemberState getCurrentMemberState() const;

        virtual ReplicationCoordinator::StatusAndDuration awaitReplication(
                const OperationContext* txn,
                const OpTime& ts,
                const WriteConcernOptions& writeConcern);

        virtual ReplicationCoordinator::StatusAndDuration awaitReplicationOfLastOp(
                const OperationContext* txn,
                const WriteConcernOptions& writeConcern);

        virtual Status stepDown(bool force,
                                const Milliseconds& waitTime,
                                const Milliseconds& stepdownTime);

        virtual Status stepDownAndWaitForSecondary(const Milliseconds& initialWaitTime,
                                                   const Milliseconds& stepdownTime,
                                                   const Milliseconds& postStepdownWaitTime);

        virtual bool isMasterForReportingPurposes();

        virtual bool canAcceptWritesForDatabase(const StringData& dbName);

        virtual bool canServeReadsFor(const NamespaceString& collection);

        virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx);

        virtual Status setLastOptime(const OID& rid, const OpTime& ts, const BSONObj& config);

        virtual void processReplSetGetStatus(BSONObjBuilder* result);

        virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj);

        virtual Status processHeartbeat(const BSONObj& cmdObj, BSONObjBuilder* resultObj);

        virtual Status processReplSetReconfig(OperationContext* txn,
                                              const BSONObj& newConfigObj,
                                              bool force,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetInitiate(OperationContext* txn,
                                              const BSONObj& configObj,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetGetRBID(BSONObjBuilder* resultObj);

        virtual void incrementRollbackID();

        virtual Status processReplSetFresh(const StringData& setName,
                                           const StringData& who,
                                           unsigned id,
                                           int cfgver,
                                           const OpTime& opTime,
                                           BSONObjBuilder* resultObj);

        virtual Status processReplSetElect(const StringData& set,
                                           unsigned whoid,
                                           int cfgver,
                                           const OID& round,
                                           BSONObjBuilder* resultObj);
    private:
        Status _stepDownHelper(bool force,
                               const Milliseconds& initialWaitTime,
                               const Milliseconds& stepdownTime,
                               const Milliseconds& postStepdownWaitTime);

        // Rollback id. used to check if a rollback happened during some interval of time
        // TODO: ideally this should only change on rollbacks NOT on mongod restarts also.
        int _rbid;
    };

} // namespace repl
} // namespace mongo

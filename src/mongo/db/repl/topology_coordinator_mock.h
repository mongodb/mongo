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

#include "mongo/db/repl/topology_coordinator.h"

namespace mongo {

    class OperationContext;

namespace repl {

    class TopologyCoordinatorMock : public TopologyCoordinator {
    public:

        TopologyCoordinatorMock();
        virtual ~TopologyCoordinatorMock() {};
        
        virtual void setLastApplied(const OpTime& optime);
        virtual void setCommitOkayThrough(const OpTime& optime);
        virtual void setLastReceived(const OpTime& optime);

        virtual HostAndPort getSyncSourceAddress() const;

        virtual void chooseNewSyncSource(Date_t now); // this is basically getMemberToSyncTo()

        virtual void blacklistSyncSource(const HostAndPort& host, Date_t until);

        virtual void registerConfigChangeCallback(const ConfigChangeCallbackFn& fn);
        virtual void registerStateChangeCallback(const StateChangeCallbackFn& fn);

        virtual void signalDrainComplete();

        virtual bool prepareRequestVoteResponse(const BSONObj& cmdObj, 
                                                std::string& errmsg, 
                                                BSONObjBuilder& result);

        virtual void prepareElectCmdResponse(const BSONObj& cmdObj, BSONObjBuilder& result);

        virtual void prepareHeartbeatResponse(const ReplicationExecutor::CallbackData& data,
                                              Date_t now,
                                              const BSONObj& cmdObj, 
                                              BSONObjBuilder* resultObj,
                                              Status* result);

        virtual void updateHeartbeatInfo(Date_t now, const HeartbeatInfo& newInfo);

        virtual void relinquishPrimary(OperationContext* txn);

    };

} // namespace repl
} // namespace mongo

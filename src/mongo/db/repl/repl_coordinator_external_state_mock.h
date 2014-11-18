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

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/optime.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/repl_coordinator_external_state.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

    class ReplicationCoordinatorExternalStateMock : public ReplicationCoordinatorExternalState {
        MONGO_DISALLOW_COPYING(ReplicationCoordinatorExternalStateMock);
    public:
        class GlobalSharedLockAcquirer;

        ReplicationCoordinatorExternalStateMock();
        virtual ~ReplicationCoordinatorExternalStateMock();
        virtual void startThreads();
        virtual void startMasterSlave(OperationContext*);
        virtual void shutdown();
        virtual void forwardSlaveHandshake();
        virtual void forwardSlaveProgress();
        virtual OID ensureMe(OperationContext*);
        virtual bool isSelf(const HostAndPort& host);
        virtual HostAndPort getClientHostAndPort(const OperationContext* txn);
        virtual StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* txn);
        virtual Status storeLocalConfigDocument(OperationContext* txn, const BSONObj& config);
        virtual StatusWith<OpTime> loadLastOpTime(OperationContext* txn);
        virtual StatusWith<OpTime> logOpMessage(OperationContext* txn,
                                                const std::string& msg);
        virtual void closeConnections();
        virtual void clearShardingState();
        virtual void signalApplierToChooseNewSyncSource();
        virtual ReplicationCoordinatorExternalState::GlobalSharedLockAcquirer*
                getGlobalSharedLockAcquirer();
        virtual OperationContext* createOperationContext(const std::string& threadName);
        virtual void dropAllTempCollections(OperationContext* txn);

        /**
         * Adds "host" to the list of hosts that this mock will match when responding to "isSelf"
         * messages.
         */
        void addSelf(const HostAndPort& host);

        /**
         * Sets the return value for subsequent calls to loadLocalConfigDocument().
         */
        void setLocalConfigDocument(const StatusWith<BSONObj>& localConfigDocument);

        /**
         * Sets the return value for subsequent calls to getClientHostAndPort().
         */
        void setClientHostAndPort(const HostAndPort& clientHostAndPort);

        /**
         * Sets the value that will be passed to the constructor of any future
         * GlobalSharedLockAcuirers created and returned by getGlobalSharedLockAcquirer().
         */
        void setCanAcquireGlobalSharedLock(bool canAcquire);

        /**
         * Sets the return value for subsequent calls to loadLastOpTimeApplied.
         */
        void setLastOpTime(const StatusWith<OpTime>& lastApplied);

        /**
         * Sets the return value for subsequent calls to storeLocalConfigDocument().
         * If "status" is Status::OK(), the subsequent calls will call the underlying funtion.
         */ 
        void setStoreLocalConfigDocumentStatus(Status status);

        /**
         * Sets whether or not subsequent calls to storeLocalConfigDocument() should hang
         * indefinitely or not based on the value of "hang".
         */
        void setStoreLocalConfigDocumentToHang(bool hang);

    private:
        StatusWith<BSONObj> _localRsConfigDocument;
        StatusWith<OpTime>  _lastOpTime;
        std::vector<HostAndPort> _selfHosts;
        bool _canAcquireGlobalSharedLock;
        Status _storeLocalConfigDocumentStatus;
        // mutex and cond var for controlling stroeLocalConfigDocument()'s hanging
        boost::mutex _shouldHangMutex;
        boost::condition _shouldHangCondVar;
        bool _storeLocalConfigDocumentShouldHang;
        bool _connectionsClosed;
        HostAndPort _clientHostAndPort;
    };

    class ReplicationCoordinatorExternalStateMock::GlobalSharedLockAcquirer :
            public ReplicationCoordinatorExternalState::GlobalSharedLockAcquirer {
    public:

        /**
         * The canAcquireLock argument determines what the return value of calls to try_lock will
         * be.
         */
        GlobalSharedLockAcquirer(bool canAcquireLock);
        virtual ~GlobalSharedLockAcquirer();

        virtual bool try_lock(OperationContext* txn, const Milliseconds& timeout);

    private:

        const bool _canAcquireLock;
    };


} // namespace repl
} // namespace mongo

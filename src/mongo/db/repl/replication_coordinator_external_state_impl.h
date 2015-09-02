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

#include <deque>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/sync_source_feedback.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"

namespace mongo {
namespace repl {

class SnapshotThread;

class ReplicationCoordinatorExternalStateImpl : public ReplicationCoordinatorExternalState {
    MONGO_DISALLOW_COPYING(ReplicationCoordinatorExternalStateImpl);

public:
    ReplicationCoordinatorExternalStateImpl();
    virtual ~ReplicationCoordinatorExternalStateImpl();
    void startThreads(executor::TaskExecutor* taskExecutor) override;
    virtual void startMasterSlave(OperationContext* txn);
    virtual void shutdown();
    virtual void initiateOplog(OperationContext* txn, bool updateReplOpTime);
    virtual void logTransitionToPrimaryToOplog(OperationContext* txn);
    virtual void forwardSlaveProgress();
    virtual OID ensureMe(OperationContext* txn);
    virtual bool isSelf(const HostAndPort& host);
    virtual StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* txn);
    virtual Status storeLocalConfigDocument(OperationContext* txn, const BSONObj& config);
    virtual StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* txn);
    virtual Status storeLocalLastVoteDocument(OperationContext* txn, const LastVote& lastVote);
    virtual void setGlobalTimestamp(const Timestamp& newTime);
    virtual StatusWith<OpTime> loadLastOpTime(OperationContext* txn);
    virtual HostAndPort getClientHostAndPort(const OperationContext* txn);
    virtual void closeConnections();
    virtual void killAllUserOperations(OperationContext* txn);
    virtual void clearShardingState();
    virtual void signalApplierToChooseNewSyncSource();
    virtual void signalApplierToCancelFetcher();
    virtual OperationContext* createOperationContext(const std::string& threadName);
    virtual void dropAllTempCollections(OperationContext* txn);
    void dropAllSnapshots() final;
    void updateCommittedSnapshot(SnapshotName newCommitPoint) final;
    void forceSnapshotCreation() final;
    virtual bool snapshotsEnabled() const;

    std::string getNextOpContextThreadName();

private:
    // Guards starting threads and setting _startedThreads
    stdx::mutex _threadMutex;

    // True when the threads have been started
    bool _startedThreads;

    // The SyncSourceFeedback class is responsible for sending replSetUpdatePosition commands
    // for forwarding replication progress information upstream when there is chained
    // replication.
    SyncSourceFeedback _syncSourceFeedback;

    // Thread running SyncSourceFeedback::run().
    std::unique_ptr<stdx::thread> _syncSourceFeedbackThread;

    // Thread running runSyncThread().
    std::unique_ptr<stdx::thread> _applierThread;

    // Thread running BackgroundSync::producerThread().
    std::unique_ptr<stdx::thread> _producerThread;

    // Mutex guarding the _nextThreadId value to prevent concurrent incrementing.
    stdx::mutex _nextThreadIdMutex;
    // Number used to uniquely name threads.
    long long _nextThreadId;

    std::unique_ptr<SnapshotThread> _snapshotThread;
};

}  // namespace repl
}  // namespace mongo

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

#pragma once

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <functional>
#include <memory>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

class ReplicationCoordinator;

/**
 * Data replicator external state implementation for testing.
 */

class DataReplicatorExternalStateMock : public DataReplicatorExternalState {
public:
    DataReplicatorExternalStateMock();

    executor::TaskExecutor* getTaskExecutor() const override;
    std::shared_ptr<executor::TaskExecutor> getSharedTaskExecutor() const override;

    OpTimeWithTerm getCurrentTermAndLastCommittedOpTime() override;

    void processMetadata(const rpc::ReplSetMetadata& metadata,
                         const rpc::OplogQueryMetadata& oqMetadata) override;

    ChangeSyncSourceAction shouldStopFetching(const HostAndPort& source,
                                              const rpc::ReplSetMetadata& replMetadata,
                                              const rpc::OplogQueryMetadata& oqMetadata,
                                              const OpTime& previousOpTimeFetched,
                                              const OpTime& lastOpTimeFetched) const override;

    ChangeSyncSourceAction shouldStopFetchingOnError(
        const HostAndPort& source, const OpTime& lastOpTimeFetched) const override;

    std::unique_ptr<OplogBuffer> makeInitialSyncOplogBuffer(OperationContext* opCtx) const override;

    std::unique_ptr<OplogApplier> makeOplogApplier(
        OplogBuffer* oplogBuffer,
        OplogApplier::Observer* observer,
        ReplicationConsistencyMarkers* consistencyMarkers,
        StorageInterface* storageInterface,
        const OplogApplier::Options& options,
        ThreadPool* writerPool) final;

    StatusWith<ReplSetConfig> getCurrentConfig() const override;

    StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx) const override;

    Status storeLocalConfigDocument(OperationContext* opCtx, const BSONObj& config) override;

    StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx) const override;

    JournalListener* getReplicationJournalListener() override;

    void setCurrentTerm(long long newTerm) {
        stdx::lock_guard<Latch> lk(_mutex);
        currentTerm = newTerm;
    }

    long long getCurrentTerm() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return currentTerm;
    }

    void setLastCommittedOpTime(OpTime newOpTime) {
        stdx::lock_guard<Latch> lk(_mutex);
        lastCommittedOpTime = newOpTime;
    }

    void setShouldStopFetchingResult(ChangeSyncSourceAction result) {
        stdx::lock_guard<Latch> lk(_mutex);
        shouldStopFetchingResult = result;
    }

    void setReplSetConfigResult(StatusWith<ReplSetConfig> config) {
        stdx::lock_guard<Latch> lk(_mutex);
        replSetConfigResult = config;
    }

    bool getIsPrimary() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return replMetadataProcessed.getIsPrimary();
    }

    bool getHasPrimaryIndex() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return oqMetadataProcessed.hasPrimaryIndex();
    }

    bool getMetadataWasProcessed() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return metadataWasProcessed;
    }

    HostAndPort getLastSyncSourceChecked() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return lastSyncSourceChecked;
    }

    OpTime getSyncSourceLastOpTime() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return syncSourceLastOpTime;
    }

    bool getSyncSourceHasSyncSource() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return syncSourceHasSyncSource;
    }

    // Task executor.
    std::shared_ptr<executor::TaskExecutor> taskExecutor = nullptr;

    // Override to change applyOplogBatch behavior.
    using ApplyOplogBatchFn = std::function<StatusWith<OpTime>(
        OperationContext*, std::vector<OplogEntry>, OplogApplier::Observer*)>;
    ApplyOplogBatchFn applyOplogBatchFn;

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("DataReplicatorExternalStateMock::_mutex");

    // Returned by shouldStopFetching.
    ChangeSyncSourceAction shouldStopFetchingResult = ChangeSyncSourceAction::kContinueSyncing;

    // Returned by getCurrentTermAndLastCommittedOpTime.
    long long currentTerm = OpTime::kUninitializedTerm;
    OpTime lastCommittedOpTime;

    // Set by processMetadata.
    rpc::ReplSetMetadata replMetadataProcessed;
    rpc::OplogQueryMetadata oqMetadataProcessed;
    bool metadataWasProcessed = false;

    // Set by shouldStopFetching.
    mutable HostAndPort lastSyncSourceChecked;
    mutable OpTime syncSourceLastOpTime;
    mutable bool syncSourceHasSyncSource = false;

    StatusWith<ReplSetConfig> replSetConfigResult = ReplSetConfig();
};


}  // namespace repl
}  // namespace mongo

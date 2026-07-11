// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>

namespace mongo {
namespace repl {

class ReplicationCoordinator;

class ReplicationCoordinatorExternalState;

/**
 * Data replicator external state implementation using a replication coordinator.
 */

class [[MONGO_MOD_OPEN]] DataReplicatorExternalStateImpl : public DataReplicatorExternalState {
public:
    DataReplicatorExternalStateImpl(
        ReplicationCoordinator* replicationCoordinator,
        ReplicationCoordinatorExternalState* replicationCoordinatorExternalState);

    executor::TaskExecutor* getTaskExecutor() const override;
    std::shared_ptr<executor::TaskExecutor> getSharedTaskExecutor() const override;

    OpTimeWithTerm getCurrentTermAndLastCommittedOpTime() override;

    void processMetadata(const rpc::ReplSetMetadata& replMetadata,
                         const rpc::OplogQueryMetadata& oqMetadata) override;

    ChangeSyncSourceAction shouldStopFetching(const HostAndPort& source,
                                              const rpc::ReplSetMetadata& replMetadata,
                                              const rpc::OplogQueryMetadata& oqMetadata,
                                              const OpTime& previousOpTimeFetched,
                                              const OpTime& lastOpTimeFetched) const override;

    ChangeSyncSourceAction shouldStopFetchingOnError(
        const HostAndPort& source, const OpTime& lastOpTimeFetched) const override;

    std::unique_ptr<OplogBuffer> makeInitialSyncOplogBuffer(OperationContext* opCtx) const override;

    /**
     * These arguments are passed directly to OplogApplierImpl's constructor along with some other
     * parameters owned by this DataReplicationExternalStateImpl.
     * These arguments are required by OplogApplier to get the next applier batch and to apply
     * a batch of operations in parallel.
     * None of the the arguments will be owned by OplogApplierImpl.
     */
    std::unique_ptr<OplogApplier> makeOplogApplier(
        OplogBuffer* oplogBuffer,
        OplogApplier::Observer* observer,
        ReplicationConsistencyMarkers* consistencyMarkers,
        StorageInterface* storageInterface,
        const OplogApplier::Options& options,
        ThreadPool* workerPool) final;

    StatusWith<ReplSetConfig> getCurrentConfig() const override;

    StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx) const override;

    Status storeLocalConfigDocument(OperationContext* opCtx, const BSONObj& config) override;

    StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx) const override;

    JournalListener* getReplicationJournalListener() override;

private:
    // Not owned by us.
    ReplicationCoordinator* _replicationCoordinator;

    // Not owned by us.
    ReplicationCoordinatorExternalState* _replicationCoordinatorExternalState;
};


}  // namespace repl
}  // namespace mongo

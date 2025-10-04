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

#include "mongo/base/status_with.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

namespace MONGO_MOD_PUB mongo {

namespace executor {
class TaskExecutor;
}  // namespace executor

namespace repl {

class InitialSyncer;

/**
 * Holds current term and last committed optime necessary to populate find/getMore command requests.
 */
using OpTimeWithTerm = OpTimeWith<long long>;

/**
 * This class represents the interface the InitialSyncer uses to interact with the
 * rest of the system.  All functionality of the InitialSyncer that would introduce
 * dependencies on large sections of the server code and thus break the unit testability of
 * InitialSyncer should be moved here.
 */
class MONGO_MOD_PUB DataReplicatorExternalState {
    DataReplicatorExternalState(const DataReplicatorExternalState&) = delete;
    DataReplicatorExternalState& operator=(const DataReplicatorExternalState&) = delete;

public:
    DataReplicatorExternalState() = default;

    virtual ~DataReplicatorExternalState() = default;

    /**
     * Returns task executor for scheduling tasks to be run asynchronously.
     */
    virtual executor::TaskExecutor* getTaskExecutor() const = 0;
    virtual std::shared_ptr<executor::TaskExecutor> getSharedTaskExecutor() const = 0;

    /**
     * Returns the current term and last committed optime.
     * Returns (OpTime::kUninitializedTerm, OpTime()) if not available.
     */
    virtual OpTimeWithTerm getCurrentTermAndLastCommittedOpTime() = 0;

    /**
     * Forwards the parsed metadata in the query results to the replication system.
     */
    virtual void processMetadata(const rpc::ReplSetMetadata& replMetadata,
                                 const rpc::OplogQueryMetadata& oqMetadata) = 0;

    /**
     * Evaluates quality of sync source. Accepts the current sync source; the last optime on this
     * sync source (from metadata); and whether this sync source has a sync source (also from
     * metadata).
     */
    virtual ChangeSyncSourceAction shouldStopFetching(const HostAndPort& source,
                                                      const rpc::ReplSetMetadata& replMetadata,
                                                      const rpc::OplogQueryMetadata& oqMetadata,
                                                      const OpTime& previousOpTimeFetched,
                                                      const OpTime& lastOpTimeFetched) const = 0;

    /**
     * Evaluates quality of sync source. This is intended to be called on error when no
     * current metadata is available.
     */
    virtual ChangeSyncSourceAction shouldStopFetchingOnError(
        const HostAndPort& source, const OpTime& lastOpTimeFetched) const = 0;

    /**
     * This function creates an oplog buffer of the type specified at server startup.
     */
    virtual std::unique_ptr<OplogBuffer> makeInitialSyncOplogBuffer(
        OperationContext* opCtx) const = 0;

    /**
     * Creates an OplogApplier using the provided options.
     */
    virtual std::unique_ptr<OplogApplier> makeOplogApplier(
        OplogBuffer* oplogBuffer,
        OplogApplier::Observer* observer,
        ReplicationConsistencyMarkers* consistencyMarkers,
        StorageInterface* storageInterface,
        const OplogApplier::Options& options,
        ThreadPool* workerPool) = 0;

    /**
     * Returns the current in-memory replica set config if there is one, or an error why there
     * isn't.
     */
    virtual StatusWith<ReplSetConfig> getCurrentConfig() const = 0;

    /**
     * Returns the current stored replica set config if there is one, or an error why there isn't.
     */
    virtual StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx) const = 0;

    /**
     * Stores the replica set config document in local storage, or returns an error.
     */
    virtual Status storeLocalConfigDocument(OperationContext* opCtx, const BSONObj& config) = 0;

    /**
     * Returns the current stored replica set "last vote" if there is one, or an error why there
     * isn't.
     */
    virtual StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx) const = 0;

    /**
     * Returns the replication journal listener.
     */
    virtual JournalListener* getReplicationJournalListener() = 0;
};

}  // namespace repl
}  // namespace MONGO_MOD_PUB mongo

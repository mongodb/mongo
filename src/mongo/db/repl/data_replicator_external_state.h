/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class OldThreadPool;

namespace executor {
class TaskExecutor;
}  // namespace executor

namespace repl {

class DataReplicator;

/**
 * Holds current term and last committed optime necessary to populate find/getMore command requests.
 */
using OpTimeWithTerm = OpTimeWith<long long>;

/**
 * This class represents the interface the DataReplicator uses to interact with the
 * rest of the system.  All functionality of the DataReplicator that would introduce
 * dependencies on large sections of the server code and thus break the unit testability of
 * DataReplicator should be moved here.
 */
class DataReplicatorExternalState {
    MONGO_DISALLOW_COPYING(DataReplicatorExternalState);

public:
    DataReplicatorExternalState() = default;

    virtual ~DataReplicatorExternalState() = default;

    /**
     * Returns task executor for scheduling tasks to be run asynchronously.
     */
    virtual executor::TaskExecutor* getTaskExecutor() const = 0;

    /**
     * Returns shared db worker thread pool for collection cloning.
     */
    virtual OldThreadPool* getDbWorkThreadPool() const = 0;

    /**
     * Returns the current term and last committed optime.
     * Returns (OpTime::kUninitializedTerm, OpTime()) if not available.
     */
    virtual OpTimeWithTerm getCurrentTermAndLastCommittedOpTime() = 0;

    /**
     * Forwards the parsed metadata in the query results to the replication system.
     */
    virtual void processMetadata(const rpc::ReplSetMetadata& metadata) = 0;

    /**
     * Evaluates quality of sync source. Accepts the current sync source; the last optime on this
     * sync source (from metadata); and whether this sync source has a sync source (also from
     * metadata).
     */
    virtual bool shouldStopFetching(const HostAndPort& source,
                                    const rpc::ReplSetMetadata& metadata) = 0;

    /**
     * This function creates an oplog buffer of the type specified at server startup.
     */
    virtual std::unique_ptr<OplogBuffer> makeInitialSyncOplogBuffer(
        OperationContext* txn) const = 0;

    /**
     * Creates an oplog buffer suitable for steady state replication.
     */
    virtual std::unique_ptr<OplogBuffer> makeSteadyStateOplogBuffer(
        OperationContext* txn) const = 0;

    /**
     * Returns the current replica set config if there is one, or an error why there isn't.
     */
    virtual StatusWith<ReplicaSetConfig> getCurrentConfig() const = 0;

private:
    /**
     * Applies the operations described in the oplog entries contained in "ops" using the
     * "applyOperation" function.
     *
     * Used exclusively by the DataReplicator to construct a MultiApplier.
     */
    virtual StatusWith<OpTime> _multiApply(OperationContext* txn,
                                           MultiApplier::Operations ops,
                                           MultiApplier::ApplyOperationFn applyOperation) = 0;

    /**
     * Used by _multiApply() to write operations to database during steady state replication.
     *
     * Used exclusively by the DataReplicator to construct a MultiApplier.
     */
    virtual void _multiSyncApply(MultiApplier::OperationPtrs* ops) = 0;

    /**
     * Used by _multiApply() to write operations to database during initial sync.
     * Fetches missing documents from "source".
     *
     * Used exclusively by the DataReplicator to construct a MultiApplier.
     */
    virtual void _multiInitialSyncApply(MultiApplier::OperationPtrs* ops,
                                        const HostAndPort& source) = 0;

    // Provides DataReplicator with access to _multiApply, _multiSyncApply and
    // _multiInitialSyncApply.
    friend class DataReplicator;
};

}  // namespace repl
}  // namespace mongo

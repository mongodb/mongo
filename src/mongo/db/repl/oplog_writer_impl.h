/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_writer.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

class OplogWriterStats {
public:
    void incrementBatchSize(uint64_t n);
    TimerStats& getBatches();
    BSONObj getReport() const;
    operator BSONObj() const {
        return getReport();
    }

private:
    TimerStats _batches;
    Counter64 _batchSize;
};

/**
 * Writes oplog entries.
 *
 * Primarily used to write batches of operations fetched from a sync source during steady
 * state replication and initial sync. Startup recovery can also use this to recover the
 * change collection.
 *
 * When used for steady state replication, runs a thread that reads batches of operations
 * from an oplog buffer populated through the BackgroundSync interface and writes them to
 * the oplog and/or change collection.
 */
class OplogWriterImpl : public OplogWriter {
    OplogWriterImpl(const OplogWriterImpl&) = delete;
    OplogWriterImpl& operator=(const OplogWriterImpl&) = delete;

public:
    /**
     * Constructs this OplogWriter with specific options.
     */
    OplogWriterImpl(executor::TaskExecutor* executor,
                    OplogBuffer* writeBuffer,
                    OplogBuffer* applyBuffer,
                    ReplicationCoordinator* replCoord,
                    StorageInterface* storageInterface,
                    Observer* observer,
                    const OplogWriter::Options& options);

    /**
     * Writes a batch of oplog entries to the oplog and/or the change collection.
     *
     * The current implementation uses one thread to write to the oplog collection,
     * and in serverless environment uses another thread to write to the serverless
     * change collection in parallel.
     *
     * If the batch write is successful, returns the optime of the last op written,
     * which should be the last op in the batch.
     *
     * External states such as oplog visibility, replication opTimes and journaling
     * are not updated in this function.
     */
    StatusWith<OpTime> writeOplogBatch(OperationContext* opCtx,
                                       const std::vector<BSONObj>& ops) override;

    /**
     * Finalizes the batch after writing it to storage, which updates various external
     * components that care about the opTime of the last op written in this batch.
     */
    void finalizeOplogBatch(OperationContext* opCtx,
                            const OpTimeAndWallTime& lastOpTimeAndWallTime,
                            bool flushJournal);

private:
    using writeDocsFn = std::function<Status(OperationContext*,
                                             std::vector<InsertStatement>::const_iterator,
                                             std::vector<InsertStatement>::const_iterator)>;

    /**
     * Runs oplog write in a loop until shutdown() is called.
     *
     * Retrieves operations from the writeBuffer in batches that will be applied using
     * writeOplogBatch(), after which the batches will be pushed to the applyBuffer.
     */
    void _run() override;

    void _writeOplogBatchImpl(OperationContext* opCtx,
                              const std::vector<InsertStatement>& docs,
                              bool writeOplogColl,
                              bool writeChangeColl);

    OplogBuffer* const _applyBuffer;

    // Not owned by us.
    ReplicationCoordinator* const _replCoord;

    // Not owned by us.
    StorageInterface* _storageInterface;

    // Not owned by us.
    Observer* const _observer;

    bool _applyBufferInDrainMode = false;
};

}  // namespace repl
}  // namespace mongo

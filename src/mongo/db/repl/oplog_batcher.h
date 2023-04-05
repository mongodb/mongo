/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace repl {

constexpr std::size_t kMaxPrepareOpsPerBatch = 128;

class OplogApplier;

/**
 * Stores a batch of oplog entries for oplog application.
 */
class OplogBatch {
public:
    explicit OplogBatch(std::size_t batchLimitOps) {
        _batch.reserve(batchLimitOps);
    }
    bool empty() const {
        return _batch.empty();
    }
    const OplogEntry& front() const {
        invariant(!_batch.empty());
        return _batch.front();
    }
    const OplogEntry& back() const {
        invariant(!_batch.empty());
        return _batch.back();
    }
    const std::vector<OplogEntry>& getBatch() const {
        return _batch;
    }

    void emplace_back(OplogEntry oplog) {
        invariant(!_mustShutdown);
        _batch.emplace_back(std::move(oplog));
    }
    void pop_back() {
        _batch.pop_back();
    }

    /**
     * A batch with this set indicates that the upstream stages of the pipeline are shutdown and
     * no more batches will be coming.
     *
     * This can only happen with empty batches.
     */
    bool mustShutdown() const {
        return _mustShutdown;
    }
    void setMustShutdownFlag() {
        invariant(empty());
        _mustShutdown = true;
    }

    /**
     * Passes the term when the buffer is exhausted to a higher level in case the node has stepped
     * down and then stepped up again. See its caller for more context.
     */
    boost::optional<long long> termWhenExhausted() const {
        return _termWhenExhausted;
    }
    void setTermWhenExhausted(long long term) {
        invariant(empty());
        _termWhenExhausted = term;
    }

    /**
     * Leaves this object in an unspecified state. Only assignment and destruction are valid.
     */
    std::vector<OplogEntry> releaseBatch() {
        return std::move(_batch);
    }

private:
    std::vector<OplogEntry> _batch;
    bool _mustShutdown = false;
    boost::optional<long long> _termWhenExhausted;
};

/**
 * Consumes batches of oplog entries from the OplogBuffer to give to the oplog applier, freeing
 * up space for more operations to be fetched from a sync source and allocated onto the OplogBuffer.
 */
class OplogBatcher {
    OplogBatcher(const OplogBatcher&) = delete;
    OplogBatcher& operator=(const OplogBatcher&) = delete;

public:
    /**
     * Controls what can popped from the oplog buffer into a single batch of operations that can be
     * applied using OplogApplier::applyOplogBatch().
     */
    class BatchLimits {
    public:
        size_t bytes = 0;
        size_t ops = 0;

        // If provided, the batch will not include any operations with timestamps after this point.
        // This is intended for implementing secondaryDelaySecs, so it should be some number of
        // seconds before now.
        boost::optional<Date_t> secondaryDelaySecsLatestTimestamp = {};

        // If non-null, the batch will include operations with timestamps either
        // before-and-including this point or after it, not both.
        Timestamp forceBatchBoundaryAfter;
    };

    /**
     * Constructs an OplogBatcher
     */
    OplogBatcher(OplogApplier* oplogApplier, OplogBuffer* oplogBuffer);

    virtual ~OplogBatcher();

    /**
     * Returns the batch of oplog entries and clears _ops so the batcher can store a new batch.
     */
    OplogBatch getNextBatch(Seconds maxWaitTime);

    /**
     * Starts up a thread to continuously pull from the OplogBuffer into the OplogBatcher's oplog
     * batch.
     */
    void startup(StorageInterface* storageInterface);

    /**
     * Shuts down the thread that pulls from the OplogBuffer to the oplog batch.
     */
    void shutdown();

    /**
     * Returns a new batch of ops to apply.
     * A batch may consist of:
     *     at most "BatchLimits::ops" OplogEntries
     *     at most "BatchLimits::bytes" worth of OplogEntries
     *     only OplogEntries from before the "BatchLimits::secondaryDelaySecsLatestTimestamp" point
     *     a single command OplogEntry (excluding applyOps, which are grouped with CRUD ops)
     *
     * If waitToFillBatch is non-zero and any data is available, waits for more data up to that many
     * milliseconds from the start of the batch when the batch is not full.  The wait is
     * interruptible but aside from ending the wait, interrupts will be ignored to avoid losing
     * data. (that is, on interrupt, data already in the batch is returned immediately)
     */
    StatusWith<std::vector<OplogEntry>> getNextApplierBatch(
        OperationContext* opCtx,
        const BatchLimits& batchLimits,
        Milliseconds waitToFillBatch = Milliseconds(0));

    /**
     * Returns the number of logical operations represented by an oplog entry.
     * This is usually one but may be greater than one in certain cases, such as in a
     * commitTransaction command.
     */
    static std::size_t getOpCount(const OplogEntry& entry);

private:
    enum class BatchAction { kContinueBatch, kStartNewBatch, kProcessIndividually };

    class BatchStats {
    public:
        std::size_t totalOps = 0;
        std::size_t totalBytes = 0;
        std::size_t prepareOps = 0;
    };

    /**
     * Returns how we should batch an oplog entry: grouping with the current batch, starting a new
     * new batch, or processing it individually in its own batch.
     */
    BatchAction _getBatchActionForEntry(const OplogEntry& entry, const BatchStats& batchStats);

    /**
     * If secondaryDelaySecs is enabled, this function calculates the most recent timestamp of any
     * oplog entries that can be be returned in a batch.
     */
    boost::optional<Date_t> _calculateSecondaryDelaySecsLatestTimestamp();

    /**
     * Pops the operation at the front of the OplogBuffer.
     */
    void _consume(OperationContext* opCtx, OplogBuffer* oplogBuffer);

    void _run(StorageInterface* storageInterface);

    OplogApplier* _oplogApplier;
    OplogBuffer* const _oplogBuffer;

    Mutex _mutex = MONGO_MAKE_LATCH("OplogBatcher::_mutex");
    stdx::condition_variable _cv;

    /**
     * The latest batch of oplog entries ready for the applier.
     */
    OplogBatch _ops;

    std::unique_ptr<stdx::thread> _thread;
};

/**
 * Returns maximum number of operations in each batch that can be applied using
 * applyOplogBatch().
 */
std::size_t getBatchLimitOplogEntries();

/**
 * Calculates batch limit size (in bytes) using the maximum capped collection size of the oplog
 * size.  Must not be called from within a WriteUnitOfWork.
 * Batches are limited to 10% of the oplog.
 */
std::size_t getBatchLimitOplogBytes(OperationContext* opCtx, StorageInterface* storageInterface);

}  // namespace repl
}  // namespace mongo

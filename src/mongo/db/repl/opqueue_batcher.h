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

#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/repl/oplog_applier_impl.h"

namespace mongo {
namespace repl {

class OpQueue {
public:
    explicit OpQueue(std::size_t batchLimitOps) : _bytes(0) {
        _batch.reserve(batchLimitOps);
    }

    size_t getBytes() const {
        return _bytes;
    }
    size_t getCount() const {
        return _batch.size();
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
        _bytes += oplog.getRawObjSizeBytes();
        _batch.emplace_back(std::move(oplog));
    }
    void pop_back() {
        _bytes -= back().getRawObjSizeBytes();
        _batch.pop_back();
    }

    /**
     * A batch with this set indicates that the upstream stages of the pipeline are shutdown and
     * no more batches will be coming.
     *
     * This can only happen with empty batches.
     *
     * TODO replace the empty object used to signal draining with this.
     */
    bool mustShutdown() const {
        return _mustShutdown;
    }
    void setMustShutdownFlag() {
        invariant(empty());
        _mustShutdown = true;
    }

    /**
     * If the oplog buffer is exhausted, return the term before we learned that the buffer was
     * empty.
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
    size_t _bytes;
    bool _mustShutdown = false;
    boost::optional<long long> _termWhenExhausted;
};

class OpQueueBatcher {
    OpQueueBatcher(const OpQueueBatcher&) = delete;
    OpQueueBatcher& operator=(const OpQueueBatcher&) = delete;

public:
    /**
     * Constructs an OpQueueBatcher
     */
    OpQueueBatcher(OplogApplier* oplogApplier,
                   StorageInterface* storageInterface,
                   OplogBuffer* oplogBuffer,
                   OplogApplier::GetNextApplierBatchFn getNextApplierBatchFn);

    virtual ~OpQueueBatcher();

    /**
     *  Retrieves the next batch of ops that are ready to apply.
     */
    OpQueue getNextBatch(Seconds maxWaitTime);

private:
    /**
     * If slaveDelay is enabled, this function calculates the most recent timestamp of any oplog
     * entries that can be be returned in a batch.
     */
    boost::optional<Date_t> _calculateSlaveDelayLatestTimestamp();

    void run();

    OplogApplier* _oplogApplier;
    StorageInterface* const _storageInterface;
    OplogBuffer* const _oplogBuffer;
    OplogApplier::GetNextApplierBatchFn const _getNextApplierBatchFn;

    Mutex _mutex = MONGO_MAKE_LATCH("OpQueueBatcher::_mutex");
    stdx::condition_variable _cv;
    OpQueue _ops;

    // This only exists so the destructor invariants rather than deadlocking.
    bool _isDead = false;

    stdx::thread _thread;  // Must be last so all other members are initialized before starting.
};


}  // namespace repl
}  // namespace mongo

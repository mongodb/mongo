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

#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"

namespace mongo {
namespace repl {

/**
 * Oplog buffer backed by a bounded, in-memory queue that supports batched operations
 * like tryPopBatch() but does not support point operations like peek(), tryPop().
 *
 * Values of this buffer are stored in batches and popped out in batches, in the same
 * way as they were pushed in. An important assumption is that each batch pushed into
 * the buffer is not too large in byte size, normally less than 16MB.
 *
 * Note: This buffer only works for single-producer, single-consumer use cases.
 */
class OplogBufferBatchedQueue final : public OplogBuffer {
public:
    explicit OplogBufferBatchedQueue(std::size_t maxSize);
    OplogBufferBatchedQueue(std::size_t maxSize, Counters* counters);

    void startup(OperationContext* opCtx) override;
    void shutdown(OperationContext* opCtx) override;
    void push(OperationContext* opCtx,
              Batch::const_iterator begin,
              Batch::const_iterator end,
              boost::optional<std::size_t> bytes = boost::none) override;
    void waitForSpace(OperationContext* opCtx, std::size_t size) override;
    bool isEmpty() const override;
    std::size_t getMaxSize() const override;
    std::size_t getSize() const override;
    std::size_t getCount() const override;
    void clear(OperationContext* opCtx) override;
    bool tryPop(OperationContext* opCtx, Value* value) {
        MONGO_UNIMPLEMENTED;
    }
    bool tryPopBatch(OperationContext* opCtx, OplogBatch<Value>* batch) override;
    bool waitForDataFor(Milliseconds waitDuration, Interruptible* interruptible) override;
    bool waitForDataUntil(Date_t deadline, Interruptible* interruptible) override;
    bool peek(OperationContext* opCtx, Value* value) {
        MONGO_UNIMPLEMENTED;
    }
    boost::optional<Value> lastObjectPushed(OperationContext* opCtx) const {
        MONGO_UNIMPLEMENTED;
    };

    // In drain mode, the queue does not block. It is the responsibility of the caller to ensure
    // that no items are added to the queue while in drain mode; this is enforced by invariant().
    void enterDrainMode() final;
    void exitDrainMode() final;
    bool inDrainMode() final;

private:
    void _waitForSpace_inlock(stdx::unique_lock<Latch>& lk, std::size_t size);
    void _clear_inlock(WithLock lk);

    mutable Mutex _mutex = MONGO_MAKE_LATCH("OplogBufferBatchedQueue::_mutex");
    stdx::condition_variable _notEmptyCV;
    stdx::condition_variable _notFullCV;
    const std::size_t _maxSize;
    std::size_t _curSize = 0;
    std::size_t _waitSize = 0;
    std::size_t _curCount = 0;
    bool _drainMode = false;
    bool _isShutdown = false;
    Counters* const _counters;
    std::list<OplogBatch<Value>> _queue;
};

}  // namespace repl
}  // namespace mongo

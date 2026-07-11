// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"

#include <mutex>

namespace [[MONGO_MOD_PUBLIC]] mongo {
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
              boost::optional<const Cost&> cost = boost::none) override;
    void waitForSpace(OperationContext* opCtx, const Cost& cost) override;
    bool isEmpty() const override;
    std::size_t getSize() const override;
    std::size_t getCount() const override;
    void clear(OperationContext* opCtx) override;
    bool tryPop(OperationContext* opCtx, Value* value) override {
        MONGO_UNIMPLEMENTED;
    }
    bool tryPopBatch(OperationContext* opCtx, OplogBatch<Value>* batch) override;
    bool waitForDataFor(Milliseconds waitDuration, Interruptible* interruptible) override;
    bool waitForDataUntil(Date_t deadline, Interruptible* interruptible) override;
    bool peek(OperationContext* opCtx, Value* value) override {
        MONGO_UNIMPLEMENTED;
    }
    boost::optional<Value> lastObjectPushed(OperationContext* opCtx) const override {
        MONGO_UNIMPLEMENTED;
    };

    // In drain mode, the queue does not block. It is the responsibility of the caller to ensure
    // that no items are added to the queue while in drain mode; this is enforced by invariant().
    void enterDrainMode() final;
    void exitDrainMode() final;

private:
    void _waitForSpace(std::unique_lock<std::mutex>& lk, std::size_t size);
    void _clear(WithLock lk);

    mutable std::mutex _mutex;
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

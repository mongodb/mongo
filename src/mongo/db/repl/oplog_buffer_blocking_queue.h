// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/duration.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <mutex>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {

/**
 * Oplog buffer backed by an in-memory blocking queue that supports point operations
 * like peek(), tryPop() but does not support batch operations like tryPopBatch().
 *
 * Note: This buffer only works for single-producer, single-consumer use cases.
 */
class OplogBufferBlockingQueue final : public OplogBuffer {
public:
    struct Options {
        bool clearOnShutdown = true;
        Options() {}
    };

    explicit OplogBufferBlockingQueue(std::size_t maxSize, std::size_t maxCount);
    OplogBufferBlockingQueue(std::size_t maxSize,
                             std::size_t maxCount,
                             Counters* counters,
                             Options options);

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
    bool tryPop(OperationContext* opCtx, Value* value) override;
    bool waitForDataFor(Milliseconds waitDuration, Interruptible* interruptible) override;
    bool waitForDataUntil(Date_t deadline, Interruptible* interruptible) override;
    bool peek(OperationContext* opCtx, Value* value) override;
    boost::optional<Value> lastObjectPushed(OperationContext* opCtx) const override;

    // In drain mode, the queue does not block. It is the responsibility of the caller to ensure
    // that no items are added to the queue while in drain mode; this is enforced by invariant().
    void enterDrainMode() final;
    void exitDrainMode() final;

private:
    void _waitForSpace(std::unique_lock<std::mutex>& lk, const Cost& cost);
    void _clear(WithLock lk);
    void _push(Batch::const_iterator begin, Batch::const_iterator end, const Cost& cost);

    mutable std::mutex _mutex;
    stdx::condition_variable _notEmptyCV;
    stdx::condition_variable _notFullCV;
    const std::size_t _maxSize;
    const std::size_t _maxCount;
    std::size_t _curSize = 0;
    std::size_t _curCount = 0;
    std::size_t _waitSize = 0;
    std::size_t _waitCount = 0;
    bool _drainMode = false;
    bool _isShutdown = false;
    Counters* const _counters;
    std::deque<BSONObj> _queue;
    const Options _options;
};

}  // namespace repl
}  // namespace mongo

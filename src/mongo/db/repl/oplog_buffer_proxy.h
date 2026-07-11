// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/util/duration.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <memory>
#include <mutex>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

class StorageInterface;

/**
 * Oplog buffer proxy that caches front and back (most recently pushed) oplog entries in the target
 * oplog buffer.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] OplogBufferProxy : public OplogBuffer {
    OplogBufferProxy(const OplogBufferProxy&) = delete;
    OplogBufferProxy& operator=(const OplogBufferProxy&) = delete;

public:
    explicit OplogBufferProxy(std::unique_ptr<OplogBuffer> target);

    /**
     * Returns target oplog buffer.
     */
    OplogBuffer* getTarget() const;

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

    // ---- Testing API ----
    boost::optional<Value> getLastPeeked_forTest() const;

private:
    // Target oplog buffer. Owned by us.
    std::unique_ptr<OplogBuffer> _target;

    // If both mutexes have to be acquired, acquire _lastPushedMutex first.
    mutable std::mutex _lastPushedMutex;
    boost::optional<Value> _lastPushed;

    mutable std::mutex _lastPeekedMutex;
    boost::optional<Value> _lastPeeked;
};

}  // namespace repl
}  // namespace mongo

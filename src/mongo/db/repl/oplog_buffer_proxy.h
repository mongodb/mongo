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

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <memory>

#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

class StorageInterface;

/**
 * Oplog buffer proxy that caches front and back (most recently pushed) oplog entries in the target
 * oplog buffer.
 */
class OplogBufferProxy : public OplogBuffer {
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
              boost::optional<std::size_t> bytes = boost::none) override;
    void waitForSpace(OperationContext* opCtx, std::size_t size) override;
    bool isEmpty() const override;
    std::size_t getMaxSize() const override;
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
    mutable Mutex _lastPushedMutex = MONGO_MAKE_LATCH("OplogBufferProxy::_lastPushedMutex");
    boost::optional<Value> _lastPushed;

    mutable Mutex _lastPeekedMutex = MONGO_MAKE_LATCH("OplogBufferProxy::_lastPeekedMutex");
    boost::optional<Value> _lastPeeked;
};

}  // namespace repl
}  // namespace mongo

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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/repl/oplog_batch.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

class OperationContext;

namespace repl {

/**
 * Interface for temporary container of oplog entries (in BSON format) from sync source by
 * OplogFetcher that will be read by applier in the InitialSyncer.
 *
 * Implementations are only required to support one pusher and one popper.
 */
class MONGO_MOD_OPEN OplogBuffer {
    OplogBuffer(const OplogBuffer&) = delete;
    OplogBuffer& operator=(const OplogBuffer&) = delete;

public:
    /**
     * Type of item held in the oplog buffer;
     */
    using Value = BSONObj;

    /**
     * Batch of oplog entries (in BSON format) for bulk read/write operations.
     */
    using Batch = std::vector<Value>;

    /**
     * Cost of items in this oplog buffer.
     */
    struct Cost;

    /**
     * Counters for this oplog buffer.
     */
    class Counters;

    OplogBuffer() = default;
    virtual ~OplogBuffer() = default;

    /**
     * Causes the oplog buffer to initialize its internal state (start threads if appropriate,
     * create backing storage, etc). This method may be called at most once for the lifetime of an
     * oplog buffer.
     */
    virtual void startup(OperationContext* opCtx) = 0;

    /**
     * Signals to the oplog buffer that it should shut down. This method may block. After
     * calling shutdown, it is illegal to perform read/write operations on this oplog buffer.
     *
     * It is legal to call this method multiple times, but it should only be called after startup
     * has been called.
     */
    virtual void shutdown(OperationContext* opCtx) = 0;

    /**
     * Pushes operations in the iterator range [begin, end) into the oplog buffer without blocking.
     */
    virtual void push(OperationContext* opCtx,
                      Batch::const_iterator begin,
                      Batch::const_iterator end,
                      boost::optional<const Cost&> cost = boost::none) = 0;

    /**
     * Returns when enough space is available.
     */
    virtual void waitForSpace(OperationContext* opCtx, const Cost& cost) = 0;

    /**
     * Returns true if oplog buffer is empty.
     */
    virtual bool isEmpty() const = 0;

    /**
     * Total size of all oplog entries in this oplog buffer as measured by the BSONObj::objsize()
     * function.
     */
    virtual std::size_t getSize() const = 0;

    /**
     * Returns the number/count of items in the oplog buffer.
     */
    virtual std::size_t getCount() const = 0;

    /**
     * Clears oplog buffer.
     */
    virtual void clear(OperationContext* opCtx) = 0;

    /**
     * Returns false if oplog buffer is empty. "value" is left unchanged.
     * Otherwise, removes last item (saves in "value") from the oplog buffer and returns true.
     */
    virtual bool tryPop(OperationContext* opCtx, Value* value) = 0;

    /**
     * Returns false if oplog buffer is empty. "batch" is left unchanged.
     * Otherwise, removes last batch (saves in "batch") from the oplog buffer and returns true.
     */
    virtual bool tryPopBatch(OperationContext* opCtx, OplogBatch<Value>* batch) {
        MONGO_UNIMPLEMENTED;
    }

    /**
     * Waits uninterruptibly for "waitDuration" for an operation to be pushed into the oplog buffer.
     * Returns false if oplog buffer is still empty after "waitDuration".
     * Otherwise, returns true.
     */
    bool waitForData(Seconds waitDuration) {
        return waitForDataFor(duration_cast<Milliseconds>(waitDuration),
                              Interruptible::notInterruptible());
    };

    /**
     * Interruptible wait with millisecond granularity.
     *
     * Waits "waitDuration" for an operation to be pushed into the oplog buffer.
     * Returns false if oplog buffer is still empty after "waitDuration".
     * Otherwise, returns true.
     * Throws if the interruptible is interrupted.
     */
    virtual bool waitForDataFor(
        Milliseconds waitDuration,
        Interruptible* interruptible = Interruptible::notInterruptible()) = 0;

    /**
     * Same as waitForDataFor(Milliseconds, Interruptible) above but takes a deadline instead
     * of a duration.
     */
    virtual bool waitForDataUntil(
        Date_t deadline, Interruptible* interruptible = Interruptible::notInterruptible()) = 0;

    /**
     * Returns false if oplog buffer is empty.
     * Otherwise, returns true and sets "value" to last item in oplog buffer.
     */
    virtual bool peek(OperationContext* opCtx, Value* value) = 0;

    /**
     * Returns the item most recently added to the oplog buffer or nothing if the buffer is empty.
     */
    virtual boost::optional<Value> lastObjectPushed(OperationContext* opCtx) const = 0;

    /**
     * Enters "drain mode".  May only be called by the producer.  When the buffer is in drain mode,
     * "waitForData" will return immediately even if there is data in the queue.  This
     * is an optimization and subclasses may choose not to implement this function.
     */
    virtual void enterDrainMode() {
        MONGO_UNIMPLEMENTED;
    }

    /**
     * Leaves "drain mode".  May only be called by the producer.
     */
    virtual void exitDrainMode() {
        MONGO_UNIMPLEMENTED;
    }
};

struct OplogBuffer::Cost {
    std::size_t size = 0;
    std::size_t count = 0;
};

class MONGO_MOD_PRIVATE OplogBuffer::Counters {
public:
    // Number of operations in this OplogBuffer.
    Counter64 count;

    // Total size of operations in this OplogBuffer. Measured in bytes.
    Counter64 size;

    // Maximum number of operations in this OplogBuffer.
    Counter64 maxCount;

    // Maximum size of operations in this OplogBuffer. Measured in bytes.
    Counter64 maxSize;

    /**
     * Sets maximum number of operations for this OplogBuffer.
     * This function should only be called by a single thread.
     */
    void setMaxCount(std::size_t newMaxCount) {
        maxCount.increment(newMaxCount - maxCount.get());
    }

    /**
     * Sets maximum size of operations for this OplogBuffer.
     * This function should only be called by a single thread.
     */
    void setMaxSize(std::size_t newMaxSize) {
        maxSize.increment(newMaxSize - maxSize.get());
    }

    /**
     * Clears counters.
     * This function should only be called by a single thread.
     */
    void clear() {
        count.decrement(count.get());
        size.decrement(size.get());
    }

    void increment(const Value& value) {
        count.increment(1);
        size.increment(std::size_t(value.objsize()));
    }

    void incrementN(std::size_t cnt, std::size_t sz) {
        count.increment(cnt);
        size.increment(sz);
    }

    void decrement(const Value& value) {
        count.decrement(1);
        size.decrement(std::size_t(value.objsize()));
    }

    void decrementN(std::size_t cnt, std::size_t sz) {
        count.decrement(cnt);
        size.decrement(sz);
    }
};

class MONGO_MOD_OPEN OplogBufferMetrics {
public:
    OplogBuffer::Counters* getWriteBufferCounter() {
        return &_writeBufferCounter;
    }

    OplogBuffer::Counters* getApplyBufferCounter() {
        return &_applyBufferCounter;
    }

    BSONObj getReport() const {
        BSONObjBuilder applierSubBuilder;
        applierSubBuilder.append("count", _applyBufferCounter.count.get());
        applierSubBuilder.append("sizeBytes", _applyBufferCounter.size.get());
        applierSubBuilder.append("maxSizeBytes", _applyBufferCounter.maxSize.get());
        if (feature_flags::gReduceMajorityWriteLatency.isEnabled()) {
            BSONObjBuilder builder;
            BSONObjBuilder writerSubBuilder;
            writerSubBuilder.append("count", _writeBufferCounter.count.get());
            writerSubBuilder.append("sizeBytes", _writeBufferCounter.size.get());
            writerSubBuilder.append("maxSizeBytes", _writeBufferCounter.maxSize.get());
            builder.append("write", writerSubBuilder.obj());

            applierSubBuilder.append("maxCount", _applyBufferCounter.maxCount.get());
            builder.append("apply", applierSubBuilder.obj());

            return builder.obj();
        }
        return applierSubBuilder.obj();
    }

    operator BSONObj() const {
        return getReport();
    }

private:
    OplogBuffer::Counters _writeBufferCounter;
    OplogBuffer::Counters _applyBufferCounter;
};

/**
 * An OplogBuffer interface which also supports random access by timestamp.
 * The entries in a RandomAccessOplogBuffer must be pushed in strict timestamp order.
 *
 * The user of a RandomAccessOplogBuffer may seek to or find timestamps which have already been read
 * from the buffer.  It is up to the implementing subclass to ensure that such timestamps are
 * available to be read.
 */
class MONGO_MOD_PRIVATE RandomAccessOplogBuffer : public OplogBuffer {
public:
    enum SeekStrategy {
        kInexact = 0,
        kExact = 1,
    };

    /**
     * Retrieves an oplog entry by timestamp. Returns ErrorCodes::NoSuchKey if no such entry is
     * found.  Does not change current position of oplog buffer.
     */
    virtual StatusWith<Value> findByTimestamp(OperationContext* opCtx, const Timestamp& ts) = 0;

    /**
     * Change current position of oplog buffer to point to the entry with timestamp 'ts'.  If
     * 'exact' is true, return NoSuchKey if the timestamp is not found. Otherwise, position will
     * be before the next timestamp greater than or equal to 'ts'.
     */
    virtual Status seekToTimestamp(OperationContext* opCtx,
                                   const Timestamp& ts,
                                   SeekStrategy exact = SeekStrategy::kExact) = 0;
};

}  // namespace repl
}  // namespace mongo

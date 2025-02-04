/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

class ConcurrentMemoryAggregator;
class ChunkedMemoryAggregator;

// Updatable version of `MemoryUsageTokenImpl`.
template <typename Tracker>
class MemoryUsageHandleImpl : public MemoryUsageTokenImpl<Tracker> {
public:
    MemoryUsageHandleImpl(Tracker* tracker = nullptr) : MemoryUsageTokenImpl<Tracker>(0, tracker) {}
    MemoryUsageHandleImpl(int64_t initial, Tracker* tracker = nullptr)
        : MemoryUsageTokenImpl<Tracker>(initial, tracker) {}

    void add(int64_t diff) {
        this->_curMemoryUsageBytes += diff;
        this->_tracker->add(diff);
    }

    void set(int64_t total) {
        add(total - this->_curMemoryUsageBytes);
    }
};  // class MemoryUsageHandleImpl

using SimpleMemoryUsageHandle = MemoryUsageHandleImpl<SimpleMemoryUsageTracker>;

/**
 * Leaf nodes of the tree, which are allocated from `ChunkedMemoryAggregator` nodes. These are meant
 * to only ever be used in a single-threaded context. These handles are how memory updates are made
 * to the tree, no updates can be made directly to `ConcurrentMemoryAggregator` or
 * `ChunkedMemoryAggregator`, they must all be made through the `MemoryUsageHandle`. A
 * `ChunkedMemoryAggregator` can have many `MemoryUsageHandle` nodes under it. Updates are always
 * sent to the `ChunkedMemoryAggregator` every time a child `MemoryUsageHandle` is updated.
 */
using MemoryUsageHandle = MemoryUsageHandleImpl<ChunkedMemoryAggregator>;

/**
 * Tracks memory usage in a thread-safe manner through `MemoryUsageHandle`s which are allocated
 * through `createUsageHandle()`. You can have many active usage handles for a single
 * `ChunkedMemoryAggregator` (e.g. you can call `createUsageHandle()` more than once), and each
 * individual handle will propagate it's memory updates to the same `ChunkedMemoryAggregator`
 * instance.
 *
 * `ChunkedMemoryAggregator` reports approximate memory usage to the parent
 * `ConcurrentMemoryAggregator`, based on
 * `ChunkedMemoryAggregator::Options::memoryUsageUpdateBatchSize`. The larger the batch size, the
 * less updates to the parent memory aggregator but less accurate aggregated memory usage in the
 * parent aggregator, while the smaller the batch size, the more frequent updates to the parent
 * memory aggregator and more accurate aggregated memory usage in the parent aggregator. This will
 * *always* over-count the memory usage and never under-count, e.g. if the batch size is 32MB, and a
 * usage handle under a `ChunkedMemoryAggregator` uses 10 bytes, that will be rounded up and
 * reported to the parent `ConcurrentMemoryAggregator` as 32MB of memory usage.
 *
 * You should use `ConcurrentMemoryAggregator::createChunkedMemoryAggregator` to create an instance
 * of a chunked memory aggregator. All updates made to a chunked memory aggregator's usage handle is
 * propagated to the chunked memory aggregator, which then propagates it to the parent
 * `ConcurrentMemoryAggregator` only if there has been a significant change in memory since the last
 * update to the `ConcurrentMemoryAggregator` to avoid contention since memory updates may be very
 * frequent and since `ConcurrentMemoryAggregator` is meant to be shared across multiple threads.
 */
class ChunkedMemoryAggregator {
private:
    struct PrivateTag {};

public:
    struct Options {
        /**
         * `ChunkedMemoryAggregator` will only publish changes upstream to the parent
         * `ConcurrentMemoryAggregator` when the change in memory is significant, which is driven by
         * this threshold.
         */
        int64_t memoryUsageUpdateBatchSize{32 * 1024 * 1024};
    };  // struct Options

    /**
     * Creates a chunked memory aggregator as a child under the parent memory aggregator. This
     * should only be created through `ConcurrentMemoryAggregator::createChunkedMemoryAggregator`.
     */
    ChunkedMemoryAggregator(PrivateTag,
                            Options options,
                            int64_t id,
                            ConcurrentMemoryAggregator* parent);

    ~ChunkedMemoryAggregator();

    /**
     * Creates a usage handle that can be used to propagate memory usage updates to
     * this chunked memory aggregator. You can have multiple active usage handles for a
     * `ChunkedMemoryAggregator`, but each `MemoryUsageHandle` is meant to only be used in
     * a single threaded context.
     */
    MemoryUsageHandle createUsageHandle(int64_t initialBytes = 0);

    /**
     * Returns the current memory usage for this `ChunkedMemoryAggregator`. This will always be
     * the exact number reported by the children `MemoryUsageHandle` and `MemoryUsageToken`
     * instances.
     */
    int64_t getCurrentMemoryUsageBytes() const;

    /**
     * Returns the unique ID that was assigned to this `ChunkedMemoryAggregator` upon construction.
     */
    int64_t getId() const;

    /**
     * Triggers the usage monitor callback to be invoked in the parent memory aggregator.
     */
    void poll();

private:
    friend class ConcurrentMemoryAggregator;
    friend class MemoryUsageTokenImpl<ChunkedMemoryAggregator>;
    friend class MemoryUsageHandleImpl<ChunkedMemoryAggregator>;

    /**
     * Updates the current memory usage of the chunked memory aggregator. This will propagate the
     * delta changes upstream to the parent `ConcurrentMemoryAggregator` only if the accumulated
     * memory usage has changed by atleast
     * `ChunkedMemoryAggregator::Options::memoryUsageUpdateBatchSize` since the last update to the
     * parent `ConcurrentMemoryAggregator`, so that we avoid frequent small
     * +/- updates.
     */
    void add(int64_t delta);

    /**
     * Computes the approximate update to send to the upstream `ConcurrentMemoryAggregator`. This is
     * an estimate based on `ChunkedMemoryAggregator::memoryUsageUpdateBatchSize`, but it will
     * always over count and never under count.
     */
    int64_t computeUpstreamUpdate(int64_t oldValue, int64_t newValue) const;

    Options _options;

    /**
     * ID from the parent `ConcurrentMemoryAggregator` to uniquely identify this
     * `ChunkedMemoryAggregator`.
     */
    int64_t _id{0};

    // Parent memory aggregator to propagate updates to.
    ConcurrentMemoryAggregator* _parent{nullptr};

    // Current memory usage for this chunked memory aggregator.
    AtomicWord<int64_t> _curMemoryUsageBytes;
};  // class ChunkedMemoryAggregator

};  // namespace mongo
